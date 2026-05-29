#ifndef UPTIME_H
#define UPTIME_H

#include <Arduino.h>

#define UPTIME_FILE "/uptime.json"

void     uptimeSetup();
void     uptimeLoop();

// Total seconds since boot. Overflow-safe across the ~49.7-day millis() wrap.
uint32_t uptimeCurrentSec();

// All-time record (in seconds) persisted on SPIFFS.
uint32_t uptimeRecordSec();

// "⏱ Up for 0d 3h. Record: 12d 7h" — two-line block for /status.
String   uptimeBuildStatus();

#endif
