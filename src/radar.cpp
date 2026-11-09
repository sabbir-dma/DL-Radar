#include "radar.h"
#include "config.h"
#include "display.h"
#include <Arduino.h>
#include <SDI12.h>
#include <math.h>

void radarTask(void *parameter);
// ================= TIME CORE =================


// ================= CONFIG =================
#define SDI12_PIN 27

char SENSOR_ADDR = '0';   // after factory reset = 0

SDI12 sdi12(SDI12_PIN);

TaskHandle_t radarTaskHandle = NULL;

// One SDI-12 bus, several callers (MainTask polling, MQTT commands, radarTask).
// Every public operation takes this recursive mutex so frames never interleave.
static SemaphoreHandle_t sdi12Mutex = NULL;
struct BusLock {
    bool ok;
    BusLock() {
        if (!sdi12Mutex) sdi12Mutex = xSemaphoreCreateRecursiveMutex();
        ok = xSemaphoreTakeRecursive(sdi12Mutex, pdMS_TO_TICKS(20000)) == pdTRUE;
        if (!ok) Serial.println("[SDI12] bus busy (lock timeout)");
    }
    ~BusLock() { if (ok) xSemaphoreGiveRecursive(sdi12Mutex); }
};

// Last stage height read back from the sensor (NAN = unknown)
static float g_stageHeight = NAN;
float radarStageHeight() { return g_stageHeight; }

// Parse "a+1.23-4.5+6" -> values (address char skipped). Returns count.
int radarParseValues(const String &raw, float *out, int maxn) {
    int n = 0, len = raw.length(), i = 1;
    while (i < len && n < maxn) {
        char c = raw[i];
        if (c == '+' || c == '-') {
            int j = i + 1;
            while (j < len && raw[j] != '+' && raw[j] != '-') j++;
            out[n++] = raw.substring(i, j).toFloat();
            i = j;
        } else {
            i++;
        }
    }
    return n;
}
QueueHandle_t radarRequestQueue;
QueueHandle_t radarResultQueue;


// Radar Task priorities
#define RADAR_TASK_PRIORITY 2
#define RADAR_TASK_STACK 4 * 1024

bool isValidResponse(String res) {
    res.trim();
    if (res.length() == 0) return false;

    // Most SDI-12 valid responses start with sensor address (e.g., '0')
    if (res.charAt(0) == SENSOR_ADDR) return true;

    return false;
}

// ================= LOW LEVEL =================
String readSDI12(uint32_t timeout = 2000) {
  String res = "";
  uint32_t start = millis();

  while (millis() - start < timeout) {
    while (sdi12.available()) {
      char c = sdi12.read();

      if (c == '\n' || c == '\r') {
        if (res.length()) return res;
      } else {
        res += c;
      }
    }
  }
  return res;
}

void sendCmd(String cmd, String title) {
  Serial.println("\n----------------------------------");
  Serial.println("📡 " + title);
  Serial.println(">>> " + cmd);

  sdi12.clearBuffer();
  sdi12.sendCommand(cmd);
}

// ================= FACTORY RESET =================
bool factoryReset() {
  BusLock lock; if (!lock.ok) return false;
  sendCmd(String(SENSOR_ADDR) + "XFDS!", "Factory Reset");

  String res = readSDI12(3000);
  Serial.println("<<< " + res);

  Serial.println("⚠️ Sensor Reset Done!");
  Serial.println("👉 Address is now: 0");
  Serial.println("👉 Units reset to FEET");
  Serial.println("👉 Samples reset to default");

  return isValidResponse(res);
}

// ================= Sensor Info =================
String scanSensor() {
  sendCmd("?!", "Scan Bus");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return res;
}

String getSensorInfo() {
  BusLock lock; if (!lock.ok) return String("");
  sendCmd(String(SENSOR_ADDR) + "I!", "Sensor Info");
  String res = readSDI12(2000);
  Serial.println("Sensor Info: " + res);
  return res;
}

