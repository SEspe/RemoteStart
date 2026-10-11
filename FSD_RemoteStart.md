# Functional Specification Document
## Remote Start System — Honda EU70IS & Wallas Heater
**Version:** 1.62  
**Author:** SEspe  
**Date:** 2026-10-11  

This is **the clean, current specification**: what the system is required to do,
stated in the present tense.

It deliberately carries **no history**. The development record — what changed at
each version and why — is in
[`FSD_RemoteStart_CHANGELOG.md`](FSD_RemoteStart_CHANGELOG.md). Read this file
to build the thing; read that one before reopening a question that was already
settled. This project is the origin of the house conventions the sibling
projects inherit, and several of those entries are cited by name in their
specifications.

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

> **Wiring drawings live in [docs/HARDWARE.md](docs/HARDWARE.md)** — MasterRemote's opto-isolated
> Venus interface board, the manual-switch and indicator field wiring, the EU7000is remote-connector
> mapping, and the end-to-end Honda start flow. This section stays the pin-level reference; the
> hardware guide is the schematic-level one.

### 2.1 Units

| Unit         | MCU      | Function role                              | WiFi requirement |
|--------------|----------|---------------------------------------------|-------------------|
| MasterRemote  | ESP32    | Master — relay/button input, ESP-NOW hub    | Required          |
| SlaveHonda   | ESP32    | Function slave — Honda generator control    | Preferred; falls back to ESP-NOW channel scan (§3.2) if unreachable |
| SlaveWallas  | ESP32-C6 | Function slave — Wallas heater control; runs standalone without MasterRemote (§3.5) | Required          |
| ESPNowRelay  | ESP32-C6 | Optional store-and-forward repeater (§13)   | Preferred; falls back to ESP-NOW channel scan (§3.2) if unreachable |

Each unit uses its own factory MAC address (read via `esp_wifi_get_mac()`; never overridden with `esp_wifi_set_mac()`). Peer MAC addresses are not hardcoded anywhere in firmware — units identify each other by function role and self-register dynamically over ESP-NOW at boot (§3.2). This means any unit's hardware module can be replaced (new factory MAC) without reflashing or reconfiguring the other two.

> **SlaveWallas chip note:** ESP32-C6 (not ESP32). Requires `idf.py set-target esp32c6` before the first build/flash of that project, and uses the onboard USB-Serial/JTAG console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) rather than a separate UART bridge chip.

### 2.2 MasterRemote Pin Assignments

