#include "main.h"

#include "config_portal.h"
#include "display.h"
#include "radar.h"

// ==================== Global Definitions ====================
TaskHandle_t mainTaskHandle = NULL;
Preferences preferences;

unsigned long HEARTBEAT_INTERVAL;
unsigned long DATA_INTERVAL;

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

        HEARTBEAT_INTERVAL = preferences.getULong("hb_inv", 180000);
        DATA_INTERVAL      = preferences.getULong("dt_inv", 120000);

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

        HEARTBEAT_INTERVAL = preferences.getULong("hb_inv", 180000);
        DATA_INTERVAL      = preferences.getULong("dt_inv", 120000);
        
        preferences.end();
    #endif

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

    xTaskCreatePinnedToCore(mainTask, "MainTask", MAIN_TASK_STACK, NULL, MAIN_TASK_PRIORITY, &mainTaskHandle, 1);

    SerialMon.println("Setup complete!!!");
    SerialMon.println("=================================\n");
}

void loop() {
    // FreeRTOS scheduler takes over - nothing to do here
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (softAP_started) {
        server.handleClient();
        delay(2);
    }
}

// ==================== Main Task ====================
void mainTask(void* parameter) {
    unsigned long lastHeartbeatTime = 0;
    unsigned long lastDataTime = 0;  // Added to track sensor data interval
    
    SerialMon.println("Main Task started");
    
    while (1) {
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Send heartbeat every minute
        if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
            if (deviceOnline) {
                publishHeartbeat();
                vTaskDelay(pdMS_TO_TICKS(100));
                SerialMon.println("Heartbeat published");
                sendLedCommand(LED_HEARTBEAT);
                lastHeartbeatTime = millis();
            }
        }

        // Send sensor data every DATA_INTERVAL
        if (millis() - lastDataTime >= DATA_INTERVAL) {
            lastDataTime = millis();
            if (deviceOnline) {
                SerialMon.println("Reading sensors and publishing data...");

                Serial.println("[MAIN] Calling radar read");
                Serial.flush();
                readAllSensorsAndPublish();
                Serial.println("[MAIN] Radar read done");
                Serial.flush();

                SerialMon.println("Sensor data published");
                sendLedCommand(LED_RF_HB);
            }
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
    }

    if (message.startsWith("set_pub_interval:")) {
        unsigned long newInterval = message.substring(17).toInt();

        if (newInterval >= 30 && newInterval <= 900) {
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
            Serial.println("interval time limit exeeded! [30-900]");
            MQTTMessage response2 = {};     // ← zeroes retries
            snprintf(response2.topic, sizeof(response2.topic), "%s", MQTT_PUB.c_str());
            snprintf(response2.payload, sizeof(response2.payload), "['limit exeeded (30-900)']");
            sendLedCommand(LED_PING_ACK);
            xQueueSend(mqttPublishQueue, &response2, pdMS_TO_TICKS(100));
        }
    }

    if (message == "ping") {
        SerialMon.println("MQTT Command: Ping received");
        publishHeartbeat();
        return;
    }

    if (message == "restart") {
        Serial.println("Esp Restarting ...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }

    if (message == "data") {
        SerialMon.println("MQTT Command: Data Request received");
        // publishEnergyData();
        Serial.println("[MAIN] Calling radar read");
        Serial.flush();
        readAllSensorsAndPublish();
        Serial.println("[MAIN] Radar read done");
        Serial.flush();
        
        return;
    }

    if (message == "wifi_ap") {
        dev_config();
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
        firmwareUrl = defaultFirmwareUrl;

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

    //Get Strength Level form RSSI
    int signalLevel = rssiToSignalLevel(csq);

    //Update Display Sigal Strength
    updateSignalStrength(signalLevel);

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
                "\"chip_temp\":%.1f"
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
        temperatureRead()
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
        vTaskSuspend(mainTaskHandle);
        vTaskSuspend(networkTaskHandle);
        xSemaphoreGive(taskSuspendMutex);
        SerialMon.println("All tasks suspended for OTA");
    }
}

void resumeAllTasks() {
    if (xSemaphoreTake(taskSuspendMutex, portMAX_DELAY)) {
        otaInProgress = false;
        tasksSuspended = false;
        vTaskResume(mainTaskHandle);
        vTaskResume(networkTaskHandle);
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

bool performOTAUpdate() {
    SerialMon.println("\n[OTA] Starting OTA update");

    String host;
    String path;
    int port;
    if (!parseURL(firmwareUrl, host, path, port)) {
        SerialMon.println("[OTA] URL parse failed");
        return false;
    }

    SerialMon.printf("[OTA] Host: %s\n", host.c_str());
    SerialMon.printf("[OTA] Path: %s\n", path.c_str());

    TinyGsmClient otaClient(modem);
    unsigned long otaStartTime = millis();
    const unsigned long OTA_TOTAL_TIMEOUT = 300000;

    // Connect
    SerialMon.printf("[OTA] Connecting to %s:%d\n", host.c_str(), port);
    int connectAttempts = 0;
    bool serverConnected = false;
    while (connectAttempts < 3 && !serverConnected) {
        connectAttempts++;
        if (otaClient.connect(host.c_str(), port)) {
            serverConnected = true;
            SerialMon.println("[OTA] Connected to server");
        } else {
            SerialMon.println("[OTA] Connection failed");
            if (connectAttempts < 3) {
                delay(2000);
                if (!modem.isGprsConnected()) {
                    SerialMon.println("[OTA] GPRS lost during OTA, aborting");
                    return false;
                }
            }
        }
    }
    if (!serverConnected) {
        otaClient.stop();
        return false;
    }

    // Send HTTP request
    String request = String("GET ") + path + " HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n" +
                     "User-Agent: GMS32-OTA\r\n" +
                     "Connection: keep-alive\r\n\r\n";
    otaClient.print(request);

    // Parse response
    unsigned long headerTimeout = millis();
    bool headersComplete = false;
    int contentLength = 0;
    bool httpOK = false;
    while (millis() - headerTimeout < 10000 && !headersComplete) {
        if (otaClient.available()) {
            String line = otaClient.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) {
                headersComplete = true;
                break;
            }
            if (line.startsWith("HTTP/1.") && line.indexOf("200") > 0) httpOK = true;
            if (line.startsWith("Content-Length:")) contentLength = line.substring(15).toInt();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!headersComplete || !httpOK || contentLength <= 0) {
        otaClient.stop();
        return false;
    }

    if (!Update.begin(contentLength, U_FLASH)) {
        otaClient.stop();
        return false;
    }

    // Download and write
    uint8_t buffer[512];
    size_t totalWritten = 0;
    unsigned long lastDataTime = millis();
    unsigned long lastProgressTime = millis();
    const unsigned long DATA_TIMEOUT = 60000;
    const unsigned long PROGRESS_INTERVAL = 1000;

    while (totalWritten < contentLength) {
        if (millis() - otaStartTime > OTA_TOTAL_TIMEOUT) {
            Update.abort();
            otaClient.stop();
            return false;
        }
        if (!modem.isGprsConnected()) {
            Update.abort();
            otaClient.stop();
            return false;
        }
        if (otaClient.available()) {
            lastDataTime = millis();
            int bytesAvailable = otaClient.available();
            int toRead = min(min(bytesAvailable, (int)sizeof(buffer)),
                             (int)(contentLength - totalWritten));
            if (toRead > 0) {
                int bytesRead = otaClient.read(buffer, toRead);
                if (bytesRead > 0) {
                    if (Update.write(buffer, bytesRead) != bytesRead) {
                        Update.abort();
                        otaClient.stop();
                        return false;
                    }
                    totalWritten += bytesRead;
                    if (millis() - lastProgressTime > PROGRESS_INTERVAL) {
                        float percent = (100.0 * totalWritten) / contentLength;
                        SerialMon.printf("[OTA] Progress: %.1f%% (%d/%d bytes)\n",
                                         percent, totalWritten, contentLength);
                        lastProgressTime = millis();
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (millis() - lastDataTime > DATA_TIMEOUT) {
                Update.abort();
                otaClient.stop();
                return false;
            }
        }
        esp_task_wdt_reset();
    }

    if (!Update.end() || !Update.isFinished()) {
        otaClient.stop();
        return false;
    }

    SerialMon.println("[OTA] Firmware update successful!");
    otaClient.stop();
    delay(3000);
    ESP.restart();
    return true;
}
