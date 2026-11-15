#pragma once

#include <Arduino.h>

// Returns "YYMMDDHHMMSS" string
// Currently uses uptime placeholder — replace with RTC/NTP when available
String getTimestamp14();

// Returns current AOS state (true/false)
// Replace with your actual source (deviceArmed, GPS fix, etc.)
bool getAOS();

// "YYMMDDHHMMSS" -> "YY-MM-DD HH:MM:SS" (for log lines)
String prettyTs(const String &ts12);
// current RTC time as "YY-MM-DD HH:MM:SS"
String nowPretty();
