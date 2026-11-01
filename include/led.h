// led.h
#pragma once

#include <Arduino.h>
#include <FastLED.h>
#include "config.h"   // Assumes this contains deviceOnline, gprsConnected, etc.

// ============================================================
// LED Hardware Configuration
// ============================================================
#define LED_PIN         4
#define NUM_LEDS        1
#define LED_TASK_PRIORITY      1
#define LED_CMD_QUEUE_SIZE     10
#define LED_TASK_STACK         2048

// ============================================================
// LED Command Types
// ============================================================
enum LedCommandType {
    LED_OFFLINE,
    LED_CONNECTING,
    LED_GSM_INIT,
    LED_MQTT_CONNECTING,
    LED_ONLINE,
    LED_HEARTBEAT,
    LED_MQTT_RECEIVE,
    LED_BUTTON_PRESS,
    LED_BUTTON_HOLD_3SEC,
    LED_BUTTON_HOLD_5SEC,
    LED_BUTTON_HOLD_10SEC,
    LED_OTA_IN_PROGRESS,
    LED_PING_ACK,
    LED_RF_CMD,
    LED_RF_ACK,
    LED_RF_HB,
};

// ============================================================
// LED Command Structure
// ============================================================
struct LedCommand {
    LedCommandType type;
    uint8_t brightness;
};

// ============================================================
// Global Extern Declarations
// ============================================================
extern CRGB leds[NUM_LEDS];
extern TaskHandle_t ledTaskHandle;
extern QueueHandle_t ledCommandQueue;

// ============================================================
// Function Prototypes
// ============================================================
void FastLED_setup();
void sendLedCommand(LedCommandType type);
void ledTask(void* parameter);