// ================= POWER MODE Configuration =================
bool setPowerHigh() {
  BusLock lock; if (!lock.ok) return false;
  sendCmd(String(SENSOR_ADDR) + "XPMH!", "Set HIGH Power Mode");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return isValidResponse(res);
}

bool setPowerLow() {
  BusLock lock; if (!lock.ok) return false;
  sendCmd(String(SENSOR_ADDR) + "XPML!", "Set LOW Power Mode");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return isValidResponse(res);
}

String getPowerMode() {
  BusLock lock; if (!lock.ok) return String("");
  sendCmd(String(SENSOR_ADDR) + "XPM!", "Get Power Mode");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return res;
}

// ================= Unit Configuration =================
bool setUnitMeters() {
  BusLock lock; if (!lock.ok) return false;
  sendCmd(String(SENSOR_ADDR) + "XSUM!", "Set Unit: Meters");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return isValidResponse(res);
}

bool setUnitFeets() {
  BusLock lock; if (!lock.ok) return false;
  sendCmd(String(SENSOR_ADDR) + "XSUFT!", "Set Unit: Meters");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return isValidResponse(res);
}

String getUnit() {
  BusLock lock; if (!lock.ok) return String("");
  sendCmd(String(SENSOR_ADDR) + "XSU!", "Get Unit");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return res;
}

// ================= Sample Configuration =================
bool setSamples(int n) {
  BusLock lock; if (!lock.ok) return false;
  sendCmd(String(SENSOR_ADDR) + "XNSM3+" + String(n) + "!", "Set Samples");
  String res = readSDI12();
  Serial.println("<<< " + res);

  // Simple success check (depends on sensor response)
  return isValidResponse(res);
}

String getSamples() {
  BusLock lock; if (!lock.ok) return String("");
  sendCmd(String(SENSOR_ADDR) + "XNSM3!", "Get Samples");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return res;
}

// ================= Stage Height Configuration =================
String getStageHeight() {
  BusLock lock; if (!lock.ok) return String("");
  sendCmd(String(SENSOR_ADDR) + "XHAZ!", "Get Stage");
  String res = readSDI12();
  Serial.println("<<< " + res);
  return res;
}

// Read stage height from the sensor and push to the display only if it changed.
bool radarRefreshStageHeight() {
  String res = getStageHeight();
  float v[1];
  if (!isValidResponse(res) || radarParseValues(res, v, 1) < 1) return false;
  if (isnan(g_stageHeight) || fabsf(v[0] - g_stageHeight) > 0.0005f) {
    g_stageHeight = v[0];
    updateStageHeight(g_stageHeight);
  }
  return true;
}

// Try the value formats in turn; success = the sensor reads back the value we set.
bool setStageHeight(float h) {
  BusLock lock; if (!lock.ok) return false;

  const char *fmts[] = { "XHAZ%.2f!", "XHAZ+%.2f!" };
  for (const char *f : fmts) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), f, h);
    sendCmd(String(SENSOR_ADDR) + cmd, "Set Stage");
    String res = readSDI12();
    Serial.println("<<< " + res);
    if (!isValidResponse(res)) continue;           // no / bad reply -> try next format

    g_stageHeight = NAN;                            // force display refresh
    if (radarRefreshStageHeight() && fabsf(g_stageHeight - h) < 0.01f) {
      Serial.printf("[STAGE] set OK (%s)\n", cmd);
      return true;
    }
    Serial.printf("[STAGE] sensor replied but value is %.2f, wanted %.2f\n", g_stageHeight, h);
  }
  return false;
}