| GPIO | Direction    | Signal                             |
|------|--------------|------------------------------------|
|  2   | OUTPUT       | Onboard LED — heartbeat blink      |
|  4   | INPUT        | Wallas Start — Victron Venus relay (**active LOW**, v1.29 — LOW = start demanded, HIGH = idle), fed from optocoupler U2's open collector with an external 10 kΩ pull-up (R5) to 3V3, plus the internal pull-up, so an unconnected pin reads HIGH = idle (v1.55) |
|  5   | INPUT PULLUP | Wallas Manual Start — maintained on/off toggle switch, **reconnected 2026-07-04 (v1.44)**, active HIGH like GPIO14's Honda manual switch — opposite polarity of GPIO4 (was unwired/obsolete v1.29–v1.43). OR'd into `g_wallas_start_cmd` as a plain level source alongside the Victron relay, web override, and weekly timer |
| 13   | OUTPUT       | Honda Running Feedback LED         |
| 14   | INPUT PULLUP | Honda Manual Start (push-button)   |
| 15   | INPUT        | Honda Start — Victron Venus relay (**active LOW** — LOW = start demanded, HIGH = idle), fed from optocoupler U1's open collector with an external 10 kΩ pull-up to 3V3, plus the internal pull-up, so an unconnected pin reads HIGH = idle (v1.55). Same polarity as GPIO4's Wallas input. See [Hardware Guide §2.2](docs/HARDWARE.md#22-isolated-inputs-from-the-victron-venus-raspberry-pi) |

**Input enable mask (v1.55).** Each of the four inputs above can be excluded as a command source from the Debug tab's **Input Sources** card (`POST /api/inputs/set`, §6). All four are enabled by default; the mask is saved in NVS as u8 `inen` (bit 0 GPIO15, bit 1 GPIO14, bit 2 GPIO4, bit 3 GPIO5). A disabled input still reports its pin in `/api/status`, but contributes nothing to `g_honda_start_cmd`/`g_wallas_start_cmd`, and a disabled GPIO14 also clears `HondaIgnitionOn` in the Honda message. Use it for an input that is not wired: with the internal pull-ups, an unconnected Victron input already reads idle, but an unconnected **manual-switch** input reads **On**, because the switch grounds its pin when Off. Every recompute site calls one function per command (`honda_cmd_compute()`/`wallas_cmd_compute()` in `main.c`), so the mask applies identically in the ISRs, the boot read, the timer tick and the web stop handlers.

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

Wiring to the heater (terminals 4/5/6 of the Wallas 12 V control block, relay NO across 4–5) and the ESP32-C6 relay-module board (relay on GPIO19, so it runs this firmware unchanged) are in [Hardware Guide §3](docs/HARDWARE.md#3-wallas-chain).

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

#### MasterRemote ↔ SlaveWallas weekly timer (`wallas_timer_msg_t`, §3.5)
```c
typedef struct __attribute__((packed)) {
    char     label[32];   // "Master->WallasTimer" or "SlaveWallas->Timer"
    uint32_t rev;         // revision; the higher one is kept by both units
    uint8_t  day[7][5];   // enabled, start hh, start mm, stop hh, stop mm; [0] = Sunday
} wallas_timer_msg_t;     // 71 bytes
```

#### SlaveWallas → Master button press (`wallas_webcmd_msg_t`, §3.5)
```c
typedef struct __attribute__((packed)) {
    char    label[32];    // "SlaveWallas->WebCmd"
    uint8_t start;        // 1 = Start Wallas, 0 = Stop Wallas
} wallas_webcmd_msg_t;    // 33 bytes
```

Every unit tells message types apart **by length first**, then by label: 32 beacon, 33 button press, 35 command, 65 SlaveWallas heartbeat, 66 SlaveHonda heartbeat, 71 timer. A new message type must have a length distinct from all of these. The two above are `packed` so the ESP32 and ESP32-C6 builds agree byte for byte; each unit `_Static_assert`s their size.

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
| SlaveWallas standalone after   | 30 s     | No beacon from its Master within this window → SlaveWallas runs standalone (§3.5) |
| Weekly timer sync              | 60 s     | Each side sends its table while the other is present; also 1 s after an edit and immediately on (re)contact (§3.5) |

### 3.5 SlaveWallas standalone mode and weekly timer sync

SlaveWallas works with or without MasterRemote. Which one drives the Wallas relay is decided continuously, once a second:

| State | When | Relay follows |
|-------|------|---------------|
| **Following MasterRemote** | A beacon from its Master within the last 30 s | Master's command (`master_msg_t`), which already ORs the Victron relay, manual switch, web Start/Stop and the weekly timer (§6.1a). SlaveWallas does not evaluate its own timer. |
| **Standalone** | No Master beacon for 30 s, or never one since boot | SlaveWallas's own **Start/Stop** buttons ORed with its own copy of the **weekly timer**. Same "sticky OR" as on Master: Stop clears the button's demand, but an active timer window keeps the heater on. |

- **Losing MasterRemote** drops every source that lives on Master. After 30 s the relay follows the local sources only, so the heater turns off unless the local timer or a local Start asks for it. No heater is left running with nobody in control.
- **MasterRemote coming back:** SlaveWallas sends a heartbeat at once, so a Master that lost its roster registers it again. Master force-sends the Wallas command on (re)registration. Until that command arrives the relay holds its state. If **Start** had been pressed locally while standalone, SlaveWallas passes it to Master as a button press (below), so the heater does not drop at the hand-over.
- **Start/Stop on SlaveWallas while Master is present** are sent to Master as `wallas_webcmd_msg_t` and act exactly like Master's own Start/Stop Wallas buttons (`g_web_wallas_start`), so both pages always agree.
- **Weekly timer:** both units hold the full 7-day table in NVS (`walltimer` blob + `walltrev` u32 revision, in each unit's own namespace). Every edit on either unit raises its `rev` to the edit's wall-clock time (once NTP has synced; never lower than the old rev + 1), so **the newest edit wins**. While both are present, each sends `wallas_timer_msg_t` to the other every 60 s, 1 s after a local edit (one send covers the tab's 7-request save) and immediately on (re)contact. A receiver adopts a table with a higher rev and answers a lower one with its own. A MasterRemote table saved before this mechanism (no `walltrev`) counts as rev 1, so it outranks a never-edited SlaveWallas (rev 0) and is copied to it.
- **Trust:** SlaveWallas accepts timer messages only from its registered Master MAC; MasterRemote accepts timer and button messages only from the MAC holding the SlaveWallas role. When relayed, both are the relay's MAC (§13), the same as for commands and heartbeats.
- SlaveWallas runs its own SNTP client (`pool.ntp.org`, `Europe/Oslo` TZ, same as Master) so its timer works standalone. Before the first sync its timer contributes nothing.

---

## 4. WiFi Configuration (First Startup)

On first power-up — or any boot where NVS has no stored WiFi credentials at all — each unit opens a SoftAP captive portal implemented directly in `main.c` using `esp_wifi` and `esp_http_server`. No external WiFiManager library is used.

> This portal is for *initial setup only*. A runtime connect failure with credentials already stored behaves differently per unit: MasterRemote and SlaveWallas retry/reopen the portal as before (WiFi is mandatory for both). SlaveHonda instead falls back to the ESP-NOW channel scan described in §3.2 and does **not** reopen the portal — the generator location is expected to sometimes be out of WiFi range, and reopening a config AP there would just strand the unit.

### Procedure for each unit

1. Power on the unit (fresh flash or after NVS erase).
2. The unit starts a WiFi access point:

   | Unit         | AP Name              | Password    | Address after setup |
   |--------------|----------------------|-------------|---------------------|
   | MasterRemote  | `MasterRemote-Config` | `honda1234` | `http://masterremote.local` |
   | SlaveHonda   | `SlaveHonda-Config`  | `honda1234` | `http://slavehonda.local` |
   | SlaveWallas  | `SlaveWallas-Config` | `honda1234` | `http://slavewallas.local` |
   | ESPNowRelay  | `ESPNowRelay-Config` | `honda1234` | `http://espnowrelay.local` |

3. Connect a smartphone or laptop to that AP.
4. Navigate to `http://192.168.4.1/wifi-setup` **in a normal browser tab** (a phone's automatic "sign in to network" window closes when the AP disappears, so it cannot follow the redirect in step 7).
5. Tap **Scan Networks** to list nearby SSIDs (signal strength + lock icon for secured networks), or type the SSID manually. Tapping a result fills in the SSID field.
6. Enter the WiFi password (if required) and submit the form. A note under the button says where the page will go next.
7. Credentials are written to NVS (`esp_err_t nvs_set_str()`), and the unit reboots. The **Saved!** page then polls `http://<unit>.local/api/version` every 2 s (3 s timeout each, from 6 s after saving, up to ~90 s) and **redirects to `http://<unit>.local/` by itself** once the unit answers on the home network. If it never does, the page keeps the manual fallback: the `.local` link, the router's client list (look for the hostname), and — for the slaves — MasterRemote's Nodes tab. The same page follows a save on the WiFi tab and the IP Configuration card.
8. On subsequent boots the unit reads credentials from NVS and connects directly in STA mode.

**Fixed names (v1.61).** Every unit sets its **DHCP hostname** (`esp_netif_set_hostname`, before DHCP starts) and announces itself over **mDNS** (`espressif/mdns` managed component, `_http._tcp` on port 80) as `MDNS_HOSTNAME` from `main/version.h` — `masterremote`, `slavehonda`, `slavewallas`, `espnowrelay`. mDNS is best-effort (a failed `mdns_init()` is logged and ignored) and **same-subnet only**: a client on another subnet, behind a router hop, still needs the IP. Two units of the same kind on one subnet would collide; the mDNS component then renames the second (e.g. `slavewallas-2`). A SlaveHonda in ESP-NOW fallback has no IP, so its name does not resolve.

> **Important:** All three units must connect to the **same** WiFi network so they share a channel for ESP-NOW.

### Portal implementation

The portal is served by `web_server.c`:

- `GET /wifi-setup` — HTML credential form with in-page network scan (JS fetches `/api/scan` and renders a clickable list)
- `GET /api/scan` — triggers `esp_wifi_scan_start()` (active scan, capped at 20 results) and returns a JSON array of `{ssid, rssi, auth}`
- `POST /wifi-save` — URL-encoded body parsed, SSID/password written to NVS, unit restarts

During the portal, WiFi runs in `WIFI_MODE_APSTA` (rather than AP-only) so the STA radio can perform the scan while the SoftAP keeps serving the setup page. A `g_portal_mode` flag suppresses the normal `WIFI_EVENT_STA_START → esp_wifi_connect()` auto-connect handler while the portal is active, since no credentials are configured yet.

> **Status:** this scan feature is implemented in `main.c`/`web_server.c` on all three units but not yet committed to git as of this revision.

### Static IP configuration (v1.37)

Unlike the SSID/password form above (portal-only in practice until v1.37, see `FSD_RemoteStart_CHANGELOG.md`), IP configuration is set from the always-available **WiFi** tab in each unit's normal web dashboard — no portal required, since the unit already has an IP to reach that dashboard on. Default behavior is unchanged: DHCP, via the STA netif's built-in DHCP client.

To switch a unit to a static IP: open its dashboard → **WiFi** tab → **IP Configuration** card → select **Static IP** → fill in IP address, subnet mask, gateway, and (optionally) DNS server → **Save & Apply**. The unit validates the IPv4 syntax of each field server-side, persists to NVS, and reboots to apply. On the next boot, `apply_static_ip()` (`main.c`) calls `esp_netif_dhcpc_stop()` and `esp_netif_set_ip_info()` (+ `esp_netif_set_dns_info()` if a DNS server was set) on the STA netif *before* `esp_wifi_start()`/`esp_wifi_connect()`, so DHCP is never even attempted in static mode.

> **Caution:** syntax validation catches typos (e.g. `192.168.1.999`) but cannot catch a static IP that's syntactically valid yet wrong for the actual network (wrong subnet, address already in use, wrong gateway) — WiFi association itself doesn't depend on IP configuration, so the unit will still join the AP and simply be unreachable over IP. Recovery is the same as a lost WiFi password: erase NVS over USB (below) to fall back to DHCP.

### Resetting WiFi credentials

Erase only the NVS partition (preserves firmware). This also resets IP configuration back to DHCP (same `NVS_NAMESPACE`, §5):
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

### 6.1 MasterRemote — Six Tabs

Access at: `http://<MasterRemote-IP>/`

| Tab          | Content                                                      | Refresh  |
|--------------|--------------------------------------------------------------|----------|
| Pin Status   | Live state of 5 pins + 3 global state flags + manual Start/Stop Honda and Start/Stop Wallas buttons | 2 s auto |
| Debug (v1.40) | WiFi Link (own MAC v1.42, RSSI, channel) + System resource monitor (heap, uptime, WiFi reconnects, §10) + Input Sources: one checkbox per physical input, staged and saved with one **Set** button (v1.55, §2.2) + Manual GPIO Test: live status dot for every GPIO, ON/OFF buttons on the non-reserved subset (v1.48 — note GPIO4/GPIO14 are externally-driven inputs on this unit, safe to read, not safe to drive) | 2 s auto |
| Nodes        | Table of registered slave nodes (§3.2): role, firmware version, MAC, IP, WiFi, RSSI, channel, connected status, last seen, running status, role-specific detail | 2 s auto |
| Weekly Timer | NTP clock readout + 7-row table (Sun–Sat): enable checkbox, start `HH:MM`, stop `HH:MM` for the Wallas heater, kept in sync with SlaveWallas (§3.5) | 2 s, rev-gated |
| WiFi (v1.37) | WiFi Network card (configured SSID + associated-AP readout, v1.47; SSID/password form + scan) + IP Configuration card (live address source/IP/mask/gateway/DNS readout, v1.47; DHCP/Static IP radio, IP/mask/gateway/DNS fields) | On tab open |
| OTA Update   | Firmware Versions table for **all four units** — GitHub / Stored / Installed (v1.49, §7.3) + Install-from-GitHub-Releases card (v1.45) with a Go GitHub link (v1.48) + file input & XHR upload to `/ota/upload` | On tab open + manual |

The Nodes tab is a genuine HTML `<table>` (one row per node) driven directly by MasterRemote's in-RAM peer roster (§3.2), so it reflects real discovered nodes rather than fixed content — a node that hasn't registered yet (e.g. SlaveHonda still mid channel-scan) shows "—" for MAC/IP/RSSI/Channel/FW until its first heartbeat arrives. The IP cell is a clickable link (v1.43) that opens the slave's own web UI in a new tab, when an IP is known. "Connected" = a heartbeat was received within the last 30 s (node stale timeout, §3.4); IP shows blank/"—" for a node currently running WiFi-less (SlaveHonda in ESP-NOW fallback); RSSI/Channel come from the slave's own last-heard-from-Master reading (§3.3), not MasterRemote's own link.

**Manual control:** the Pin Status tab's Start/Stop buttons are a command source OR'd in with the Victron relay and physical manual button (`g_web_honda_start`/`g_web_wallas_start` in `main.c`). Pressing Start latches the override on immediately (no need to wait for a GPIO edge); Stop clears the override and recomputes the command from current physical pin state (and, for Wallas, the weekly timer — see §6.1a). A web-triggered start persists across physical switch changes until explicitly stopped via the UI (or the corresponding physical button/relay signal is what next changes state and this recompute runs) — it does not silently clear itself.

#### 6.1a NTP Clock & Weekly Wallas Timer

MasterRemote syncs time via SNTP (`esp_netif_sntp`, server `pool.ntp.org`) once WiFi is connected, in the `CET-1CEST,M3.5.0,M10.5.0/3` POSIX timezone (Europe/Oslo — CET in winter, CEST in summer, DST transitions handled automatically). The clock is considered "synced" the first time `time(NULL)` returns an epoch past a fixed sanity floor (2024-01-01 UTC) — this exists specifically so an un-synced clock reading ~1970 can never accidentally satisfy a schedule match.

The **Weekly Timer** tab configures one independent on/off window per day of the week (Sunday–Saturday, matching `struct tm.tm_wday` directly) for the Wallas heater: an enable flag, a start `HH:MM`, and a stop `HH:MM`, persisted as a single NVS blob (`walltimer` key, same `NVS_NAMESPACE` as WiFi credentials) so it survives reboot/OTA.

A background task (`timer_task`, 20 s tick) evaluates the current local day and time-of-day against that day's window and sets `g_timer_wallas_start` — this is a **fourth Wallas command source**, OR'd in alongside the Victron relay, physical manual button, and web override (§3.2-style multi-source OR, same pattern as the manual buttons). A window may cross midnight (e.g. start `22:00`, stop `02:00`) and is evaluated correctly either way. If today is disabled, or the clock hasn't synced yet, the timer contributes nothing.

> **Same "sticky OR" behavior as the physical relay:** because this is one more source OR'd into the same command, manually pressing **Stop Wallas** during an active timer window only clears the *web* override — the timer's own contribution is still true, so `timer_task`'s next 20 s tick (or any GPIO edge) will turn it back on. This mirrors how a stuck-active Victron relay signal already behaves today; it is not a new inconsistency introduced by the timer.

The Weekly Timer tab polls `/api/timer` every 2 s while it is open, but only redraws the table when `rev` has changed — an edit from SlaveWallas (§3.5) or another browser. It never overwrites unsaved edits or a field being typed in: in that case a note says the timer was changed on the other unit, that **Set** will replace it, and offers a link to load the latest instead. The clock line also shows the time of the last change. SlaveWallas's Weekly Timer tab behaves the same. Edits are staged locally, not saved per-field: changing any checkbox or time input only enables a single **Set** button below the table (disabled by default, and re-disabled once the save completes); clicking it fires one `POST /api/timer/set` per day (all 7) with the current form values in parallel.

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
| POST   | `/api/inputs/set`   | Input enable mask, `?mask=0-15` (bit 0 GPIO15, 1 GPIO14, 2 GPIO4, 3 GPIO5); saved to NVS, recomputes both commands at once; 400 outside 0–15 (v1.55, §2.2) |
| GET    | `/wifi-setup`       | WiFi credential form (portal page; also embedded in the WiFi tab, v1.37) |
| GET    | `/api/scan`         | JSON array of nearby networks          |
| GET    | `/api/gpio/set`     | DEBUG: force a non-reserved pin to OUTPUT, `?pin=N&level=0\|1` (v1.48) |
| GET    | `/api/gpio/status`  | DEBUG: live level of every GPIO 0–39, read-only (v1.48) |
| POST   | `/wifi-save`        | Save credentials to NVS, restart       |
| GET    | `/api/ipconfig`     | JSON: configured SSID + associated AP SSID (v1.47), saved DHCP/static IP config, currently-active IP/mask/gateway (v1.37) + DNS (v1.47) |
| POST   | `/api/ipconfig/save`| Save IP config (`mode=dhcp\|static&ip=&mask=&gw=&dns=`) to NVS, restart (v1.37) |
| POST   | `/api/reboot`       | Manual reboot, no state change (v1.38) |
| GET    | `/api/version`      | JSON: this unit's installed + stored (inactive-slot) version; CORS-open (v1.49, §7.3) |
| GET    | `/api/versions`     | JSON: all four units' installed + stored, collected device-side; `?relay=<ip>` adds the relay row (v1.50, §7.3) |
| GET    | `/api/ota/github/latest` | JSON: newest published version per unit, from the release asset names (v1.49, §7.3) |
| GET    | `/api/ota/github/releases` | JSON: recent releases carrying a binary for this unit (v1.45) |
| POST   | `/api/ota/github/install`  | Start a background download+flash of a release asset (v1.45) |
| GET    | `/api/ota/github/progress` | JSON: install state/percent/error/trace (v1.45) |
| POST   | `/ota/upload`       | Raw binary OTA upload                  |

### 6.2 SlaveHonda & SlaveWallas — Local Web UI

Access at: `http://<Slave-IP>/`

| Tab        | Content                                               | Refresh  | Units |
|------------|--------------------------------------------------------|----------|-------|
| Status     | Relay states, running feedback, last received command, plus (SlaveWallas) heater indicator LED lamp. **SlaveWallas:** a Mode line (Following MasterRemote / Standalone) and **Start Wallas / Stop Wallas** buttons (§3.5) | 2 s auto | Both  |
| Debug (v1.40, renamed from "Debug GPIO") | Radio Link card (own MAC, v1.42; WiFi AP RSSI + channel, v1.33; ESP-NOW RSSI + channel + MasterRemote's MAC, v1.41/v1.42) + System resource monitor (heap, uptime, WiFi reconnects, §10) + live status dot for every GPIO; ON/OFF buttons on the non-reserved subset (hardware bring-up) | 2 s auto | Both  |
| Weekly Timer | Same 7-day table and staged **Set** button as MasterRemote's (§6.1a), kept in sync with it (§3.5); a line says whether this table is driving the heater (standalone) or MasterRemote is | 2 s, rev-gated (§6.1a) | SlaveWallas |
| WiFi (v1.37) | WiFi Network card (configured SSID + associated-AP readout, v1.47; SSID/password form + scan) + IP Configuration card (live address source/IP/mask/gateway/DNS readout, v1.47; DHCP/Static IP radio, IP/mask/gateway/DNS fields) | On tab open | Both |
| OTA Update | Firmware Versions row for this unit — GitHub / Stored / Installed (v1.49, §7.3) + Install-from-GitHub-Releases card (v1.45) with a Go GitHub link (v1.48) + file input & XHR upload to `/ota/upload` | On tab open + manual | Both  |

HTTP endpoints on both units: `/`, `/api/status`, `/api/sysinfo` (v1.25, §10), `/api/gpio/set?pin=N&level=0|1`, `/api/gpio/status`, `/wifi-setup`, `/api/scan`, `/wifi-save`, `/api/ipconfig`, `/api/ipconfig/save` (v1.37), `/api/reboot` (v1.38), `/api/version` + `/api/ota/github/latest` (v1.49, §7.3), `/api/ota/github/releases|install|progress` (v1.45), `/ota/upload`. SlaveWallas only: `POST /api/wallas/start|stop`, `GET /api/timer`, `POST /api/timer/set` (same parameters as Master's, §6.1). SlaveWallas's `/api/status` adds `sa` (standalone), `lw` (local Start active) and `ta` (own timer window active); its `/api/timer` adds `sa`. Both units' `/api/timer` include `rev`.

`/api/gpio/set` forces a manipulable pin to OUTPUT and drives it (active, overrides normal operation). `/api/gpio/status` is read-only (`gpio_get_level()` on every GPIO, safe to call even on reserved pins since it never reconfigures anything) and backs the Debug tab's status dots — every pin gets a dot, only the non-reserved subset gets ON/OFF buttons. The non-reserved/manipulable set differs per chip:

| Unit        | Chip         | Manipulable GPIOs                          | Reserved (status-only)                                    |
|-------------|--------------|----------------------------------------------|--------------------------------------------------------------|
| SlaveHonda  | ESP32        | 4,13,14,16,17,18,19,21,22,23,25,26,27,32,33  | Strapping (0,2,5,12,15), embedded flash (6-11), UART0 console (1,3), input-only (34-39) |
| SlaveWallas | ESP32-C6     | 0,1,2,3,6,7,10,11,14,16-23                  | Strapping (4,5,8,9,15), USB-Serial/JTAG (12,13)              |

> **SlaveHonda in ESP-NOW fallback (§3.2):** the unit has no IP address while WiFi-less, so its own web UI (and OTA) is unreachable in that state. Its status is only visible remotely via MasterRemote's Nodes tab (§6.1). To OTA-update SlaveHonda, it must be within WiFi range at boot (or reachable via the config portal) at least once.

> **Debug tab GPIO test (SlaveWallas only, §11):** `/api/gpio/set` unconditionally calls `gpio_reset_pin()` + `gpio_set_direction(OUTPUT)` + `gpio_set_level()` on whatever pin number is given, for physically verifying wiring when pin assignments are uncertain. It has no awareness of what a pin is normally used for — toggling GPIO19 (relay), GPIO18 (feedback input), GPIO2, or GPIO23 (LEDs) here overrides the unit's normal control of that pin until the next reboot. Not gated behind any confirmation; intended for hardware bring-up sessions only, not left exposed in a production deployment.

### 6.3 Status JSON format

`GET /api/status` on MasterRemote returns:
```json
{
  "pHS": false,   "pHM": false,   "pWS": false,
  "pWM": false,   "pFB": false,
  "gHS": false,   "gHR": false,   "gWS": false,
  "inEn": 15,
  "rssi": -62,    "ch": 1
}
```
Keys: `p` = pin level, `g` = global state flag; `HS` = Honda Start, `HM` = Honda Manual, `WS` = Wallas Start, `WM` = Wallas Manual, `FB` = Feedback, `HR` = Honda Running. `inEn` = the input enable mask (§2.2); `p` fields show the pin whether or not it is enabled, `g` fields reflect only enabled inputs. `rssi` = MasterRemote's WiFi signal strength in dBm (from `esp_wifi_sta_get_ap_info()`); `ch` = its current WiFi/ESP-NOW channel (`wifi_ap_record_t.primary`). Both read 0 if not connected.

---

## 7. OTA Update Procedure

There are two independent install paths on the OTA tab, on all four units: manual file upload, and (since v1.45) installing straight from this repo's GitHub Releases.

### 7.1 Manual file upload

The OTA handler in `web_server.c` uses `esp_ota_ops.h` with streaming writes:

```c
esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
// stream 1 KB chunks from HTTP body
esp_ota_write(handle, buf, len);
esp_ota_end(handle);
esp_ota_set_boot_partition(partition);
esp_restart();
```

Steps:

1. Build the firmware: `cd <Unit> && idf.py build`
2. Locate the binary: `<Unit>/build/<Unit>.bin`
3. Open `http://<device-ip>/` → **OTA Update** tab.
4. Click **Choose File**, select the `.bin`.
5. Click **Upload & Flash**. Progress is shown as a percentage.
6. The device reboots automatically. The new version appears in the page header.

> The upload uses a JavaScript `XMLHttpRequest` with `Content-Type: application/octet-stream`. The server reads the raw binary body in 1 KB chunks — no multipart parsing needed.

### 7.2 Install from GitHub Releases (since v1.45)

The device itself can fetch and flash a release binary, without downloading anything to your computer first:

1. Open `http://<device-ip>/` → **OTA Update** tab → **Install from GitHub Releases** card.
2. Click **Refresh Releases**. The device queries `api.github.com/repos/SEspe/RemoteStart/releases` and lists only the releases that include a binary matching this unit (`GET /api/ota/github/releases`).
3. Pick a release from the dropdown and click **Install Selected**, then confirm.
4. `POST /api/ota/github/install` validates the asset URL, then a background task downloads and flashes it via `esp_https_ota()` while the page polls `GET /api/ota/github/progress` every second to show a live percentage.
5. The device reboots automatically on success, same as manual upload. On failure, the tab shows the error and lets you retry.

Requires the device to have WiFi internet access to `api.github.com`/`github.com` (not just LAN access to your browser) — if that's unavailable, use the manual upload path instead.

### 7.3 Firmware Versions table (since v1.49)

The OTA tab opens with a **Firmware Versions** card, three values per unit:

| Column | Source | Meaning |
|--------|--------|---------|
| **GitHub** | `GET /api/ota/github/latest`, parsed from the `<Unit>_v<ver>.bin` asset names | Newest version published for that unit |
| **Stored** | `GET /api/version` (via `/api/versions` on MasterRemote) → `esp_ota_get_partition_description()` on the **inactive** OTA slot | The previous image, still on flash and still bootable |
| **Installed** | `GET /api/version` (via `/api/versions` on MasterRemote) → `FIRMWARE_VERSION` | What the unit is running now |

The Installed box turns amber when GitHub offers a newer version (dotted-numeric comparison). An empty cell means "unknown": no matching release asset, an OTA slot never written, or a unit that didn't answer.

**MasterRemote shows all four units in one table**, built from a single same-origin call to `GET /api/versions` (v1.50). MasterRemote does the collecting: its own row locally, SlaveHonda's and SlaveWallas's by fetching `/api/version` over LAN HTTP at the addresses in the ESP-NOW roster (so the table follows a DHCP move), each with a 4 s timeout so one silent unit leaves an empty row instead of stalling the table. ESPNowRelay is not part of that roster (§13 — the repeater is transparent by design and never announces itself), so its address is typed once into the field under the table, remembered by the browser (`localStorage`, per-browser, not device state) and passed to `/api/versions?relay=<ip>`; it is validated against an IPv4 alphabet before being used in a URL. The other three units show a single row for themselves, read locally.

> **Why the device does the collecting** (v1.50): v1.49 had the page fetch each slave directly. A browser treats `http://<slave-ip>/` as a different origin from the page served by MasterRemote, and in practice those reads came back empty — the slave rows stayed blank on real hardware even though every unit answered `/api/version` correctly when queried on its own. `/api/version` keeps its `Access-Control-Allow-Origin: *` header for direct use, but the table no longer depends on it.

> The Stored value is only as good as the app descriptor in the passive slot. Since v1.49 each unit's `CMakeLists.txt` sets `PROJECT_VER` from `main/version.h` so the descriptor carries the real firmware version; images built before that carry a `git describe` string instead, so a freshly updated unit shows an odd-looking Stored value until it has been updated twice.

---

## 8. Versioning & Release Artifacts

Each unit's version is defined independently in `main/version.h`:

```c
#define FIRMWARE_VERSION  "1.0.0"
#define FIRMWARE_NAME     "MasterRemote"   // SlaveHonda / SlaveWallas
```

Since v1.49 each unit's top-level `CMakeLists.txt` reads that header at configure time and sets `PROJECT_VER` from it, so the same string ends up in the app descriptor inside the image — that is what makes a *passive* partition's version readable at runtime (§7.3). `CMAKE_CONFIGURE_DEPENDS` on `main/version.h` keeps it in step, so a version bump re-runs CMake rather than baking a stale descriptor.

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

First flash must be done via USB — from the browser flasher (§11.1) or with ESP-IDF. OTA handles all subsequent updates. Release `.bin` assets are app images only; they are not sufficient on their own for a blank board, which also needs the bootloader, partition table and OTA data.

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
| `/api/ipconfig/save` given a syntactically invalid IP/mask/gateway/DNS (v1.37) | HTTP 400, nothing saved, no reboot — the unit keeps its prior working config |
| Saved static IP config unreadable/invalid at boot (v1.37) | `apply_static_ip()` logs a warning and falls back to DHCP rather than applying a broken config |
| Saved static IP config valid syntax but wrong for the actual LAN (v1.37) | Not detected — WiFi still associates, but the unit is unreachable over IP. Recovery: erase NVS over USB (§4) to fall back to DHCP |
| Honda: no running feedback                         | Slave reports `HondaRunning = false`; master may retry after 30 s block |
| Master: no slave heartbeat (node stale, 30 s)       | Nodes tab shows "disconnected"; MasterRemote stops sending to that role until it re-registers |
| SlaveHonda start sequence (~14 s warm-up + crank)   | Runs in its own task (v1.28) — web server and ESP-NOW stay responsive throughout. A stop command received mid-sequence aborts it within ~100 ms (relays de-energized immediately by `honda_stop()`, the sequence task notices and exits) |

### 10.1 System Resource Monitor (`/api/sysinfo`, v1.25, `heapMinAgo` added v1.26)

All four units expose `GET /api/sysinfo` and surface it as a **System** card on the **Debug** tab (v1.40 — previously "Debug GPIO" on SlaveHonda/SlaveWallas only; MasterRemote and ESPNowRelay showed it on their Status tab since neither had a Debug tab before v1.40). It returns:

| Field        | Source                                          | What it means                                                                 |
|--------------|---------------------------------------------------|---------------------------------------------------------------------------------|
| `heap`       | `esp_get_free_heap_size()`                       | Free heap bytes *right now* — a snapshot that bounces up and down constantly as the device allocates/frees things (HTTP responses, JSON buffers, WiFi scans). Not reliable on its own for spotting a leak, since a low reading can just be a busy moment. |
| `heapMin`    | Self-sampled low-water mark (`g_heap_min`)       | The lowest free-heap value *observed by the periodic sampler* since boot (10 s tick in `status_task` on the slaves, 20 s tick in `timer_task` on MasterRemote) — not `esp_get_minimum_free_heap_size()`'s byte-exact tracking, traded away in v1.26 so this value stays paired with a matching timestamp (`heapMinAgo`); IDF's own tracked minimum has no "when". Monotonically non-increasing, so it's still the reliable leak signal: a real leak shows up as this creeping steadily downward across days/weeks even while `heap` looks fine moment-to-moment. This is the field to watch. |
| `heapMinAgo` | `(now - g_heap_min_ts_us) / 1e6` (v1.26)         | Seconds since `heapMin` last dropped to a new low. Distinguishes "dipped once at boot, flat ever since" (large `heapMinAgo`, nothing to worry about) from "still actively dropping" (small `heapMinAgo`, a leak in progress) — `heapMin` alone can't tell these apart. |
| `uptime`     | `esp_timer_get_time() / 1e6`                     | Seconds since boot. Needed to judge whether a drop in `heapMin` is meaningful (e.g. "down 20 KB over 3 days" is a leak; "down 20 KB in the first minute after boot" is normal one-time startup allocation). |
| `wifiDisc`   | `g_wifi_disconnect_count` (never reset)          | Total WiFi STA disconnect events since boot. Tracks how flaky a unit's link actually is over time, and doubles as a live check that the v1.25 reconnect fix (§10) is working — this number can climb while the web server stays reachable throughout. |
| `wifiDiscAgo`| `(now - g_wifi_last_disc_ts_us) / 1e6`, or `-1` if never (v1.39) | Seconds since the *most recent* disconnect. On its own tells you how long it's been since the last hiccup; its real value is **cross-unit** — query all four within a few seconds of each other and compare: all four showing a similarly small `wifiDiscAgo` means a synchronized, shared router/AP-side event (power blip, scheduled reboot, DHCP renewal, channel interference); one unit alone showing a small value while the others show large/`-1` means that unit's own RF link, unrelated to the others. |

This monitor is local per-unit only (§12) — it was added specifically because the v1.25 outage (all three units' web servers dying after an extended run, requiring a manual power cycle) had no leading indicator; watching `heapMin` and `wifiDisc`/`wifiDiscAgo` going forward should surface a similar problem before it causes another outage.

---

## 11. Build & Flash Reference

### 11.1 Browser flasher (first flash of a new unit, no toolchain)

A new unit is flashed from **https://sespe.github.io/RemoteStart/**, an [esp-web-tools](https://esphome.github.io/esp-web-tools/) page that writes over Web Serial (Chrome or Edge on desktop). It has one button per unit; each points at that unit's own manifest.

| Unit | Chip family | Bootloader | Partition table | OTA data | App |
|------|-------------|-----------|-----------------|----------|-----|
| MasterRemote | ESP32    | 0x1000 | 0x8000 | 0xe000 | 0x10000 |
| SlaveHonda   | ESP32    | 0x1000 | 0x8000 | 0xe000 | 0x10000 |
| SlaveWallas  | ESP32-C6 | 0x0    | 0x8000 | 0xe000 | 0x10000 |
| ESPNowRelay  | ESP32-C6 | 0x0    | 0x8000 | 0xe000 | 0x10000 |

- **Source:** `docs/webflash/index.html` (the page) and `docs/webflash/manifest.template.json` (one template, filled in per unit).
- **Publishing:** `.github/workflows/webflash-pages.yml` runs on every push to `master` that touches a unit folder or the flasher, and on demand. It builds all four units with ESP-IDF v6.0.1, stages each unit's four images under `<Unit>/` with a `manifest.json` carrying that unit's `FIRMWARE_VERSION`, chip family and bootloader offset, and deploys the result to GitHub Pages. The repo's Pages source must be set to **GitHub Actions**.
- **What it serves** is the current `master` build, not necessarily the latest tagged release. That is fine for a first flash, since the unit's OTA tab can move it to any release afterwards.
- **Erase:** the manifest sets `new_install_prompt_erase`, so the page offers a full erase. Take it on any board with unknown earlier firmware: a plain flash leaves everything outside the written regions untouched, and stale data there has left a board whose logs reported a running SoftAP while its radio transmitted nothing.
- **Chip check:** esp-web-tools refuses a manifest whose chip family does not match the connected chip, so the C6 units cannot be flashed onto an ESP32 or vice versa. MasterRemote and SlaveHonda share a chip family, so the page cannot catch a mix-up between those two — the operator must press the right button.
- After flashing, the unit boots into its first-startup portal (§4).

### 11.2 ESP-IDF

#### Prerequisites

- ESP-IDF v6.0.1 installed and sourced (`idf.py` on PATH)
- MasterRemote/SlaveHonda: ESP32 board connected via USB. SlaveWallas: ESP32-C6 board connected via USB (native USB-Serial/JTAG, no separate UART chip needed).

#### Commands

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

#### IDE

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
- ~~`gpio_get_level()` on a pin configured `GPIO_MODE_OUTPUT` does not read back the pin's driven state~~ — **resolved in v1.36**: all status-relevant output pins are now configured `GPIO_MODE_INPUT_OUTPUT`, which enables the read-back buffer, so `/api/status` output-pin fields (SlaveHonda `ign`/`sta`, SlaveWallas `relay`/`led`, MasterRemote `pFB`) now reflect the genuinely driven hardware level. (Historical note: before v1.36 a plain OUTPUT pin always read 0, making SlaveHonda's inverted dots permanently true and SlaveWallas's/Master's fields permanently false.)
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
| Weekly timer / button press (§3.5) | `Master->WallasTimer` from the trusted Master goes to SlaveWallas; `SlaveWallas->Timer` and `SlaveWallas->WebCmd` from the learned SlaveWallas go to Master. Forwarded **verbatim**, counted with the Wallas command / heartbeat traffic. These never claim a role. |
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
- **Web UI** (`http://<relay-ip>/`), four tabs (WiFi tab added v1.37, Debug tab added v1.40):
  - **Status** — *ESP-NOW Traffic* table (per class: Rx count, forwarded, failed, last sender MAC, last forward destination MAC, RSSI of last frame, last seen), *Peers* table (MasterRemote / SlaveHonda / SlaveWallas: MAC, RSSI, last seen). 2 s auto-refresh.
  - **Debug** — *Radio Link* card (renamed from "WiFi Link" v1.42: own MAC, AP RSSI + channel, plus a per-peer ESP-NOW RSSI + MAC summary for MasterRemote/SlaveHonda/SlaveWallas) + *System* card (§10.1). 2 s auto-refresh while open.
  - **WiFi** — WiFi Network card (configured SSID + associated-AP readout, v1.47; SSID/password form + scan) + IP Configuration card (live address source/IP/mask/gateway/DNS readout, v1.47; DHCP/Static IP radio, IP/mask/gateway/DNS fields), same as the other three units (§4/§6.1/§6.2).
  - **OTA Update** — same as all other units.
- **HTTP endpoints:** `/`, `/api/status`, `/api/sysinfo`, `/wifi-setup`, `/wifi-save`, `/api/scan`, `/api/ipconfig`, `/api/ipconfig/save` (v1.37), `/api/reboot` (v1.38), `/api/version` + `/api/ota/github/latest` (v1.49, §7.3), `/api/ota/github/releases|install|progress` (v1.45), `/ota/upload`.
- All accumulated platform lessons are applied from day one: `WIFI_PS_NONE` + max TX power (§v1.32), indefinite post-boot reconnect (§v1.25), URL-decoded portal credentials (§v1.28), handler-cap headroom + checked route registration (§v1.27), self-sampled heap low-water mark with timestamp (§10.1).
