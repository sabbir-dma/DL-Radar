#include "gsm.h"
// #include "energy_meter.h"
#include "sensors.h"
//These Function is gsm.h property I have redeclared it
void publishHeartbeat();
//=================================


// ==================== Configuration ====================
const char apn[] = "internet";
const char user[] = "";
const char pass[] = "";

const char* broker = "broker2.dma-bd.com";
const char* mqttUser = "broker2";
const char* mqttPass = "Secret!@#$1234";

String firmwareUrl = "";
const char* defaultFirmwareUrl = "http://iot2.dma-bd.com:5000/download/DL301-26_AWS.bin";

// ==================== Task Handles ====================
TaskHandle_t networkTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;

// ==================== Mutexes Handles ====================
SemaphoreHandle_t mqttMutex = NULL;
SemaphoreHandle_t modemMutex = NULL;
SemaphoreHandle_t taskSuspendMutex = NULL;

// ==================== Counters ====================
uint8_t modemInitRetries = 0;
uint8_t gprsRetries = 0;
uint8_t gsmErrorCount = 0;
const uint8_t MAX_MODEM_INIT_RETRIES = 3;
const uint8_t MAX_GPRS_RETRIES = 3;
const uint8_t MAX_GSM_ERRORS = 3;

// ==================== Objects ====================
TinyGsm modem(SerialAT);
TinyGsmClient mqttClient(modem);
PubSubClient mqtt(mqttClient);

// ==================== Helper Functions ====================
String mqttStateToText(int state) {
    switch (state) {
        case 0:   return "Connected";
        case -1:  return "Connection Timeout";
        case -2:  return "Connection Lost";
        case -3:  return "Connect Failed";
        case -4:  return "Disconnected";
        case -5:  return "Bad Protocol";
        case -6:  return "Bad Client ID";
        case -7:  return "Unavailable";
        case -8:  return "Bad Credentials";
        case -9:  return "Unauthorized";
        default:  return "Unknown";
    }
}

// ==================== GSM Setup ====================
void GSM_setup() {
    pinMode(MODEM_PWR, OUTPUT);
    digitalWrite(MODEM_PWR, LOW);

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(1000);

    mqttMutex = xSemaphoreCreateMutex();
    modemMutex = xSemaphoreCreateMutex();
    taskSuspendMutex = xSemaphoreCreateMutex();

    SerialMon.printf("MQTT_MAX_PACKET_SIZE = %d\n", MQTT_MAX_PACKET_SIZE);

    xTaskCreatePinnedToCore(
        networkTask,
        "NetworkTask",
        NETWORK_TASK_STACK,
        NULL,
        NETWORK_TASK_PRIORITY,
        &networkTaskHandle,
        1
    );
}

// ==================== Network Functions ====================
bool powerCycleModem() {
    SerialMon.println("NetworkTask: Power cycling modem...");
    
    digitalWrite(MODEM_PWR, LOW);
    delay(1200);
    digitalWrite(MODEM_PWR, HIGH);
    delay(5000);
    
    SerialAT.println("AT");
    delay(100);
    
    String response = SerialAT.readString();
    if (response.indexOf("OK") == -1) {
        SerialMon.println("NetworkTask: Modem AT test failed");
        return false;
    }
    
    SerialMon.println("NetworkTask: Modem powered on successfully");
    return true;
}

bool initializeModem() {
    sendLedCommand(LED_GSM_INIT);
    updateStatus("GSM Init...");

    SerialMon.println("NetworkTask: Initializing modem...");
    
    if (!modem.restart()) {
        SerialMon.println("NetworkTask: Modem restart failed");
        return false;
    }
    
    String modemInfo = modem.getModemInfo();
    SerialMon.print("NetworkTask: Modem Info: ");
    SerialMon.println(modemInfo);
    
    SerialMon.println("NetworkTask: Waiting for network...");
    
    if (!modem.waitForNetwork(180000)) {
        SerialMon.println("NetworkTask: Network registration timeout");
        return false;
    }
    
    SerialMon.println("NetworkTask: Network registered");
    return true;
}

bool connectToGPRS() {
    sendLedCommand(LED_CONNECTING);
    updateStatus("GSM Conn...");

    SerialMon.println("NetworkTask: Connecting to GPRS...");
    
    if (modem.isGprsConnected()) {
        modem.gprsDisconnect();
        delay(1000);
    }
    
    if (!modem.gprsConnect(apn, user, pass)) {
        SerialMon.println("NetworkTask: GPRS connection failed");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));   // ← let PDP context fully activate
    
    SerialMon.printf("GPRS connected - IP: %s, Signal: %d\n",
        modem.getLocalIP().c_str(),
        modem.getSignalQuality());
    
    return true;
}

