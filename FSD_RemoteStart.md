# Functional Specification Document
## Remote Start System — Honda EU70IS & Wallas Heater
**Version:** 1.2  
**Author:** Stein Espe  
**Date:** 2026-07-01  
**Changelog:**
- v1.2 — Migrated firmware and CI to ESP-IDF v6.0.1; SlaveWallas moved to ESP32-C6; added WiFi network scan to the config portal
- v1.1 — Converted firmware framework from Arduino to ESP-IDF v5

---

## 1. System Overview

The Remote Start System provides wireless remote start/stop control for a Honda EU70IS petrol generator and a Wallas diesel heater installed on a vessel or caravan, triggered by a Victron Venus relay output or manual switches.

Three ESP32 microcontrollers communicate over the **ESP-NOW** peer-to-peer protocol and connect to a local WiFi network for web-based monitoring and OTA firmware updates.

```
 Victron Venus  ──relay──►  MasterHonda  ──ESP-NOW──►  SlaveHonda  (Honda EU70IS)
 Manual switches ──────────►      │       ──ESP-NOW──►  SlaveWallas (Wallas heater)
                                  │
                            Web UI / OTA
```

**Development framework:** ESP-IDF v6.0.1 (native Espressif SDK). No Arduino framework or third-party libraries are used.

---

## 2. Hardware

### 2.1 Units

| Unit         | MCU      | Role              | Custom MAC        |
|--------------|----------|-------------------|--------------------|
| MasterHonda  | ESP32    | Master controller | 30:AE:A4:89:92:7A |
| SlaveHonda   | ESP32    | Honda generator   | 30:AE:A4:1A:AE:33 |
| SlaveWallas  | ESP32-C6 | Wallas heater     | 30:AE:A4:1A:AE:30 |

Custom MAC addresses are applied at startup via `esp_wifi_set_mac()` before `esp_wifi_start()`, so hardware modules can be replaced without updating peer tables in other units.

> **SlaveWallas chip note:** ESP32-C6 (not ESP32). Requires `idf.py set-target esp32c6` before the first build/flash of that project, and uses the onboard USB-Serial/JTAG console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) rather than a separate UART bridge chip.

### 2.2 MasterHonda Pin Assignments

| GPIO | Direction    | Signal                             |
|------|--------------|------------------------------------|
|  2   | OUTPUT       | Onboard LED — heartbeat blink      |
|  4   | INPUT        | Wallas Start — Victron Venus relay |
|  5   | INPUT PULLUP | Wallas Manual Start (push-button)  |
| 13   | OUTPUT       | Honda Running Feedback LED         |
| 14   | INPUT PULLUP | Honda Manual Start (push-button)   |
| 15   | INPUT        | Honda Start — Victron Venus relay  |

### 2.3 SlaveHonda Pin Assignments

| GPIO | Direction | Signal                                    |
|------|-----------|-------------------------------------------|
|  2   | OUTPUT    | Onboard LED — heartbeat blink             |
|  4   | OUTPUT    | External status LED                       |
| 13   | OUTPUT    | Starter relay (active LOW = crank ON)     |
| 14   | OUTPUT    | Ignition relay (active LOW = ignition ON) |
| 15   | INPUT     | Running feedback (LOW = engine running)   |

**Honda start sequence timing:**
1. Ignition relay ON (LOW) → 10 s warm-up
2. Starter relay ON (LOW) → 3 s crank
3. Starter relay OFF → 1 s settle
4. Read running feedback pin to confirm engine state

### 2.4 SlaveWallas Pin Assignments

| GPIO | Direction | Signal                                   |
|------|-----------|------------------------------------------|
|  0   | OUTPUT    | Wallas heater relay (HIGH = heater ON)   |
|  2   | OUTPUT    | Onboard LED (fast blink when running)    |
| 13   | INPUT     | Wallas running feedback (HIGH = running) |

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

### 3.2 Message Structures

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

### 3.3 Timing