// ================= MEASUREMENT =================
bool detectRadar() {

    for (int attempt = 0; attempt < 3; attempt++) {

        Serial.printf("🔍 Detect attempt %d\n", attempt + 1);

        sdi12.clearBuffer();
        vTaskDelay(pdMS_TO_TICKS(100));

        // =========================
        // REAL SDI-12 ADDRESS CHECK
        // =========================
        sendCmd("?!", "Scan Bus");

        String scan = readSDI12(1200);

        Serial.println("Scan: " + scan);

        // 🔥 MUST contain valid SDI-12 address (0-9, a-z, A-Z)
        bool valid = false;

        for (char c : scan) {
            if ((c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z')) {
                valid = true;
                break;
            }
        }

        if (!valid) {
            Serial.println("⚠️ No valid SDI-12 address");
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        Serial.println("✅ Radar detected");
        return true;
    }

    Serial.println("❌ Radar not detected after retries");
    return false;
}

bool measureM3(String &rawData) {
    BusLock lock;
    if (!lock.ok) { rawData = "RADAR_BUSY"; return false; }

    rawData = "";  // always reset

    // =========================
    // STEP 1: CHECK SENSOR
    // =========================
    if (!detectRadar()) {
        rawData = "RADAR_NOT_AVAILABLE";
        updateDistance(NAN);
        return false;
    }

    // =========================
    // STEP 2: MEASUREMENT
    // =========================
    for (int attempt = 0; attempt < 3; attempt++) {

        Serial.printf("🔁 Measurement attempt %d\n", attempt + 1);

        sdi12.clearBuffer();

        sendCmd(String(SENSOR_ADDR) + "M3!", "Start M3");

        String resp = readSDI12(2000);

        if (resp.length() < 4) continue;

        int t = resp.substring(1, 4).toInt();
        if (t <= 0) t = 3;

        vTaskDelay(pdMS_TO_TICKS((t + 1) * 1000));

        sdi12.clearBuffer();

        sendCmd(String(SENSOR_ADDR) + "D0!", "Read D0");

        rawData = readSDI12(2500);

        Serial.println("RAW: " + rawData);

        if (rawData.length() == 0) {
            rawData = "RADAR_EMPTY";
            continue;
        }

        int plusCount = 0;
        for (char c : rawData) if (c == '+') plusCount++;

        if (plusCount < 6) {
            rawData = "RADAR_INVALID";
            continue;
        }

        Serial.println("✅ Valid radar data");
        {
            float v[RADAR_MAX_VALUES];
            if (radarParseValues(rawData, v, RADAR_MAX_VALUES) > RADAR_DISTANCE_FIELD) {
                // Displayed distance = stage height - measured distance
                if (isnan(g_stageHeight)) radarRefreshStageHeight();   // retry if boot read failed
                updateDistance(isnan(g_stageHeight) ? NAN
                                                    : g_stageHeight - v[RADAR_DISTANCE_FIELD]);
            }
        }
        return true;
    }

    rawData = "RADAR_READ_ERROR";
    updateDistance(NAN);
    return false;
}

void radarTask(void *parameter) {

    RadarRequest req;
    RadarResult result;

    while (1) {

        if (xQueueReceive(radarRequestQueue, &req, portMAX_DELAY) == pdTRUE) {

            Serial.println("📡 RadarTask triggered");

            String data;

            bool ok = measureM3(data);

            result.success = ok;
            result.rawData = data;   // 🔥 ALWAYS SET EXPLICITLY

            // Serial.println("==================================");
            // Serial.println("📡 Measurement done");
            // Serial.println("📊 Radar Result: " + data);
            // Serial.println("==================================");
            // Serial.println();

            if (!ok) {
                Serial.println("❌ Radar failed: " + data);
            } else {
                Serial.println("✅ Radar success");
            }

            xQueueSend(radarResultQueue, &result, portMAX_DELAY);
        }
    }
}

// ================= SETUP =================
void radar_setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n🚀 RADAR FULL CONTROL SYSTEM");

  sdi12.begin();
  sdi12.clearBuffer();

  getSensorInfo();
  setPowerLow();
  setSamples(60);
  getStageHeight();
  radarRefreshStageHeight();     // one-time read; updated again only on setStageHeight

  radarRequestQueue = xQueueCreate(3, sizeof(RadarRequest));
  radarResultQueue  = xQueueCreate(3, sizeof(RadarResult));

  xTaskCreatePinnedToCore(radarTask, "Radar", RADAR_TASK_STACK, NULL, RADAR_TASK_PRIORITY, &radarTaskHandle, 1);

  Serial.println("\n✅ Setup Done\n");
}
