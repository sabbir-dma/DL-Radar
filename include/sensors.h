#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// ============================================================
// Channel writer — shared JSON output helper
// ============================================================
struct ChannelWriter {
    char   *buf;
    size_t  bufSize;
    int     len;      // current write position
    bool    first;    // true until first channel written
    bool    anyOk;    // true if any channel was written
    char   raw[256];
};

// Append a successful channel:  {"alias":"...","value":1.23,"unit":"V"}
void appendChannel(ChannelWriter &w,
                   const char *alias,
                   float value,
                   const char *unit);

// Append an error channel:      {"alias":"...","value":"error","unit":"V"}
void appendChannelError(ChannelWriter &w,
                        const char *alias,
                        const char *unit);

// ============================================================
// Dispatcher
// ============================================================
void initSensors();                  // call once in setup()
void readAllSensorsAndPublish();     // call periodically

// ============================================================
// Protocol reader prototypes
// Each protocol implements this signature.
// Return true if at least one channel was appended.
// ============================================================
bool readModbusSensor(JsonObject sensor, ChannelWriter &w);
bool readSDI12Sensor(JsonObject sensor, ChannelWriter &w);
// bool readAnalogSensor(JsonObject sensor, ChannelWriter &w);   // future
// bool readI2CSensor   (JsonObject sensor, ChannelWriter &w);   // future