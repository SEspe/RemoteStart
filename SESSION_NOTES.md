# Session notes — 2026-10-11

One long session: browser flasher, two firmware features, Wallas hardware docs with a safety
disclaimer, MIT license, repo cleanup. Full detail is in [FSD_RemoteStart_CHANGELOG.md](FSD_RemoteStart_CHANGELOG.md)
v1.54–v1.61; this file is the hand-over: what is where, what is verified, what is open.

## Shipped today

| FSD | Commit / tag | What |
|-----|--------------|------|
| v1.54 | `dbcd302` | **Browser flasher** at https://sespe.github.io/RemoteStart/ — esp-web-tools, one button per unit, rebuilt from `master` by `webflash-pages.yml` (BirdBox pattern). Pages source = GitHub Actions. Also fixed the release notes' USB instructions (app-only `write_flash 0x10000` doesn't boot a blank board). |
| v1.55 | `e744ee2`, `master-v1.7.0` | **MasterRemote inputs:** internal pull-ups on GPIO15/GPIO4 (an absent interface board now reads idle, not "start"), and a Debug-tab **Input Sources** enable mask (`/api/inputs/set?mask=0-15`, NVS `inen`, default all on). All recompute sites go through `honda_cmd_compute()`/`wallas_cmd_compute()`. |
| v1.56 | `12aaef9`, **`v1.35.0`** | **SlaveWallas standalone** + **weekly timer on both units, synced** over ESP-NOW (newest edit wins), SlaveWallas Start/Stop buttons, self-refreshing Weekly Timer tabs. MasterRemote 1.8.2, SlaveWallas 1.7.2, ESPNowRelay 1.2.0, SlaveHonda 1.4.13 (unchanged). FSD §3.5. |
| v1.57 | `f2c2c97`, `fbca35c` | **Wallas wiring** in the hardware guide §3.1–3.2: older **Wallas M26**, 12 V block 6 −, 5 +, 4 joined to 5 = on. Planned ESP32-C6 single-relay module: supply 5/6, COM→5, **NO**→4, ≥ 0.7 mm². |
| v1.58 | `340985e` | Removed the five unused Arduino `.ino` sketches (last in `fbca35c`). |
| v1.59 | `a376c1f` | **Safety precautions + at-your-own-risk disclaimer** for connecting SlaveWallas to a heater: CAUTION block opening Hardware Guide §3, banner at the top of the guide, README "Safety and disclaimer" section. |
| — | `cccbd90` | Hardware guide §3.2 links an **example (unverified) eBay listing** for the relay module. |
| v1.60 | `fd59711` | **MIT license** (`LICENSE`, as BirdBox); README License section excludes the two third-party images. |
| — | `9b18acf` | README **screenshots** of SlaveWallas (standalone Status tab, Weekly Timer tab), taken by the user on 1.7.2; stored in `docs/screenshots/`. |
| v1.61 | `7a628cf`, **`v1.36.0`** | **`<unit>.local` + self-redirecting setup page:** DHCP hostname + mDNS on all four units (`masterremote`, `slavehonda`, `slavewallas`, `espnowrelay`); after Connect & Save / Save & Apply the Saved! page polls `<unit>.local` and opens it by itself; a note under each save button says so. MasterRemote 1.9.0, SlaveHonda 1.5.0, SlaveWallas 1.8.0, ESPNowRelay 1.3.0. The browser flasher serves these builds. |
| — | `79b3815` | README Build & Flash: setup walk-through with each unit's `.local` address, when `.local` works, and two flasher troubleshooting notes ("Failed to initialize" → BOOT+EN; port busy → close the flasher's Logs & console). OTA section uses `<unit>.local`. |
| v1.62 | `683ae4f` | **Relay module's relay = GPIO19, active HIGH** (three Start/Stop pulses → three clicks, user confirmed). The board runs standard SlaveWallas firmware; the bench SlaveWallas (.220) **is** this relay module. |

## Decisions the user made (don't re-ask)

- **Standalone is automatic:** 30 s without a Master beacon, or none since boot.
- **Master lost while heating:** fall back to SlaveWallas's own timer/buttons; heater off unless
  one of those wants it.
