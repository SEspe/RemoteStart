# Functional Specification Document
## Remote Start System — Honda EU70IS & Wallas Heater
**Version:** 1.2  
**Author:** Stein Espe  
**Date:** 2026-07-01  
**Changelog:**
- v1.3 — Removed custom MAC addressing; units now self-discover peers by function role over ESP-NOW. SlaveHonda gains a WiFi-optional fallback (ESP-NOW channel scan for MasterHonda when it cannot reach the router). MasterHonda's Clients tab becomes a Nodes tab (MAC, IP, connected status, last seen). **Breaking change** — see §3.2 and §12.
- v1.2 — Migrated firmware and CI to ESP-IDF v6.0.1; SlaveWallas moved to ESP32-C6; added WiFi network scan to the config portal
- v1.1 — Converted firmware framework from Arduino to ESP-IDF v5

---

## 1. System Overview

The Remote Start System provides wireless remote start/stop control for a Honda EU70IS petrol generator and a Wallas diesel heater installed on a vessel or caravan, triggered by a Victron Venus relay output or manual switches.

Three ESP32 microcontrollers communicate over the **ESP-NOW** peer-to-peer protocol. Each unit uses its own factory (burned-in) MAC address — there is no custom/hardcoded MAC assignment. Units find each other dynamically by function role (§3.2) rather than by a pre-shared peer table, so any unit's module can be swapped without reconfiguring the others.

MasterHonda and SlaveWallas connect to a local WiFi network for web-based monitoring, OTA, and to derive their ESP-NOW channel. SlaveHonda prefers WiFi too, but if it cannot reach the router (e.g. weak signal near the generator) it instead scans ESP-NOW channels to find MasterHonda directly and operates without a WiFi/IP connection.

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

| Unit         | MCU      | Function role                              | WiFi requirement |
|--------------|----------|---------------------------------------------|-------------------|
| MasterHonda  | ESP32    | Master — relay/button input, ESP-NOW hub    | Required          |
| SlaveHonda   | ESP32    | Function slave — Honda generator control    | Preferred; falls back to ESP-NOW channel scan (§3.2) if unreachable |
| SlaveWallas  | ESP32-C6 | Function slave — Wallas heater control      | Required          |

Each unit uses its own factory MAC address (read via `esp_wifi_get_mac()`; never overridden with `esp_wifi_set_mac()`). Peer MAC addresses are not hardcoded anywhere in firmware — units identify each other by function role and self-register dynamically over ESP-NOW at boot (§3.2). This means any unit's hardware module can be replaced (new factory MAC) without reflashing or reconfiguring the other two.

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

### 3.2 Node Roles, Peer Discovery & Channel Acquisition

No unit's peer MAC is known in advance (§2.1), so peers are learned dynamically at runtime instead of read from a hardcoded table. Regular command/heartbeat traffic (§3.3) stays **unicast** exactly as before — only the discovery beacon described below is broadcast. Nothing is persisted to NVS; the roster is rebuilt from scratch every boot, which is intentional — it is what makes hardware swaps work without touching other units.

#### Channel acquisition per unit

| Unit        | How it gets an ESP-NOW channel                                                                 |
|-------------|--------------------------------------------------------------------------------------------------|
| MasterHonda | Connects to WiFi (required); its ESP-NOW channel is whatever channel it associates on.           |
| SlaveWallas | Connects to WiFi (required, same SSID as Master); channel matches automatically since both are on the same AP/router. |
| SlaveHonda  | Attempts WiFi connect first, using the same stored credentials and 15 s timeout as the other units (§4). **If it succeeds**, it behaves exactly like SlaveWallas — channel comes from the AP. **If it fails** (credentials exist but the router is unreachable), it does **not** reopen the config portal. Instead it enters **ESP-NOW channel scan**: it sets the radio to each 2.4 GHz channel 1–13 in turn (`esp_wifi_set_channel()`), dwelling on each one long enough to catch a MasterHonda beacon, until it hears one. It then locks to that channel. The config portal is still used on a genuinely unconfigured unit (no stored credentials at all) — see §4. |

#### Discovery beacon (new message)

