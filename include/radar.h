#pragma once

#include <Arduino.h>
#include <SDI12.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum RadarCmdType {
    RCMD_FACTORY_RESET,
    RCMD_SET_POWER_HIGH, RCMD_SET_POWER_LOW, RCMD_GET_POWER,
    RCMD_SET_UNIT_M, RCMD_SET_UNIT_FT, RCMD_GET_UNIT,
    RCMD_SET_SAMPLES, RCMD_GET_SAMPLES,
    RCMD_SET_STAGE, RCMD_GET_STAGE
};
struct RadarCmd {
    RadarCmdType type;
    float        value;     // samples / stage height
};

extern char SENSOR_ADDR;
extern QueueHandle_t radarCmdQueue;

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

// Queue a sensor command; the radar task runs it and publishes the reply over MQTT.
bool radarSendCommand(RadarCmdType type, float value = 0);
void radarBusHold(bool on);       // hold/release the SDI-12 bus (see suspendAllTasks)
uint32_t radarLastMeasureMs();   // duration of the last good measurement (ms)
