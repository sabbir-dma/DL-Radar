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

// Index (0-based, after the address char) of the distance value in the M3/D0 reply.
#define RADAR_DISTANCE_FIELD 0
#define RADAR_MAX_VALUES     8

int   radarParseValues(const String &raw, float *out, int maxn);
float radarStageHeight();            // NAN until read from the sensor
bool  radarRefreshStageHeight();