bool connectToMQTT() {
    sendLedCommand(LED_MQTT_CONNECTING);
    updateStatus("MQTT Conn...");

    SerialMon.println("NetworkTask: Connecting to MQTT...");

    String clientId = "GMS-" + DEVICE_ID;
    bool connected = mqtt.connect(clientId.c_str(), mqttUser, mqttPass);

    if (!connected) {
        connected = mqtt.connect(clientId.c_str(), mqttUser, mqttPass);
    }

    for(int i = 0; i < 15 && !connected; i++) {
        SerialMon.printf("NetworkTask: MQTT initial connect retry %d\n", i+1);
        if (!connected) {
            connected = mqtt.connect(clientId.c_str(), mqttUser, mqttPass);
        }
        else {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (connected) {
        SerialMon.println("NetworkTask: MQTT connected");
        // String fullSubTopic = MQTT_SUB + DEVICE_ID;
        // mqtt.subscribe(fullSubTopic.c_str());
        // SerialMon.printf("NetworkTask: Subscribed to: %s\n", fullSubTopic.c_str());

        mqtt.subscribe(MQTT_SUB.c_str());
        SerialMon.printf("Subscribed to: %s\n", MQTT_SUB.c_str());
    } else {
        Serial.printf("MQTT Connect failed (%d %s)\n", 
                        mqtt.state(), mqttStateToText(mqtt.state()).c_str());
    }

    return connected;
}

// ==================== Network Task ====================
// ==================== Network Task ====================
void networkTask(void* parameter) {
    mqtt.setServer(broker, 1883);
    mqtt.setKeepAlive(30);
    mqtt.setSocketTimeout(15);
    mqtt.setCallback(mqttCallback);

    SerialMon.println("Network Task started");
    sendLedCommand(LED_OFFLINE);
    updateStatus("Dev OFFLINE");

    unsigned long lastSuccessfulOperation = 0;
    uint8_t consecutiveMqttFailures = 0;
    const uint8_t MAX_CONSECUTIVE_MQTT_FAILURES = 10;

    while (1) {
        // --------------------------------------------------
        // Skip while OTA is running
        // --------------------------------------------------
        if (otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        gprsConnected = false;
        mqttConnected = false;
        deviceOnline = false;
        consecutiveMqttFailures = 0;

        SerialMon.println("\n=== NETWORK: Starting connection sequence ===");

        // --------------------------------------------------
        // PHASE 1: Modem power cycle (if needed)
        // --------------------------------------------------
        if (!modemWasInitialized || (millis() - lastSuccessfulOperation > 300000)) {
            SerialMon.println("NetworkTask: Initial modem power cycle...");
            if (!powerCycleModem()) {
                SerialMon.println("NetworkTask: Modem power cycle failed!");
                delay(2000);
            }
        }

        // --------------------------------------------------
        // PHASE 2: Modem initialization with retries
        // --------------------------------------------------
        int modemInitRetries = 0;
        while (!initializeModem() && modemInitRetries < MAX_MODEM_INIT_RETRIES) {
            modemInitRetries++;
            SerialMon.printf("NetworkTask: Modem init failed, retry %d/%d\n",
                             modemInitRetries, MAX_MODEM_INIT_RETRIES);
            if (modemInitRetries == 1) {
                SerialMon.println("NetworkTask: Trying modem soft reset...");
                modem.restart();
                delay(3000);
            }
            delay(5000);
        }

        if (modemInitRetries >= MAX_MODEM_INIT_RETRIES) {
            SerialMon.println("NetworkTask: Max modem init retries reached! ESP restarting...");
            delay(2000);
            ESP.restart();
        }

        modemWasInitialized = true;
        SerialMon.println("NetworkTask: Modem initialized successfully");

        // --------------------------------------------------
        // PHASE 3: GPRS + MQTT connect loop
        // --------------------------------------------------
        int gprsRetries = 0;
        int gsmErrorCount = 0;
        unsigned long connectionStartTime = millis();

        while (true) {
            if (millis() - connectionStartTime > 300000) {
                SerialMon.println("NetworkTask: Connection sequence timeout, restarting...");
                break;
            }

            // -------- GPRS connect with retries --------
            if (!connectToGPRS()) {
                gprsRetries++;
                SerialMon.printf("NetworkTask: GPRS connect failed, retry %d/%d\n",
                                 gprsRetries, MAX_GPRS_RETRIES);

                if (gprsRetries >= MAX_GPRS_RETRIES) {
                    gsmErrorCount++;
                    SerialMon.printf("NetworkTask: GSM Error count: %d/%d\n",
                                     gsmErrorCount, MAX_GSM_ERRORS);

                    if (gsmErrorCount >= MAX_GSM_ERRORS) {
                        SerialMon.println("NetworkTask: Max GSM errors! ESP restarting...");
                        delay(2000);
                        ESP.restart();
                    }

                    unsigned long lastGsmErrorTime = millis();
                    while (millis() - lastGsmErrorTime < GSM_ERROR_RETRY_DELAY) {
                        sendLedCommand(LED_OFFLINE);
                        updateStatus("Dev OFFLINE");
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    gprsRetries = 0;
                }
                delay(5000);
                continue;
            }

            gprsConnected = true;
            SerialMon.println("NetworkTask: GPRS connected");
            vTaskDelay(pdMS_TO_TICKS(500));
            lastSuccessfulOperation = millis();

            // -------- MQTT connect + main loop --------
            while (gprsConnected) {

                // Connect to MQTT with retries
                if (!connectToMQTT()) {
                    consecutiveMqttFailures++;
                    SerialMon.printf("NetworkTask: MQTT connection failed %d/%d, rc=%d\n",
                                     consecutiveMqttFailures,
                                     MAX_CONSECUTIVE_MQTT_FAILURES,
                                     mqtt.state());

                    if (consecutiveMqttFailures >= MAX_CONSECUTIVE_MQTT_FAILURES) {
                        SerialMon.println("NetworkTask: Too many MQTT failures, restarting GPRS...");
                        modem.gprsDisconnect();
                        gprsConnected = false;
                        consecutiveMqttFailures = 0;
                        delay(2000);
                        break;
                    }
                    delay(3000);
                    continue;
                }

                // MQTT connected
                consecutiveMqttFailures = 0;
                mqttConnected = true;
                deviceOnline = true;
                sendLedCommand(LED_ONLINE);
                updateStatus("Device ONLINE");

                SerialMon.println("NetworkTask: Device ONLINE");
                lastSuccessfulOperation = millis();

                // Send initial heartbeat + energy data
                publishHeartbeat();
                vTaskDelay(pdMS_TO_TICKS(1000));
                // publishEnergyData();
                readAllSensorsAndPublish();
                vTaskDelay(pdMS_TO_TICKS(100));

                unsigned long lastConnectionCheck = millis();
                unsigned long lastKeepalive = millis();
                const unsigned long CONNECTION_CHECK_INTERVAL = 10000;
                const unsigned long KEEPALIVE_INTERVAL = 30000;

                // ==========================================
                // MAIN CONNECTED LOOP
                // ==========================================
                while (gprsConnected && mqttConnected) {

                    // ---- 1. Process incoming MQTT ----
                    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(200))) {
                        mqtt.loop();
                        xSemaphoreGive(mqttMutex);
                    }

                    // ---- 2. Verify MQTT is still alive ----
                    bool alive = false;
                    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(200))) {
                        alive = mqtt.connected();
                        xSemaphoreGive(mqttMutex);
                    }
                    if (!alive) {
                        SerialMon.println("NetworkTask: MQTT lost, reconnecting...");
                        mqttConnected = false;
                        deviceOnline = false;
                        break;
                    }

                    // ---- 3. Drain ONE message from queue ----
                    MQTTMessage msg = {};
                    if (xQueueReceive(mqttPublishQueue, &msg, 0) == pdTRUE) {
                        bool published = false;

                        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(500))) {
                            size_t payloadLen = strlen(msg.payload);

                            if (mqtt.beginPublish(msg.topic, payloadLen, false)) {
                                const char* p = msg.payload;
                                size_t remaining = payloadLen;
                                bool writeOk = true;

                                while (remaining > 0) {
                                    size_t chunk = (remaining > 64) ? 64 : remaining;
                                    size_t written = mqtt.write((const uint8_t*)p, chunk);

                                    if (written != chunk) {
                                        writeOk = false;
                                        break;
                                    }
                                    p += chunk;
                                    remaining -= chunk;
                                    vTaskDelay(1);
                                }

                                if (writeOk) {
                                    published = mqtt.endPublish();
                                }
                            }
                            xSemaphoreGive(mqttMutex);
                        }

                        if (published) {
                            SerialMon.printf("NetworkTask: Published - %s (%u bytes)\n",
                                             msg.topic, (unsigned)strlen(msg.payload));
                        } else {
                            SerialMon.printf("NetworkTask: Publish FAILED (state=%d, connected=%d)\n",
                                             mqtt.state(), mqtt.connected());

                            if (msg.retries < 2) {
                                msg.retries++;
                                xQueueSendToFront(mqttPublishQueue, &msg, 0);
                            } else {
                                SerialMon.println("NetworkTask: dropping after retries");
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }

                    // ---- 4. Periodic connection check ----
                    if (millis() - lastConnectionCheck >= CONNECTION_CHECK_INTERVAL) {
                        lastConnectionCheck = millis();

                        if (!modem.isGprsConnected()) {
                            SerialMon.println("NetworkTask: GPRS connection lost");
                            gprsConnected = false;
                            mqttConnected = false;
                            deviceOnline = false;
                            sendLedCommand(LED_OFFLINE);
                            updateStatus("Dev OFFLINE");
                            break;
                        }

                        bool mqttAlive = false;
                        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(500))) {
                            mqttAlive = mqtt.connected();
                            xSemaphoreGive(mqttMutex);
                        }
                        if (!mqttAlive) {
                            SerialMon.println("NetworkTask: MQTT connection lost");
                            mqttConnected = false;
                            deviceOnline = false;
                            break;
                        }

                        lastSuccessfulOperation = millis();
                    }

                    // ---- 5. Periodic keepalive ----
                    if (millis() - lastKeepalive >= KEEPALIVE_INTERVAL) {
                        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(1000))) {
                            mqtt.loop();
                            xSemaphoreGive(mqttMutex);
                        }
                        lastKeepalive = millis();
                    }

                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                // ==========================================
                // END MAIN CONNECTED LOOP
                // ==========================================

                // ---- GPRS lost: break out to reconnect from GPRS ----
                if (!gprsConnected) {
                    mqttConnected = false;
                    deviceOnline = false;
                    sendLedCommand(LED_OFFLINE);
                    updateStatus("Dev OFFLINE");
                    SerialMon.println("NetworkTask: GPRS lost, restarting connection...");
                    break;
                }

                // ---- Only MQTT lost: reconnect MQTT within same GPRS ----
                if (!mqttConnected) {
                    deviceOnline = false;
                    SerialMon.println("NetworkTask: MQTT lost, attempting reconnect...");

                    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(1000))) {
                        mqtt.disconnect();
                        xSemaphoreGive(mqttMutex);
                    }
                    delay(2000);
                    // loop continues -> connectToMQTT() retried
                }
            }

            // ---- Cleanup before GPRS restart ----
            if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(1000))) {
                mqtt.disconnect();
                xSemaphoreGive(mqttMutex);
            }

            if (modem.isGprsConnected()) {
                modem.gprsDisconnect();
                delay(1000);
            }

            gprsConnected = false;
            mqttConnected = false;
            deviceOnline = false;
            sendLedCommand(LED_OFFLINE);
            updateStatus("Dev OFFLINE");

            SerialMon.println("NetworkTask: Connection lost, restarting...");
            delay(3000);
            break;
        }

        SerialMon.println("NetworkTask: Restarting connection sequence...");
        delay(5000);
    }
}

