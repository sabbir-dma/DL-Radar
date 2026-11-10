#include "main.h"
#include <stdarg.h>
#include "esp32/rom/crc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <math.h>

#include "config_portal.h"
#include "display.h"
#include "radar.h"
#include "storage.h"

// ==================== Global Definitions ====================
TaskHandle_t mainTaskHandle = NULL;
Preferences preferences;

unsigned long HEARTBEAT_INTERVAL;
unsigned long DATA_INTERVAL;
volatile uint8_t dataFormat = FMT_LEGACY;
volatile bool showStale = false;

// ==================== Setup ====================
void setup() {
    SerialMon.begin(115200);
    delay(100);

    SerialMon.println("\n====================================");
    SerialMon.println(" == DL301-26 for BWDB RADAR ==");
    SerialMon.println("------------------------------------");
    SerialMon.printf ("  ==   FW Version: %s   ==", FW_VERSION);
    SerialMon.println();
    SerialMon.printf ("  ==   HW Version: %s   ==", HW_VERSION);
    SerialMon.println();
    SerialMon.println("  ====================================\n");
    SerialMon.println();
    
    sendLedCommand(LED_OFFLINE);

    pinMode(ACLINE_PIN, INPUT);  
    
    // Get MAC address for fallback ID
    MAC_FALLBACK_ID = getMACDeviceID();
    SerialMon.println("MAC Fallback ID: " + MAC_FALLBACK_ID);

    // Read or set Device ID
    #if OTA
        preferences.begin("device", true);
        DEVICE_ID = preferences.getString("DID", MAC_FALLBACK_ID);

        HEARTBEAT_INTERVAL = preferences.getULong("hb_inv", 300000);
        DATA_INTERVAL      = preferences.getULong("dt_inv", 300000);

        preferences.end();

        Serial.print("HB Interval: "); Serial.println(HEARTBEAT_INTERVAL);
        Serial.print("PUB Interval: "); Serial.println(DATA_INTERVAL);
        SerialMon.println("[OTA MODE] Read-only mode: DEVICE_ID = " + DEVICE_ID);
    #else
        preferences.begin("device", false);
        DEVICE_ID = preferences.getString("DID", "");

        if (DEVICE_ID.isEmpty() || DEVICE_ID != UNIQUE_DEVICE_ID) {
            SerialMon.println("[NORMAL MODE] Invalid or missing ID. Writing new ID...");
            DEVICE_ID = UNIQUE_DEVICE_ID;
            preferences.putString("DID", DEVICE_ID);
        }

        HEARTBEAT_INTERVAL = preferences.getULong("hb_inv", 300000);
        DATA_INTERVAL      = preferences.getULong("dt_inv", 300000);
        
        preferences.end();
    #endif

    preferences.begin("device", true);
    dataFormat = preferences.getUChar("dfmt", FMT_JSON) == FMT_LEGACY ? FMT_LEGACY : FMT_JSON;
    preferences.end();
    SerialMon.printf("Data format: %s\n", dataFormat == FMT_LEGACY ? "legacy" : "json");
    preferences.begin("device", true);
    showStale = preferences.getBool("stale", false);
    preferences.end();
    SerialMon.printf("Stale marker: %s\n", showStale ? "on" : "off");

    SerialMon.println("Final Device ID: " + DEVICE_ID);
    SerialMon.println("=================================\n");

    setupMQTTTopics();

    // Read device armed state from preferences
    preferences.begin("device_config", false);
    deviceArmed = preferences.getBool("armed", true);
    SerialMon.println("Device Armed?: " + String(deviceArmed ? "Yes" : "No"));

    preferences.end();
    //---------------------------------

    // ✅ Create queue BEFORE starting any task
    mqttPublishQueue = xQueueCreate(MQTT_PUB_QUEUE_SIZE, sizeof(MQTTMessage));
    if (mqttPublishQueue == NULL) {
        SerialMon.println("FATAL: mqttPublishQueue creation failed");
        while (1) delay(1000);
    }
    SerialMon.printf("Queue created: %p, item size: %u\n", mqttPublishQueue, (unsigned)sizeof(MQTTMessage));

    

    // Initialize peripherals and tasks
    FastLED_setup();
    Display_setup();
    GSM_setup();
    radar_setup();
    storage_setup();
    initSensors();

    //////////////////////////////////////////////////////////////////
    if (!LittleFS.begin(true)) {
        Serial.println("ERROR: LittleFS mount failed!");
        return;
    }
    Serial.println("LittleFS mounted.");
    //////////////////////////////////////////////////////////////////
    
    //Modbus Config
    if (!loadConfig()) {
            SerialMon.println("Config load failed! Halting.");
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    SerialMon.printf("Config version: %d\n", config["config_version"] | 0);
    printConfig();

    sensors_start();     // acquisition: reads sensors + logs to SD, online or not
    xTaskCreatePinnedToCore(mainTask, "MainTask", MAIN_TASK_STACK, NULL, MAIN_TASK_PRIORITY, &mainTaskHandle, 1);

    SerialMon.println("Setup complete!!!");
    SerialMon.println("=================================\n");
}

void loop() {
    // FreeRTOS scheduler takes over - nothing to do here
    storage_handlePeriodic();
    if (softAP_started) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    } else {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ==================== Main Task ====================
void mainTask(void* parameter) {
    unsigned long lastHeartbeatTime = 0;

    SerialMon.println("Main Task started");
    updateVoltages(storage_getBatteryVoltage(), storage_getSolarVoltage());
    updateBatteryPercentage(100);  // Initialize battery percentage to 100% at startup

    // Publish times are multiples of DATA_INTERVAL on the (RTC) clock.
    // The SensorTask has the reading ready ~3.5 min before; here we only send what is stored.
    auto slotOf = []() -> uint32_t {
        uint32_t iv = DATA_INTERVAL / 1000;
        if (iv < 30) iv = 30;
        return (sensors_scheduleNow() / iv) * iv;
    };
    uint32_t lastPubSlot = slotOf();       // no publish at boot; first one at the next boundary
    bool     pendingPub  = false;          // slot reached while offline -> retry briefly
    uint32_t pendingSince = 0;

    while (1) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Heartbeat every HEARTBEAT_INTERVAL
        if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
            if (deviceOnline) {
                publishHeartbeat();
                vTaskDelay(pdMS_TO_TICKS(100));
                SerialMon.println("Heartbeat published");
                sendLedCommand(LED_HEARTBEAT);
                lastHeartbeatTime = millis();
            }
        }

        // Exact-time data publish
        uint32_t cur = slotOf();
        if (cur < lastPubSlot) lastPubSlot = cur;            // clock moved back
        if (cur > lastPubSlot) {
            lastPubSlot = cur;
            pendingPub = true;
            pendingSince = sensors_scheduleNow();
        }
        if (pendingPub) {
            if (deviceOnline) {
                SerialMon.println("Publishing latest data...");
                sensors_publishLatest();
                updateVoltages(storage_getBatteryVoltage(), storage_getSolarVoltage());
                updateBatteryPercentage((int)((storage_getBatteryVoltage() / 7.4) * 100));
                pendingPub = false;
            } else if (sensors_scheduleNow() - pendingSince > 90) {
                SerialMon.println("Offline at publish time - skipped (data is on the SD card)");
                pendingPub = false;
            }
        }

        // MQTT "data": publish the stored latest data right now
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            sensors_publishLatest();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void dev_config() {
    // displayConfigPage();
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
        Serial.println("ERROR: SoftAP failed!");
        return;
    }
    Serial.println();
        Serial.println("SoftAP started.");
        Serial.print("SSID: ");     Serial.println(AP_SSID);
        Serial.print("Password: "); Serial.println(AP_PASSWORD);
        Serial.print("IP: ");       Serial.println(WiFi.softAPIP());

        setupWebServer();
        softAP_started = true;
        suspendAllTasks();
}

//========================================================
// ==================== MQTT Callback ====================
//========================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    sendLedCommand(LED_MQTT_RECEIVE);
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    message.trim();

    SerialMon.println("\n========== MQTT Message Received ==========");
    SerialMon.print("Topic: ");
    SerialMon.println(topic);
    SerialMon.print("Payload: ");
    SerialMon.println(message);
    SerialMon.println("==========================================\n");

    message.trim();
    message.replace(" ", "");
    SerialMon.println("📥 Message: " + message);

    if (message.startsWith("set_hb_interval:")) {
        unsigned long newInterval = message.substring(16).toInt();

        if (newInterval >= 15 && newInterval <= 600) {
            newInterval = newInterval * 1000;
            preferences.begin("device", false);  // Read/write mode
            preferences.putULong("hb_inv", newInterval);
            preferences.end();
            HEARTBEAT_INTERVAL = newInterval;

            MQTTMessage response = {};     // ← zeroes retries
            snprintf(response.topic, sizeof(response.topic), "%s", MQTT_PUB.c_str());
            snprintf(response.payload, sizeof(response.payload), "['Successfully set limit: %s']", String(newInterval).c_str());
            sendLedCommand(LED_PING_ACK);
            xQueueSend(mqttPublishQueue, &response, pdMS_TO_TICKS(100));

            // ESP.restart();
        }
        else {
            Serial.println("interval time limit exeeded! [15-600]");
            MQTTMessage response2 = {};     // ← zeroes retries
            snprintf(response2.topic, sizeof(response2.topic), "%s", MQTT_PUB.c_str());
            snprintf(response2.payload, sizeof(response2.payload), "['limit exeeded (15-600)']");
            sendLedCommand(LED_PING_ACK);
            xQueueSend(mqttPublishQueue, &response2, pdMS_TO_TICKS(100));
        }
        return;
    }

    if (message.startsWith("set_pub_interval:")) {
        unsigned long newInterval = message.substring(17).toInt();

        if (newInterval >= 240 && newInterval <= 3600) {
            newInterval = newInterval * 1000;
            preferences.begin("device", false);
            preferences.putULong("dt_inv", newInterval);
            preferences.end();
            DATA_INTERVAL = newInterval;

            MQTTMessage response = {};     // ← zeroes retries
            snprintf(response.topic, sizeof(response.topic), "%s", MQTT_PUB.c_str());
            snprintf(response.payload, sizeof(response.payload), "['Successfully set limit: %s']", String(newInterval).c_str());
            sendLedCommand(LED_PING_ACK);
            xQueueSend(mqttPublishQueue, &response, pdMS_TO_TICKS(100));
            // ESP.restart();
        }
        else {
            Serial.println("interval time limit exceeded! [240-3600]");
            MQTTMessage response2 = {};     // ← zeroes retries
            snprintf(response2.topic, sizeof(response2.topic), "%s", MQTT_PUB.c_str());
            snprintf(response2.payload, sizeof(response2.payload), "['limit exceeded (240-3600)']");
            sendLedCommand(LED_PING_ACK);
            xQueueSend(mqttPublishQueue, &response2, pdMS_TO_TICKS(100));
        }
        return;
    }

    if (message == "ping") {
        SerialMon.println("MQTT Command: Ping received");
        publishHeartbeat();
        return;
    }

    // ---- helper: queue a text reply on the publish topic ----
    auto reply = [](const char *fmt, ...) {
        MQTTMessage r = {};
        snprintf(r.topic, sizeof(r.topic), "%s", MQTT_PUB.c_str());
        va_list ap; va_start(ap, fmt);
        vsnprintf(r.payload, sizeof(r.payload), fmt, ap);
        va_end(ap);
        sendLedCommand(LED_PING_ACK);
        xQueueSend(mqttPublishQueue, &r, pdMS_TO_TICKS(100));
    };

    // ---- Radar sensor commands: only queued here; radarTask runs them and replies ----
    struct { const char *name; RadarCmdType type; } radarCmds[] = {
        { "factoryReset",  RCMD_FACTORY_RESET  },
        { "setPowerHigh",  RCMD_SET_POWER_HIGH },
        { "setPowerLow",   RCMD_SET_POWER_LOW  },
        { "getPowerMode",  RCMD_GET_POWER      },
        { "setUnitMeters", RCMD_SET_UNIT_M     },
        { "setUnitFeets",  RCMD_SET_UNIT_FT    },
        { "getUnit",       RCMD_GET_UNIT       },
        { "getSamples",    RCMD_GET_SAMPLES    },
        { "getStageHeight",RCMD_GET_STAGE      },
    };
    for (auto &c : radarCmds) {
        if (message == c.name) {
            if (!radarSendCommand(c.type)) reply("['Sensor busy, try again']");
            return;
        }
    }

    // setSamples:<1-1000>
    if (message.startsWith("setSamples:")) {
        long n = message.substring(11).toInt();
        if (n < 1 || n > 1000)                         reply("['Samples limit exceeded (1-1000)']");
        else if (!radarSendCommand(RCMD_SET_SAMPLES, (float)n)) reply("['Sensor busy, try again']");
        return;
    }

    // setStageHeight:<1-1000>
    if (message.startsWith("setStageHeight:")) {
        float h = message.substring(15).toFloat();
        if (h < 1.0f || h > 1000.0f)                   reply("['Stage height limit exceeded (1-1000)']");
        else if (!radarSendCommand(RCMD_SET_STAGE, h)) reply("['Sensor busy, try again']");
        return;
    }

    // Data format:  set_data_format:json | set_data_format:legacy | get_data_format
    {
        String lower = message;
        lower.toLowerCase();
        if (lower.startsWith("set_data_format:")) {
            String v = lower.substring(16);
            uint8_t f;
            if      (v == "json")   f = FMT_JSON;
            else if (v == "legacy") f = FMT_LEGACY;
            else { reply("['Invalid format (json|legacy)']"); return; }
            preferences.begin("device", false);
            preferences.putUChar("dfmt", f);
            preferences.end();
            dataFormat = f;
            reply("['Data format set: %s']", f == FMT_LEGACY ? "legacy" : "json");
            return;
        }
        if (lower.startsWith("set_stale:")) {
            String v = lower.substring(10);
            if (v != "on" && v != "off") { reply("['Invalid value (on|off)']"); return; }
            showStale = (v == "on");
            preferences.begin("device", false);
            preferences.putBool("stale", showStale);
            preferences.end();
            reply("['Stale marker: %s']", showStale ? "on" : "off");
            return;
        }
        if (lower == "get_stale") {
            reply("['Stale marker: %s']", showStale ? "on" : "off");
            return;
        }
        if (lower == "get_data_format") {
            reply("['Data format: %s']", dataFormat == FMT_LEGACY ? "legacy" : "json");
            return;
        }
    }

    if (message == "restart") {
        Serial.println("Esp Restarting ...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }

    // "data": publish the latest stored reading now (no sensor access)
    if (message == "data") {
        SerialMon.println("MQTT Command: data (latest stored)");
        if (mainTaskHandle) xTaskNotifyGive(mainTaskHandle);
        return;
    }

    // "data_read": read the sensor now, publish when the reading is done (~3 min in low-power mode)
    if (message == "data_read") {
        SerialMon.println("MQTT Command: data_read");
        sensors_requestReadAndPublish();
        MQTTMessage r = {};
        snprintf(r.topic, sizeof(r.topic), "%s", MQTT_PUB.c_str());
        snprintf(r.payload, sizeof(r.payload), "['Reading sensor, data will be published when done']");
        xQueueSend(mqttPublishQueue, &r, pdMS_TO_TICKS(100));
        return;
    }

    // Interval query (seconds)
    if (message == "get_interval" || message == "get_hb_interval" || message == "get_pub_interval") {
        MQTTMessage r = {};
        snprintf(r.topic, sizeof(r.topic), "%s", MQTT_PUB.c_str());
        if (message == "get_hb_interval")
            snprintf(r.payload, sizeof(r.payload), "['hb_interval:%lus']", HEARTBEAT_INTERVAL / 1000);
        else if (message == "get_pub_interval")
            snprintf(r.payload, sizeof(r.payload), "['pub_interval:%lus']", DATA_INTERVAL / 1000);
        else
            snprintf(r.payload, sizeof(r.payload), "['hb_interval:%lus','pub_interval:%lus']",
                     HEARTBEAT_INTERVAL / 1000, DATA_INTERVAL / 1000);
        sendLedCommand(LED_PING_ACK);
        xQueueSend(mqttPublishQueue, &r, pdMS_TO_TICKS(100));
        return;
    }

    if (message == "wifi_ap") {
        dev_config();
        return;
    }

    if (message == "get_sim_info") {
        String operatorCode = modem.getOperator();
        String operatorName = operatorCode;

        if (operatorCode == "47001") operatorName = "Grameenphone";
        else if (operatorCode == "47002") operatorName = "Robi";
        else if (operatorCode == "47003") operatorName = "Banglalink";
        else if (operatorCode == "47004") operatorName = "Teletalk";

        Serial.print("Operator: ");
        Serial.println(operatorName);

        String imsi = modem.getSimCCID();
        String simID = "";
        if (imsi.length() >= 18) {
            simID = imsi.substring(imsi.length() - 18);
        }
        Serial.print("SIM ID: ");
        Serial.println(simID);

        // MQTTMessage response2;
        MQTTMessage response2 = {};     // ← zeroes retries
        snprintf(response2.topic, sizeof(response2.topic), "%s", MQTT_PUB.c_str());
        snprintf(response2.payload, sizeof(response2.payload), "['Operator:%s','ICCID:%s']",
                operatorName.c_str(), simID.c_str());
        sendLedCommand(LED_PING_ACK);
        xQueueSend(mqttPublishQueue, &response2, pdMS_TO_TICKS(100));
        return;
    }

    if (message == "arm:1") {
        SerialMon.println("MQTT Command: Arm device");
        deviceArmed = true;
        preferences.begin("device_config", false);
        preferences.putBool("armed", true);
        preferences.end();
        return;
    }

    if (message == "arm:0") {
        SerialMon.println("MQTT Command: Disarm device");
        deviceArmed = false;
        preferences.begin("device_config", false);
        preferences.putBool("armed", false);
        preferences.end();
        return;
    }

    if (message.startsWith("ota")) {
        SerialMon.println("MQTT Command: OTA update received");
        // Spaces were stripped above, so "ota http://x/y.bin" arrives as "otahttp://x/y.bin"
        String rest = message.substring(3);
        if (rest.startsWith("http://") || rest.startsWith("https://")) firmwareUrl = rest;
        else                                                          firmwareUrl = defaultFirmwareUrl;
        SerialMon.println("OTA URL: " + firmwareUrl);

        // MQTTMessage response;
        MQTTMessage response = {};     // ← zeroes retries
        snprintf(response.topic, sizeof(response.topic), "%s", MQTT_PUB.c_str());
        snprintf(response.payload, sizeof(response.payload), "%s,%s",
                "OTA update starting", firmwareUrl.c_str());
        xQueueSend(mqttPublishQueue, &response, pdMS_TO_TICKS(100));
        createOTATask();
        return;
    }

    if (message.startsWith("http://") || message.startsWith("https://")) {
        firmwareUrl = message;

        MQTTMessage response = {};     // ← zeroes retries
        snprintf(response.topic, sizeof(response.topic), "%s", MQTT_PUB.c_str());
        snprintf(response.payload, sizeof(response.payload), "%s,%s",
                "OTA update starting", firmwareUrl.c_str());
        xQueueSend(mqttPublishQueue, &response, pdMS_TO_TICKS(100));
        createOTATask();
        return;
    }

    if (message.startsWith("ota ")) {
        String link = message.substring(4);
        link.trim();
        if (link.startsWith("http://") || link.startsWith("https://")) {
            firmwareUrl = link;
            createOTATask();
            return;
        }
    }

    // Mesh command forwarding
    int commaIndex = message.indexOf(',');
    if (commaIndex < 0) {
        SerialMon.println("⚠️ Format: receiver,command");
        return;
    }

    char receiver[16];
    char command[64];
    strncpy(receiver, message.substring(0, commaIndex).c_str(), sizeof(receiver) - 1);
    receiver[sizeof(receiver) - 1] = '\0';
    strncpy(command, message.substring(commaIndex + 1).c_str(), sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    command[strcspn(command, "\r\n")] = 0;
    receiver[strcspn(receiver, "\r\n")] = 0;

}

// ==================== Heartbeat Publishing ====================
const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_SW:        return "sw_reset";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_EXT:       return "external";
        default:                return "unknown";
    }
}

//================================================
// Publish heartbeat message to MQTT broker
//================================================
void publishHeartbeat() {
    static uint32_t packetCounter = 0;
    packetCounter++;

    uint8_t csq     = modem.getSignalQuality();
    int     heap     = ESP.getFreeHeap();
    int     minHeap  = ESP.getMinFreeHeap();
    int     uptime_m = millis() / 60000;

    bool acLine = digitalRead(ACLINE_PIN) == HIGH;

    float solarV = storage_getSolarVoltage();
    float battV  = storage_getBatteryVoltage();

    //Get Strength Level form RSSI
    int signalLevel = rssiToSignalLevel(csq);

    //Update Display Sigal Strength
    updateSignalStrength(signalLevel);
    updateVoltages(battV, solarV);

    // MQTTMessage hbMsg;
    MQTTMessage hbMsg = {};     // ← zeroes retries

    int len = snprintf(hbMsg.payload, sizeof(hbMsg.payload),
        "{"
            "\"device_id\":\"%s\","
            "\"type\":\"heartbeat\","
            "\"data\":{"
                "\"fwv\":\"%s\","
                "\"hwv\":\"%s\","
                "\"armed\":%d,"
                "\"ac_line\":%d,"
                "\"uptime_min\":%d,"
                "\"csq\":%d,"
                "\"heap\":%d,"
                "\"min_heap\":%d,"
                "\"reset_reason\":\"%s\","
                "\"cpu_mhz\":%d,"
                "\"chip_temp\":%.1f,"
                "\"solar_voltage\":%.3f,"
                "\"battery_voltage\":%.3f"
            "}"
        "}",
        DEVICE_ID.c_str(),
        FW_VERSION,
        HW_VERSION,
        deviceArmed ? 1 : 0,
        acLine ? 1 : 0,
        uptime_m,
        csq,
        heap,
        minHeap,
        resetReasonStr(),
        (int)ESP.getCpuFreqMHz(),
        temperatureRead(),
        solarV,
        battV
    );

    if (len < 0 || len >= (int)sizeof(hbMsg.payload)) {
        SerialMon.println("Heartbeat: payload truncated!");
        return;
    }

    snprintf(hbMsg.topic, sizeof(hbMsg.topic), "%s", MQTT_PUB.c_str());

    SerialMon.println(hbMsg.payload);

    if (xQueueSend(mqttPublishQueue, &hbMsg, pdMS_TO_TICKS(100)) == pdTRUE) {
        SerialMon.println("MainTask: Heartbeat queued");
    } else {
        SerialMon.println("MainTask: Heartbeat queue failed");
    }
}

// ==================== Helper Functions ====================
void suspendAllTasks() {
    if (xSemaphoreTake(taskSuspendMutex, portMAX_DELAY)) {
        otaInProgress = true;
        tasksSuspended = true;
        radarBusHold(true);                 // wait until no SDI-12 transaction is running
        vTaskSuspend(mainTaskHandle);
        vTaskSuspend(networkTaskHandle);
        if (sensorTaskHandle) vTaskSuspend(sensorTaskHandle);
        radarBusHold(false);
        xSemaphoreGive(taskSuspendMutex);
        SerialMon.println("All tasks suspended for OTA / config mode");
    }
}

void resumeAllTasks() {
    if (xSemaphoreTake(taskSuspendMutex, portMAX_DELAY)) {
        otaInProgress = false;
        tasksSuspended = false;
        vTaskResume(mainTaskHandle);
        vTaskResume(networkTaskHandle);
        if (sensorTaskHandle) vTaskResume(sensorTaskHandle);
        xSemaphoreGive(taskSuspendMutex);
        SerialMon.println("All tasks resumed");
    }
}

// ==================== OTA Functions ====================
bool checkForNewFirmware() {
    SerialMon.println("OTA: Checking for new firmware...");
    // Implement your actual version check here
    return true;  // Placeholder
}

bool parseURL(String url, String &host, String &path, int &port) {
    port = 80;
    if (url.startsWith("http://")) {
        url.remove(0, 7);
    } else if (url.startsWith("https://")) {
        url.remove(0, 8);
        port = 443;
    }
    int slashIndex = url.indexOf('/');
    if (slashIndex == -1) return false;
    String hostPort = url.substring(0, slashIndex);
    path = url.substring(slashIndex);
    int colonIndex = hostPort.indexOf(':');
    if (colonIndex != -1) {
        host = hostPort.substring(0, colonIndex);
        port = hostPort.substring(colonIndex + 1).toInt();
    } else {
        host = hostPort;
    }
    return true;
}

// Fetch bytes [start, start+len) with an HTTP Range request into dst.
// Returns bytes stored (== len, except the last piece) or -1 on error.
// `total` receives the full file size (from Content-Range).
static int otaFetchRange(TinyGsmClient &c, const String &host, int port, const String &path,
                         uint32_t start, uint32_t len, uint8_t *dst, uint32_t &total) {
    c.stop();
    if (!c.connect(host.c_str(), port)) return -1;

    c.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUser-Agent: GMS32-OTA\r\nConnection: close\r\nRange: bytes=" +
            start + "-" + (start + len - 1) + "\r\n\r\n");

    uint32_t t0 = millis();
    int  status = 0;
    long contentLen = -1;
    bool headersDone = false;
    while (millis() - t0 < 15000 && !headersDone) {
        if (c.available()) {
            String line = c.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) { headersDone = true; break; }
            if (line.startsWith("HTTP/1."))         status = line.substring(9, 12).toInt();
            if (line.startsWith("Content-Length:")) contentLen = line.substring(15).toInt();
            if (line.startsWith("Content-Range:")) {
                int slash = line.indexOf('/');
                if (slash > 0) total = line.substring(slash + 1).toInt();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!headersDone || status != 206 || contentLen <= 0 || contentLen > (long)len) return -1;

    uint32_t got = 0, lastData = millis(), lastGprsCheck = millis();
    while (got < (uint32_t)contentLen) {
        if (c.available()) {
            int n = c.read(dst + got, min((uint32_t)1024, (uint32_t)contentLen - got));
            if (n > 0) { got += n; lastData = millis(); }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
            if (millis() - lastData > 20000) return -1;
        }
        if (millis() - lastGprsCheck > 5000) {          // not after every read: AT traffic slows the download
            lastGprsCheck = millis();
            if (!modem.isGprsConnected()) return -1;
        }
    }
    c.stop();
    return (int)got;
}

// Download in pieces. Each piece is read twice and written to flash only when both
// reads are identical, so a bad byte from the GSM link can never reach flash.
bool performOTAUpdate() {
    SerialMon.println("\n[OTA] Starting OTA update");

    String host, path;
    int port;
    if (!parseURL(firmwareUrl, host, path, port)) {
        SerialMon.println("[OTA] URL parse failed");
        return false;
    }
    SerialMon.printf("[OTA] Host: %s  Path: %s\n", host.c_str(), path.c_str());

    // Close MQTT first (network task is suspended; avoid broker close notices in the stream)
    mqtt.disconnect();
    mqttClient.stop();
    mqttConnected = false;
    deviceOnline  = false;
    delay(500);
    while (SerialAT.available()) SerialAT.read();

    uint32_t CHUNK = 32768;
    uint8_t *bufA = (uint8_t *)malloc(CHUNK);
    uint8_t *bufB = (uint8_t *)malloc(CHUNK);
    if (!bufA || !bufB) {                        // low memory: use smaller pieces
        free(bufA); free(bufB);
        CHUNK = 16384;
        bufA = (uint8_t *)malloc(CHUNK);
        bufB = (uint8_t *)malloc(CHUNK);
    }
    if (!bufA || !bufB) {
        SerialMon.println("[OTA] ABORT: not enough memory");
        free(bufA); free(bufB);
        return false;
    }

    TinyGsmClient otaClient(modem, 1);           // mux 1: socket 0 belongs to MQTT
    const unsigned long otaStart = millis();
    const unsigned long OTA_TOTAL_TIMEOUT = 3600000UL;   // 60 min
    uint32_t total = 0, offset = 0;
    bool began = false, ok = false;
    int pieceRetries = 0, totalRetries = 0;

    while (true) {
        if (millis() - otaStart > OTA_TOTAL_TIMEOUT) { SerialMon.println("[OTA] ABORT: total time limit"); break; }
        if (total && offset >= total) { ok = true; break; }

        uint32_t want = total ? min(CHUNK, total - offset) : CHUNK;

        int n1 = otaFetchRange(otaClient, host, port, path, offset, want, bufA, total);
        int n2 = (n1 > 0) ? otaFetchRange(otaClient, host, port, path, offset, (uint32_t)n1, bufB, total) : -1;

        if (n1 > 0 && n2 == n1 && memcmp(bufA, bufB, n1) == 0) {
            if (!began) {
                if (total == 0 || !Update.begin(total, U_FLASH)) {
                    SerialMon.printf("[OTA] ABORT: Update.begin failed (%s)\n", Update.errorString());
                    break;
                }
                began = true;
                updateOtaProgress(0);
            }
            if (Update.write(bufA, n1) != (size_t)n1) {
                SerialMon.printf("[OTA] ABORT: flash write failed (%s)\n", Update.errorString());
                break;
            }
            offset += n1;
            pieceRetries = 0;
            int pct = (int)((100ULL * offset) / total);
            updateOtaProgress(pct);
            char bar[21];
            for (int i = 0; i < 20; i++) bar[i] = i < pct / 5 ? '#' : '-';
            bar[20] = 0;
            SerialMon.printf("[OTA] [%s] %3d%% (%u/%u bytes)\n", bar, pct, (unsigned)offset, (unsigned)total);
        } else {
            pieceRetries++; totalRetries++;
            SerialMon.printf("[OTA] piece @%u mismatch/failed (n1=%d n2=%d), retry %d/6\n",
                             (unsigned)offset, n1, n2, pieceRetries);
            if (pieceRetries >= 6) { SerialMon.println("[OTA] ABORT: too many retries"); break; }
            delay(1000);
        }
        esp_task_wdt_reset();
    }

    free(bufA); free(bufB);
    otaClient.stop();
    SerialMon.printf("[OTA] pieces repeated: %d\n", totalRetries);

    if (!ok) { if (began) Update.abort(); return false; }

    if (!Update.end() || !Update.isFinished()) {
        SerialMon.printf("[OTA] ABORT: image check failed (%s)\n", Update.errorString());
        return false;
    }

    SerialMon.println("[OTA] Firmware update successful!");
    delay(3000);
    ESP.restart();
    return true;
}
