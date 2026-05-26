#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>

// Call once after the initial WiFi connect succeeds. Stores credentials so
// the loop can cycle the connection on its own when the internet drops.
void networkBegin(const String& ssid, const String& password);

// Non-blocking. Periodically tests upstream reachability; on failure cycles
// WiFi (disconnect → reconnect → re-test) and retries every minute until
// internet comes back. Each call performs at most one short step.
void networkLoop();

#endif