- **Timer conflicts:** last edit wins (rev = edit's NTP epoch, never below rev + 1).
- **SlaveWallas buttons with Master present** act exactly like Master's own web Start/Stop.
- **Clock:** NTP per unit, no timer before first sync, no RTC / no time-from-Master — "OK for now".
- **Manual-switch fail-safe:** firmware mask now; rewiring each switch to its other contact
  (ground = On, read inverted) is the real fix, at a later hardware visit.
- **Wallas wiring:** relay **NO** across terminals 4–5, board powered from 5/6 (the user's own
  description of their M26). Every Wallas wiring text must carry the safety precautions and the
  at-your-own-risk disclaimer (Hardware Guide §3, README `#safety`).
- **License:** MIT, "Copyright (c) 2026 SEspe", as BirdBox. The two third-party images in
  `docs/hardware/` are excluded in the README's License section.

## Test bench (192.168.10.x)

| Unit | IP | MAC | Firmware |
|------|----|-----|----------|
| MasterRemote (spare board) | 192.168.10.224 | `30:AE:A4:4B:E1:78` | 1.8.2, input mask 5 (manual switches off — nothing wired) |
| SlaveWallas | 192.168.10.220 (`slavewallas.local`) | `98:A3:16:9D:8D:0C` | 1.8.0 — the user did a full erase + flash round trip with the browser flasher on COM13 (native USB) and set it up again through the portal; it came back on `slavewallas.local`. The erase wiped its timer table (the test MasterRemote's newer copy will sync back). |

At the end of the session the test MasterRemote (.224) was **unplugged** and SlaveWallas was
running standalone. Timer table: Sun 19:24–19:26 and Mon 19:22–19:25 saved but unticked, all
other days empty, last change 19:41:07.

A third bare ESP32 (`30:ae:a4:4c:31:b0`) was probed on COM12; its auto-reset doesn't work —
hold BOOT, tap EN, release BOOT before flashing. The test MasterRemote beacons on channel 6:
if the real MasterRemote goes quiet > 30 s, a real slave in radio range could adopt it.

**The user tests on the bench in parallel.** The timer entries there are theirs (Sun 19:24–19:26,
Mon 19:22–19:25, both unticked now). Ask before editing them; I overwrote Wednesday twice today
and had to clean up.

## Verified on the bench

- Browser flasher: full erase + flash of MasterRemote and SlaveWallas, first-boot portal, pairing.
- 1.7.0 inputs: GPIO4/15 read 0 → 1 with nothing connected; mask 5 dropped both commands and the
  slave relay; bad mask → 400; survives reboot.
- Standalone: Start/Stop through Master; timer sync both ways in ~2 s; Master unplugged →
  `sa:true`, local Start/Stop drive the relay, a window covering "now" switched it on (`ta:true`);
  boot with no Master → standalone; Master back → follows it and has the standalone edits.

## Still open

1. **Production units are NOT updated** — latest release is **v1.36.0**. .100–.103 still run MasterRemote 1.6.15,
   SlaveHonda 1.4.13, SlaveWallas 1.6.13, ESPNowRelay 1.1.9 (1.1.12 never installed; it was
   offline 2026-08-08). Update to **v1.36.0** from each OTA tab. **If ESPNowRelay is in the path,
   update it with the others** — an old relay drops the new timer/button messages (sizes 71/33).
   Check SlaveWallas `/api/status` for an active command before rebooting it.
2. **Untested:** `.local` names and the redirect on MasterRemote, SlaveHonda and ESPNowRelay (built, not run — verified only on SlaveWallas); a local Start made while standalone being handed to Master when it returns;
   anything through ESPNowRelay (no relay on the bench).
3. **New SlaveWallas board (ESP32-C6 relay module): relay GPIO found — GPIO19**, works with the
   standard firmware. Remaining: wire it to the Wallas M26 (Hardware Guide §3.2 — read the
   safety block first) and test at the heater while present.
4. Manual-switch rewiring (above). Fix the redrawn MasterRemote schematic (hardware guide §6).
5. FSD §12 still says MasterRemote's `max_uri_handlers` is 22 (it's 32) — stale line.

## What went wrong today (lessons)

- **"COM port taken"** was the user's own Chrome tab: the web flasher's **Logs & console** view holds the port while open. Check with a .NET `SerialPort.Open()` test; don't kill Chrome (it closes every tab) — ask the user to close the dialog/tab or replug.

- An inline Python edit wrote a **literal NUL byte** into `web_server.c` (`'\\0'` inside a
  heredoc'd triple-quoted string). Long patch scripts go in a scratchpad file, not a heredoc.
- `re.sub` with `\"` in the replacement left stray backslashes; read back after regex edits.
- Editing the test bench's timer while the user was also using it.
- The "Failed to initialize" web-flasher error was the board's auto-reset, not the page —
  reproduce with esptool before blaming the flasher.
