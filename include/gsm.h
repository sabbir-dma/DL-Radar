#pragma once

// This code is designed to run on an ESP32 device with a SIM7600 GSM module.
#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_USE_GPRS true
#define TINY_GSM_USE_WIFI false
#define MQTT_MAX_PACKET_SIZE 512
#define TINY_GSM_RX_BUFFER 512
#define TINY_GSM_YIELD() { delay(1); }

#include <Arduino.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <Update.h>   // if needed for OTA, otherwise remove
#include "led.h"
#include "display.h"
#include "config.h"   // for SerialAT, SerialMon, DEVICE_ID, MQTT_*, etc.

// ==================== Configuration ====================
extern const char apn[];
extern const char user[];
extern const char pass[];

// MQTT Configuration
extern const char* broker;
extern const char* mqttUser;
extern const char* mqttPass;

// OTA URLs
extern String firmwareUrl;
extern const char* defaultFirmwareUrl;

// ==================== Task Handles ====================
extern TaskHandle_t networkTaskHandle;
extern TaskHandle_t otaTaskHandle;

// ==================== Mutexes Handles ====================
extern SemaphoreHandle_t mqttMutex;
extern SemaphoreHandle_t modemMutex;
extern SemaphoreHandle_t taskSuspendMutex;

// ==================== Task Priorities ====================
#define NETWORK_TASK_PRIORITY 3
#define OTA_TASK_PRIORITY 4

// ==================== Task Stack Sizes ====================
#define NETWORK_TASK_STACK 8192
#define OTA_TASK_STACK 16384

// ==================== Counters ====================
extern uint8_t modemInitRetries;
extern uint8_t gprsRetries;
extern uint8_t gsmErrorCount;
extern const uint8_t MAX_MODEM_INIT_RETRIES;
extern const uint8_t MAX_GPRS_RETRIES;
extern const uint8_t MAX_GSM_ERRORS;

// ==================== Objects ====================
extern TinyGsm modem;
extern TinyGsmClient mqttClient;
extern PubSubClient mqtt;

// ==================== Function Prototypes ====================
void GSM_setup();

bool powerCycleModem();
bool initializeModem();
bool connectToGPRS();
bool connectToMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishHeartbeat();
bool checkForNewFirmware();
bool performOTAUpdate();

void networkTask(void* parameter);
void otaTask(void* parameter);

void suspendAllTasks();
void resumeAllTasks();
void createOTATask();
void deleteOTATask();

String mqttStateToText(int state);