#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <Arduino.h>

// Loads SSID and password from SPIFFS. Returns true on success, false if the
// credentials file is missing or unreadable.
bool wifiConfigLoad(String& ssid, String& password);

// Persists SSID and password to SPIFFS. Returns true on success.
bool wifiConfigSave(const String& ssid, const String& password);

#endif
