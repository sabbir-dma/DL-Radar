# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 (esp32dev, Arduino framework, PlatformIO) firmware for the "DL301-26" data logger for an **SDI-12 radar water-level sensor** (BWDB). Sensor is used in low-power mode (one reading takes ~3 min). The logger reads it on a schedule, logs every reading to an SD card, and publishes to an MQTT broker over a SIM7600 GSM modem (TinyGSM) when online. A WiFi SoftAP config portal (web UI from LittleFS) is used for configuration, data retrieval and firmware upload. No tests exist (`test/` is empty) and there is no lint setup.

## Commands

PlatformIO is not on PATH here: use `~/.platformio/penv/bin/pio`.

- Build: `pio run`
- Upload firmware: `pio run -t upload` (the serial monitor must be closed, it holds the port)
- Upload web UI + `data/config.json` to LittleFS: `pio run -t uploadfs` — needed after editing `data/`; **overwrites the device's config.json**
- Serial monitor (115200): `pio device monitor`

## Tasks (FreeRTOS)

| Task | Core / prio | Job |
|---|---|---|
| NetworkTask ([src/gsm.cpp](src/gsm.cpp)) | 1 / 3 | modem power/init, GPRS, MQTT connect, `mqtt.loop()` (runs `mqttCallback`), drains `mqttPublishQueue` (a failed publish is re-queued up to 2 times) |
| SensorTask ([src/sensors.cpp](src/sensors.cpp)) | 1 / 2 | slot schedule: read, SD log, publish at exact slot times |
| MainTask ([src/main.cpp](src/main.cpp)) | 1 / 2 | heartbeat timer; MQTT `data` request |
| RadarTask ([src/radar.cpp](src/radar.cpp)) | 1 / 2 | runs sensor commands queued by MQTT (`radarCmdQueue`) and publishes the reply |
| DisplayTask ([src/display.cpp](src/display.cpp)) | 0 / 1 | OLED pages, touch wake, 5 s long-press → config mode |
| LedTask ([src/led.cpp](src/led.cpp)) | 0 / 1 | status RGB LED |
| OTATask | 1 / 4 | created on MQTT `ota`; downloads and flashes firmware |
| Arduino `loop()` | 1 / 1 | `storage_handlePeriodic()` (daily NTP→RTC sync) and, in config mode, `server.handleClient()` |

## Data flow (the important part)

