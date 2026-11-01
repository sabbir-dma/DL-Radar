#include "config.h"

// ==================== Global Variable Definitions ====================

bool deviceArmed = true;

QueueHandle_t mqttPublishQueue = NULL;

String DEVICE_ID = "";
String MAC_FALLBACK_ID = "";

unsigned long lastHeartbeatTime = 0;
unsigned long lastDataTime = 0;

unsigned long lastGsmErrorTime = 0;
unsigned long buttonHoldStart = 0;
bool buttonHolding = false;

volatile bool mqttConnected = false;
volatile bool gprsConnected = false;
volatile bool deviceOnline = false;
volatile bool otaInProgress = false;
volatile bool tasksSuspended = false;
bool modemWasInitialized = false;

// MQTT Topics (now dynamic, built at runtime with DEVICE_ID)

String MQTT_PUB;
String MQTT_SUB;


void setupMQTTTopics() {
    MQTT_PUB          = "DMA-DL/PUB/"           + DEVICE_ID;
    MQTT_SUB          = "DMA-DL/SUB/"           + DEVICE_ID;
}


// Helper function definition
String getMACDeviceID() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[13];
    sprintf(macStr, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}