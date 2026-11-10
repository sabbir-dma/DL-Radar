#include "sensors.h"
#include "modbus.h"
#include "radar.h"
#include "storage.h"
#include "config.h"
static String currentRawData = "";
#include "timeutil.h"
#include "led.h"
#include "display.h"

// ============================================================
// Channel writer helpers
// ============================================================
void appendChannel(ChannelWriter &w,
                   const char *alias,
                   float value,
                   const char *unit) {
    int written = snprintf(w.buf + w.len, w.bufSize - w.len,
        "%s{\"alias\":\"%s\",\"value\":%.3f,\"unit\":\"%s\"}",
        w.first ? "" : ",", alias, value, unit);

    if (written < 0 || w.len + written >= (int)w.bufSize) {
        Serial.println("[WRITER] JSON truncated mid-channel!");
        return;
    }
    w.len += written;
    w.first = false;
    w.anyOk = true;
}

void appendChannelError(ChannelWriter &w,
                        const char *alias,
                        const char *unit) {
    int written = snprintf(w.buf + w.len, w.bufSize - w.len,
        "%s{\"alias\":\"%s\",\"value\":\"error\",\"unit\":\"%s\"}",
        w.first ? "" : ",", alias, unit);

    if (written < 0 || w.len + written >= (int)w.bufSize) {
        Serial.println("[WRITER] JSON truncated mid-error!");
        return;
    }
    w.len += written;
    w.first = false;
}

// ============================================================
// Latest readings
// ------------------------------------------------------------
// Acquisition (reading + SD logging) runs on its own schedule and always keeps
// the newest result per sensor in `slots`.  Publishing just sends what is stored,
// so it happens at the exact interval even if the sensor is slow or failing.
// ============================================================
#define MAX_SLOTS   12
#define SLOT_CH     700

struct SensorSlot {
    bool  used;
    bool  everRead;          // at least one good reading stored
    bool  lastOk;            // newest attempt succeeded (false => published data is stale)
    int   id;
    char  name[32];
    char  type[16];
    char  ts[16];            // YYMMDDHHMMSS of the stored reading
    char  channels[SLOT_CH]; // JSON channel list text
    char  raw[256];
    float batt, solar;
};
static SensorSlot slots[MAX_SLOTS];
static SemaphoreHandle_t slotMutex = NULL;
TaskHandle_t sensorTaskHandle = NULL;
extern unsigned long DATA_INTERVAL;           // ms, defined in main.cpp

void initSensors() {
    slotMutex = xSemaphoreCreateMutex();
    Serial.println("[SENSORS] Dispatcher initialized");
}

// Time base for the schedule: RTC epoch if valid, else uptime
uint32_t sensors_scheduleNow() {
    uint32_t e = storage_getEpoch();
    return e ? e : (uint32_t)(millis() / 1000);
}

static void lowerCopy(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i < n - 1; i++) dst[i] = tolower((unsigned char)src[i]);
    dst[i] = 0;
}

// ---- read every enabled sensor, store the result, log to SD ----
void sensors_acquire() {
    Serial.println("[SENSORS] --- acquire ---");
    if (!slotMutex) return;

    static char chBuf[SLOT_CH];
    JsonArray sensors = config["sensors"];
    int idx = 0;

    for (JsonObject sensor : sensors) {
        if (idx >= MAX_SLOTS) break;
        int si = idx++;
        if (!(sensor["enabled"] | sensor["sensor_enable"] | true)) continue;

        char type[16];
        lowerCopy(type, sizeof(type), sensor["type"] | sensor["sensor_type"] | "modbus");
        const char *name = sensor["name"] | "Unknown";
        int id = sensor["id"] | 0;

        Serial.printf("[SENSORS] Reading: %s (id=%d, type=%s)\n", name, id, type);

        chBuf[0] = 0;
        ChannelWriter w = { .buf = chBuf, .bufSize = sizeof(chBuf), .len = 0,
                            .first = true, .anyOk = false, .raw = {0} };

        bool ok = false;
        if (strcmp(type, "modbus") == 0) {
            ok = readModbusSensor(sensor, w);
        } else if (strcmp(type, "sdi12") == 0 || strcmp(type, "sdi-12") == 0 || strcmp(type, "radar") == 0) {
            ok = readSDI12Sensor(sensor, w);
        } else {
            Serial.printf("[SENSORS] Unknown type: %s\n", type);
            continue;
        }

        String ts = getTimestamp14();
        float batt = storage_getBatteryVoltage();
        float solar = storage_getSolarVoltage();

        xSemaphoreTake(slotMutex, portMAX_DELAY);
        SensorSlot &sl = slots[si];
        sl.used = true;
        sl.id = id;
        strncpy(sl.name, name, sizeof(sl.name) - 1);
        strncpy(sl.type, type, sizeof(sl.type) - 1);
        if (ok || !sl.everRead) {
            // good reading, or nothing good stored yet (keep the error text so something is published)
            strncpy(sl.channels, chBuf, sizeof(sl.channels) - 1);
            strncpy(sl.raw, w.raw, sizeof(sl.raw) - 1);
            strncpy(sl.ts, ts.c_str(), sizeof(sl.ts) - 1);
            sl.batt = batt;
            sl.solar = solar;
            if (ok) sl.everRead = true;
        }
        sl.lastOk = ok;
        xSemaphoreGive(slotMutex);

        if (!ok) Serial.println("[SENSORS] read FAILED -> previous reading stays as latest");

        // SD logging happens here, always - independent of network state
        storage_logData(String(name), String(w.raw));
    }
    Serial.println("[SENSORS] --- acquire done ---");
}