1. **Slots.** Publish times are the clock times where `minute % interval == 0` (interval 5/10/15/20/30 min, default 5; `dt_inv` in NVS `device`). `sensors_scheduleNow()` = RTC epoch (uptime if RTC invalid).
2. **Per slot S (SensorTask, `runSlot`).** Reading starts `lead` = `readLeadSec` before S (default 180 s, NVS `device/lead`, MQTT `set_read_lead`/`get_read_lead`, 60-600 s; longer if the last measurement took longer). Each sensor gets up to 2 attempts. If the result is ready early it is held until S. Then, stamped with S's timestamp (even when the reading ended late): SD log (`storage_logDataAt`, online or not) and publish (waits up to 90 s for the device to come online, else skipped; NetworkTask makes 3 send attempts).
3. **Failed slot.** The last good reading is re-used (logged + published with S's timestamp, `stale`) as long as it was published fewer than 2 times (counted per reading, `gPubCount`); after that the real error text (e.g. `RADAR_NO_RESPONSE`, `RADAR_EMPTY`, `RADAR_INVALID`) is logged and published instead.
   After boot one extra read fills "latest" (display, MQTT `data`); it is not logged. MQTT `data_read` also reads now and is not logged. Never read sensors from the network task.
4. **Formats.** `dataFormat` (NVS `device/dfmt`, MQTT `set_data_format`): `json` or `legacy` = `<device_id>,<raw sdi12>+<battery V>+<solar V>+<YYMMDDHHMMSS>` (SDI-12 sensors only). `showStale` (NVS `device/stale`) adds `"stale":true` to JSON when the previous reading is re-published.

## SDI-12 radar ([src/radar.cpp](src/radar.cpp))

- One bus, several callers → every operation holds `BusLock`. A running measurement checks `g_cmdPending` every 100 ms and gives the bus up when a command (MQTT/portal) is waiting, then measures again.
- Sensor address is learned once, stored in NVS (`radar/addr`) and reused. `?!` scan only: at boot if the stored address does not answer, or after 3 failed measurements (3 scans, then one more measure).
- `M3!` → wait `ttt+1` s → `D0!`. A reply needs ≥ 6 `+` values. Battery % on OLED: 2S pack, 6.4 V = 0 % .. 8.4 V = 100 % (`BATT_EMPTY_V`/`BATT_FULL_V` in [include/display.h](include/display.h)). Distance shown on OLED = stage height − `D0` value #`RADAR_DISTANCE_FIELD` ([include/radar.h](include/radar.h)).
- Stage height lives in the sensor (`XHAZ`); read at boot and after set, shown on OLED.
- `suspendAllTasks()` (config mode / OTA) takes the bus first so no task is suspended while holding it.

## Config portal ([include/config_portal.h](include/config_portal.h), header-only) + web UI in [data/](data/)

Started by touch long-press (5 s) or MQTT `wifi_ap` → SoftAP `DL301-26`, `http://192.168.4.1`; normal tasks are suspended meanwhile. **Closes by itself after 10 min** (not during a firmware upload) and resumes the tasks. Endpoints: `/api/config` (GET/POST/reset; POST saves `config.json` and **reboots**), `/api/stage`, `/api/files`, `/api/data` (preview, max 500 rows), `/api/retrieve` + `/api/retrieve/download` (filters the date range into a temp file on SD, then streams it), `/api/sd/*`, `/api/update` (firmware upload, multipart). Frontend and endpoints must stay in sync. `normalizeSensors()` keeps only `sdi12` or only `modbus` block per sensor.

## Storage ([src/storage.cpp](src/storage.cpp))

SD (`/YYYYMM.csv`, comma separated, first column `YYMMDDHHMMSS`), DS3231 RTC, INA219 (battery V), solar V on GPIO34 (ADC1, `analogReadMilliVolts` × `SOLAR_DIVIDER_RATIO` 11, 16-sample average). RTC/INA219 share I2C → guarded by `I2CLock`. RTC is synced from NTP via modem AT commands: requested by `storage_handlePeriodic()` on every new day (and at boot), executed by the NetworkTask when online (retry every 10 min on failure); also MQTT `rtc_sync` / `set_rtc`.

## MQTT commands (subscribe topic `DMA-RADAR/SUB/<device id>`; replies on `DMA-RADAR/PUB`)

`help` (example get/set commands), `get_info` (device summary JSON), `ping`, `restart`, `data` (publish stored latest), `data_read` (read now, publish when done), `get_interval|get_hb_interval|get_pub_interval`, `set_hb_interval:<15-600 s>`, `set_pub_interval:<300|600|900|1200|1800 s>`, `set_data_format:json|legacy`, `get_data_format`, `set_stale:on|off`, `get_stale`, `set_battpct:on|off` / `get_battpct` (battery % text on OLED, NVS `device/battpct`), `get_rtc`, `rtc_sync` (NTP now), `set_rtc:YYMMDDHHMMSS`, `wifi_ap`, `get_sim_info`, `arm:1|0`, `ota [url]`; radar: `setStageHeight:<1-1000>`, `getStageHeight`, `setSamples:<1-1000>`, `getSamples`, `setPowerHigh|setPowerLow|getPowerMode`, `setUnitMeters|setUnitFeets|getUnit`, `factoryReset`. The callback strips spaces; sensor commands only enqueue to `radarCmdQueue`.

## OTA ([src/main.cpp](src/main.cpp) `performOTAUpdate`)

HTTP download over GSM in 32 KB range requests; each piece is read twice and flashed only if both reads are identical (the GSM link corrupts bytes randomly; a single pass fails the image check). MQTT is disconnected first. Device always restarts afterwards. Needs a server that supports `Range`.

## Identity / build ([include/config.h](include/config.h))

Device ID = `WORK_PACKAGE + DEVICE_TYPE + DEVICE_CODE_UPLOAD_DATE + DEVICE_SERIAL_ID`; **edit `DEVICE_SERIAL_ID` per unit before flashing**. `OTA 0` writes `UNIQUE_DEVICE_ID` into NVS; `OTA 1` keeps the stored ID (use for images sent over the air). Bump `FW_VERSION`. Topics are built in [src/config.cpp](src/config.cpp). Default partition table (2 OTA slots ≈1.25 MB; image is ≈1.08 MB).

## Notes

- `ExampleCode/*.txt` are reference snippets, not compiled.
- Pins: modem TX 17 / RX 16 / PWR 15, SDI-12 27, SD CS 5, I2C 21/22, OLED SPI 23/18 (DC 26, CS 25, RST 2), touch 35.
