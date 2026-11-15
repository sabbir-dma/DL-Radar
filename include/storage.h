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

// YYMMDDHHMMSS for an RTC epoch (as returned by storage_getEpoch)
String storage_tsFromEpoch(uint32_t epoch);

// Append one CSV row stamped with ts14 (the schedule slot time)
void storage_logDataAt(const String &sensorName, const String &rawData, const String &ts14);

// Append one CSV row for a sensor reading (stamped with the current RTC time)
void storage_logData(const String &sensorName, const String &rawData);

// Call periodically (e.g., from main loop) to sync RTC from NTP at 00:00:01
void storage_handlePeriodic();

// ---- RTC clock sync ----
// Daily NTP sync is requested automatically (first moment of each new day, and at boot);
// the NetworkTask runs it when online (it owns the modem), retrying every 10 min on failure.
void storage_requestNtpSync(bool replyOverMqtt = false);   // also used by MQTT "rtc_sync"
bool storage_ntpPending();
void storage_serviceNtp();                  // call from NetworkTask while online
bool storage_setRtc(const char *ts12);      // "YYMMDDHHMMSS" (MQTT "set_rtc")
