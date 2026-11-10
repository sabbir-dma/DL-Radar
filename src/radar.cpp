#include "radar.h"
#include "config.h"
#include "display.h"
#include "led.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SDI12.h>
#include <math.h>
#include <stdarg.h>

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

// Number of non-measurement operations (MQTT/portal commands) waiting for the bus.
// A running measurement checks this and gives the bus up so commands answer quickly.
static volatile int g_cmdPending = 0;
static inline void pendingAdd(int d) { __atomic_fetch_add(&g_cmdPending, d, __ATOMIC_SEQ_CST); }

struct BusLock {
    bool ok;
    bool urgent;
    explicit BusLock(bool isCommand = true) : urgent(isCommand) {
        if (!sdi12Mutex) sdi12Mutex = xSemaphoreCreateRecursiveMutex();
        if (urgent) pendingAdd(1);
        ok = xSemaphoreTakeRecursive(sdi12Mutex, pdMS_TO_TICKS(20000)) == pdTRUE;
        if (!ok) Serial.println("[SDI12] bus busy (lock timeout)");
    }
    ~BusLock() {
        if (ok) xSemaphoreGiveRecursive(sdi12Mutex);
        if (urgent) pendingAdd(-1);
    }
};

// Used by suspendAllTasks(): wait for any running SDI-12 transaction, so a task is
// never suspended while it still owns the bus (that would block the bus until reboot).
static bool g_busHeld = false;
void radarBusHold(bool on) {
    if (on) {
        if (!sdi12Mutex) sdi12Mutex = xSemaphoreCreateRecursiveMutex();
        pendingAdd(1);                       // ask a running measurement to give way
        g_busHeld = xSemaphoreTakeRecursive(sdi12Mutex, pdMS_TO_TICKS(15000)) == pdTRUE;
        pendingAdd(-1);
    } else if (g_busHeld) {
        g_busHeld = false;
        xSemaphoreGiveRecursive(sdi12Mutex);
    }
}

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
QueueHandle_t radarCmdQueue = NULL;


// Radar Task priorities
#define RADAR_TASK_PRIORITY 2
#define RADAR_TASK_STACK 6 * 1024

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
    vTaskDelay(1);                       // let other tasks / idle run while waiting
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
// The sensor address is learned once (at setup) and kept in flash.
// A bus scan ("?!") is only done when the stored address stops answering.
static void saveSensorAddr(char c) {
    Preferences p;
    p.begin("radar", false);
    p.putUChar("addr", (uint8_t)c);
    p.end();
}

static char loadSensorAddr() {
    Preferences p;
    p.begin("radar", false);
    uint8_t a = p.getUChar("addr", 0);
    p.end();
    return a ? (char)a : '0';
}

// Does the sensor answer on SENSOR_ADDR?  ("a!" = acknowledge active)
static bool sdi12Ack() {
    sdi12.clearBuffer();
    sendCmd(String(SENSOR_ADDR) + "!", "Ack");
    String res = readSDI12(1200);
    Serial.println("<<< " + res);
    return res.length() >= 1 && res.charAt(0) == SENSOR_ADDR;
}

