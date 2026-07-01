# RemoteStart — Honda EU70IS & Wallas Heater Remote Start

ESP32-based wireless remote start system for a Honda EU70IS generator and Wallas diesel heater, controlled via a Victron Venus relay, manual switches, or the web UI.

## Units

| Unit         | MCU      | Purpose                                   | Web UI tabs |
|--------------|----------|--------------------------------------------|-------------|
| MasterRemote  | ESP32    | Master — relay/button input, ESP-NOW hub  | Pin Status, Nodes, OTA Update |
| SlaveHonda   | ESP32    | Honda generator control (WiFi-preferred, ESP-NOW fallback) | Status, OTA Update |
| SlaveWallas  | ESP32-C6 | Wallas heater control (WiFi required)     | Status, Debug GPIO, OTA Update |

## Framework

**ESP-IDF v6.0.1** (native C, CMake, FreeRTOS). No Arduino framework and no third-party libraries — WiFi, HTTP server, OTA, and the config portal are all built directly on ESP-IDF components.

## Features

- **ESP-NOW** peer-to-peer communication between units — no router hop needed for control traffic
- **Dynamic peer discovery** — each unit uses its own factory MAC and self-registers by function role over a broadcast beacon; no hardcoded peer table (see FSD §3.2)
- **SlaveHonda WiFi-optional fallback** — if it can't reach the router, it scans ESP-NOW channels for MasterRemote directly and keeps operating without an IP
- **Native captive portal** — first-startup WiFi setup with network scan, served by `esp_http_server` directly
- **Web UI per unit** — live pin status, node roster with per-link RSSI/channel (MasterRemote), manual start/stop override, OTA update
- **OTA firmware updates** — upload a `.bin` via the browser, no USB cable needed after the first flash
- **Individual versioning** — each unit has its own `FIRMWARE_VERSION` in `main/version.h`

## First Startup

1. Power on the unit
2. Connect to its config AP (e.g. `MasterRemote-Config`, password `honda1234`)
3. Navigate to `http://192.168.4.1/wifi-setup`, scan or type your WiFi credentials
4. The unit reboots and connects to your network

MasterRemote and SlaveWallas require WiFi. SlaveHonda prefers it but can operate WiFi-less over ESP-NOW.

## Build & Flash

```bash
# One-time ESP-IDF v6.0.1 environment setup (see FSD §11), then per unit:
cd MasterRemote            # or SlaveHonda / SlaveWallas
idf.py set-target esp32c6 # SlaveWallas only, first build
idf.py -p COMx flash
```

## OTA Update

1. `idf.py build`
2. Open `http://<device-ip>/` → **OTA Update** tab
3. Upload `<Unit>/build/<Unit>.bin`

## Documentation

See [FSD_RemoteStart.md](FSD_RemoteStart.md) for the full Functional Specification Document.

## Author

SEspe
