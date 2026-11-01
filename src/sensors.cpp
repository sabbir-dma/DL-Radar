#include "sensors.h"
#include "modbus.h"
#include "radar.h"
#include "config.h"
#include "timeutil.h"

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
// Per-sensor last-read tracking (for poll_interval_ms)
// ============================================================
static uint32_t lastReadMs[256] = {0};   // indexed by sensor ID (0..255)

static bool shouldPoll(JsonObject sensor) {
    int id = sensor["id"] | -1;
    if (id < 0 || id > 255) return true;

    uint32_t interval = sensor["poll_interval_ms"] | 0;
    if (interval == 0) return true;      // no per-sensor interval → use global

    uint32_t now = millis();
    if (now - lastReadMs[id] < interval) return false;

    lastReadMs[id] = now;
    return true;
}

// ============================================================
// Init — call in setup()
// ============================================================
void initSensors() {
    // Nothing protocol-specific needed right now.
    // Add I2C.begin(), Serial1.begin() for SDI-12, etc. here in the future.
    Serial.println("[SENSORS] Dispatcher initialized");
}

// ============================================================
// Main dispatcher — call periodically
// ============================================================
void readAllSensorsAndPublish() {
    Serial.println("[SENSORS] --- poll cycle ---");
    Serial.flush();

    JsonArray sensors = config["sensors"];

    for (JsonObject sensor : sensors) {
        if (!(sensor["enabled"] | sensor["sensor_enable"] | true)) continue;
        if (!shouldPoll(sensor)) continue;

        const char *typeRaw = sensor["type"] | sensor["sensor_type"] | "modbus";
        // normalize to lowercase for comparison
        static char typeBuf[32];
        strncpy(typeBuf, typeRaw, sizeof(typeBuf)-1);
        typeBuf[sizeof(typeBuf)-1] = 0;
        for (int i = 0; typeBuf[i]; i++) typeBuf[i] = tolower((unsigned char)typeBuf[i]);
        const char *type = typeBuf;
        const char *name = sensor["name"] | "Unknown";
        int id           = sensor["id"]   | 0;
        bool sdLog       = sensor["sd_card_logging"] | sensor["sd_logging"] | false;

        Serial.printf("[SENSORS] Reading: %s (id=%d, type=%s, sd=%s)\n",
                      name, id, type, sdLog ? "yes" : "no");
        Serial.flush();

        // ---- Build JSON header ----
        static char jsonBuf[1024];
        int len = snprintf(jsonBuf, sizeof(jsonBuf),
            "{\"ts\":\"%s\",\"aos\":%s,"
            "\"device_id\":\"%s\","
            "\"type\":\"data\","
            "\"source\":\"%s\","
            "\"sensor_id\":%d,"
            "\"sensor_name\":\"%s\","
            "\"data\":[",
            getTimestamp14().c_str(),
            getAOS() ? "true" : "false",
            DEVICE_ID.c_str(),
            type,
            id,
            name);

        if (len < 0 || len >= (int)sizeof(jsonBuf)) {
            Serial.println("[SENSORS] JSON header truncated!");
            continue;
        }

        ChannelWriter w = {
            .buf = jsonBuf,
            .bufSize = sizeof(jsonBuf),
            .len = len,
            .first = true,
            .anyOk = false
        };

        // ---- Dispatch ----
        bool handled = false;
        if (strcmp(type, "modbus") == 0) {
            readModbusSensor(sensor, w);
            handled = true;
        } else if (strcmp(type, "sdi12") == 0 || strcmp(type, "sdi-12") == 0 || strcmp(type, "radar") == 0) {
            readSDI12Sensor(sensor, w);
            handled = true;
        }
        // future:
        // else if (strcmp(type, "sdi12")  == 0) { readSDI12Sensor(sensor, w); handled = true; }
        // else if (strcmp(type, "analog") == 0) { readAnalogSensor(sensor, w); handled = true; }
        // else if (strcmp(type, "i2c")    == 0) { readI2CSensor(sensor, w);    handled = true; }

        if (!handled) {
            Serial.printf("[SENSORS] Unknown type: %s\n", type);
            continue;
        }

        // ---- Close JSON ----
        int tail = snprintf(jsonBuf + w.len, sizeof(jsonBuf) - w.len, "]}");
        if (tail < 0 || w.len + tail >= (int)sizeof(jsonBuf)) {
            Serial.println("[SENSORS] JSON tail truncated!");
            continue;
        }
        w.len += tail;

        Serial.println("--------------------------------------");
        Serial.flush();

        // ---- Publish (even all-error payloads go out) ----
        if (mqttPublishQueue == NULL) {
            Serial.println("[SENSORS] mqttPublishQueue is NULL");
            continue;
        }

        MQTTMessage msg = {};
        strncpy(msg.topic, MQTT_PUB.c_str(), sizeof(msg.topic) - 1);
        strncpy(msg.payload, jsonBuf, sizeof(msg.payload) - 1);
        msg.retries = 0;
        msg.sd_log  = sdLog;    // if you added this field earlier

        if (xQueueSend(mqttPublishQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.printf("[SENSORS] %s queued (%u bytes)\n",
                          name, (unsigned)strlen(jsonBuf));
        } else {
            Serial.println("[SENSORS] MQTT queue full");
        }
        Serial.flush();
    }

    Serial.println("[SENSORS] --- cycle done ---");
    Serial.flush();
}

// ============================================================
// SDI-12 (radar) reader — read & publish raw data
// ============================================================
bool readSDI12Sensor(JsonObject sensor, ChannelWriter &w) {
    const char *addr = sensor["sdi12"]["address"] | "0";
    if (addr && addr[0]) SENSOR_ADDR = addr[0];

    String rawData;
    bool ok = measureM3(rawData);

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
    w.anyOk = true;

    return true;
}

