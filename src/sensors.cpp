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
// Readings, schedule, SD log, publish
// ------------------------------------------------------------
// Publish "slots" are the clock times where minute % interval == 0 (interval 5/10/15/20/30 min).
// For every slot S:
//   * reading starts LEAD seconds before S (3 min, longer if the sensor needs longer)
//   * up to 2 attempts per sensor
//   * result is logged to SD and published with the timestamp of S (exactly at S, or right
//     away if the reading finished after S)
//   * failed slot: previous good reading is re-used ONCE; if the previous slot also failed,
//     the real error text is logged / published instead
// ============================================================
#define MAX_SLOTS   6
#define SLOT_CH     700

struct SensorSlot {
    bool  used;
    int   id;
    char  name[32];
    char  type[16];
    // newest slot result (what SD got / what is published)
    bool  valid;
    bool  stale;             // copy of the previous reading (read failed this slot)
    char  ts[16];            // YYMMDDHHMMSS (slot time)
    char  channels[SLOT_CH];
    char  raw[256];
    float batt, solar;
    // last GOOD reading
    bool  haveGood;
    bool  fromGood;          // `channels/raw` above are (a copy of) the last good reading
    uint8_t gPubCount;       // how many times the last good reading was published (max 2)
    char  gChannels[SLOT_CH];
    char  gRaw[256];
    float gBatt, gSolar;
};
static SensorSlot slots[MAX_SLOTS];

// result of one read pass (all sensors)
struct ReadRes {
    bool  used, ok;
    char  channels[SLOT_CH];
    char  raw[256];
    float batt, solar;
};
static ReadRes results[MAX_SLOTS];

static SemaphoreHandle_t slotMutex = NULL;
TaskHandle_t sensorTaskHandle = NULL;
extern unsigned long DATA_INTERVAL;           // ms, defined in main.cpp

void initSensors() {
    slotMutex = xSemaphoreCreateMutex();
    Serial.println("[SENSORS] Dispatcher initialized");
}

// Time base: RTC epoch if valid, else uptime
uint32_t sensors_scheduleNow() {
    uint32_t e = storage_getEpoch();
    return e ? e : (uint32_t)(millis() / 1000);
}

static void lowerCopy(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i < n - 1; i++) dst[i] = tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static void copyStr(char *dst, size_t n, const char *src) {
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

// ---- read every enabled sensor (max 2 attempts each) into results[] ----
static void readAllSensors() {
    JsonArray sensors = config["sensors"];
    static char chBuf[SLOT_CH];
    int idx = 0;
    memset(results, 0, sizeof(results));

    for (JsonObject sensor : sensors) {
        if (idx >= MAX_SLOTS) break;
        int si = idx++;
        if (!(sensor["enabled"] | sensor["sensor_enable"] | true)) continue;

        char type[16];
        lowerCopy(type, sizeof(type), sensor["type"] | sensor["sensor_type"] | "modbus");
        const char *name = sensor["name"] | "Unknown";
        int id = sensor["id"] | 0;
        bool isSdi = (strcmp(type, "sdi12") == 0 || strcmp(type, "sdi-12") == 0 || strcmp(type, "radar") == 0);
        bool isMod = (strcmp(type, "modbus") == 0);
        if (!isSdi && !isMod) { Serial.printf("[SENSORS] Unknown type: %s\n", type); continue; }

        // fixed info
        xSemaphoreTake(slotMutex, portMAX_DELAY);
        SensorSlot &sl = slots[si];
        sl.used = true;
        sl.id = id;
        copyStr(sl.name, sizeof(sl.name), name);
        copyStr(sl.type, sizeof(sl.type), type);
        xSemaphoreGive(slotMutex);

        ReadRes &r = results[si];
        r.used = true;
        for (int attempt = 1; attempt <= 2 && !r.ok; attempt++) {
            Serial.printf("[SENSORS] %s attempt %d/2\n", name, attempt);
            chBuf[0] = 0;
            ChannelWriter w = { .buf = chBuf, .bufSize = sizeof(chBuf), .len = 0,
                                .first = true, .anyOk = false, .raw = {0} };
            r.ok = isSdi ? readSDI12Sensor(sensor, w) : readModbusSensor(sensor, w);
            copyStr(r.channels, sizeof(r.channels), chBuf);
            copyStr(r.raw, sizeof(r.raw), w.raw);
            if (!r.ok) Serial.printf("[SENSORS] %s attempt %d failed: %s\n", name, attempt, r.raw);
        }
        r.batt = storage_getBatteryVoltage();
        r.solar = storage_getSolarVoltage();
    }
}

// ---- publish the stored newest result of every sensor (no sensor access) ----
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
        if (!sl.used || !sl.valid) continue;

        MQTTMessage msg = {};
        strncpy(msg.topic, MQTT_PUB.c_str(), sizeof(msg.topic) - 1);

        bool sdi = (strcmp(sl.type, "sdi12") == 0 || strcmp(sl.type, "sdi-12") == 0 || strcmp(sl.type, "radar") == 0);

        if (dataFormat == FMT_LEGACY && sdi) {
            snprintf(msg.payload, sizeof(msg.payload), "%s,%s+%.2f+%.2f+%s",
                     DEVICE_ID.c_str(), sl.raw, sl.batt, sl.solar, sl.ts);
        } else {
            snprintf(msg.payload, sizeof(msg.payload),
                "{\"ts\":\"%s\",\"aos\":%s,\"device_id\":\"%s\",\"type\":\"data\","
                "\"source\":\"%s\",\"sensor_id\":%d,\"sensor_name\":\"%s\"%s,\"data\":[%s]}",
                sl.ts, getAOS() ? "true" : "false", DEVICE_ID.c_str(),
                sl.type, sl.id, sl.name,
                (showStale && sl.stale) ? ",\"stale\":true" : "",
                sl.channels);
        }
        msg.retries = 0;

        if (xQueueSend(mqttPublishQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.printf("[PUBLISH] %s queued at %s, data time %s%s\n", sl.name, nowPretty().c_str(),
                          prettyTs(sl.ts).c_str(), sl.stale ? " (previous reading)" : "");
            if (sl.fromGood) {                       // count publications of the good reading
                xSemaphoreTake(slotMutex, portMAX_DELAY);
                if (slots[i].gPubCount < 255) slots[i].gPubCount++;
                xSemaphoreGive(slotMutex);
            }
        } else
            Serial.println("[PUBLISH] MQTT queue full");
    }
    sendLedCommand(LED_RF_HB);
}

