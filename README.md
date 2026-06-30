# RemoteStart — Honda EU70IS & Wallas Heater Remote Start

ESP32-based wireless remote start system for a Honda EU70IS generator and Wallas diesel heater, controlled via a Victron Venus relay or manual switches.

## Units

| Unit         | MCU   | Purpose               | Web UI  |
|--------------|-------|-----------------------|---------|
| MasterHonda  | ESP32 | Master controller     | 3 tabs  |
| SlaveHonda   | ESP32 | Honda generator slave | 2 tabs  |
| SlaveWallas  | ESP32 | Wallas heater slave   | 2 tabs  |

## Features

- **ESP-NOW** peer-to-peer communication (no router required for control)
- **WiFiManager** captive portal for first-startup WiFi configuration
- **Web UI** on each unit — pin status, client status, OTA update tab
- **ElegantOTA** — firmware updates via browser, no USB cable needed
- **Individual versioning** — each unit has its own `FIRMWARE_VERSION`

## First Startup

1. Power on the unit
2. Connect to its config AP (e.g. `MasterHonda-Config`, password `honda1234`)
3. Navigate to `192.168.4.1`, enter your WiFi credentials
4. Unit reboots, connects to your network

All three units must connect to the **same** WiFi network to share an ESP-NOW channel.

## Required Libraries

Install via Arduino IDE Library Manager:

- **WiFiManager** by tzapu
- **ESPAsyncWebServer** by lacamera
- **AsyncTCP** by dvarrel
- **ElegantOTA** by Ayush Sharma (v3.x)

Board: **esp32** by Espressif Systems

## OTA Update

1. Compile in Arduino IDE → *Sketch → Export Compiled Binary*
2. Open `http://<device-ip>/` → OTA Update tab
3. Upload the `.bin` file

## Documentation

See [FSD_RemoteStart.md](FSD_RemoteStart.md) for the full Functional Specification Document.

## Author

Stein Espe
