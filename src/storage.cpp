#include "storage.h"
#include "config.h"
#include "gsm.h"
#include "timeutil.h"

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_INA219.h>

#define I2C_SDA 21
#define I2C_SCL 22
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23
#define SD_CS     5
#define SOLAR_PIN 34
#define SOLAR_MIN_V 3.0f            // solar voltage below this is shown as 0
#define SOLAR_DIVIDER_RATIO 11.0f   // (100k + 10k) / 10k  - check against your hardware

static RTC_DS3231 rtc;
static Adafruit_INA219 ina219;
static SPIClass sdSPI(VSPI);

static bool rtcReady = false;
static bool inaReady = false;
static bool sdReady  = false;

static unsigned long lastNtpSyncDay = 0;

// RTC and INA219 share one I2C bus and are used from several tasks -> serialise access
static SemaphoreHandle_t i2cMutex = NULL;
struct I2CLock {
    I2CLock()  { if (!i2cMutex) i2cMutex = xSemaphoreCreateRecursiveMutex();
                 xSemaphoreTakeRecursive(i2cMutex, portMAX_DELAY); }
    ~I2CLock() { xSemaphoreGiveRecursive(i2cMutex); }
};

void storage_setup() {
    if (!i2cMutex) i2cMutex = xSemaphoreCreateRecursiveMutex();
    Serial.println("[STORAGE] Initializing...");
    Wire.begin(I2C_SDA, I2C_SCL);

    if (!rtc.begin()) {
        Serial.println("[RTC] DS3231 NOT FOUND!");
    } else {
        rtcReady = true;
        Serial.println("[RTC] DS3231 detected.");
        if (rtc.lostPower()) Serial.println("[RTC] Time lost! (NTP sync will fix)");
    }

    if (!ina219.begin()) {
        Serial.println("[INA219] NOT FOUND!");
    } else {
        inaReady = true;
        ina219.setCalibration_32V_2A();
        Serial.println("[INA219] Detected!");
    }

    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, 20000000) &&
        !SD.begin(SD_CS, sdSPI, 10000000)) {
        Serial.println("[SD] Initialization FAILED!");
    } else {
        sdReady = true;
        Serial.println("[SD] Initialization SUCCESS!");
    }

    pinMode(SOLAR_PIN, INPUT);
    analogSetPinAttenuation(SOLAR_PIN, ADC_11db);   // 0..~3.1 V at the pin

    Serial.printf("[STORAGE] RTC=%s INA=%s SD=%s\n",
                  rtcReady ? "OK" : "FAIL",
                  inaReady ? "OK" : "FAIL",
                  sdReady  ? "OK" : "FAIL");
}

bool storage_sd_ready()  { return sdReady; }
bool storage_rtc_ready() { return rtcReady; }
bool storage_ina_ready() { return inaReady; }

uint32_t storage_getEpoch() {
    if (!rtcReady) return 0;
    I2CLock lk;
    DateTime now = rtc.now();
    if (now.year() < 2024) return 0;      // RTC not set yet
    return now.unixtime();
}

String storage_tsFromEpoch(uint32_t e) {
    DateTime d(e);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d%02d%02d%02d%02d%02d",
             d.year() % 100, d.month(), d.day(), d.hour(), d.minute(), d.second());
    return String(buf);
}

String storage_getTimestamp14() {
    if (!rtcReady) return String("000000000000");
    I2CLock lk;
    DateTime now = rtc.now();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d%02d%02d%02d%02d%02d",
             now.year() % 100, now.month(), now.day(),
             now.hour(), now.minute(), now.second());
    return String(buf);
}

// GPIO34 = ADC1_CH6 (input only, fine with WiFi on). analogReadMilliVolts() applies the
// chip's factory calibration (raw/4095*3.3 is wrong: the ADC is non-linear and full scale at
// 11 dB is ~3.9 V). Average 16 samples to remove noise.
float storage_getSolarVoltage() {
    uint32_t mv = 0;
    for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(SOLAR_PIN);
    float v = (mv / 16.0f) / 1000.0f * SOLAR_DIVIDER_RATIO;
    // Below 3 V the reading is only ADC offset / noise (the ESP32 ADC reports ~0.1-0.15 V at 0 V,
    // x11 divider = ~1.5 V): show 0.
    if (v < SOLAR_MIN_V) return 0.0f;
    return v;
}

