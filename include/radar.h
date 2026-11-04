#pragma once

#include <Arduino.h>
#include <SDI12.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct RadarRequest {
    bool trigger;
};

struct RadarResult {
    bool success;
    String rawData;
};

extern char SENSOR_ADDR;
extern QueueHandle_t radarRequestQueue;
extern QueueHandle_t radarResultQueue;

void radar_setup();
bool measureM3(String &rawData);
bool factoryReset();
bool setPowerHigh();
bool setPowerLow();
String getPowerMode();
bool setUnitMeters();
bool setUnitFeets();
String getUnit();
bool setSamples(int n);
String getSamples();
bool setStageHeight(float h);
String getStageHeight();