// Ask the bus for the sensor address ("?!"), up to 3 times. Caller holds the bus.
static bool scanForSensor() {
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("🔍 Scan attempt %d/3\n", attempt);
        sdi12.clearBuffer();
        vTaskDelay(pdMS_TO_TICKS(100));
        sendCmd("?!", "Scan Bus");
        String res = readSDI12(1200);
        Serial.println("Scan: " + res);
        if (res.length() >= 1) {
            char c = res.charAt(0);
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                if (c != SENSOR_ADDR) Serial.printf("Sensor address is '%c' (was '%c')\n", c, SENSOR_ADDR);
                SENSOR_ADDR = c;
                saveSensorAddr(c);
                Serial.println("✅ Radar found");
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    Serial.println("❌ Radar not found");
    return false;
}

static uint32_t g_lastMeasureMs = 0;
uint32_t radarLastMeasureMs() { return g_lastMeasureMs; }

// One full measurement. Caller holds the bus. Sets `aborted` if it stopped early
// because a command is waiting (the sensor drops the measurement on any new command).
static bool measureM3Once(String &rawData, bool &aborted) {
    aborted = false;
    rawData = "";  // always reset

    // 3 measurement attempts on the stored address (no bus scan here)
    for (int attempt = 0; attempt < 3; attempt++) {

        Serial.printf("🔁 Measurement attempt %d\n", attempt + 1);

        sdi12.clearBuffer();

        sendCmd(String(SENSOR_ADDR) + "M3!", "Start M3");

        String resp = readSDI12(2000);

        if (resp.length() < 4) continue;

        int t = resp.substring(1, 4).toInt();
        if (t <= 0) t = 3;

        // Wait for the sensor, in slices, so a waiting command can take the bus
        for (int waited = 0; waited < (t + 1) * 1000; waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (g_cmdPending > 0) {
                Serial.println("[RADAR] measurement paused: command waiting");
                aborted = true;
                return false;
            }
        }

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

    if (rawData.length() == 0 || rawData.startsWith("RADAR_") == false) rawData = "RADAR_READ_ERROR";
    return false;
}

bool measureM3(String &rawData) {
    uint32_t t0 = millis();
    bool scanned = false, ok = false;

    for (int tries = 0; tries < 3; tries++) {
        bool aborted = false;
        {
            BusLock lock(false);            // measurement: not urgent
            if (!lock.ok) { rawData = "RADAR_BUSY"; return false; }

            ok = measureM3Once(rawData, aborted);

            // 3 failed measurements -> look for the sensor (3 scans), then measure once more
            if (!ok && !aborted && !scanned) {
                scanned = true;
                if (scanForSensor()) ok = measureM3Once(rawData, aborted);
            }
        }                                    // bus released here
        if (!aborted) {
            if (ok) g_lastMeasureMs = millis() - t0;
            return ok;
        }

        // A command wants the bus: let it run, then measure again
        uint32_t w0 = millis();
        while (g_cmdPending > 0 && millis() - w0 < 30000) vTaskDelay(pdMS_TO_TICKS(50));
    }
    rawData = "RADAR_BUSY";
    return false;
}

// ================= COMMAND QUEUE =================
// MQTT callback only enqueues; this task talks to the sensor and publishes the reply.
bool radarSendCommand(RadarCmdType type, float value) {
    if (!radarCmdQueue) return false;
    RadarCmd c = { type, value };
    pendingAdd(1);                           // measurement sees this immediately and pauses
    if (xQueueSend(radarCmdQueue, &c, pdMS_TO_TICKS(100)) == pdTRUE) return true;
    pendingAdd(-1);
    return false;
}

static void radarReply(const char *fmt, ...) {
    MQTTMessage r = {};
    snprintf(r.topic, sizeof(r.topic), "%s", MQTT_PUB.c_str());
    va_list ap; va_start(ap, fmt);
    vsnprintf(r.payload, sizeof(r.payload), fmt, ap);
    va_end(ap);
    sendLedCommand(LED_PING_ACK);
    if (mqttPublishQueue) xQueueSend(mqttPublishQueue, &r, pdMS_TO_TICKS(100));
}

// "0H" / "0M" style reply -> true if the char after the address is `c`
static bool replyIs(const String &res, char c) {
    return res.length() > 1 && res.charAt(0) == SENSOR_ADDR && res.indexOf(c, 1) > 0;
}

static void radarHandle(const RadarCmd &c) {
    switch (c.type) {
    case RCMD_FACTORY_RESET:
        if (factoryReset()) { SENSOR_ADDR = '0'; radarReply("['Sensor factory reset OK (address 0)']"); }
        else                  radarReply("['Sensor factory reset failed']");
        break;
    case RCMD_SET_POWER_HIGH:
        radarReply(setPowerHigh() ? "['Power set to HIGH']" : "['Failed to set power to HIGH']");
        break;
    case RCMD_SET_POWER_LOW:
        radarReply(setPowerLow() ? "['Power set to LOW']" : "['Failed to set power to LOW']");
        break;
    case RCMD_GET_POWER: {
        String r = getPowerMode();
        if      (replyIs(r, 'H')) radarReply("['Current Power Mode: HIGH']");
        else if (replyIs(r, 'L')) radarReply("['Current Power Mode: LOW']");
        else                      radarReply("['Failed to get power mode (%s)']", r.c_str());
        break; }
    case RCMD_SET_UNIT_M:
        radarReply(setUnitMeters() ? "['Unit set to METERS']" : "['Failed to set unit to METERS']");
        break;
    case RCMD_SET_UNIT_FT:
        radarReply(setUnitFeets() ? "['Unit set to FEET']" : "['Failed to set unit to FEET']");
        break;
    case RCMD_GET_UNIT: {
        String r = getUnit();
        if      (replyIs(r, 'M')) radarReply("['Current Unit: METERS']");
        else if (replyIs(r, 'F')) radarReply("['Current Unit: FEET']");
        else                      radarReply("['Failed to get unit (%s)']", r.c_str());
        break; }
    case RCMD_SET_SAMPLES: {
        int n = (int)c.value;
        if (setSamples(n)) radarReply("['Successfully set samples: %d','Sensor: %s']", n, getSamples().c_str());
        else               radarReply("['Failed to set samples']");
        break; }
    case RCMD_GET_SAMPLES: {
        String r = getSamples();
        if (r.length() && r.charAt(0) == SENSOR_ADDR) radarReply("['Current Samples: %s']", r.c_str());
        else                                          radarReply("['Failed to get samples']");
        break; }
    case RCMD_SET_STAGE:
        if (setStageHeight(c.value)) radarReply("['Successfully set stage height: %.2f']", radarStageHeight());
        else                         radarReply("['Failed to set stage height']");
        break;
    case RCMD_GET_STAGE:
        if (radarRefreshStageHeight() && !isnan(radarStageHeight()))
             radarReply("['Current Stage Height: %.2f']", radarStageHeight());
        else radarReply("['Failed to get stage height']");
        break;
    }
}

void radarTask(void *parameter) {
    RadarCmd cmd;
    while (1) {
        if (xQueueReceive(radarCmdQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            radarHandle(cmd);
            pendingAdd(-1);
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

  // Address: use the one stored in flash; scan only if it does not answer (3 tries)
  SENSOR_ADDR = loadSensorAddr();
  Serial.printf("Stored sensor address: '%c'\n", SENSOR_ADDR);
  if (!sdi12Ack()) {
      Serial.println("Stored address not answering -> scanning");
      scanForSensor();
  }

  getSensorInfo();
//   setPowerLow();
  setSamples(60);
  getStageHeight();
  radarRefreshStageHeight();     // one-time read; updated again only on setStageHeight

  radarCmdQueue = xQueueCreate(8, sizeof(RadarCmd));

  xTaskCreatePinnedToCore(radarTask, "Radar", RADAR_TASK_STACK, NULL, RADAR_TASK_PRIORITY, &radarTaskHandle, 1);

  Serial.println("\n✅ Setup Done\n");
}
