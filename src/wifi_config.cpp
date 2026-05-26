#include "wifi_config.h"
#include <FS.h>

#define WIFI_FILE "/configWifi.json"

bool wifiConfigLoad(String& ssid, String& password) {
  File file = SPIFFS.open(WIFI_FILE, "r");
  if (!file) {
    Serial.println("Failed to open Wi-Fi credentials file.");
    return false;
  }

  ssid     = file.readStringUntil('\n');
  password = file.readStringUntil('\n');
  ssid.trim();
  password.trim();
  file.close();

  Serial.println("Wi-Fi credentials loaded from file.");
  Serial.println("SSID: ");
  Serial.println(ssid);
  Serial.println("password: ");
  Serial.println(password);
  return true;
}

bool wifiConfigSave(const String& ssid, const String& password) {
  File file = SPIFFS.open(WIFI_FILE, "w");
  if (!file) return false;

  file.println(ssid);
  file.println(password);
  file.close();
  Serial.println("Wi-Fi credentials saved.");
  return true;
}
