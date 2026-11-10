#pragma once

#include <Arduino.h>

void storage_setup();
bool storage_sd_ready();
bool storage_rtc_ready();
bool storage_ina_ready();

// Seconds since epoch of the RTC clock (0 if RTC missing / time not valid yet)
uint32_t storage_getEpoch();

// Returns "YYMMDDHHMMSS"
String storage_getTimestamp14();

float storage_getSolarVoltage();
float storage_getBatteryVoltage();
int   storage_getCSQ();

// Append one CSV row for a sensor reading
void storage_logData(const String &sensorName, const String &rawData);

// Call periodically (e.g., from main loop) to sync RTC from NTP at 00:00:01
void storage_handlePeriodic();
