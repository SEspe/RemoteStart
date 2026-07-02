# Functional Specification Document
## Remote Start System — Honda EU70IS & Wallas Heater
**Version:** 1.35  
**Author:** SEspe  
**Date:** 2026-07-02  
**Changelog:**
- v1.35 — **Root-caused and fixed the day-long intermittent "web server unreachable while ESP-NOW keeps working" pattern on all units: httpd session-socket exhaustion.** ESP-IDF's `HTTPD_DEFAULT_CONFIG()` allows 7 concurrent session sockets with `lru_purge_enable` **off** — every client connection abandoned without a clean close (a browser keep-alive, a monitoring poll that hit its client-side timeout, a process exiting mid-request) permanently occupies a slot, and once all 7 are held by dead sessions the server stops servicing HTTP entirely until a reboot. Definitively diagnosed live on MasterRemote: ICMP ping fine (6–10 ms), raw TCP handshake to :80 *accepted* (lwIP's kernel-level backlog), but HTTP requests never serviced — the application-level worker had no free session slots. This, alongside the v1.32 power-save latency, was the second half of the recurring flakiness: heavy status polling with hard client timeouts (as done throughout 2026-07-02) is exactly the traffic pattern that leaks sessions fastest, which is why the units being actively monitored were the ones that kept wedging, and why a reboot always "fixed" it. Fixed with `cfg.lru_purge_enable = true` on **all four units** — when the pool is full, the least-recently-used session is evicted to admit the new connection (the standard ESP-IDF remedy; worst case an idle browser tab gets its keep-alive dropped and transparently reconnects). MasterRemote 1.5.9, SlaveHonda 1.3.10, SlaveWallas 1.5.9, ESPNowRelay 1.0.1; no data-model or API change.
- v1.34 — **New fourth unit: ESPNowRelay (ESP32-C6, v1.0.0)** — an optional store-and-forward ESP-NOW relay/repeater that extends radio reach between MasterRemote and a slave that can't hear it directly (§13). Designed as a **transparent proxy requiring zero firmware changes on the existing three units**: it re-broadcasts MasterRemote's discovery beacon verbatim (a far slave adopts the relay as its "Master" and heartbeats to it), forwards those heartbeats verbatim to the real Master (which registers the relay's MAC as that role's holder), and forwards Master's commands verbatim to the real slave (destination picked from the command's label). Trust and direct↔relayed failover come free from the existing MAC + 30 s staleness rules — every unit trusts its immediate radio peer, whichever path a unit hears consistently holds the role claim, and a stale path re-homes automatically. When all direct links are healthy the relay sits in passive standby. Same infrastructure as the other units: web UI (Status tab with per-class traffic counters — Rx/Fwd/Fail, last sender/receiver MAC, RSSI, last seen — a live peer roster, WiFi Link and System cards; OTA Update tab), first-boot config portal (`ESPNowRelay-Config`), `/api/sysinfo`, `WIFI_PS_NONE` + max TX power, indefinite post-boot WiFi reconnect, URL-decoded portal credentials, httpd handler-cap headroom + checked registration — all prior lessons applied from day one. WiFi-preferred with SlaveHonda-style ESP-NOW channel-scan fallback (relays without an IP/web UI if the router is unreachable). Onboard LED (GPIO2): fast blink = traffic forwarded within the last 10 s, slow = idle/standby. CI: new `build-relay` job, `relay-v*` single-unit tag pattern, and inclusion in unified `v*` releases.
- v1.33 — Both slaves' Status tab gains a **WiFi Link** card (AP signal strength in dBm + channel), mirroring MasterRemote's — previously the slaves' own web UI showed no radio info at all, so judging their router link quality required checking MasterRemote's Nodes tab (which shows the ESP-NOW link RSSI, a different radio path) or guessing. `/api/status` on both slaves gains `rssi`/`ch` fields sourced from `esp_wifi_sta_get_ap_info()`; if the STA isn't associated (SlaveHonda in ESP-NOW fallback — though its web UI is unreachable in that state anyway), the fields read 0 and the UI shows "not associated". SlaveHonda 1.3.9, SlaveWallas 1.5.8; additive JSON change only.
- v1.32 — Disabled WiFi modem-sleep power save (`esp_wifi_set_ps(WIFI_PS_NONE)`) and set max TX power (`esp_wifi_set_max_tx_power(84)`, 21 dBm) on all three units, right after `esp_wifi_start()` on the STA connect path. Motivated by a persistently flaky web server on SlaveWallas even after a fresh reconnect at good RSSI (-60 dBm) — the default modem-sleep power save (confirmed active in the boot log: `wifi:pm start, type: 1`) makes the radio sleep between beacons and only wake at DTIM intervals to receive buffered packets, adding latency to every HTTP exchange independent of raw signal strength; combined with any marginal link, this compounds into the timeout/flakiness pattern seen throughout 2026-07-02. Max TX power improves the uplink (how well the router hears each unit) but doesn't affect the RSSI *readings* these units report (that's the router's TX power, which isn't under this firmware's control) — it's a secondary, low-cost improvement, not the primary fix. `set_max_tx_power` isn't `ESP_ERROR_CHECK`'d since the true ceiling is module-dependent; a rejected value just logs a warning and leaves the IDF default in place. All three units: MasterRemote 1.5.8, SlaveHonda 1.3.8, SlaveWallas 1.5.7; no data-model or wire-format change.
- v1.31 — **Fixed a live safety bug in v1.30's running-feedback exclusion: SlaveHonda was re-cranking the starter on an already-running engine roughly every 30 seconds.** Confirmed on the physical generator: a manual Start Honda from MasterRemote's web UI started the generator successfully, but the Debug tab's "starting" indicator kept flickering back to green and ignition/starter kept appearing active well after the ~14 s start sequence should have finished. Root cause: `send_status()` (SlaveHonda's 10 s heartbeat) unconditionally forced `g_honda_running = false` on every call, stomping whatever `honda_start()` had just set — so SlaveHonda *always* reported "not running" to MasterRemote no matter what actually happened. MasterRemote only stops resending a Honda start command once it sees the reported running state match the desired one (§10); with SlaveHonda permanently reporting false, Master kept resending every `HONDA_RESTART_BLOCK_MS` (30 s) forever, and each resend — since SlaveHonda also genuinely showed not-running-and-not-starting between cycles — spawned a brand new crank sequence on the already-running engine. Fixed by (1) removing `send_status()`'s `g_honda_running = false` line entirely — it no longer touches that variable, only reports whatever `honda_start()`/`honda_stop()` last set — and (2) changing `honda_start()`'s post-sequence value from `false` to `true` on the non-aborted completion path: since GPIO15 still can't measure the real state (§2.3/v1.30), this assumes success rather than failure, because a false *positive* here only costs an unretried failed start (recoverable with a manual Stop then Start), while a false *negative* was actively re-engaging the starter motor on a running engine — the more dangerous failure mode by far. `honda_stop()` is unchanged (still correctly sets `g_honda_running = false` on every stop, aborted or explicit). SlaveHonda-only (1.3.7); no data-model or wire-format change. **Deployed and verified live within minutes of discovery** — see §10/§12.
- v1.30 — A new SlaveHonda unit was deployed (§2.3), whose GPIO15 (running feedback) is not yet wired to a physical sensor. Confirmed live: with the generator and ignition genuinely off, the unit still reported `running:true` — the pin has no pull resistor configured (by design, meant to be actively driven by a real feedback sensor) and floats to a false-positive reading when unconnected. This isn't just a display bug: MasterRemote's Honda send gate only fires on a mismatch between `g_honda_start_cmd` and the slave's last-known running state, so a false "already running" reading would silently prevent a genuine start command from ever being sent — the same failure shape as the v1.17 force-send bug, but from the opposite direction (a false positive suppressing a real send, rather than a stale equal-state swallowing a manual press). Fixed by hardcoding `g_honda_running = false` at both sites that previously read the pin (`honda_start()`'s post-crank check, `send_status()`), and having `/api/status`'s `run` field report `g_honda_running` instead of independently re-reading the pin, so there's a single source of truth. GPIO15 is still configured as an input (harmless) but no longer trusted for any command or status logic. SlaveHonda-only (1.3.6); no data-model or wire-format change. **When GPIO15 is wired to a real sensor on this unit, both hardcoded-false sites need to be reverted to `!gpio_get_level(PIN_RUNNING_FB)`** (see the code comments at each site).
- v1.29 — Replaced the physical MasterRemote unit with a new board (§2.2), which changed the Wallas GPIO wiring: **GPIO4 (Wallas Start / Victron relay) is now active-LOW** (opposite of the old board and opposite of GPIO15's Honda relay input, which stayed active-HIGH), and **GPIO5 (Wallas Manual Start) is now permanently unwired/obsolete** — the physical manual button was removed from this installation and will not be reinstated. Before this fix, `g_wallas_start_cmd` read GPIO4 non-inverted (backwards — idle read as "start demanded" and vice versa) and still OR'd in GPIO5's now-floating, unpredictable reading as a live command source, which could assert a start with no real input driving it. Fixed all four recompute sites (`isr_wallas_recompute`, `gpio_init()`'s boot-time read, `timer_task`, `h_wallas_stop`) to invert GPIO4 and drop GPIO5 from the command entirely; the ISR service no longer attaches a handler to GPIO5 (still configured as a pulled-up input so it doesn't float wildly at the electrical level, just doesn't drive anything). `/api/status`'s `pWS` field now reports the corrected (inverted) reading; `pWM` still reports GPIO5's raw level for diagnostics but is labeled "unused" in the UI. Verified live against the physical Victron relay both ON and OFF before and after the fix. MasterRemote-only (1.5.7); no data-model or wire-format change. See §2.2/§10. Full-codebase review pass; six fixes, two of them serious. **(1) SlaveHonda crashed (panic + reboot) at the end of every start sequence:** `honda_start()` was spawned via `xTaskCreate` but *returned* when the sequence finished — a FreeRTOS task function must never return, so ESP-IDF aborted the whole unit ~14 s after each start command, dropping the ignition relay of the generator it had just started. Now wrapped in a proper task (`honda_start_task`) that self-deletes. **(2) A stop command during the start sequence didn't cancel it:** the 10 s warm-up and 3 s crank were single blocking delays that never re-checked `g_start_cmd`, so a stop mid-warm-up still cranked the starter for 3 s afterwards. The delays now poll `g_start_cmd` in 100 ms steps and abort promptly. Ownership of `g_honda_starting` moved to the sequence task itself (claimed by the spawner *before* task creation, cleared only by the task on exit), which also closes two duplicate-sequence races: `honda_stop()` clearing the flag mid-sequence, and Master's redundant 20 ms double-send spawning two tasks. **(3) MasterRemote's GPIO ISRs each recomputed the command from a subset of sources** — the third occurrence of the §12 OR-recompute bug class: e.g. releasing the Honda manual button recomputed `g_honda_start_cmd` *without* the Victron relay term, shutting down a generator the Victron still demanded (and unlike Wallas, Honda has no periodic recompute to self-heal). Replaced the four per-pin ISRs with two per-command ISRs (`isr_honda_recompute`/`isr_wallas_recompute`), each ORing ALL of its command's sources, attached to every input pin that feeds that command; boot-time initial reads now use the same full expressions. **(4) MasterRemote reconnect churn in portal mode:** its WiFi event handler lacked the `!g_portal_mode` guard both slaves have, so the portal's APSTA start kept retrying the failed STA credentials while the user was configuring. **(5) `/api/sysinfo` reported a garbage `heapMin`** (the `UINT32_MAX` sentinel, ~4 GB) during the first 10 s sampling interval after slave boot — `status_task` now takes a baseline sample immediately at task start. **(6) The WiFi portal saved URL-encoded credentials verbatim:** a password/SSID containing a space, `+`, `&`, or any `%XX`-encoded symbol was stored in encoded form, rebooting the unit into a connect-fail/portal loop; `h_wifi_save` now URL-decodes both fields on all three units. Also hardened `h_root` to check `esp_netif_get_ip_info()`'s return before formatting the header IP. Two review findings deliberately accepted as-is: unlocked cross-task reads of roster strings (worst case a momentarily torn string in JSON, harmless), and `/api/timer/set` treating missing query keys as 00:00 (only called by our own UI, which always sends all keys). MasterRemote 1.5.6, SlaveHonda 1.3.5, SlaveWallas 1.5.6; no data-model or wire-format change.
- v1.27 — Fixed a silent OTA-breaking bug on SlaveHonda: `httpd_config_t.max_uri_handlers` was hardcoded to `8`, exactly matching the route count *before* v1.25/v1.26 added `/api/sysinfo` — the 9th route pushed `/ota/upload` (last in the registration array) past the cap, and since the registration loop never checked `httpd_register_uri_handler()`'s return value, it failed *silently*: the server started fine, every other route worked, and only `/ota/upload` 404'd, with no log line pointing at the cause. Discovered when a routine OTA deploy of v1.10.0 failed against SlaveHonda with an immediate HTTP 404. SlaveWallas and MasterRemote turned out to have the identical fragility — both were sized to *exactly* their current route count (`9` and `14` respectively) with zero headroom, meaning either would have silently broken the same way the next time a route was added, with no warning. Fixed by (1) giving all three units headroom above their current route count (SlaveHonda/SlaveWallas → 16, MasterRemote → 22) and (2) having the registration loop log `ESP_LOGE` on any individual handler registration failure, so this class of bug is loud instead of silent from now on. MasterRemote 1.5.5, SlaveHonda 1.3.4, SlaveWallas 1.5.5; no data-model or wire-format change.
- v1.26 — Two follow-ups to the v1.25 System resource monitor and Debug GPIO tab. (1) `heapMin` previously came straight from `esp_get_minimum_free_heap_size()`, which is exact but has no notion of *when* that minimum happened — useless for telling "dipped once at boot, flat ever since" apart from "actively still dropping". Replaced with a self-sampled low-water mark (`g_heap_min`/`g_heap_min_ts_us` in `main.c`, updated in each unit's existing periodic task — `status_task` on the slaves, `timer_task` on MasterRemote) paired with a timestamp, so the value and its age are always consistent. New `heapMinAgo` field on `/api/sysinfo`; the System card now shows e.g. "180 KB — 2h 14m ago". Trades IDF's byte-exact tracking for a periodic sample (10 s slaves / 20 s Master) — an acceptable approximation for spotting a slow leak, which is the only thing this field is for. (2) Fixed the Debug GPIO tab's "GPIOnn : ●" status dot sitting visibly below the text's vertical center — `vertical-align:middle` on an inline `<span>` aligns to a font-metric-dependent point that doesn't reliably match a 12px circle against arbitrary text. Replaced with `display:inline-flex;align-items:center` on the row's label span, the same flexbox-based approach already used for row alignment elsewhere in this UI, so centering no longer depends on font metrics. SlaveHonda/SlaveWallas only (no Debug GPIO tab on MasterRemote). MasterRemote 1.5.4, SlaveHonda 1.3.3, SlaveWallas 1.5.4; no data-model or wire-format change.
- v1.25 — Fixed a severe bug where all three units' web servers (and IP-based access generally) would permanently die after an extended runtime following any transient WiFi hiccup (router reboot, brief outage, interference). `wifi_event_handler`'s `WIFI_EVENT_STA_DISCONNECTED` case capped reconnect attempts at `MAX_RETRY` (5) and then gave up forever, setting a `WIFI_FAIL_BIT` nobody was listening for post-boot — that event group wait only happens once, during the initial `wifi_init_and_connect()` boot sequence. Once exhausted, no code path ever called `esp_wifi_connect()` again, so the STA stayed permanently disconnected until a manual power cycle. ESP-NOW kept working throughout (it operates at the radio layer, independent of STA/AP association), which is why heartbeats and commands stayed up even as the web UI became unreachable and the Nodes tab showed a slave with no IP (`get_ip_str()`/the heartbeat's `ip` field both go empty once the netif has no address). Reported after MasterRemote needed a manual power cycle to recover; SlaveHonda and SlaveWallas were unreachable at the same time, consistent with one shared WiFi outage tripping the same latent bug on all three. Fixed by latching whether the STA has ever connected since boot (`s_wifi_ever_connected`, set on the first `IP_EVENT_STA_GOT_IP`): once true, `WIFI_EVENT_STA_DISCONNECTED` retries indefinitely instead of giving up after 5 tries — the retry cap now only governs the initial boot decision to fall back to the config portal, never a later in-service disconnect. Identical fix applied to MasterRemote (1.5.3), SlaveHonda (1.3.2), SlaveWallas (1.5.3); no data-model or wire-format change. Also added a **System** resource monitor (free heap, heap low-water mark, uptime, WiFi reconnect count) to each unit's own web UI, so a slow resource leak can be caught before it causes an outage like this one — the low-water mark (`esp_get_minimum_free_heap_size()`) is the reliable leak signal, since it survives momentary recoveries that a plain "current free heap" reading would hide. New `GET /api/sysinfo` endpoint on all three units; surfaced in SlaveHonda's/SlaveWallas's Debug GPIO tab and MasterRemote's Pin Status tab (Master has no Debug GPIO tab). A new `g_wifi_disconnect_count` counter (never reset) tracks total STA disconnects since boot, useful for judging how flaky a given unit's WiFi link actually is.
- v1.24 — Fixed the Debug GPIO tab's live-level dot (v1.23) being genuinely invisible, not just hard to see: the dot is a `<span class="led">`, but `.led`'s CSS only set `width`/`height`/`border-radius` — properties a `<span>` (inline by default) silently ignores. Added `display:inline-block` (plus `vertical-align:middle`) to `.led` so the dot actually renders wherever it's used, span or div. This was a pre-existing bug, not introduced by v1.23's reorder — it just made the missing dot more noticeable once it was no longer buried in front of bold text. SlaveHonda has the identical Debug GPIO tab code and the same bug, so it received the same two fixes (CSS + "GPIOnn : ●" reorder) for parity. SlaveWallas v1.5.2, SlaveHonda v1.3.1; no data-model or API change.
- v1.23 — Debug GPIO tab (SlaveWallas): each pin row's live-level dot was easy to miss, sitting inline in front of the bold, centered "GPIOnn" label. Reformatted to "GPIOnn : ●" (label first, dot trailing after the colon) so the indicator reads clearly; the ON/OFF control buttons remain on their own line below, unchanged. SlaveWallas-only, web UI change (v1.5.1), no data-model or API change.
- v1.22 — Weekly Timer tab UX: editing a day's enabled/start/stop no longer saves per-field on every change. All 7 rows now stage locally and a single **Set** button (disabled until something is edited) commits all changed rows at once with one click, then re-disables. Avoids a stray keystroke or checkbox click silently firing a save mid-edit. MasterRemote-only, web UI change (v1.5.2), no data-model or API change.
- v1.21 — Fixed the weekly Wallas timer (v1.20) never actually activating: `timer_task` set `g_timer_wallas_start` every tick, but nothing recomputed `g_wallas_start_cmd` from it outside of a GPIO edge or a web button click — a plain clock tick reaching a schedule boundary was neither. Same class of bug as the Honda force-send fix (§12): setting an OR-source flag isn't enough if nothing re-evaluates the combined command. `timer_task` now recomputes `g_wallas_start_cmd` directly every tick. Caught immediately during live testing (the "1.5.0" build silently never turned the heater on for an active window). MasterRemote-only fix (v1.5.1).
- v1.20 — Added an NTP clock and weekly Wallas timer to MasterRemote (§3.2, §6.1). MasterRemote syncs time via SNTP (`pool.ntp.org`) in the CET/CEST timezone (`Europe/Oslo`, auto-DST). A new **Weekly Timer** tab lets each day of the week be independently enabled/disabled with a start `HH:MM` and stop `HH:MM`, persisted in NVS. The timer is a fourth Wallas command source, OR'd in alongside the Victron relay, physical manual button, and web override — active whenever today is enabled and the current local time falls in `[start, stop)` (a window crossing midnight, e.g. 22:00–02:00, is handled correctly). Never activates before the clock has completed its first sync (guarded by a sanity floor, so an un-synced ~1970 clock can't produce a false match). MasterRemote-only change (v1.5.0) — no slave firmware changes needed.
- v1.19 — Hardened command authenticity (§3.2/§12): previously, a slave accepted a `master_msg_t` start/stop command from **any** ESP-NOW sender of the right message size — the source MAC was never checked. Slaves now only act on commands from the exact MAC they registered as Master via the beacon, and reject a *new* beacon claimant while their current Master is still active (`MASTER_STALE_US` = 30 s) rather than blindly re-homing trust — closing the "spoof a beacon first, then send commands from that MAC" loophole. Symmetrically, MasterRemote's `register_peer()` now refuses to hand a slave role (SlaveHonda/SlaveWallas) to a new MAC while the current holder is still within the node-stale timeout, so a rogue sender can't hijack a role and feed Master false status. This is MAC-address verification, not cryptographic authentication — it stops stray/accidental ESP-NOW traffic and casual spoofing, not a determined attacker who deliberately clones a MAC. Full protection against that would need ESP-NOW's per-peer encryption (`peer.encrypt`/LMK), which remains available but unused — see §12. All three units updated (MasterRemote 1.4.0, SlaveHonda 1.3.0, SlaveWallas 1.5.0); no wire-format change, so this can be deployed to any subset first without breaking compatibility with not-yet-updated units.
- v1.18 — SlaveHonda's onboard LED (GPIO2) now matches SlaveWallas's blink pattern: fast (200 ms half-period) when start commanded by Master (`g_start_cmd`), slow (1 s half-period) when idle — previously a fixed 1 Hz blink regardless of state. SlaveHonda-only fix (v1.2.2), no MasterRemote/SlaveWallas changes needed.
- v1.17 — Fixed the manual **Start/Stop Honda** web buttons (§6.1) silently doing nothing in some cases. `master_task`'s Honda send is edge-triggered — it only transmits when `g_honda_start_cmd != g_slave_honda_running` (Master's last-known feedback from the slave), unlike Wallas which just resends every 15 s unconditionally. If that last-known state already happened to equal the button's target state (e.g. an unconnected/floating running-feedback sensor reading as permanently "running"), the mismatch check was never true and the command never went out. Added `g_honda_force_send`, set by both `/api/honda/start` and `/api/honda/stop`, which forces one send on the next `master_task` tick regardless of the mismatch check or the 30 s restart-block cooldown, then clears itself. MasterRemote-only fix (v1.3.1) — no slave firmware changes needed.
- v1.16 — CI release notes now list each unit's name and version as its own line (e.g. "**MasterRemote** v1.3.0"), derived automatically from the downloaded artifact filenames in the `release` job — no changes needed to the per-unit build jobs. See §8.
- v1.15 — Cleaned up the 18 old per-unit GitHub Releases (`master-v*`/`honda-v*`/`wallas-v*`) left over from before the release reorg in v1.13 — each was a single-binary release that cluttered the Releases page and risked someone downloading a stale, possibly protocol-incompatible `.bin` by mistake. Deleted the *releases* (and their uploaded binaries) but kept the underlying *git tags* — no history was lost, `git checkout <tag>` still works for all of them. `v1.5.0` is now the only entry on the Releases page. No firmware or doc content changed.
- v1.14 — Renamed the master unit from **MasterHonda** to **MasterRemote** throughout: project directory (`MasterHonda/` → `MasterRemote/`, git history preserved via `git mv`), `FIRMWARE_NAME`, config-portal AP SSID (`MasterRemote-Config`), CMake project name, HTML page title/header, and every mention in this document, the README, and CI workflow. `NVS_NAMESPACE` was deliberately left as `"mhonda"` so the already-deployed unit keeps its stored WiFi credentials across the update. **Breaking change**: the discovery beacon's identity string changed length (`"MasterHonda"` = 11 chars → `"MasterRemote"` = 12 chars) and both slaves' `strncmp()` match against it — all three units must be reflashed together, or SlaveHonda/SlaveWallas will stop recognizing the beacon and lose discovery. Historical changelog entries below still say "MasterHonda" for the versions where that was the current name.
- v1.13 — Reorganized releases: introduced a release-level version, independent of each unit's own `FIRMWARE_VERSION` (which keep incrementing independently per unit, unchanged by this reorg). All three units are now released together under one combined git tag/GitHub Release at the release version (e.g. `v1.5.0`) instead of separate `master-v*`/`honda-v*`/`wallas-v*` tags — the CI workflow already supported this (a `v*` tag builds all three and attaches all three `.bin` files, each still named with its own unit's version, to one GitHub Release); the per-unit tag patterns just hadn't been in active use this session. Per-unit tags remain available in CI for a genuine single-unit hotfix, but `v*` is now the standard release path. See §8.
- v1.12 — SlaveHonda gains a Debug GPIO tab, mirroring SlaveWallas's (same `GET /api/gpio/set`/`GET /api/gpio/status` design), adapted for the ESP32-classic pin map: manipulable subset is GPIO 4,13,14,16-19,21-23,25-27,32,33; reserved-and-status-only covers strapping (0,2,5,12,15), embedded flash (6-11), UART0 console (1,3), and input-only pins (34-39). Status dots cover the full GPIO 0-39 range. SlaveWallas's regular Status tab gains a lamp-style row for the GPIO23 heater indicator LED (was previously only visible via the Debug tab, not the normal status view).
- v1.11 — SlaveWallas Debug GPIO tab now shows a live status dot (green = HIGH) for every GPIO 0-30, not just the manipulable subset, via a new read-only `GET /api/gpio/status`. Reserved pins (strapping, USB-JTAG) show status only, no ON/OFF buttons, since `gpio_get_level()` alone is passive (just reads the input register) and safe to call on any pin, unlike `h_gpio_set`'s active reconfiguration.
- v1.10 — SlaveWallas: the onboard status LED (GPIO2) blink speed is now keyed off `g_start_cmd` (start commanded by Master) instead of `g_wallas_running` (the running-feedback sensor on GPIO18) — the feedback sensor isn't wired up on this unit yet, so blink speed previously couldn't reflect anything meaningful.
- v1.9 — SlaveWallas: corrected heater relay pin, GPIO16 → GPIO19 (matches actual wiring, confirmed via the Debug GPIO tab added in v1.8). GPIO2 confirmed as the onboard status/heartbeat LED (blinks by design — slow when idle, fast when the heater is running; no change needed there).
- v1.8 — SlaveWallas: corrected stale pin labels on the Status tab (relay was still labeled "p0" after the GPIO0→16 move, feedback still labeled "p13" after the GPIO13→18 move — display-only, the underlying GPIO defines were already correct). Added a **Debug GPIO** tab: `GET /api/gpio/set?pin=N&level=0|1` reconfigures any non-reserved GPIO (0,1,2,3,6,7,10,11,14,16-23; strapping pins 4/5/8/9/15 and USB-JTAG 12/13 excluded) to OUTPUT and drives it, for hardware bring-up when physical pin wiring is uncertain. Overrides normal operation of that pin until reboot — hardware debug only, not for production use.
- v1.7 — Fixed a heap buffer overflow in `h_root()` on all three units: the `%IP%` template placeholder (4 chars) was replaced in-place via `memmove`/`memcpy` inside a buffer sized only for the original template (`strdup()`), so any IP string longer than 4 characters (i.e. almost any real IP) overflowed the allocation, corrupting the heap and crashing the device shortly after — usually visible as the TCP connection to `/` resetting mid-response, while smaller endpoints like `/api/status` kept working fine. Rewritten to build the substituted page into a freshly, exactly-sized buffer instead of mutating a fixed-size one in place. Discovered because SlaveHonda's root page happened to work (no WiFi IP → 1-char `"?"` placeholder, which shrinks rather than overflows) while SlaveWallas's (a real IP) reliably crashed on load.
- v1.6 — Slave heartbeats (`slave_msg_t`/`slave_wallas_msg_t`) now carry per-link RSSI, channel, and firmware version, sourced from the receiving radio's `rx_ctrl` on the last frame heard from MasterRemote (works whether or not the slave has its own WiFi/AP association). MasterRemote's Nodes tab is redesigned as a table (one row per node, RSSI/Channel/FW columns) instead of stacked cards, and the Pin Status tab gains manual **Start/Stop Honda** and **Start/Stop Wallas** buttons — a third command source OR'd in alongside the Victron relay and physical manual buttons via new `/api/honda/start\|stop` and `/api/wallas/start\|stop` endpoints. **Breaking change** (message struct sizes changed) — all three units must be upgraded together, same as §3.2.
- v1.5 — Fixed an OTA upload bug on all three units: `h_ota_upload()` retried `httpd_req_recv()` forever on a timeout instead of giving up, so a client aborting mid-upload could permanently wedge the httpd worker (and, since there's only one, the entire web server) until a USB reflash. Now bounded to 5 consecutive timeouts before aborting cleanly. SlaveWallas: moved `PIN_WALLAS_FB` off GPIO13 (reserved for native USB-Serial/JTAG D+, §2.4) to GPIO18.
- v1.4 — SlaveWallas: fixed heater relay pin (GPIO0 → GPIO16, matches actual wiring); added a GPIO23 indicator LED that mirrors the relay output for hardware-level confirmation. MasterRemote: added WiFi signal strength (RSSI) and channel to the Pin Status tab / `/api/status`
- v1.3 — Removed custom MAC addressing; units now self-discover peers by function role over ESP-NOW. SlaveHonda gains a WiFi-optional fallback (ESP-NOW channel scan for MasterRemote when it cannot reach the router). MasterRemote's Clients tab becomes a Nodes tab (MAC, IP, connected status, last seen). **Breaking change** — see §3.2 and §12.
- v1.2 — Migrated firmware and CI to ESP-IDF v6.0.1; SlaveWallas moved to ESP32-C6; added WiFi network scan to the config portal
- v1.1 — Converted firmware framework from Arduino to ESP-IDF v5

---

## 1. System Overview

The Remote Start System provides wireless remote start/stop control for a Honda EU70IS petrol generator and a Wallas diesel heater installed on a vessel or caravan, triggered by a Victron Venus relay output or manual switches.

Three ESP32 microcontrollers communicate over the **ESP-NOW** peer-to-peer protocol, optionally extended by a fourth unit — **ESPNowRelay**, a store-and-forward repeater that transparently bridges Master and a slave that are out of each other's direct radio reach (§13). Each unit uses its own factory (burned-in) MAC address — there is no custom/hardcoded MAC assignment. Units find each other dynamically by function role (§3.2) rather than by a pre-shared peer table, so any unit's module can be swapped without reconfiguring the others.

MasterRemote and SlaveWallas connect to a local WiFi network for web-based monitoring, OTA, and to derive their ESP-NOW channel. SlaveHonda prefers WiFi too, but if it cannot reach the router (e.g. weak signal near the generator) it instead scans ESP-NOW channels to find MasterRemote directly and operates without a WiFi/IP connection.

```
 Victron Venus  ──relay──►  MasterRemote  ──ESP-NOW──►  SlaveHonda  (Honda EU70IS)
 Manual switches ──────────►      │       ──ESP-NOW──►  SlaveWallas (Wallas heater)
                                  │
                            Web UI / OTA
```

**Development framework:** ESP-IDF v6.0.1 (native Espressif SDK). No Arduino framework or third-party libraries are used.

---

## 2. Hardware

### 2.1 Units

| Unit         | MCU      | Function role                              | WiFi requirement |
|--------------|----------|---------------------------------------------|-------------------|
| MasterRemote  | ESP32    | Master — relay/button input, ESP-NOW hub    | Required          |
| SlaveHonda   | ESP32    | Function slave — Honda generator control    | Preferred; falls back to ESP-NOW channel scan (§3.2) if unreachable |
| SlaveWallas  | ESP32-C6 | Function slave — Wallas heater control      | Required          |
| ESPNowRelay  | ESP32-C6 | Optional store-and-forward repeater (§13)   | Preferred; falls back to ESP-NOW channel scan (§3.2) if unreachable |

Each unit uses its own factory MAC address (read via `esp_wifi_get_mac()`; never overridden with `esp_wifi_set_mac()`). Peer MAC addresses are not hardcoded anywhere in firmware — units identify each other by function role and self-register dynamically over ESP-NOW at boot (§3.2). This means any unit's hardware module can be replaced (new factory MAC) without reflashing or reconfiguring the other two.

> **SlaveWallas chip note:** ESP32-C6 (not ESP32). Requires `idf.py set-target esp32c6` before the first build/flash of that project, and uses the onboard USB-Serial/JTAG console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) rather than a separate UART bridge chip.