// ---- one schedule slot: read -> wait for S -> decide -> SD log -> publish ----
static void runSlot(uint32_t S) {
    bool haveRtc = storage_getEpoch() != 0;
    String slotPretty = haveRtc ? prettyTs(storage_tsFromEpoch(S)) : String("(no RTC)");
    Serial.printf("[SLOT] reading START at %s  -> data time (slot) = %s\n", nowPretty().c_str(), slotPretty.c_str());

    readAllSensors();
    Serial.printf("[SLOT] reading END   at %s\n", nowPretty().c_str());

    // Result ready before S: hold it until the exact slot time
    uint32_t t0 = millis();
    while (sensors_scheduleNow() < S && millis() - t0 < 600000UL) vTaskDelay(pdMS_TO_TICKS(200));

    String ts = haveRtc ? storage_tsFromEpoch(S) : getTimestamp14();
    Serial.printf("[SLOT] slot time reached/passed: now %s, logging + publishing as %s\n",
                  nowPretty().c_str(), prettyTs(ts).c_str());

    for (int i = 0; i < MAX_SLOTS; i++) {
        ReadRes &r = results[i];
        if (!r.used) continue;

        xSemaphoreTake(slotMutex, portMAX_DELAY);
        SensorSlot &sl = slots[i];
        copyStr(sl.ts, sizeof(sl.ts), ts.c_str());

        if (r.ok) {
            // fresh data
            copyStr(sl.channels, sizeof(sl.channels), r.channels);
            copyStr(sl.raw, sizeof(sl.raw), r.raw);
            sl.batt = r.batt; sl.solar = r.solar;
            sl.stale = false;
            copyStr(sl.gChannels, sizeof(sl.gChannels), r.channels);
            copyStr(sl.gRaw, sizeof(sl.gRaw), r.raw);
            sl.gBatt = r.batt; sl.gSolar = r.solar;
            sl.haveGood = true;
            sl.fromGood = true;
            sl.gPubCount = 0;
        } else if (sl.haveGood && sl.gPubCount < 2) {
            // failed slot: repeat the last good reading (a reading is published at most 2 times)
            copyStr(sl.channels, sizeof(sl.channels), sl.gChannels);
            copyStr(sl.raw, sizeof(sl.raw), sl.gRaw);
            sl.batt = r.batt; sl.solar = r.solar;
            sl.stale = true;
            sl.fromGood = true;
            Serial.printf("[SLOT] %s failed -> previous reading used (%s)\n", sl.name, r.raw);
        } else {
            // that reading was already published twice (or none exists): report the real reason
            copyStr(sl.channels, sizeof(sl.channels), r.channels);
            copyStr(sl.raw, sizeof(sl.raw), r.raw);
            sl.batt = r.batt; sl.solar = r.solar;
            sl.stale = false;
            sl.fromGood = false;
            Serial.printf("[SLOT] %s failed again -> error reported: %s\n", sl.name, r.raw);
        }
        sl.valid = true;
        String name = sl.name, raw = sl.raw;
        bool isSdi = (strcmp(sl.type, "sdi12") == 0 || strcmp(sl.type, "sdi-12") == 0 || strcmp(sl.type, "radar") == 0);
        float vb = sl.batt, vs = sl.solar;
        xSemaphoreGive(slotMutex);

        // SD logging: always, online or not, stamped with the slot time
        storage_logDataAt(name, raw, ts);

        // OLED shows exactly what was logged / is published (same reading, same battery/solar)
        if (isSdi) { radarShowReading(raw.c_str()); updateVoltages(vb, vs); }
    }

    // Publish: now if online, otherwise wait up to 90 s for the connection, else skip
    uint32_t w0 = millis();
    while (!deviceOnline && millis() - w0 < 90000UL) vTaskDelay(pdMS_TO_TICKS(500));
    sensors_publishLatest();
    Serial.printf("[SLOT] done, slot=%u\n", (unsigned)S);
}

