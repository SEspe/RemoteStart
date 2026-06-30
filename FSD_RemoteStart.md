# Functional Specification Document
## Remote Start System — Honda EU70IS & Wallas Heater
**Version:** 1.0  
**Author:** Stein Espe  
**Date:** 2026-06-30

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

---

## 2. Hardware

### 2.1 Units

| Unit         | MCU    | Role              | Custom MAC              |
|--------------|--------|-------------------|-------------------------|
| MasterHonda  | ESP32  | Master controller | 30:AE:A4:89:92:7A       |
| SlaveHonda   | ESP32  | Honda generator   | 30:AE:A4:1A:AE:33       |
| SlaveWallas  | ESP32  | Wallas heater     | 30:AE:A4:1A:AE:30       |

Custom MAC addresses are set at startup via `esp_wifi_set_mac()` so that hardware can be replaced without re-programming the peers.

### 2.2 MasterHonda Pin Assignments

| Pin | Direction | Signal                              |
|-----|-----------|-------------------------------------|
|  2  | OUTPUT    | Onboard LED — heartbeat blink       |
|  4  | INPUT     | Wallas Start — Victron Venus relay  |
|  5  | INPUT_PU  | Wallas Manual Start (push-button)   |
| 13  | OUTPUT    | Honda Running Feedback LED          |
| 14  | INPUT_PU  | Honda Manual Start (push-button)    |
| 15  | INPUT     | Honda Start — Victron Venus relay   |

### 2.3 SlaveHonda Pin Assignments

| Pin | Direction | Signal                                       |
|-----|-----------|----------------------------------------------|
|  2  | OUTPUT    | Onboard LED — heartbeat blink                |
|  4  | OUTPUT    | External status LED                          |
| 13  | OUTPUT    | Starter relay (active LOW = crank)           |
| 14  | OUTPUT    | Ignition relay (active LOW = ignition ON)    |
| 15  | INPUT     | Running feedback (LOW = engine running)      |

**Start sequence timing:**
1. Ignition relay ON → 10 s warm-up
2. Starter relay ON → 3 s crank
3. Starter relay OFF → 1 s settle
4. Check running feedback pin

### 2.4 SlaveWallas Pin Assignments

| Pin | Direction | Signal                                  |
|-----|-----------|------------------------------------------|
|  0  | OUTPUT    | Wallas heater relay (HIGH = heater ON)   |
|  2  | OUTPUT    | Onboard LED (fast blink when running)    |
| 13  | INPUT     | Wallas running feedback (HIGH = running) |

---

## 3. Communication Architecture

### 3.1 ESP-NOW

- **Protocol:** IEEE 802.11 ESP-NOW peer-to-peer, no router required for control messages.
- **Channel:** Dynamically matched to the connected WiFi AP channel (all three units must connect to the same WiFi network).
- **Encryption:** Disabled (controlled private network; enable `peer.encrypt = true` for production hardening).

### 3.2 Message Structures

All structures must be identical on sender and receiver.

#### Master → Slave (MasterMsg)
```c
typedef struct {
  char  a[32];          // Label string
  bool  HondaRunningFB; // Echo of running status
  bool  HondaIgnitionOn;// Ignition on flag
  bool  HondaStart;     // Command: true = start, false = stop
                        //  (also carries WallasStart for SlaveWallas)
} MasterMsg;
```

#### SlaveHonda → Master (SlaveMsg)
```c
typedef struct {
  char  a[32];
  bool  HondaIgnitionOn;
  bool  HondaStarting;
  bool  HondaRunning;
} SlaveMsg;
```

#### SlaveWallas → Master (SlaveWallasMsg)
```c
typedef struct {
  char  a[32];
  bool  WallasRunning;
  bool  WallasStart;
} SlaveWallasMsg;
```

### 3.3 Timing

| Parameter                | Value  | Description                              |
|--------------------------|--------|------------------------------------------|
| Honda restart block      | 30 s   | Minimum interval between Honda sends     |
| Wallas send interval     | 15 s   | Periodic Wallas command refresh          |
| SlaveHonda status period | 10 s   | Slave → Master heartbeat                 |
| SlaveWallas status period| 10 s   | Slave → Master heartbeat                 |
| Honda ignition warm-up   | 10 s   | Delay between ignition ON and cranking   |
| Honda crank time         | 3 s    | Duration of starter motor activation     |

---

## 4. WiFi Configuration (First Startup)

All three units use **WiFiManager** (by tzapu) to collect WiFi credentials on first power-up. Credentials are stored in non-volatile flash and used automatically on subsequent boots.

### Procedure for each unit

1. Power on the unit (fresh / factory firmware).
2. The unit opens a configuration access point:

   | Unit         | AP Name                | Password    |
   |--------------|------------------------|-------------|
   | MasterHonda  | `MasterHonda-Config`   | `honda1234` |
   | SlaveHonda   | `SlaveHonda-Config`    | `honda1234` |
   | SlaveWallas  | `SlaveWallas-Config`   | `honda1234` |