float storage_getBatteryVoltage() {
    if (!inaReady) return 0.0f;
    I2CLock lk;
    return ina219.getBusVoltage_V();
}

int storage_getCSQ() {
    int q = modem.getSignalQuality();
    if (q < 0) return 0;
    return q;
}
// Log one row stamped with ts14 (YYMMDDHHMMSS) - the row goes into that timestamp's month file
void storage_logDataAt(const String &sensorName, const String &rawData, const String &ts14) {
    if (!sdReady) return;
    if (ts14.length() < 12) return;

    char fname[32];
    snprintf(fname, sizeof(fname), "/20%.2s%.2s.csv", ts14.c_str(), ts14.c_str() + 2);

    bool needHeader = !SD.exists(fname);

    File file = SD.open(fname, FILE_APPEND);
    if (!file) {
        Serial.println("[SD] Open for append FAILED!");
        return;
    }

    if (needHeader) {
        file.println("timestamp,device_id,AOS,LOS,csq,fwv,hwv,heap,uptime,solar_voltage,battery_voltage,sensor_name,raw_data");
    }

    int csq = storage_getCSQ();
    float solar = storage_getSolarVoltage();
    float batt  = storage_getBatteryVoltage();

    String safe = rawData;
    safe.replace(",", " ");
    safe.replace("\n", " ");
    safe.replace("\r", " ");

    file.printf("%s,%s,%s,%s,%d,%s,%s,%u,%lu,%.3f,%.3f,%s,%s\n",
                ts14.c_str(),
                DEVICE_ID.c_str(),
                getAOS() ? "true" : "false",
                sdReady ? "true" : "false",
                csq,
                FW_VERSION,
                HW_VERSION,
                (unsigned)ESP.getFreeHeap(),
                (unsigned long)(millis() / 1000),
                solar,
                batt,
                sensorName.c_str(),
                safe.c_str());

    file.close();
    Serial.println("[SD] Data logged: " + String(fname) + "  ts=" + ts14);
}

void storage_logData(const String &sensorName, const String &rawData) {
    storage_logDataAt(sensorName, rawData, storage_getTimestamp14());
}

static String sendAT(const String &command, uint32_t timeout = 5000) {
    while (SerialAT.available()) SerialAT.read();
    SerialMon.println("[AT] " + command);
    SerialAT.println(command);

    String response;
    uint32_t start = millis();
    while (millis() - start < timeout) {
        while (SerialAT.available()) response += (char)SerialAT.read();
        if (response.indexOf("\r\nOK\r\n") >= 0 ||
            response.indexOf("\r\nERROR\r\n") >= 0) break;
        delay(10);
    }
    return response;
}

bool storage_updateRTCFromNTP() {
    if (!rtcReady) {
        Serial.println("[RTC] RTC not initialized!");
        return false;
    }

    Serial.println("[RTC NTP] Starting sync...");
    String cfg = sendAT("AT+CNTP=\"pool.ntp.org\",24", 5000);
    if (cfg.indexOf("OK") < 0) {
        Serial.println("[NTP] Configuration FAILED!");
        return false;
    }

    // "AT+CNTP" answers OK at once; the real result "+CNTP: <code>" arrives later,
    // so keep reading until it shows up (0 = success).
    while (SerialAT.available()) SerialAT.read();
    SerialAT.println("AT+CNTP");
    Serial.println("[NTP] Requesting time...");

    String result;
    bool ntpOk = false;
    uint32_t t0 = millis();
    while (millis() - t0 < 40000) {
        while (SerialAT.available()) result += (char)SerialAT.read();
        int p = result.indexOf("+CNTP:");
        if (p >= 0 && result.indexOf('\n', p) >= 0) {          // full result line received
            ntpOk = (result.indexOf("+CNTP: 0", p) >= 0);
            break;
        }
        if (result.indexOf("ERROR") >= 0) break;
        delay(20);
    }
    if (!ntpOk) {
        Serial.printf("[NTP] Synchronization FAILED! (%s)\n", result.c_str());
        return false;
    }
    Serial.println("[NTP] Synchronization successful.");

    String resp = sendAT("AT+CCLK?");
    char *pos = strstr(resp.c_str(), "+CCLK: \"");
    if (!pos) {
        Serial.println("[NTP] Cannot read modem clock!");
        return false;
    }
    pos += strlen("+CCLK: \"");
    int year, month, day, hour, minute, second;
    int parsed = sscanf(pos, "%2d/%2d/%2d,%2d:%2d:%2d",
                        &year, &month, &day, &hour, &minute, &second);
    if (parsed != 6) {
        Serial.println("[NTP] Invalid time format!");
        return false;
    }
    year += 2000;

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        Serial.println("[NTP] Invalid date/time!");
        return false;
    }

    { I2CLock lk; rtc.adjust(DateTime(year, month, day, hour, minute, second)); }
    Serial.println("[RTC] Updated successfully from NTP.");
    return true;
}