// ---- publish the stored readings (no sensor access) ----
void sensors_publishLatest() {
    if (!deviceOnline || mqttPublishQueue == NULL || !slotMutex) {
        Serial.println("[PUBLISH] offline - skipped (data is on the SD card)");
        return;
    }

    for (int i = 0; i < MAX_SLOTS; i++) {
        static SensorSlot sl;                       // local copy, keep the lock short
        xSemaphoreTake(slotMutex, portMAX_DELAY);
        sl = slots[i];
        xSemaphoreGive(slotMutex);
        if (!sl.used) continue;

        MQTTMessage msg = {};
        strncpy(msg.topic, MQTT_PUB.c_str(), sizeof(msg.topic) - 1);

        bool sdi = (strcmp(sl.type, "sdi12") == 0 || strcmp(sl.type, "sdi-12") == 0 || strcmp(sl.type, "radar") == 0);
        const char *ts = sl.ts[0] ? sl.ts : "000000000000";

        if (dataFormat == FMT_LEGACY && sdi) {
            snprintf(msg.payload, sizeof(msg.payload), "%s,%s+%.2f+%.2f+%s",
                     DEVICE_ID.c_str(), sl.raw, sl.batt, sl.solar, ts);
        } else {
            snprintf(msg.payload, sizeof(msg.payload),
                "{\"ts\":\"%s\",\"aos\":%s,\"device_id\":\"%s\",\"type\":\"data\","
                "\"source\":\"%s\",\"sensor_id\":%d,\"sensor_name\":\"%s\"%s,\"data\":[%s]}",
                ts, getAOS() ? "true" : "false", DEVICE_ID.c_str(),
                sl.type, sl.id, sl.name,
                (showStale && sl.everRead && !sl.lastOk) ? ",\"stale\":true" : "",
                sl.channels);
        }
        msg.retries = 0;

        if (xQueueSend(mqttPublishQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE)
            Serial.printf("[PUBLISH] %s queued%s\n", sl.name, (sl.everRead && !sl.lastOk) ? " (previous reading)" : "");
        else
            Serial.println("[PUBLISH] MQTT queue full");
    }
    sendLedCommand(LED_RF_HB);
}

// Kept for compatibility: read now, then publish
void readAllSensorsAndPublish() {
    sensors_acquire();
    sensors_publishLatest();
}

// ---- acquisition task ----
// Measures so the result is ready before the publish time:
//   publish times are multiples of DATA_INTERVAL (RTC aligned), reading starts LEAD seconds before.
static volatile bool g_publishAfterRead = false;

void sensors_requestReadAndPublish() {
    g_publishAfterRead = true;
    if (sensorTaskHandle) xTaskNotifyGive(sensorTaskHandle);
}

static void sensorTask(void *) {
    uint32_t lastMeasuredSlot = 0;
    bool first = true;                       // one reading right at start so "data" has something

    for (;;) {
        bool force = ulTaskNotifyTake(pdTRUE, 0) > 0;

        uint32_t iv = DATA_INTERVAL / 1000;
        if (iv < 30) iv = 30;
        uint32_t now = sensors_scheduleNow();
        uint32_t nextSlot = (now / iv + 1) * iv;

        // lead = 3.5 min, or the last real measurement time + 30 s if that is longer
        uint32_t lead = 210;
        uint32_t lastMs = radarLastMeasureMs();
        if (lastMs / 1000 + 30 > lead) lead = lastMs / 1000 + 30;

        if (first || force || (nextSlot != lastMeasuredSlot && now + lead >= nextSlot)) {
            first = false;
            lastMeasuredSlot = nextSlot;
            sensors_acquire();
            updateVoltages(storage_getBatteryVoltage(), storage_getSolarVoltage());
            if (g_publishAfterRead) {        // MQTT "data_read"
                g_publishAfterRead = false;
                sensors_publishLatest();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void sensors_start() {
    if (sensorTaskHandle) return;
    xTaskCreatePinnedToCore(sensorTask, "SensorTask", 8192, NULL, 2, &sensorTaskHandle, 1);
}

// ============================================================
// SDI-12 (radar) reader — read & publish raw data
// ============================================================
bool readSDI12Sensor(JsonObject sensor, ChannelWriter &w) {
    // SENSOR_ADDR is learned once at setup (stored in flash); no config override here
    String rawData;
    bool ok = measureM3(rawData);

    strncpy(w.raw, rawData.c_str(), sizeof(w.raw) - 1);
    w.raw[sizeof(w.raw) - 1] = 0;

    // Sanitize raw data for JSON string (replace double quotes)
    String safe = rawData;
    safe.replace("\"", "'");

    if (!ok) {
        int written = snprintf(w.buf + w.len, w.bufSize - w.len,
            "%s{\"alias\":\"radar\",\"value\":\"%s\",\"unit\":\"\"}",
            w.first ? "" : ",", safe.c_str());
        if (written < 0 || w.len + written >= (int)w.bufSize) {
            Serial.println("[WRITER] JSON truncated mid-channel!");
            return false;
        }
        w.len += written;
        w.first = false;
        // not anyOk (error payload) but still publish raw error string
        return false;
    }

    int written = snprintf(w.buf + w.len, w.bufSize - w.len,
        "%s{\"alias\":\"radar\",\"value\":\"%s\",\"unit\":\"\"}",
        w.first ? "" : ",", safe.c_str());
    if (written < 0 || w.len + written >= (int)w.bufSize) {
        Serial.println("[WRITER] JSON truncated mid-channel!");
        return false;
    }
    w.len += written;
    w.first = false;
    if (ok) w.anyOk = true;

    return ok;
}

