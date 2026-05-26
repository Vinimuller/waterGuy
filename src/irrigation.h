#ifndef IRRIGATION_H
#define IRRIGATION_H

#include <Arduino.h>

#define EVENT_LOOP_TICK   60000 // 1 minute
#define EVENT_LOOP_MAX    10080 // minutes in a week
#define CONFIG_FILE       "/config.json"
#define MAX_JSON_SIZE     1024

void   irrigationSetup();
void   irrigationLoop();

// Validates + writes /config.json. Returns "" on success, error string on
// failure. Emits ConfigSaved / ConfigError events accordingly.
String irrigationSaveConfig(const String& json);

// Returns the raw contents of /config.json, or "[]" if missing/unreadable.
String irrigationReadConfig();

long   irrigationCurrentMinute();

#endif