// ---------------- RTC sync scheduling ----------------
static volatile bool ntpPending     = false;
static volatile bool ntpReplyMqtt   = false;
static uint32_t      lastSyncDayKey = 0;      // yyyymmdd of the last successful sync
static uint32_t      ntpNextTryMs   = 0;

static void mqttReply(const char *text) {
    if (!mqttPublishQueue) return;
    MQTTMessage r = {};
    snprintf(r.topic, sizeof(r.topic), "%s", MQTT_PUB.c_str());
    snprintf(r.payload, sizeof(r.payload), "%s", text);
    xQueueSend(mqttPublishQueue, &r, pdMS_TO_TICKS(100));
}

void storage_requestNtpSync(bool replyOverMqtt) {
    if (replyOverMqtt) ntpReplyMqtt = true;
    ntpPending = true;
    ntpNextTryMs = 0;
}

bool storage_ntpPending() { return ntpPending; }

// Runs in the NetworkTask (modem owner) while online
void storage_serviceNtp() {
    if (!ntpPending || millis() < ntpNextTryMs) return;

    bool ok = storage_updateRTCFromNTP();
    char buf[96];
    if (ok) {
        ntpPending = false;
        DateTime n; { I2CLock lk; n = rtc.now(); }
        lastSyncDayKey = n.year() * 10000UL + n.month() * 100UL + n.day();
        snprintf(buf, sizeof(buf), "['RTC synced from NTP: %s']", storage_getTimestamp14().c_str());
    } else {
        ntpNextTryMs = millis() + 600000UL;       // retry in 10 min
        snprintf(buf, sizeof(buf), "['RTC NTP sync failed, will retry']");
    }
    if (ntpReplyMqtt) { ntpReplyMqtt = false; mqttReply(buf); }
}

bool storage_setRtc(const char *ts12) {
    if (!rtcReady || !ts12 || strlen(ts12) != 12) return false;
    for (int i = 0; i < 12; i++) if (ts12[i] < '0' || ts12[i] > '9') return false;
    auto two = [&](int i) { return (ts12[i] - '0') * 10 + (ts12[i + 1] - '0'); };
    int yy = two(0), mo = two(2), dd = two(4), hh = two(6), mi = two(8), ss = two(10);
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 59) return false;
    { I2CLock lk; rtc.adjust(DateTime(2000 + yy, mo, dd, hh, mi, ss)); }
    lastSyncDayKey = 0;           // manual set: still do the automatic NTP sync when possible
    return true;
}

// Called about once a second from loop(): schedules the daily sync
void storage_handlePeriodic() {
    if (!rtcReady) return;

    DateTime now;
    { I2CLock lk; now = rtc.now(); }
    uint32_t dayKey = now.year() * 10000UL + now.month() * 100UL + now.day();

    // New day (or never synced since boot): ask for a sync, the NetworkTask does it when online
    if (dayKey != lastSyncDayKey && !ntpPending && millis() >= ntpNextTryMs) {
        ntpPending = true;
    }
}