// ==================== OTA Task ====================
void otaTask(void* parameter) {
    SerialMon.println("OTA Task started");
    sendLedCommand(LED_OTA_IN_PROGRESS);
    updateStatus("OTA Running...");

    suspendAllTasks();

    bool otaOk = false;

    if (checkForNewFirmware()) {
        if (xSemaphoreTake(modemMutex, portMAX_DELAY)) {
            otaOk = performOTAUpdate();
            xSemaphoreGive(modemMutex);
        }
    }

    if (otaOk) {
        Serial.println("[OTA] Success, rebooting");
        delay(1500);
        ESP.restart();
    }

    modem.gprsDisconnect();
    vTaskDelay(pdMS_TO_TICKS(2000));

    resumeAllTasks();

    otaTaskHandle = NULL;
    vTaskDelete(NULL);
}

void createOTATask() {
    if (otaTaskHandle == NULL) {
        xTaskCreatePinnedToCore(
            otaTask,
            "OTATask",
            OTA_TASK_STACK,
            NULL,
            OTA_TASK_PRIORITY,
            &otaTaskHandle,
            1
        );
        SerialMon.println("OTA Task created");
    }
}

void deleteOTATask() {
    if (otaTaskHandle != NULL) {
        vTaskDelete(otaTaskHandle);
        otaTaskHandle = NULL;
        SerialMon.println("OTA Task deleted");
    }
}