While connected to WiFi, MasterHonda periodically broadcasts a small identity beacon to `FF:FF:FF:FF:FF:FF` (the broadcast address is added as a fixed ESP-NOW peer at init, same as today's peer setup, just unconditional rather than per-slave):

```c
typedef struct {
    char label[32];   // "MasterHonda"
} master_beacon_t;
```

This beacon serves two purposes: it lets SlaveWallas/SlaveHonda-with-WiFi learn Master's real MAC (`esp_now_recv_info_t.src_addr` on the received frame) without ever being told it in advance, and it is the signal SlaveHonda's channel scan (above) is listening for.

#### Registration (reuses existing heartbeat messages)

No separate "register" message is needed. The existing periodic heartbeats already sent by each slave (`slave_msg_t` from SlaveHonda, `slave_wallas_msg_t` from SlaveWallas — §3.3) do double duty as registration:

1. A slave hears Master's beacon, learns Master's MAC, and calls `esp_now_add_peer()` for it.
2. The slave then sends its normal heartbeat unicast to that MAC. Message *type/size* alone (already how the current receive handler tells the two slave message structs apart) is enough for Master to identify the role — no MAC allowlist or label parsing required.
3. MasterHonda, on receiving a heartbeat from a MAC it doesn't yet have as a peer, calls `esp_now_add_peer()` for it and adds/updates an in-RAM roster entry: `{mac, role, ip, has_wifi, last_seen}`. This roster (not a static peer table) is what MasterHonda's periodic command sends (§3.3) address, and what backs the Nodes web tab (§6.1).
4. Every subsequent heartbeat refreshes `last_seen` for that MAC. If MasterHonda reboots (roster cleared), the next heartbeat a slave sends after hearing a fresh beacon re-registers it automatically — no manual pairing step ever required.

If MasterHonda currently has no registered peer for a given role (e.g. SlaveHonda is still channel-scanning), its periodic command send for that role is simply skipped until a peer appears.

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

`slave_msg_t` and `slave_wallas_msg_t` each gain two fields to support the Nodes tab (§6.1) and registration (§3.2):
```c
    char  ip[16];      // "" if unit currently has no WiFi/IP (e.g. SlaveHonda in channel-scan mode)
    bool  has_wifi;     // true if this heartbeat was sent while WiFi-connected
```

#### MasterHonda → broadcast (`master_beacon_t`, new — discovery only)
```c
typedef struct {
    char  label[32];   // "MasterHonda"
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

> This portal is for *initial setup only*. A runtime connect failure with credentials already stored behaves differently per unit: MasterHonda and SlaveWallas retry/reopen the portal as before (WiFi is mandatory for both). SlaveHonda instead falls back to the ESP-NOW channel scan described in §3.2 and does **not** reopen the portal — the generator location is expected to sometimes be out of WiFi range, and reopening a config AP there would just strand the unit.

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
| `esp_wifi`         | WiFi STA / SoftAP, scanning, channel control, ESP-NOW (`esp_now.h`) |
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
| Nodes      | Roster of registered slave nodes (§3.2): role, MAC, IP, connected status, last seen, running status | 2 s auto |
| OTA Update | File input + XHR upload to `/ota/upload`                     | Manual   |

The Nodes tab replaces the old Clients tab. It is driven directly by MasterHonda's in-RAM peer roster (§3.2), so it reflects real discovered nodes rather than a fixed two-row layout — a node that hasn't registered yet (e.g. SlaveHonda still mid channel-scan) simply doesn't appear until its first heartbeat arrives. "Connected" = a heartbeat was received within the last 30 s (node stale timeout, §3.4); IP shows blank/"—" for a node currently running WiFi-less (SlaveHonda in ESP-NOW fallback).

HTTP endpoints:

| Method | Path           | Description                            |
|--------|----------------|----------------------------------------|
| GET    | `/`            | Main dashboard HTML                    |
| GET    | `/api/status`  | JSON: pin levels and global state      |
| GET    | `/api/nodes`   | JSON: roster (role, mac, ip, connected, last_seen, running status) |
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

> **SlaveHonda in ESP-NOW fallback (§3.2):** the unit has no IP address while WiFi-less, so its own web UI (and OTA) is unreachable in that state. Its status is only visible remotely via MasterHonda's Nodes tab (§6.1). To OTA-update SlaveHonda, it must be within WiFi range at boot (or reachable via the config portal) at least once.

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

> Adopting the discovery protocol (§3.2) is a breaking wire-protocol change — a unit running old (custom-MAC/hardcoded-peer) firmware cannot talk to one running the new discovery-based firmware. Bump MAJOR and flash all three units together.

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

## 9. Startup Sequence

### 9.1 MasterHonda & SlaveWallas (WiFi-mandatory units)

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
  │     └─ esp_now_add_peer(BROADCAST_MAC)   — fixed, for beacon (MasterHonda) / listening (SlaveWallas)
  │
  ├─ MasterHonda: xTaskCreate(beacon_task)     — broadcasts master_beacon_t every 2 s (§3.2)
  ├─ MasterHonda: xTaskCreate(master_task)     — sends to registered role peers, roster maintenance
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
| WiFi connect timeout (15 s) — MasterHonda/SlaveWallas | Portal opened (WiFi is mandatory for these units)     |
| WiFi connect timeout (15 s) — SlaveHonda, creds exist | No portal; falls back to ESP-NOW channel scan (§3.2/§9.2) |
| SlaveHonda: no MasterHonda beacon found            | Channel scan repeats indefinitely (§3.4); unit has no IP/web UI meanwhile |
| ESP-NOW init failure                               | `esp_restart()`                                          |
| Peer add failure                                   | `ESP_LOGE` log, execution continues                       |
| MasterHonda reboots (roster lost)                  | Self-healing: next heartbeat after each slave's next beacon re-registers it (§3.2), no manual pairing |
| OTA write error                                    | `esp_ota_abort()`, HTTP 500 returned, no reboot            |
| Honda: no running feedback                         | Slave reports `HondaRunning = false`; master may retry after 30 s block |
| Master: no slave heartbeat (node stale, 30 s)       | Nodes tab shows "disconnected"; MasterHonda stops sending to that role until it re-registers |
| SlaveHonda blocking start                          | Web server unresponsive for ~14 s during ignition warm-up + crank; expected behaviour (n/a while in ESP-NOW fallback — no web server) |

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

- ESP-NOW channel is derived from the connected WiFi AP for MasterHonda, SlaveWallas, and SlaveHonda-with-WiFi. If the router changes channel (unusual), those units must restart to re-sync. A SlaveHonda that is channel-scanning (no WiFi) is unaffected by router channel changes — it always finds Master's current channel by scanning — but if it has already locked on and Master's channel then changes mid-session, it loses sync until it restarts and rescans.
- The discovery beacon (§3.2) and all ESP-NOW traffic are unauthenticated and unencrypted (`peer.encrypt = false`, §3.1). Any device on channel range broadcasting a spoofed `master_beacon_t` (`label = "MasterHonda"`) could get a SlaveHonda/SlaveWallas to register it as Master and accept forged start/stop commands. This was a smaller risk with fixed custom-MAC peer allowlisting; moving to role-based discovery over broadcast increases it. Mitigate with per-peer AES + LMK (§3.1) if this is a concern for the deployment.
- `HondaStart` in `master_msg_t` carries the Wallas command to `SlaveWallas` (struct reuse from the original design). A future version should introduce a dedicated `wallas_cmd_t` message.
- During OTA upload (~10–30 s depending on file size), the HTTP server task is busy; ESP-NOW receive callbacks still fire but any response sends from that task are deferred.
- The SoftAP portal's network scan (§4) is capped at 20 results and only lists SSID/RSSI/auth-type — it does not indicate whether a network was previously seen or is the vessel's own router specifically.
- SlaveHonda has no web UI/OTA access while running WiFi-less (§6.2) — it must reach WiFi at least once (or be freshly flashed via USB) to receive an OTA update.
- Old firmware (custom MAC, hardcoded peer table) cannot interoperate with new discovery-based firmware (§8) — all three units must be upgraded together.
