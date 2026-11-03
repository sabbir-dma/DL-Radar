# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 (esp32dev, Arduino framework, PlatformIO) firmware for the "DL301-26" radar data logger (BWDB). It reads an SDI-12 radar sensor and Modbus sensors, logs to SD, and publishes over MQTT via an A7670 GSM modem (TinyGSM). A WiFi SoftAP config portal serves a web UI from LittleFS. No tests exist (`test/` is empty) and there is no lint setup.

## Commands

- Build: `pio run`
- Upload firmware: `pio run -t upload`
- Upload web UI / config.json (LittleFS from `data/`): `pio run -t uploadfs` — required after editing anything in `data/`
- Serial monitor (115200): `pio device monitor`

## Architecture

- **Entry / orchestration** ([src/main.cpp](src/main.cpp)): `setup()` loads identity from NVS `Preferences`, creates `mqttPublishQueue`, inits peripherals (LED, display, GSM, radar, storage, sensors), mounts LittleFS, loads `/config.json`, then starts `MainTask` (core 1). `MainTask` only schedules heartbeat and data-publish intervals (`HEARTBEAT_INTERVAL`, `DATA_INTERVAL`, stored in NVS keys `hb_inv`/`dt_inv`). Arduino `loop()` just runs `storage_handlePeriodic()` and, if the SoftAP is up, `server.handleClient()`.
- **FreeRTOS tasks** are spread across modules: `led`, `gsm` (modem/MQTT connection, drains `mqttPublishQueue`), `display` (SSD1306), `radar` (SDI-12, request/result queues in [include/radar.h](include/radar.h)), `main`. Modules follow the `include/X.h` + `src/X.cpp` pairing; shared globals/flags (`deviceOnline`, `mqttConnected`, `otaInProgress`, ...) are `extern`'d in [include/config.h](include/config.h).
- **Publishing path**: sensors → `MQTTMessage` (fixed-size topic/payload, `sd_log` flag) → `mqttPublishQueue` → GSM task → MQTT; `sd_log` controls SD logging ([src/storage.cpp](src/storage.cpp)).
- **Runtime config**: `data/config.json` (LittleFS) defines device/network/sensors (per-sensor Modbus params, poll interval, SD logging) and is loaded into a global `config` ArduinoJson doc by `loadConfig()` ([src/config.cpp](src/config.cpp)). Modbus polling lives in [src/modbus.cpp](src/modbus.cpp).
- **Config portal** ([include/config_portal.h](include/config_portal.h), header-only): `dev_config()` starts SoftAP, calls `setupWebServer()` and `suspendAllTasks()` (so normal operation halts while configuring). It serves `data/index.html`, `script.js`, `style.css` and a REST API (`/api/config` GET/POST/reset, `/api/files`, `/api/data`, `/api/sd/{files,download,delete,status}`). Frontend and these endpoints must stay in sync.

## Identity / build modes (include/config.h)

- Device ID = `WORK_PACKAGE + DEVICE_TYPE + DEVICE_CODE_UPLOAD_DATE + DEVICE_SERIAL_ID`. **Edit `DEVICE_SERIAL_ID` (and version defines) per unit before flashing.**
- `OTA 0` (normal): setup writes `UNIQUE_DEVICE_ID` into NVS if it differs. `OTA 1`: NVS is read-only and the stored ID is kept, falling back to the MAC-derived ID. Set `OTA 1` for OTA-built images.
- Bump `FW_VERSION`/`OTA_DATE` for releases.
- MQTT topics are built at runtime from `DEVICE_ID` (`setupMQTTTopics()`).

## Other

- `ExampleCode/*.txt` are reference snippets (SD/RTC/INA219, NTP→RTC), not compiled.
- Pins: modem TX 17 / RX 16 / PWR 15, AC-line sense 34.