// ---- read now (boot, MQTT data_read): updates "latest" with the current time, no SD log ----
static void readNow() {
    readAllSensors();
    String ts = getTimestamp14();
    for (int i = 0; i < MAX_SLOTS; i++) {
        ReadRes &r = results[i];
        if (!r.used) continue;
        xSemaphoreTake(slotMutex, portMAX_DELAY);
        SensorSlot &sl = slots[i];
        copyStr(sl.ts, sizeof(sl.ts), ts.c_str());
        copyStr(sl.channels, sizeof(sl.channels), r.channels);
        copyStr(sl.raw, sizeof(sl.raw), r.raw);
        sl.batt = r.batt; sl.solar = r.solar;
        sl.stale = false;
        sl.valid = true;
        sl.fromGood = r.ok;
        if (r.ok) {
            copyStr(sl.gChannels, sizeof(sl.gChannels), r.channels);
            copyStr(sl.gRaw, sizeof(sl.gRaw), r.raw);
            sl.gBatt = r.batt; sl.gSolar = r.solar;
            sl.haveGood = true;
            sl.gPubCount = 0;
        }
        bool isSdi = (strcmp(sl.type, "sdi12") == 0 || strcmp(sl.type, "sdi-12") == 0 || strcmp(sl.type, "radar") == 0);
        String raw = sl.raw;
        float vb = sl.batt, vs = sl.solar;
        xSemaphoreGive(slotMutex);
        if (isSdi) { radarShowReading(raw.c_str()); updateVoltages(vb, vs); }
    }
}

// ---- acquisition task ----
static volatile bool g_publishAfterRead = false;

void sensors_requestReadAndPublish() {
    g_publishAfterRead = true;
    if (sensorTaskHandle) xTaskNotifyGive(sensorTaskHandle);
}

static void sensorTask(void *) {
    uint32_t slotDone = 0;          // last slot that was processed
    bool bootRead = true;

    for (;;) {
        bool force = ulTaskNotifyTake(pdTRUE, 0) > 0;

        uint32_t iv = DATA_INTERVAL / 1000;
        if (iv < 60) iv = 300;
        uint32_t now = sensors_scheduleNow();

        // lead = readLeadSec (default 3 min, MQTT set_read_lead), or the last real measurement
        // time + 20 s if that is longer
        uint32_t lead = readLeadSec;
        uint32_t lastMs = radarLastMeasureMs();
        if (lastMs / 1000 + 20 > lead) lead = lastMs / 1000 + 20;

        // next slot to process: the one after the last done, otherwise the next clock boundary
        uint32_t S = slotDone ? slotDone + iv : (now / iv + 1) * iv;
        if (S % iv != 0 || S + 2 * iv < now) S = (now / iv + 1) * iv;   // interval changed / far behind

        if (force) {                                 // MQTT data_read
            readNow();
            if (g_publishAfterRead) { g_publishAfterRead = false; sensors_publishLatest(); }
        } else if (now + lead >= S) {                // time to start this slot's reading
            bootRead = false;
            runSlot(S);
            slotDone = S;
        } else if (bootRead) {                       // after boot: fill "latest" (display, MQTT data)
            bootRead = false;
            readNow();
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

