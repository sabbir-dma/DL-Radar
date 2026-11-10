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

float storage_getSolarVoltage() {
    int raw = analogRead(SOLAR_PIN);
    float vout = (raw / 4095.0f) * 3.3f;
    return vout * (110.0f / 10.0f);
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
void storage_logData(const String &sensorName, const String &rawData) {
    if (!sdReady || !rtcReady) return;

    DateTime now;
    { I2CLock lk; now = rtc.now(); }
    char fname[32];
    snprintf(fname, sizeof(fname), "/%04d%02d.csv", now.year(), now.month());

    bool needHeader = !SD.exists(fname);

    File file = SD.open(fname, FILE_APPEND);
    if (!file) {
        Serial.println("[SD] Open for append FAILED!");
        return;
    }

    if (needHeader) {
        file.println("timestamp,device_id,AOS,LOS,csq,fwv,hwv,heap,uptime,solar_voltage,battery_voltage,sensor_name,raw_data");
    }

    String ts = storage_getTimestamp14();
    int csq = storage_getCSQ();
    float solar = storage_getSolarVoltage();
    float batt  = storage_getBatteryVoltage();

    String safe = rawData;
    safe.replace(",", " ");
    safe.replace("\n", " ");
    safe.replace("\r", " ");

    file.printf("%s,%s,%s,%s,%d,%s,%s,%u,%lu,%.3f,%.3f,%s,%s\n",
                ts.c_str(),
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
    Serial.println("[SD] Data logged: " + String(fname));
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
    sendAT("AT+CNTP=\"pool.ntp.org\",24", 5000);
    String resp = sendAT("AT+CNTP", 60000);

    if (resp.indexOf("+CNTP: 0") < 0) {
        Serial.println("[NTP] Synchronization FAILED!");
        return false;
    }

    resp = sendAT("AT+CCLK?");
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

void storage_handlePeriodic() {
    if (!rtcReady) return;

    DateTime now;
    { I2CLock lk; now = rtc.now(); }

    if (now.hour() == 0 && now.minute() == 0 && now.second() == 1) {
        static uint32_t lastSyncDay = 0;

        uint32_t dateKey =
            now.year() * 1000UL +
            now.month() * 32UL +
            now.day();

        if (lastSyncDay != dateKey) {
            lastSyncDay = dateKey;
            storage_updateRTCFromNTP();
        }
    }
}