### 2.2 MasterRemote Pin Assignments

| GPIO | Direction    | Signal                             |
|------|--------------|------------------------------------|
|  2   | OUTPUT       | Onboard LED — heartbeat blink      |
|  4   | INPUT        | Wallas Start — Victron Venus relay (**active LOW** on the current hardware revision, v1.29 — LOW = start demanded, HIGH = idle; opposite polarity of GPIO 15's Honda relay input) |
|  5   | INPUT PULLUP | Wallas Manual Start — **obsolete as of v1.29, deliberately unwired.** Read and shown on the Pin Status tab for diagnostics only; excluded from `g_wallas_start_cmd` (an unwired pin floats unpredictably and must not be able to trigger a start) |
| 13   | OUTPUT       | Honda Running Feedback LED         |
| 14   | INPUT PULLUP | Honda Manual Start (push-button)   |
| 15   | INPUT        | Honda Start — Victron Venus relay (active HIGH — HIGH = start demanded) |

### 2.3 SlaveHonda Pin Assignments

| GPIO | Direction | Signal                                    |
|------|-----------|-------------------------------------------|
|  2   | OUTPUT    | Onboard LED (fast blink when start commanded by Master; slow = idle) |
|  4   | OUTPUT    | External status LED                       |
| 13   | OUTPUT    | Starter relay (active LOW = crank ON)     |
| 14   | OUTPUT    | Ignition relay (active LOW = ignition ON) |
| 15   | INPUT     | Running feedback (LOW = engine running). **Not wired on the current SlaveHonda unit as of v1.30.** Since the pin can't be trusted, `g_honda_running` is instead driven entirely by software state (v1.31): `true` after a start sequence completes without being aborted, `false` on any stop (explicit or aborted mid-sequence) — an assumption of success rather than a pin read, chosen because a false negative here caused MasterRemote to repeatedly re-crank the starter on an already-running engine (§10/§12), which is far worse than an unretried failed start. Revert to `!gpio_get_level(PIN_RUNNING_FB)` at both sites once a real sensor is connected. |

**Honda start sequence timing:**
1. Ignition relay ON (LOW) → 10 s warm-up
2. Starter relay ON (LOW) → 3 s crank
3. Starter relay OFF → 1 s settle
4. Read running feedback pin to confirm engine state — **currently skipped (§2.3): `g_honda_running` is set from software state (assumed success/failure), not this pin, until GPIO15 is wired**

### 2.4 SlaveWallas Pin Assignments

| GPIO | Direction | Signal                                   |
|------|-----------|------------------------------------------|
| 19   | OUTPUT    | Wallas heater relay (HIGH = heater ON)   |
|  2   | OUTPUT    | Onboard LED (fast blink when start commanded by Master; slow = idle) |
| 18   | INPUT     | Wallas running feedback (HIGH = running) |
| 23   | OUTPUT    | Heater indicator LED (mirrors relay, HIGH = heater ON) |

> **Reserved — do not use as GPIO:** GPIO12/13 are hard-wired to the ESP32-C6's native USB-Serial/JTAG peripheral (D-/D+, used for console + flashing, §11). Configuring either as a regular GPIO can disconnect that pad from USB while the app is running, breaking live console/monitor access. (`PIN_WALLAS_FB` was originally GPIO13 and was moved to GPIO18 for exactly this reason — this likely explains earlier serial-connectivity flakiness with this unit before the move.)

> **GPIO23 indicator LED:** wired directly alongside the relay output (set together in `wallas_start()`/`wallas_stop()`), giving a physical, hardware-level confirmation of the relay command independent of software status polling — added because `gpio_get_level()` on an output-only-configured pin (e.g. the relay pin itself, §12) cannot reliably report back its own driven state, so the API's `relay` field should not be trusted as proof of physical relay state.

---

## 3. Communication Architecture

### 3.1 ESP-NOW

- **Protocol:** IEEE 802.11 ESP-NOW peer-to-peer; uses `esp_now.h`, provided by the `esp_wifi` component in ESP-IDF v6 (no longer a standalone `esp_now` component as in v5).
- **Channel:** Set to `0` when adding peers (`esp_now_peer_info_t.channel = 0`), which instructs the driver to use the current WiFi STA channel automatically. All three units must connect to the same WiFi router to share a channel.
- **Encryption:** Disabled (`peer.encrypt = false`). Enable per-peer AES encryption via `esp_now_peer_info_t.encrypt = true` and supply a 16-byte LMK for production hardening.
- **Receive callback API:**
  ```c
  void espnow_recv_cb(const esp_now_recv_info_t *info,
                      const uint8_t *data, int len);
  ```
- **Send callback API (ESP-IDF v6 signature):**
  ```c
  void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);
  ```
  Changed from the v5 signature `(const uint8_t *mac, esp_now_send_status_t status)`.

### 3.2 Node Roles, Peer Discovery & Channel Acquisition

No unit's peer MAC is known in advance (§2.1), so peers are learned dynamically at runtime instead of read from a hardcoded table. Regular command/heartbeat traffic (§3.3) stays **unicast** exactly as before — only the discovery beacon described below is broadcast. Nothing is persisted to NVS; the roster is rebuilt from scratch every boot, which is intentional — it is what makes hardware swaps work without touching other units.

#### Channel acquisition per unit

| Unit        | How it gets an ESP-NOW channel                                                                 |
|-------------|--------------------------------------------------------------------------------------------------|
| MasterRemote | Connects to WiFi (required); its ESP-NOW channel is whatever channel it associates on.           |
| SlaveWallas | Connects to WiFi (required, same SSID as Master); channel matches automatically since both are on the same AP/router. |
| SlaveHonda  | Attempts WiFi connect first, using the same stored credentials and 15 s timeout as the other units (§4). **If it succeeds**, it behaves exactly like SlaveWallas — channel comes from the AP. **If it fails** (credentials exist but the router is unreachable), it does **not** reopen the config portal. Instead it enters **ESP-NOW channel scan**: it sets the radio to each 2.4 GHz channel 1–13 in turn (`esp_wifi_set_channel()`), dwelling on each one long enough to catch a MasterRemote beacon, until it hears one. It then locks to that channel. The config portal is still used on a genuinely unconfigured unit (no stored credentials at all) — see §4. |

#### Discovery beacon (new message)

While connected to WiFi, MasterRemote periodically broadcasts a small identity beacon to `FF:FF:FF:FF:FF:FF` (the broadcast address is added as a fixed ESP-NOW peer at init, same as today's peer setup, just unconditional rather than per-slave):

```c
typedef struct {
    char label[32];   // "MasterRemote"
} master_beacon_t;
```

This beacon serves two purposes: it lets SlaveWallas/SlaveHonda-with-WiFi learn Master's real MAC (`esp_now_recv_info_t.src_addr` on the received frame) without ever being told it in advance, and it is the signal SlaveHonda's channel scan (above) is listening for.

**Trust, once established, is sticky (v1.19+).** A slave only accepts a *new* beacon claimant (different MAC) as Master while it doesn't already have one, or once its current Master has gone quiet for `MASTER_STALE_US` (30 s) — a second device broadcasting a beacon while the real Master is still active is ignored, not blindly trusted. Symmetrically, once a role (SlaveHonda/SlaveWallas) is registered to a MAC on MasterRemote's side, a different MAC claiming that role is rejected while the current holder is still within the node-stale timeout (§3.4). This closes the obvious spoofing loophole — send a fake beacon, then send commands "as Master" — without needing cryptography; see the residual risk noted in §12.

#### Registration (reuses existing heartbeat messages)

No separate "register" message is needed. The existing periodic heartbeats already sent by each slave (`slave_msg_t` from SlaveHonda, `slave_wallas_msg_t` from SlaveWallas — §3.3) do double duty as registration:

1. A slave hears Master's beacon, learns Master's MAC (subject to the staleness gate above), and calls `esp_now_add_peer()` for it.
2. The slave then sends its normal heartbeat unicast to that MAC. Message *type/size* alone (already how the current receive handler tells the two slave message structs apart) is enough for Master to identify the role.
3. MasterRemote, on receiving a heartbeat, calls `register_peer()`, which accepts the sender's MAC for that role only if it matches the current holder, or the role is unclaimed, or the current holder has gone stale (§3.4) — otherwise the heartbeat is dropped without updating the roster. On acceptance it calls `esp_now_add_peer()` and adds/updates an in-RAM roster entry: `{mac, role, ip, has_wifi, last_seen}`. This roster (not a static peer table) is what MasterRemote's periodic command sends (§3.3) address, and what backs the Nodes web tab (§6.1).
4. Every subsequent heartbeat from the trusted MAC refreshes `last_seen`. If MasterRemote reboots (roster cleared), the next heartbeat a slave sends after hearing a fresh beacon re-registers it automatically — no manual pairing step ever required, since a cleared roster has no "current holder" to conflict with.

If MasterRemote currently has no registered peer for a given role (e.g. SlaveHonda is still channel-scanning), its periodic command send for that role is simply skipped until a peer appears.

### 3.3 Message Structures

Structures are defined identically in each unit's `main.c`. Packing is implicit (all `bool` fields; no padding issues on ESP32).

#### Master → Slave (`master_msg_t`)
```c
typedef struct {
    char  label[32];
    bool  HondaRunningFB;   // echo of running status
    bool  HondaIgnitionOn;  // ignition state
    bool  HondaStart;       // command: true = start, false = stop
                            // (doubles as WallasStart for SlaveWallas)
} master_msg_t;
```

#### SlaveHonda → Master (`slave_msg_t`)
```c
typedef struct {
    char  label[32];
    bool  HondaIgnitionOn;
    bool  HondaStarting;
    bool  HondaRunning;
} slave_msg_t;
```

#### SlaveWallas → Master (`slave_wallas_msg_t`)
```c
typedef struct {
    char  label[32];
    bool  WallasRunning;
    bool  WallasStart;
} slave_wallas_msg_t;
```

`slave_msg_t` and `slave_wallas_msg_t` each gain these fields to support the Nodes tab (§6.1) and registration (§3.2):
```c
    char    ip[16];        // "" if unit currently has no WiFi/IP (e.g. SlaveHonda in channel-scan mode)
    bool    has_wifi;      // true if this heartbeat was sent while WiFi-connected
    int8_t  rssi;          // signal strength of the last frame heard from MasterRemote, dBm
    uint8_t channel;       // channel that frame was received on
    char    fw_version[12];// this slave's own FIRMWARE_VERSION string
```
`rssi`/`channel` are read from `esp_now_recv_info_t.rx_ctrl` on whatever the slave most recently received from Master (beacon or command) — this works identically whether or not the slave has its own WiFi/AP association, so it reports real per-link quality even for a SlaveHonda running in ESP-NOW-only fallback (§3.2).

#### MasterRemote → broadcast (`master_beacon_t`, new — discovery only)
```c
typedef struct {
    char  label[32];   // "MasterRemote"
} master_beacon_t;
```

### 3.4 Timing

| Parameter                    | Value    | Description                                        |
|-------------------------------|----------|-----------------------------------------------------|
| Honda restart block           | 30 s     | Minimum interval between Honda sends                |
| Wallas send interval          | 15 s     | Periodic Wallas command refresh                      |
| SlaveHonda status period      | 10 s     | Slave → Master heartbeat (also re-registers, §3.2)   |
| SlaveWallas status period     | 10 s     | Slave → Master heartbeat (also re-registers, §3.2)   |
| Honda ignition warm-up        | 10 s     | Delay between ignition ON and crank                  |
| Honda crank time              | 3 s      | Duration of starter relay activation                 |
| Master beacon interval        | 2 s      | `master_beacon_t` broadcast period while WiFi-connected |
| SlaveHonda scan dwell/channel  | 2.5 s    | Per-channel listen time during channel scan (> beacon interval, so a beacon is virtually guaranteed each pass) |
| SlaveHonda full scan cycle     | ~32.5 s  | Worst case to find Master: 13 channels × 2.5 s        |
| Node stale timeout             | 30 s     | No heartbeat within this window → Nodes tab shows "disconnected" and Master stops sending to that role |

---

## 4. WiFi Configuration (First Startup)

On first power-up — or any boot where NVS has no stored WiFi credentials at all — each unit opens a SoftAP captive portal implemented directly in `main.c` using `esp_wifi` and `esp_http_server`. No external WiFiManager library is used.

> This portal is for *initial setup only*. A runtime connect failure with credentials already stored behaves differently per unit: MasterRemote and SlaveWallas retry/reopen the portal as before (WiFi is mandatory for both). SlaveHonda instead falls back to the ESP-NOW channel scan described in §3.2 and does **not** reopen the portal — the generator location is expected to sometimes be out of WiFi range, and reopening a config AP there would just strand the unit.

### Procedure for each unit

1. Power on the unit (fresh flash or after NVS erase).
2. The unit starts a WiFi access point:

   | Unit         | AP Name              | Password    |
   |--------------|----------------------|-------------|
   | MasterRemote  | `MasterRemote-Config` | `honda1234` |
   | SlaveHonda   | `SlaveHonda-Config`  | `honda1234` |
   | SlaveWallas  | `SlaveWallas-Config` | `honda1234` |

3. Connect a smartphone or laptop to that AP.
4. Navigate to `http://192.168.4.1/wifi-setup`.
5. Tap **Scan Networks** to list nearby SSIDs (signal strength + lock icon for secured networks), or type the SSID manually. Tapping a result fills in the SSID field.
6. Enter the WiFi password (if required) and submit the form.
7. Credentials are written to NVS (`esp_err_t nvs_set_str()`), and the unit reboots.
8. On subsequent boots the unit reads credentials from NVS and connects directly in STA mode.

> **Important:** All three units must connect to the **same** WiFi network so they share a channel for ESP-NOW.

### Portal implementation

The portal is served by `web_server.c`:

- `GET /wifi-setup` — HTML credential form with in-page network scan (JS fetches `/api/scan` and renders a clickable list)
- `GET /api/scan` — triggers `esp_wifi_scan_start()` (active scan, capped at 20 results) and returns a JSON array of `{ssid, rssi, auth}`
- `POST /wifi-save` — URL-encoded body parsed, SSID/password written to NVS, unit restarts

During the portal, WiFi runs in `WIFI_MODE_APSTA` (rather than AP-only) so the STA radio can perform the scan while the SoftAP keeps serving the setup page. A `g_portal_mode` flag suppresses the normal `WIFI_EVENT_STA_START → esp_wifi_connect()` auto-connect handler while the portal is active, since no credentials are configured yet.

> **Status:** this scan feature is implemented in `main.c`/`web_server.c` on all three units but not yet committed to git as of this revision.

### Resetting WiFi credentials

Erase only the NVS partition (preserves firmware):
```
esptool.py -p COMx erase_region 0x9000 0x5000
```
Or erase the full flash and re-flash:
```
idf.py -p COMx erase-flash flash
```

---

## 5. Project Structure

Each unit is an independent ESP-IDF project:

```
<Unit>/
├── CMakeLists.txt          ← top-level project file (idf.py entry point)
├── partitions.csv          ← dual OTA + NVS partition table
├── sdkconfig.defaults      ← project-level Kconfig overrides
└── main/
    ├── CMakeLists.txt      ← component registration + REQUIRES list
    ├── version.h           ← FIRMWARE_VERSION, FIRMWARE_NAME, AP name, NVS namespace
    ├── main.c              ← app_main, WiFi, ESP-NOW, GPIO, FreeRTOS tasks
    ├── web_server.c        ← HTTP server, status API, OTA upload handler, portal
    └── web_server.h
```

### Partition table (`partitions.csv`)

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000
otadata,  data, ota,     0xe000,   0x2000
app0,     app,  ota_0,   0x10000,  0x1E0000
app1,     app,  ota_1,   0x1F0000, 0x1E0000
```

Both `app0` and `app1` are 1.875 MB, sufficient for the firmware including embedded HTML. `otadata` tracks which partition is the active boot target.

### ESP-IDF components used

| ESP-IDF component  | Purpose                                   |
|--------------------|-------------------------------------------|
| `esp_wifi`         | WiFi STA / SoftAP, scanning, channel control, ESP-NOW (`esp_now.h`) |
| `esp_event`        | Default event loop for WiFi/IP events     |
| `esp_netif`        | Network interface (STA + AP), SNTP client (`esp_netif_sntp.h`, MasterRemote only) |
| `nvs_flash` / `nvs`| Non-volatile storage of WiFi credentials, weekly timer config |
| `esp_driver_gpio`  | GPIO configuration, levels, ISR service   |
| `esp_http_server`  | Synchronous HTTP server                   |
| `esp_ota_ops` / `app_update` | OTA write, partition selection, reboot |
| `esp_timer`        | Microsecond timestamps for timing logic   |
| `freertos`         | Tasks, event groups, delays               |

> ESP-IDF v6 component changes vs. v5: `esp_now.h` is now provided by `esp_wifi` (no standalone `esp_now` component); GPIO driver moved to component `esp_driver_gpio`; `esp_ota_ops` is covered by `app_update`. See also the send-callback signature change in §3.1.

---

## 6. Web Interface

All HTML is embedded as `const char[]` string literals in `web_server.c`. No filesystem (SPIFFS/LittleFS) is required.

### 6.1 MasterRemote — Four Tabs

Access at: `http://<MasterRemote-IP>/`

| Tab          | Content                                                      | Refresh  |
|--------------|--------------------------------------------------------------|----------|
| Pin Status   | Live state of 5 pins + 3 global state flags + WiFi link (RSSI, channel) + System resource monitor (heap, uptime, WiFi reconnects, §10) + manual Start/Stop Honda and Start/Stop Wallas buttons | 2 s auto |
| Nodes        | Table of registered slave nodes (§3.2): role, firmware version, MAC, IP, WiFi, RSSI, channel, connected status, last seen, running status, role-specific detail | 2 s auto |
| Weekly Timer | NTP clock readout + 7-row table (Sun–Sat): enable checkbox, start `HH:MM`, stop `HH:MM` for the Wallas heater | On tab open + on edit |
| OTA Update   | File input + XHR upload to `/ota/upload`                     | Manual   |

The Nodes tab is a genuine HTML `<table>` (one row per node) driven directly by MasterRemote's in-RAM peer roster (§3.2), so it reflects real discovered nodes rather than fixed content — a node that hasn't registered yet (e.g. SlaveHonda still mid channel-scan) shows "—" for MAC/IP/RSSI/Channel/FW until its first heartbeat arrives. "Connected" = a heartbeat was received within the last 30 s (node stale timeout, §3.4); IP shows blank/"—" for a node currently running WiFi-less (SlaveHonda in ESP-NOW fallback); RSSI/Channel come from the slave's own last-heard-from-Master reading (§3.3), not MasterRemote's own link.

**Manual control:** the Pin Status tab's Start/Stop buttons are a command source OR'd in with the Victron relay and physical manual button (`g_web_honda_start`/`g_web_wallas_start` in `main.c`). Pressing Start latches the override on immediately (no need to wait for a GPIO edge); Stop clears the override and recomputes the command from current physical pin state (and, for Wallas, the weekly timer — see §6.1a). A web-triggered start persists across physical switch changes until explicitly stopped via the UI (or the corresponding physical button/relay signal is what next changes state and this recompute runs) — it does not silently clear itself.

#### 6.1a NTP Clock & Weekly Wallas Timer

MasterRemote syncs time via SNTP (`esp_netif_sntp`, server `pool.ntp.org`) once WiFi is connected, in the `CET-1CEST,M3.5.0,M10.5.0/3` POSIX timezone (Europe/Oslo — CET in winter, CEST in summer, DST transitions handled automatically). The clock is considered "synced" the first time `time(NULL)` returns an epoch past a fixed sanity floor (2024-01-01 UTC) — this exists specifically so an un-synced clock reading ~1970 can never accidentally satisfy a schedule match.

The **Weekly Timer** tab configures one independent on/off window per day of the week (Sunday–Saturday, matching `struct tm.tm_wday` directly) for the Wallas heater: an enable flag, a start `HH:MM`, and a stop `HH:MM`, persisted as a single NVS blob (`walltimer` key, same `NVS_NAMESPACE` as WiFi credentials) so it survives reboot/OTA.

A background task (`timer_task`, 20 s tick) evaluates the current local day and time-of-day against that day's window and sets `g_timer_wallas_start` — this is a **fourth Wallas command source**, OR'd in alongside the Victron relay, physical manual button, and web override (§3.2-style multi-source OR, same pattern as the manual buttons). A window may cross midnight (e.g. start `22:00`, stop `02:00`) and is evaluated correctly either way. If today is disabled, or the clock hasn't synced yet, the timer contributes nothing.

> **Same "sticky OR" behavior as the physical relay:** because this is one more source OR'd into the same command, manually pressing **Stop Wallas** during an active timer window only clears the *web* override — the timer's own contribution is still true, so `timer_task`'s next 20 s tick (or any GPIO edge) will turn it back on. This mirrors how a stuck-active Victron relay signal already behaves today; it is not a new inconsistency introduced by the timer.

The Weekly Timer tab deliberately does **not** auto-refresh every 2 s like the other tabs (unlike Pin Status/Nodes) — its inputs are live, editable form fields, and periodic refresh would overwrite whatever the user is mid-typing. It only re-fetches when the tab is opened. Edits are staged locally, not saved per-field: changing any checkbox or time input only enables a single **Set** button below the table (disabled by default, and re-disabled once the save completes); clicking it fires one `POST /api/timer/set` per day (all 7) with the current form values in parallel.

HTTP endpoints:

| Method | Path                | Description                            |
|--------|---------------------|-----------------------------------------|
| GET    | `/`                 | Main dashboard HTML                    |
| GET    | `/api/status`       | JSON: pin levels, global state, WiFi RSSI/channel |
| GET    | `/api/sysinfo`      | JSON: free heap, heap low-water mark, uptime, WiFi reconnect count (v1.25, §10) |
| GET    | `/api/nodes`        | JSON: roster (role, fw, mac, ip, has_wifi, rssi, channel, connected, last_seen, running status) |
| POST   | `/api/honda/start`  | Manual override: force Honda start cmd |
| POST   | `/api/honda/stop`   | Clear override; recompute from GPIO    |
| POST   | `/api/wallas/start` | Manual override: force Wallas start cmd|
| POST   | `/api/wallas/stop`  | Clear override; recompute from GPIO + timer |
| GET    | `/api/timer`        | JSON: current clock + all 7 days' timer config |
| POST   | `/api/timer/set`    | Save one day: `?day=0-6&enabled=0\|1&start=HH:MM&stop=HH:MM` |
| GET    | `/wifi-setup`       | WiFi credential form (portal only)     |
| GET    | `/api/scan`         | JSON array of nearby networks (portal only) |
| POST   | `/wifi-save`        | Save credentials to NVS, restart       |
| POST   | `/ota/upload`       | Raw binary OTA upload                  |

### 6.2 SlaveHonda & SlaveWallas — Local Web UI

Access at: `http://<Slave-IP>/`

| Tab        | Content                                               | Refresh  | Units |
|------------|--------------------------------------------------------|----------|-------|
| Status     | Relay states, running feedback, last received command, WiFi Link (AP RSSI + channel, v1.33), plus (SlaveWallas) heater indicator LED lamp | 2 s auto | Both  |
| Debug GPIO | System resource monitor (heap, uptime, WiFi reconnects, §10) + live status dot for every GPIO; ON/OFF buttons on the non-reserved subset (hardware bring-up) | 2 s auto | Both  |
| OTA Update | File input + XHR upload to `/ota/upload`              | Manual   | Both  |

HTTP endpoints on both units: `/`, `/api/status`, `/api/sysinfo` (v1.25, §10), `/api/gpio/set?pin=N&level=0|1`, `/api/gpio/status`, `/wifi-setup`, `/api/scan`, `/wifi-save`, `/ota/upload`.

`/api/gpio/set` forces a manipulable pin to OUTPUT and drives it (active, overrides normal operation). `/api/gpio/status` is read-only (`gpio_get_level()` on every GPIO, safe to call even on reserved pins since it never reconfigures anything) and backs the Debug tab's status dots — every pin gets a dot, only the non-reserved subset gets ON/OFF buttons. The non-reserved/manipulable set differs per chip:

| Unit        | Chip         | Manipulable GPIOs                          | Reserved (status-only)                                    |
|-------------|--------------|----------------------------------------------|--------------------------------------------------------------|
| SlaveHonda  | ESP32        | 4,13,14,16,17,18,19,21,22,23,25,26,27,32,33  | Strapping (0,2,5,12,15), embedded flash (6-11), UART0 console (1,3), input-only (34-39) |
| SlaveWallas | ESP32-C6     | 0,1,2,3,6,7,10,11,14,16-23                  | Strapping (4,5,8,9,15), USB-Serial/JTAG (12,13)              |

> **SlaveHonda in ESP-NOW fallback (§3.2):** the unit has no IP address while WiFi-less, so its own web UI (and OTA) is unreachable in that state. Its status is only visible remotely via MasterRemote's Nodes tab (§6.1). To OTA-update SlaveHonda, it must be within WiFi range at boot (or reachable via the config portal) at least once.

> **Debug GPIO (SlaveWallas only, §11):** `/api/gpio/set` unconditionally calls `gpio_reset_pin()` + `gpio_set_direction(OUTPUT)` + `gpio_set_level()` on whatever pin number is given, for physically verifying wiring when pin assignments are uncertain. It has no awareness of what a pin is normally used for — toggling GPIO19 (relay), GPIO18 (feedback input), GPIO2, or GPIO23 (LEDs) here overrides the unit's normal control of that pin until the next reboot. Not gated behind any confirmation; intended for hardware bring-up sessions only, not left exposed in a production deployment.

### 6.3 Status JSON format

`GET /api/status` on MasterRemote returns:
```json
{
  "pHS": false,   "pHM": false,   "pWS": false,
  "pWM": false,   "pFB": false,
  "gHS": false,   "gHR": false,   "gWS": false,
  "rssi": -62,    "ch": 1
}
```
Keys: `p` = pin level, `g` = global state flag; `HS` = Honda Start, `HM` = Honda Manual, `WS` = Wallas Start, `WM` = Wallas Manual, `FB` = Feedback, `HR` = Honda Running. `rssi` = MasterRemote's WiFi signal strength in dBm (from `esp_wifi_sta_get_ap_info()`); `ch` = its current WiFi/ESP-NOW channel (`wifi_ap_record_t.primary`). Both read 0 if not connected.

---

## 7. OTA Update Procedure

The OTA handler in `web_server.c` uses `esp_ota_ops.h` with streaming writes:

```c
esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
// stream 1 KB chunks from HTTP body
esp_ota_write(handle, buf, len);
esp_ota_end(handle);
esp_ota_set_boot_partition(partition);
esp_restart();
```

### Steps

1. Build the firmware: `cd <Unit> && idf.py build`
2. Locate the binary: `<Unit>/build/<Unit>.bin`
3. Open `http://<device-ip>/` → **OTA Update** tab.
4. Click **Choose File**, select the `.bin`.
5. Click **Upload & Flash**. Progress is shown as a percentage.
6. The device reboots automatically. The new version appears in the page header.

> The upload uses a JavaScript `XMLHttpRequest` with `Content-Type: application/octet-stream`. The server reads the raw binary body in 1 KB chunks — no multipart parsing needed.

---

## 8. Versioning & Release Artifacts

Each unit's version is defined independently in `main/version.h`:

```c
#define FIRMWARE_VERSION  "1.0.0"
#define FIRMWARE_NAME     "MasterRemote"   // SlaveHonda / SlaveWallas
```

### Version scheme: `MAJOR.MINOR.PATCH`

| Change type    | Bump  |
|----------------|-------|
| Breaking change | MAJOR |
| New feature     | MINOR |
| Bug fix         | PATCH |

> Adopting the discovery protocol (§3.2) is a breaking wire-protocol change — a unit running old (custom-MAC/hardcoded-peer) firmware cannot talk to one running the new discovery-based firmware. Bump MAJOR and flash all three units together.

### Release bin naming

```
MasterRemote_v1.0.0.bin
SlaveHonda_v1.0.0.bin
SlaveWallas_v1.0.0.bin
```

### Release version vs. per-unit version

There are two independent version numbers in play:

- **Per-unit `FIRMWARE_VERSION`** — bumped independently for each unit, following the MAJOR.MINOR.PATCH rule above, whenever that specific unit's firmware changes. This is what ships in each `.bin` filename and what each unit reports about itself.
- **Release version** — the git tag (`vX.Y.Z`, e.g. `v1.5.0`) used to cut a combined GitHub Release bundling all three units' *current* binaries together, whatever their individual versions happen to be at that point. It does not force the three units onto the same number.

### GitHub Actions CI/CD

Pushing a tag triggers `.github/workflows/release.yml`, which builds the affected unit(s) using `espressif/esp-idf-ci-action@v1` (pinned to ESP-IDF v6.0.1 via `env.IDF_VERSION`) and attaches the `.bin` files to a GitHub Release.

| Tag pattern  | Units built                      | Use for |
|--------------|----------------------------------|---------|
| `v*`         | All three                        | **Standard practice** — one combined release bundling all three units' current binaries |
| `master-v*`  | MasterRemote only                 | Single-unit hotfix only |
| `honda-v*`   | SlaveHonda only                  | Single-unit hotfix only |
| `wallas-v*`  | SlaveWallas only                 | Single-unit hotfix only |

First flash must be done via USB. OTA handles all subsequent updates.

> **Housekeeping:** if per-unit tags accumulate single-binary GitHub Releases again (e.g. from hotfixing one unit), it's fine to periodically delete the *releases* to keep the Releases page focused on real combined `v*` releases — just don't pass `--cleanup-tag` to `gh release delete`, so the underlying git tags (useful for `git checkout <tag>` history) survive.

---

## 9. Startup Sequence

### 9.1 MasterRemote & SlaveWallas (WiFi-mandatory units)

```
app_main()
  │
  ├─ nvs_flash_init()
  ├─ gpio_init()          — configure outputs (relays off) and inputs
  │
  ├─ wifi_init_and_connect()      — own factory MAC, never overridden
  │     ├─ NVS has credentials?
  │     │     YES → esp_wifi_start() → wait for IP (15 s)
  │     │             connected? → web_server_start() → continue
  │     │             failed?   → fall through to portal
  │     └─ NO / failed → start_config_portal()   (§4 — initial setup only)
  │                         esp_wifi AP mode, web_server_start() (/wifi-setup)
  │                         wait for POST /wifi-save → nvs_set_str() → esp_restart()
  │
  ├─ espnow_init()
  │     ├─ esp_now_init(), esp_now_register_recv_cb()/send_cb()
  │     └─ esp_now_add_peer(BROADCAST_MAC)   — fixed, for beacon (MasterRemote) / listening (SlaveWallas)
  │
  ├─ MasterRemote: xTaskCreate(beacon_task)     — broadcasts master_beacon_t every 2 s (§3.2)
  ├─ MasterRemote: xTaskCreate(master_task)     — sends to registered role peers, roster maintenance
  ├─ SlaveWallas: xTaskCreate(heartbeat_task)  — listens for beacon → registers → periodic heartbeat
  │
  └─ [FreeRTOS scheduler runs tasks]
```

### 9.2 SlaveHonda (WiFi-preferred, ESP-NOW fallback)

```
app_main()
  │
  ├─ nvs_flash_init()
  ├─ gpio_init()
  │
  ├─ wifi_init_and_connect()      — own factory MAC, never overridden
  │     ├─ NVS has credentials?
  │     │     YES → esp_wifi_start() → wait for IP (15 s)
  │     │             connected? → web_server_start() → go to 9.1-style path (beacon-based registration)
  │     │             failed?   → skip portal, go to ESP-NOW channel scan below
  │     └─ NO (never configured) → start_config_portal()   (§4 — initial setup only)
  │
  ├─ [if WiFi connect failed] espnow_channel_scan()
  │     ├─ for ch in 1..13: esp_wifi_set_channel(ch); listen up to 2.5 s for master_beacon_t
  │     ├─ beacon heard? → lock channel, esp_now_add_peer(master_mac) → proceed to registration (§3.2)
  │     └─ no beacon after full 1..13 pass → repeat scan indefinitely
  │
  ├─ espnow_init() / xTaskCreate(heartbeat_task)   — heartbeat carries has_wifi=false, ip="" in this path
  │
  └─ [FreeRTOS scheduler runs tasks]
```

---

## 10. Error Handling & Recovery

| Condition                                        | Behaviour                                               |
|---------------------------------------------------|-----------------------------------------------------------|
| No NVS credentials on boot (any unit)              | SoftAP portal opened; restarts after credentials saved   |
| WiFi connect timeout (15 s) — MasterRemote/SlaveWallas | Portal opened (WiFi is mandatory for these units)     |
| WiFi connect timeout (15 s) — SlaveHonda, creds exist | No portal; falls back to ESP-NOW channel scan (§3.2/§9.2) |
| WiFi disconnects *after* a successful boot connection (any unit) | `esp_wifi_connect()` retried indefinitely on every disconnect (v1.25) — previously gave up permanently after 5 tries, killing the web server until a power cycle while ESP-NOW kept working (§12) |
| SlaveHonda: no MasterRemote beacon found            | Channel scan repeats indefinitely (§3.4); unit has no IP/web UI meanwhile |
| ESP-NOW init failure                               | `esp_restart()`                                          |
| Peer add failure                                   | `ESP_LOGE` log, execution continues                       |
| MasterRemote reboots (roster lost)                  | Self-healing: next heartbeat after each slave's next beacon re-registers it (§3.2), no manual pairing |
| OTA write error / client aborts mid-upload         | `esp_ota_abort()`, HTTP 500 returned, no reboot. Up to 5 consecutive recv timeouts tolerated before aborting (§8) — a hard client abort no longer wedges the httpd worker indefinitely |
| Honda: no running feedback                         | Slave reports `HondaRunning = false`; master may retry after 30 s block |
| Master: no slave heartbeat (node stale, 30 s)       | Nodes tab shows "disconnected"; MasterRemote stops sending to that role until it re-registers |
| SlaveHonda start sequence (~14 s warm-up + crank)   | Runs in its own task (v1.28) — web server and ESP-NOW stay responsive throughout. A stop command received mid-sequence aborts it within ~100 ms (relays de-energized immediately by `honda_stop()`, the sequence task notices and exits) |

### 10.1 System Resource Monitor (`/api/sysinfo`, v1.25, `heapMinAgo` added v1.26)

All three units expose `GET /api/sysinfo` and surface it as a **System** card in the web UI (Debug GPIO tab on SlaveHonda/SlaveWallas; Pin Status tab on MasterRemote, which has no Debug GPIO tab). It returns:

| Field        | Source                                          | What it means                                                                 |
|--------------|---------------------------------------------------|---------------------------------------------------------------------------------|
| `heap`       | `esp_get_free_heap_size()`                       | Free heap bytes *right now* — a snapshot that bounces up and down constantly as the device allocates/frees things (HTTP responses, JSON buffers, WiFi scans). Not reliable on its own for spotting a leak, since a low reading can just be a busy moment. |
| `heapMin`    | Self-sampled low-water mark (`g_heap_min`)       | The lowest free-heap value *observed by the periodic sampler* since boot (10 s tick in `status_task` on the slaves, 20 s tick in `timer_task` on MasterRemote) — not `esp_get_minimum_free_heap_size()`'s byte-exact tracking, traded away in v1.26 so this value stays paired with a matching timestamp (`heapMinAgo`); IDF's own tracked minimum has no "when". Monotonically non-increasing, so it's still the reliable leak signal: a real leak shows up as this creeping steadily downward across days/weeks even while `heap` looks fine moment-to-moment. This is the field to watch. |
| `heapMinAgo` | `(now - g_heap_min_ts_us) / 1e6` (v1.26)         | Seconds since `heapMin` last dropped to a new low. Distinguishes "dipped once at boot, flat ever since" (large `heapMinAgo`, nothing to worry about) from "still actively dropping" (small `heapMinAgo`, a leak in progress) — `heapMin` alone can't tell these apart. |
| `uptime`     | `esp_timer_get_time() / 1e6`                     | Seconds since boot. Needed to judge whether a drop in `heapMin` is meaningful (e.g. "down 20 KB over 3 days" is a leak; "down 20 KB in the first minute after boot" is normal one-time startup allocation). |
| `wifiDisc`   | `g_wifi_disconnect_count` (never reset)          | Total WiFi STA disconnect events since boot. Tracks how flaky a unit's link actually is over time, and doubles as a live check that the v1.25 reconnect fix (§10) is working — this number can climb while the web server stays reachable throughout. |

This monitor is local per-unit only (§12) — it was added specifically because the v1.25 outage (all three units' web servers dying after an extended run, requiring a manual power cycle) had no leading indicator; watching `heapMin` and `wifiDisc` going forward should surface a similar problem before it causes another outage.

---

## 11. Build & Flash Reference

### Prerequisites

- ESP-IDF v6.0.1 installed and sourced (`idf.py` on PATH)
- MasterRemote/SlaveHonda: ESP32 board connected via USB. SlaveWallas: ESP32-C6 board connected via USB (native USB-Serial/JTAG, no separate UART chip needed).

### Commands

```bash
# Build
cd MasterRemote          # or SlaveHonda / SlaveWallas
idf.py set-target esp32c6   # SlaveWallas only, first build
idf.py build

# First flash via USB
idf.py -p COM7 flash monitor

# Erase NVS (reset WiFi credentials, keep firmware)
esptool.py -p COM7 erase_region 0x9000 0x5000

# Full erase + reflash
idf.py -p COM7 erase-flash flash monitor
```

### IDE

Open any unit folder in **VS Code with the ESP-IDF extension** (Espressif IDF). The extension detects `CMakeLists.txt` automatically. Use the status bar buttons to select port, build, and flash.

---

## 12. Known Limitations

- ESP-NOW channel is derived from the connected WiFi AP for MasterRemote, SlaveWallas, and SlaveHonda-with-WiFi. If the router changes channel (unusual), those units must restart to re-sync. A SlaveHonda that is channel-scanning (no WiFi) is unaffected by router channel changes — it always finds Master's current channel by scanning — but if it has already locked on and Master's channel then changes mid-session, it loses sync until it restarts and rescans.
- The discovery beacon (§3.2) and all ESP-NOW traffic are unencrypted (`peer.encrypt = false`, §3.1). As of v1.19, both directions verify the sender's MAC against the currently-registered peer for that role and refuse to re-home trust to a new claimant while the current one is still active (30 s staleness window) — this stops stray/accidental traffic and casual spoofing (e.g. someone else's ESP-NOW device sending a coincidentally-matching packet), but not a determined attacker who deliberately clones a MAC address, since there's still no cryptographic proof of identity. Full protection against that would need per-peer AES + LMK (§3.1), which remains available but unused.
- `HondaStart` in `master_msg_t` carries the Wallas command to `SlaveWallas` (struct reuse from the original design). A future version should introduce a dedicated `wallas_cmd_t` message.
- During OTA upload (~10–30 s depending on file size), the HTTP server task is busy; ESP-NOW receive callbacks still fire but any response sends from that task are deferred.
- The SoftAP portal's network scan (§4) is capped at 20 results and only lists SSID/RSSI/auth-type — it does not indicate whether a network was previously seen or is the vessel's own router specifically.
- SlaveHonda has no web UI/OTA access while running WiFi-less (§6.2) — it must reach WiFi at least once (or be freshly flashed via USB) to receive an OTA update.
- Old firmware (custom MAC, hardcoded peer table) cannot interoperate with new discovery-based firmware (§8) — all three units must be upgraded together.
- `gpio_get_level()` on a pin configured `GPIO_MODE_OUTPUT` (output only, input not enabled) does not reliably read back the pin's actual driven state on ESP-IDF — this affects any status-API field that re-reads an output pin instead of tracking its own commanded state (e.g. SlaveWallas's `relay` field in `/api/status`, MasterRemote's `pFB`). Do not treat these fields as proof of true physical pin state; the GPIO23 heater indicator LED (§2.4) exists specifically to give SlaveWallas's relay a trustworthy hardware-level readout that doesn't depend on this.
- The weekly Wallas timer (§6.1a) depends on MasterRemote having WiFi/internet reachability for its initial and ongoing NTP sync (`pool.ntp.org`) — since MasterRemote's WiFi is already mandatory (§2.1), this doesn't add a new failure mode, but it's worth noting the timer stays fully inactive (never triggers) until the first successful sync after boot, and there's no local RTC/battery backup to bridge a sync gap after a power cycle. If the router blocks outbound NTP (uncommon but possible on a locked-down network), the timer will never activate at all.
- A manual **Stop Wallas** press during an active timer window doesn't durably override the timer — it clears the web override, but the timer's own OR'd-in contribution is still true, so it wins back within one `timer_task` tick (≤20 s). See §6.1a.
- The System resource monitor (§10, v1.25) is local per-unit only — MasterRemote's Pin Status tab shows only Master's own heap/uptime, not SlaveHonda's or SlaveWallas's. Check each slave's own web UI directly (when it has an IP) for its resource stats; there is no central aggregation.
- Before v1.25, any unit's web server could die permanently after a mid-run WiFi disconnect and require a manual power cycle to recover, even though ESP-NOW kept working the whole time (radio-layer, independent of STA/AP association) — see the v1.25 changelog entry and §10 for the root cause and fix. If a unit's web UI is unreachable but its Nodes-tab/ESP-NOW status still looks healthy on firmware older than 1.5.3/1.3.2/1.5.3, this is the likely cause; update to at least those versions.
- **When adding a new HTTP route to any unit (§6), also check `cfg.max_uri_handlers` in that unit's `web_server_start()`.** It's a hardcoded cap, not auto-sized from the route table, and `httpd_register_uri_handler()`'s return value is checked (as of v1.27) only to the extent of an `ESP_LOGE` — a route pushed past the cap still silently never works from the *web UI's* perspective, you just get a log line to explain why instead of a mystery. All three units now carry headroom above their current route count (16 on the slaves, 22 on MasterRemote), but that headroom is finite — see the v1.27 changelog entry for the SlaveHonda incident this pattern caused.
- **A hardware swap can change GPIO polarity or wire up fewer physical inputs than the firmware assumes — always re-verify against the physical unit, don't assume prior wiring carries over.** Hit twice on 2026-07-02 hardware replacements: MasterRemote's new board has GPIO4 (Wallas Victron relay) wired active-LOW instead of active-HIGH, and its GPIO5 (Wallas manual button) is now permanently unwired (v1.29/§2.2); the new SlaveHonda unit's GPIO15 (running feedback) is not yet wired at all, giving a confirmed false-positive "running" reading with the generator genuinely off (v1.30/§2.3). An unwired GPIO input with no pull resistor floats to an arbitrary, sometimes-consistent-looking value — it will not obviously look broken, so verify against a known physical state (relay on/off, generator on/off) rather than trusting a plausible-looking reading after any hardware change.
- **When a reported status value gates whether a command gets resent, the choice between "assume success" and "assume failure" for an unmeasurable value has real physical consequences — pick based on which failure mode is actually cheaper.** v1.30 excluded SlaveHonda's unwired running-feedback pin by reporting `false` unconditionally (assuming failure), which seemed like the conservative choice but wasn't: MasterRemote's Honda send gate only suppresses a resend once it sees the desired and reported states match, so a permanent `false` caused it to resend every `HONDA_RESTART_BLOCK_MS` (30 s) forever — and because sending Honda a start command isn't idempotent (each one spawns a full crank sequence, unlike Wallas's plain relay set), this repeatedly re-engaged the starter motor on an already-running engine (fixed in v1.31, see that changelog entry for the full explanation). The lesson generalizes: for a non-idempotent action gated by a mismatch check, a false negative (safe-looking) can be more dangerous than a false positive (looks risky) — work out which one the resend logic actually punishes before picking a default.

---

## 13. ESPNowRelay Repeater Node (optional, v1.34)

ESPNowRelay is a fourth, optional unit (ESP32-C6) that extends ESP-NOW radio reach between MasterRemote and a slave that can't hear it directly
 — e.g. placed midway between the house and the generator shed. It is a **transparent store-and-forward proxy**: 
 the existing three units run completely unmodified firmware and are unaware the relay exists.

### 13.1 How it relays

| Traffic | What the relay does |
|---------|---------------------|
| Master discovery beacon (broadcast, §3.2) | Re-broadcast **verbatim**. A far slave that can't hear Master directly hears the relayed beacon (src = relay's MAC), adopts the relay as its "Master" once its direct claim goes stale, and unicasts its heartbeats to the relay. |
| Slave heartbeat (unicast to the relay) | Learn/refresh that role's real MAC (staleness-gated, §3.2 semantics), then forward **verbatim** to the real Master. Master registers the relay's MAC as the role holder and unicasts that role's commands to the relay. |
| Master command (unicast to the relay) | Destination picked from the label (`Master->SlaveHonda` / `Master->SlaveWallas`), forwarded **verbatim** to that slave's learned real MAC. Only accepted from the trusted Master MAC. |
| Anything else (wrong size/label/sender) | Counted under "Other/unknown" for diagnostics, never relayed. |

Because ESP-NOW unicast is only delivered to the addressed MAC, the relay never sees direct Master↔slave traffic — it only handles frames explicitly sent to it (or broadcasts). **Failover is automatic in both directions:** each unit keeps whichever peer (direct or relayed) it hears consistently, and re-homes within the 30 s staleness window when that path dies (§3.2). With all direct links healthy, the relay idles in passive standby — its re-broadcast beacons are ignored by slaves whose direct claim on Master is still fresh (each such slave logs a warning per relayed beacon on its serial console; cosmetic only).

### 13.2 Consequences worth knowing

- A slave relaying through ESPNowRelay reports the **slave↔relay** link RSSI in its heartbeat (its "last frame heard from Master" is now a relay frame), so MasterRemote's Nodes tab shows the last-hop RSSI rather than an end-to-end figure. The Nodes tab's MAC column shows the **relay's** MAC while relaying (that's the sender Master registered for the role), but the heartbeat *content* — IP, firmware version, running/ignition states — is forwarded verbatim and remains the real slave's.
- Each relayed hop adds one store-and-forward retransmission (~1–2 ms) — negligible for this system's 2–15 s cadences.
- The relay does not appear on MasterRemote's Nodes tab as itself (no roster changes on Master by design). Its own web UI is the observation point.
- Two relays are safe against beacon ping-pong (a relay refuses a "Master" claim from a second MAC while its trusted Master is fresh), but chained/multi-hop relaying is neither designed nor tested.

### 13.3 Unit details

- **Pins:** GPIO2 onboard LED only — fast blink = traffic forwarded within the last 10 s, slow = idle/standby. No relays or inputs.
- **WiFi:** preferred (web UI/OTA/channel derivation); with saved credentials but no reachable router it falls back to the SlaveHonda-style ESP-NOW channel scan (§3.2) and relays without an IP (no web UI in that state). First boot with no credentials opens the `ESPNowRelay-Config` portal.
- **Web UI** (`http://<relay-ip>/`), two tabs:
  - **Status** — *ESP-NOW Traffic* table (per class: Rx count, forwarded, failed, last sender MAC, last forward destination MAC, RSSI of last frame, last seen), *Peers* table (MasterRemote / SlaveHonda / SlaveWallas: MAC, RSSI, last seen), *WiFi Link* card (AP RSSI + channel), *System* card (§10.1). 2 s auto-refresh.
  - **OTA Update** — same as all other units.
- **HTTP endpoints:** `/`, `/api/status`, `/api/sysinfo`, `/wifi-setup`, `/wifi-save`, `/api/scan`, `/ota/upload`.
- All accumulated platform lessons are applied from day one: `WIFI_PS_NONE` + max TX power (§v1.32), indefinite post-boot reconnect (§v1.25), URL-decoded portal credentials (§v1.28), handler-cap headroom + checked route registration (§v1.27), self-sampled heap low-water mark with timestamp (§10.1).