| Parameter                 | Value | Description                           |
|---------------------------|-------|---------------------------------------|
| Honda restart block       | 30 s  | Minimum interval between Honda sends  |
| Wallas send interval      | 15 s  | Periodic Wallas command refresh       |
| SlaveHonda status period  | 10 s  | Slave → Master heartbeat              |
| SlaveWallas status period | 10 s  | Slave → Master heartbeat              |
| Honda ignition warm-up    | 10 s  | Delay between ignition ON and crank   |
| Honda crank time          | 3 s   | Duration of starter relay activation  |

---

## 4. WiFi Configuration (First Startup)

On first power-up, each unit detects that no WiFi credentials are stored in NVS and opens a SoftAP captive portal implemented directly in `main.c` using `esp_wifi` and `esp_http_server`. No external WiFiManager library is used.

### Procedure for each unit

1. Power on the unit (fresh flash or after NVS erase).
2. The unit starts a WiFi access point:

   | Unit         | AP Name              | Password    |
   |--------------|----------------------|-------------|
   | MasterHonda  | `MasterHonda-Config` | `honda1234` |
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
| `esp_wifi`         | WiFi STA / SoftAP, scanning, custom MAC, ESP-NOW (`esp_now.h`) |
| `esp_event`        | Default event loop for WiFi/IP events     |
| `esp_netif`        | Network interface (STA + AP)              |
| `nvs_flash` / `nvs`| Non-volatile storage of WiFi credentials  |
| `esp_driver_gpio`  | GPIO configuration, levels, ISR service   |
| `esp_http_server`  | Synchronous HTTP server                   |
| `esp_ota_ops` / `app_update` | OTA write, partition selection, reboot |
| `esp_timer`        | Microsecond timestamps for timing logic   |
| `freertos`         | Tasks, event groups, delays               |

> ESP-IDF v6 component changes vs. v5: `esp_now.h` is now provided by `esp_wifi` (no standalone `esp_now` component); GPIO driver moved to component `esp_driver_gpio`; `esp_ota_ops` is covered by `app_update`. See also the send-callback signature change in §3.1.

---

## 6. Web Interface

All HTML is embedded as `const char[]` string literals in `web_server.c`. No filesystem (SPIFFS/LittleFS) is required.

### 6.1 MasterHonda — Three Tabs

Access at: `http://<MasterHonda-IP>/`

| Tab        | Content                                                      | Refresh  |
|------------|--------------------------------------------------------------|----------|
| Pin Status | Live state of 5 pins + 3 global state flags                  | 2 s auto |
| Clients    | SlaveHonda and SlaveWallas last-seen time, running status     | 2 s auto |
| OTA Update | File input + XHR upload to `/ota/upload`                     | Manual   |

HTTP endpoints:

| Method | Path           | Description                            |
|--------|----------------|----------------------------------------|
| GET    | `/`            | Main dashboard HTML                    |
| GET    | `/api/status`  | JSON: pin levels and global state      |
| GET    | `/api/clients` | JSON: slave last-seen and status       |
| GET    | `/wifi-setup`  | WiFi credential form (portal only)     |
| GET    | `/api/scan`    | JSON array of nearby networks (portal only) |
| POST   | `/wifi-save`   | Save credentials to NVS, restart       |
| POST   | `/ota/upload`  | Raw binary OTA upload                  |

### 6.2 SlaveHonda & SlaveWallas — Two Tabs

Access at: `http://<Slave-IP>/`

| Tab        | Content                                               | Refresh  |
|------------|-------------------------------------------------------|----------|
| Status     | Relay states, running feedback, last received command | 2 s auto |
| OTA Update | File input + XHR upload to `/ota/upload`              | Manual   |

HTTP endpoints: `/`, `/api/status`, `/wifi-setup`, `/api/scan`, `/wifi-save`, `/ota/upload`.

### 6.3 Status JSON format

