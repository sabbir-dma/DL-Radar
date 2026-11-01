#pragma once

#define DISPLAY_WAKE_TIMEOUT_MS   30 * 1000   // or 20000 for 30 seconds
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

#define DISPLAY_PIN 35

extern bool buttonState;

//data logger - AWS
#define OLED_MOSI       23
#define OLED_CLK        18
#define OLED_DC         26
#define OLED_CS         25
#define OLED_RST        2

#define DISPLAY_TASK_PRIORITY   1
#define DISPLAY_CMD_QUEUE_SIZE  10
#define DISPLAY_TASK_STACK      4096

enum DisplayCommandType {
    DISPLAY_BOOT,
    DISPLAY_CONFIG,
    DISPLAY_HOME,
    DISPLAY_UPDATE_HOME,
    DISPLAY_SLEEP,
    DISPLAY_WAKE,
    DISPLAY_CLEAR
};

struct DisplayData {
    float airPressure;
    float airTemperature;
    float airHumidity;
    float windSpeed;
    float windDirection;   // DO
    float rainFall;
    int   signalLevel;       // 0..5
    const char* statusText;
    int   batteryPercent;    // 0..100   ✅ <-- added
};

extern Adafruit_SSD1306 display;
extern DisplayData displayData;
extern TaskHandle_t displayTaskHandle;
extern QueueHandle_t displayCommandQueue;

void Display_setup();
void sendDisplayCommand(DisplayCommandType type);
void displayTask(void* parameter);

// Update functions
void updateAirPressure(float value);
void updateAirTemperature(float value);
void updateBatteryPercentage(int percent);
void updateAirHumidity(float value);
void updateWindSpeed(float value);
void updateWindDirection(float value);
void updateRainFall(float value);
void updateSignalStrength(int level);
void updateStatus(const char* status);
void updateAllDisplayValues(float pressure, float temperature, float humidity, float w_speed, float w_direction, float rainfall, int signal, const char* status);
void displaySleep();
void displayWake();
void displaySetBrightness(uint8_t brightness);
int rssiToSignalLevel(int rssi);