3. Connect a smartphone or laptop to that AP.
4. A captive portal opens automatically (or navigate to `192.168.4.1`).
5. Select the home/vessel WiFi network and enter the password.
6. The unit saves credentials and reboots, connecting to the router.
7. The unit's IP address is printed on the Serial monitor (115200 baud).

> **Important:** All three units must connect to the **same** WiFi network so they share a channel for ESP-NOW communication.

### Portal timeout

If no configuration is entered within **3 minutes**, the unit restarts and opens the portal again.

### Resetting WiFi credentials

Call `WiFiManager::resetSettings()` in code (e.g., triggered by long-press on a button) or erase NVS via: `esptool.py erase_region 0x9000 0x6000`.

---

## 5. Web Interface

### 5.1 MasterHonda — Three Tabs

Access at: `http://<MasterHonda-IP>/`

| Tab          | Content                                                       | Refresh  |
|--------------|---------------------------------------------------------------|----------|
| Pin Status   | Live state of all 5 input/output pins + 3 global state flags | 2 s auto |
| Clients      | SlaveHonda and SlaveWallas last-seen, running status, flags   | 2 s auto |
| OTA Update   | ElegantOTA upload page embedded in iframe                     | Manual   |

JSON API endpoints:
- `GET /api/status` — pin and global state JSON
- `GET /api/clients` — slave status JSON
- `GET /update` — ElegantOTA page (also used by OTA tab iframe)

### 5.2 SlaveHonda & SlaveWallas — Two Tabs

Access at: `http://<Slave-IP>/`

| Tab        | Content                                                 | Refresh  |
|------------|---------------------------------------------------------|----------|
| Status     | Relay states, running feedback, last received command   | 2 s auto |
| OTA Update | ElegantOTA upload page embedded in iframe               | Manual   |

JSON API endpoint:
- `GET /api/status` — pin and state JSON

---

## 6. OTA Update Procedure

1. Compile the firmware in Arduino IDE for the target board (**ESP32**).
2. Export compiled binary: *Sketch → Export Compiled Binary* → produces `<name>.ino.bin`.
3. Open the web UI for the unit and navigate to the **OTA Update** tab.
4. Click "Choose File", select the `.bin` file.
5. Click "Update". The device flashes the new firmware and reboots automatically.
6. Verify the new version appears in the page header.

---

## 7. Versioning & Release Artifacts

Each unit has an **independent** firmware version defined in its source file:

```c
#define FIRMWARE_VERSION  "1.0.0"
#define FIRMWARE_NAME     "MasterHonda"   // or SlaveHonda / SlaveWallas
```

### Version scheme: `MAJOR.MINOR.PATCH`

| Change type           | Bump        |
|-----------------------|-------------|
| Breaking change       | MAJOR       |
| New feature           | MINOR       |
| Bug fix               | PATCH       |

### Release bin file naming convention

```
MasterHonda_v1.0.0.bin
SlaveHonda_v1.0.0.bin
SlaveWallas_v1.0.0.bin
```

Each release tag in the GitHub repository contains all three bin files as individual assets, allowing each unit to be updated independently.

---

## 8. Startup Sequence (all units)

```
Power on
  │
  ├─ Set custom MAC
  │
  ├─ WiFiManager::autoConnect()
  │     ├─ Saved credentials? → Connect directly
  │     └─ No credentials? → Open config AP (3 min timeout → restart)
  │
  ├─ esp_now_init()
  │
  ├─ Register ESP-NOW callbacks
  │
  ├─ Add peers (channel 0 = current WiFi channel)
  │
  ├─ Configure GPIO pins
  │
  ├─ ElegantOTA.begin()
  │
  └─ webServer.begin()  →  [Main Loop]
```

---

## 9. Error Handling & Recovery

| Condition                  | Behaviour                                     |
|----------------------------|-----------------------------------------------|
| WiFi config portal timeout | `ESP.restart()` — opens portal again          |
| ESP-NOW init failure       | `ESP.restart()`                               |
| Peer add failure           | Log warning, continue (ESP-NOW still receives)|
| Honda: no running feedback | One crank attempt; slave reports `Running=false` to master |
| Master: no slave response  | Status shows "Never" in Clients tab; control still sends |

---

## 10. Library Dependencies

Install all via Arduino IDE **Library Manager** (Tools → Manage Libraries):

| Library            | Author              | Purpose                        |
|--------------------|---------------------|--------------------------------|
| WiFiManager        | tzapu               | Captive portal WiFi setup      |
| ESPAsyncWebServer  | lacamera            | Async HTTP server              |
| AsyncTCP           | dvarrel             | Dependency of ESPAsyncWebServer|
| ElegantOTA         | Ayush Sharma (v3.x) | Web-based OTA update page      |

Board: **esp32** by Espressif Systems (install via Boards Manager).

---

## 11. Known Limitations

- ESP-NOW channel must match the WiFi router channel. If the router changes channel (rare), restart all units.
- During OTA upload (~30 s), ESP-NOW messages are not processed.
- `SlaveHonda` blocking start sequence (ignition warm-up + crank = ~14 s) prevents web server response during that window — this is expected.
- The `HondaStart` field in `MasterMsg` carries the Wallas command to `SlaveWallas` (reuse of existing struct). A future version should use a dedicated Wallas message structure.