`GET /api/status` on MasterHonda returns:
```json
{
  "pHS": false,   "pHM": false,   "pWS": false,
  "pWM": false,   "pFB": false,
  "gHS": false,   "gHR": false,   "gWS": false
}
```
Keys: `p` = pin level, `g` = global state flag; `HS` = Honda Start, `HM` = Honda Manual, `WS` = Wallas Start, `WM` = Wallas Manual, `FB` = Feedback, `HR` = Honda Running.

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
#define FIRMWARE_NAME     "MasterHonda"   // SlaveHonda / SlaveWallas
```

### Version scheme: `MAJOR.MINOR.PATCH`

| Change type    | Bump  |
|----------------|-------|
| Breaking change | MAJOR |
| New feature     | MINOR |
| Bug fix         | PATCH |

### Release bin naming

```
MasterHonda_v1.0.0.bin
SlaveHonda_v1.0.0.bin
SlaveWallas_v1.0.0.bin
```

### GitHub Actions CI/CD

Pushing a version tag triggers `.github/workflows/release.yml`, which builds affected units using `espressif/esp-idf-ci-action@v1` (pinned to ESP-IDF v6.0.1 via `env.IDF_VERSION`) and attaches the `.bin` files to a GitHub Release.

| Tag pattern  | Units built                      |
|--------------|----------------------------------|
| `v*`         | All three                        |
| `master-v*`  | MasterHonda only                 |
| `honda-v*`   | SlaveHonda only                  |
| `wallas-v*`  | SlaveWallas only                 |

First flash must be done via USB. OTA handles all subsequent updates.

---

## 9. Startup Sequence (all units)

```
app_main()
  │
  ├─ nvs_flash_init()
  │
  ├─ gpio_init()          — configure outputs (relays off) and inputs
  │
  ├─ wifi_init_and_connect()
  │     ├─ esp_wifi_set_mac()   — apply custom MAC
  │     ├─ NVS has credentials?
  │     │     YES → esp_wifi_start() → wait for IP (15 s)
  │     │             connected? → web_server_start() → return
  │     │             failed?   → fall through to portal
  │     └─ NO / failed → start_config_portal()
  │                         esp_wifi AP mode
  │                         web_server_start()   (serves /wifi-setup)
  │                         wait for POST /wifi-save
  │                         nvs_set_str() → esp_restart()
  │
  ├─ espnow_init()
  │     ├─ esp_now_init()
  │     ├─ esp_now_register_recv_cb()
  │     └─ esp_now_add_peer()   (channel = 0)
  │
  ├─ xTaskCreate(master_task / status_task / heartbeat_task)
  │
  └─ [FreeRTOS scheduler runs tasks]
```

---

## 10. Error Handling & Recovery

| Condition                   | Behaviour                                               |
|-----------------------------|---------------------------------------------------------|
| No NVS credentials on boot  | SoftAP portal opened; restarts after credentials saved  |
| WiFi connect timeout (15 s) | Portal opened                                           |
| ESP-NOW init failure        | `esp_restart()`                                         |
| Peer add failure            | `ESP_LOGE` log, execution continues                     |
| OTA write error             | `esp_ota_abort()`, HTTP 500 returned, no reboot         |
| Honda: no running feedback  | Slave reports `HondaRunning = false`; master may retry after 30 s block |
| Master: no slave heartbeat  | Clients tab shows elapsed time; control messages still sent |
| SlaveHonda blocking start   | Web server unresponsive for ~14 s during ignition warm-up + crank; expected behaviour |

---

## 11. Build & Flash Reference

### Prerequisites

- ESP-IDF v6.0.1 installed and sourced (`idf.py` on PATH)
- MasterHonda/SlaveHonda: ESP32 board connected via USB. SlaveWallas: ESP32-C6 board connected via USB (native USB-Serial/JTAG, no separate UART chip needed).

### Commands

```bash
# Build
cd MasterHonda          # or SlaveHonda / SlaveWallas
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

- ESP-NOW channel is derived from the connected WiFi AP. If the router changes channel (unusual), all three units must be restarted to re-sync.
- `HondaStart` in `master_msg_t` carries the Wallas command to `SlaveWallas` (struct reuse from the original design). A future version should introduce a dedicated `wallas_cmd_t` message.
- During OTA upload (~10–30 s depending on file size), the HTTP server task is busy; ESP-NOW receive callbacks still fire but any response sends from that task are deferred.
- The SoftAP portal's network scan (§4) is capped at 20 results and only lists SSID/RSSI/auth-type — it does not indicate whether a network was previously seen or is the vessel's own router specifically.
