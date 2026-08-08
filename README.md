# RemoteStart — Honda EU70IS & Wallas Heater Remote Start

ESP32-based wireless remote start system for a Honda EU70IS generator and Wallas diesel heater, controlled via a Victron Venus relay, manual switches, or the web UI.

## Units

| Unit         | MCU      | Purpose                                   | Web UI tabs |
|--------------|----------|--------------------------------------------|-------------|
| MasterRemote  | ESP32    | Master — relay/button input, ESP-NOW hub  | Pin Status, Debug (incl. Manual GPIO Test), Nodes, Weekly Timer, WiFi, OTA Update |
| SlaveHonda   | ESP32    | Honda generator control (WiFi-preferred, ESP-NOW fallback) | Status, Debug, WiFi, OTA Update |
| SlaveWallas  | ESP32-C6 | Wallas heater control (WiFi required)     | Status, Debug, WiFi, OTA Update |
| ESPNowRelay  | ESP32-C6 | Optional ESP-NOW store-and-forward repeater (see FSD §13) | Status (traffic/peers), Debug, WiFi, OTA Update |

## Framework

**ESP-IDF v6.0.1** (native C, CMake, FreeRTOS). No Arduino framework — WiFi, HTTP server, OTA, and the config portal are all built directly on ESP-IDF components. One managed component, `espressif/cjson` (cJSON, pulled via `idf_component.yml` since ESP-IDF v6 moved it out of the in-tree component set), is used for the GitHub Releases OTA feature.

## Features

- **ESP-NOW** peer-to-peer communication between units — no router hop needed for control traffic
- **Dynamic peer discovery** — each unit uses its own factory MAC and self-registers by function role over a broadcast beacon; no hardcoded peer table (see FSD §3.2)
- **SlaveHonda WiFi-optional fallback** — if it can't reach the router, it scans ESP-NOW channels for MasterRemote directly and keeps operating without an IP
- **ESPNowRelay repeater** (optional) — a transparent store-and-forward proxy that extends radio reach between MasterRemote and a far slave, with zero firmware changes on the other units; live traffic counters (count, sender, receiver, RSSI, last seen) on its own web UI (see FSD §13)
- **Native captive portal** — first-startup WiFi setup with network scan, served by `esp_http_server` directly
- **Web UI per unit** — live pin status, node roster with per-link RSSI/channel (MasterRemote), manual start/stop override, OTA update
- **NTP clock & weekly Wallas timer** (MasterRemote) — SNTP-synced CET/CEST clock; per-day enable with start/stop `HH:MM` window, OR'd in as a Wallas start source alongside the Victron relay, physical manual switch, and web override (see FSD §6.1a)
- **OTA firmware updates** — upload a `.bin` via the browser, no USB cable needed after the first flash; alternatively, the device can browse and install directly from this repo's GitHub Releases with a live progress bar, no manual download needed, plus a **Go GitHub** link straight to the repo (see FSD §7.2, v1.45/v1.48). The release listing is parsed as it streams in rather than buffered whole, so it stays within heap no matter how many releases accumulate, and the LWIP socket pool leaves room for the device's own outbound connection even with a browser parked on a polling tab (see FSD §12, v1.48)
- **Individual versioning** — each unit has its own `FIRMWARE_VERSION` in `main/version.h`
- **Resilient WiFi reconnection** — once a unit has connected once since boot, it retries indefinitely on any later disconnect instead of giving up after a few tries (fixed in v1.25 after a shared WiFi hiccup killed all three units' web servers until a manual power cycle; see FSD §10/§12)
- **System resource monitor** — free heap, heap low-water mark, uptime, and WiFi reconnect count on every unit's own web UI (`/api/sysinfo`), for catching a slow resource leak before it causes an outage (see FSD §10)
- **WiFi power save disabled, max TX power** — all three units run with `WIFI_PS_NONE` and max TX power instead of ESP-IDF's default modem-sleep, since the DTIM-interval wake latency that power save trades for battery life meaningfully hurts HTTP responsiveness at marginal signal strength, and these units aren't battery-constrained (see FSD §10, v1.32)
- **WiFi tab (DHCP or static IP)** — every unit's web UI now has an always-accessible WiFi tab: change SSID/password without reopening the config portal, and optionally switch from DHCP to a static IP/subnet/gateway/DNS (switching to Static pre-fills the current live values as a starting suggestion). Defaults to DHCP; saving either setting reboots the unit to apply. The tab also has a manual **Reboot Now** button (see FSD §4, v1.37/v1.38)
- **WiFi tab states what the unit is actually on** — the configured SSID and the AP it's currently associated to (shown separately, so "credentials fine but off the air" is distinguishable from "wrong credentials"), plus the live address source (static vs DHCP), IP, mask, gateway and DNS read back from the interface itself rather than from the saved config — so the IP is reported the same way whether it was configured here or handed out by the router (see FSD §4/§6, v1.47)
- **Standardized Debug tab** — every unit now has a Debug tab (renamed from "Debug GPIO" on the slaves, newly added on MasterRemote/ESPNowRelay) holding the WiFi Link (RSSI/channel) and System resource monitor cards, keeping the Status tab focused on operational state only (see FSD §6.1/§6.2/§13, v1.40)
- **ESP-NOW RSSI on the slaves' own UI** — SlaveHonda/SlaveWallas's Debug tab "Radio Link" card now shows the ESP-NOW RSSI/channel of the last frame heard from MasterRemote, alongside the existing WiFi AP RSSI/channel — previously only visible remotely via MasterRemote's Nodes tab (see FSD §6.2, v1.41)
- **MAC addresses visible on every unit's own Radio Link card** — each unit now shows its own STA MAC address; the slaves also show MasterRemote's MAC, and ESPNowRelay's Radio Link card (renamed from "WiFi Link") adds a per-peer ESP-NOW RSSI + MAC summary (MasterRemote/SlaveHonda/SlaveWallas) — previously MAC addresses were only visible remotely (MasterRemote's Nodes tab, ESPNowRelay's Peers table), never on a unit's own page (see FSD §6.1/§6.2/§13, v1.42)

## First Startup

1. Power on the unit
2. Connect to its config AP (e.g. `MasterRemote-Config`, password `honda1234`)
3. Navigate to `http://192.168.4.1/wifi-setup`, scan or type your WiFi credentials
4. The unit reboots and connects to your network

MasterRemote and SlaveWallas require WiFi. SlaveHonda prefers it but can operate WiFi-less over ESP-NOW.

## Build & Flash

```bash
# One-time ESP-IDF v6.0.1 environment setup (see FSD §11), then per unit:
cd MasterRemote            # or SlaveHonda / SlaveWallas / ESPNowRelay
idf.py set-target esp32c6 # SlaveWallas & ESPNowRelay only, first build
idf.py -p COMx flash
```

## OTA Update

Manual upload:
1. `idf.py build`
2. Open `http://<device-ip>/` → **OTA Update** tab
3. Upload `<Unit>/build/<Unit>.bin`

Or, from GitHub Releases (device needs internet access):
1. Open `http://<device-ip>/` → **OTA Update** tab → **Install from GitHub Releases**
2. Click **Refresh Releases**, pick a release, click **Install Selected**
3. The device downloads and flashes it directly, then reboots

## Documentation

See [FSD_RemoteStart.md](FSD_RemoteStart.md) for the full Functional Specification Document.

## Author

SEspe
