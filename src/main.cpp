#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <FS.h>
#include "credentials.h"
#include "irrigation.h"
#include "telegram.h"
#include "network.h"
#include "webserver.h"
#include "wifi_config.h"

String ssid;
String password;

void startAccessPoint();

String buildDeviceInfoMessage() {
  String msg = "💧 *Water Guy Online*\n\n";

  // --- IP ---
  msg += "📡 IP: ";
  if (WiFi.status() == WL_CONNECTED) {
    msg += WiFi.localIP().toString();
  } else {
    msg += "not connected";
  }
  msg += "\n";

  // --- WiFi configured ---
  msg += "📶 WiFi configured: ";
  msg += (WiFi.SSID().length() > 0) ? "yes\n" : "no\n";

  // --- Telegram commands ---
  msg += "\n🤖 *Telegram commands:*\n";
  msg += "/status\n";
  msg += "/config\n";
  msg += "/config_set\n";
  msg += "/config_save\n";
  msg += "/config_cancel\n";

  // --- Config file ---
  msg += "\n📄 *Config file:*\n";

  String content = irrigationReadConfig();
  if (content.length() > 800) {
    content = content.substring(0, 800);
    content += "\n... (truncated)";
  }
  msg += content + "\n";

  return msg;
}

// Connect to Wi-Fi using stored credentials
void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.println("Connecting to Wi-Fi");

  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 20) {
    delay(500);
    Serial.print(".");
    retryCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to Wi-Fi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org");
    delay(2000);
    telegramSend(buildDeviceInfoMessage());
    networkBegin(ssid, password);
  } else {
    Serial.println("\nFailed to connect to Wi-Fi.");
    startAccessPoint();
  }
}

// Start the access point for configuring Wi-Fi credentials
void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("waterGuy", "~~~~~~~~");
  Serial.println("Access point started.");
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());
  delay(500);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }

  irrigationSetup();
  telegramSetup();
  startAccessPoint();
  webserverSetup();
}

typedef enum {
    INIT,
    RUNNING
} SystemState;
SystemState waterGuyState = INIT;

void loop() {
  webserverLoop();

  switch (waterGuyState) {
      case INIT:
          if(millis() > (15000) && !WiFi.softAPgetStationNum()){
            Serial.println("Getting out init state");
            if (wifiConfigLoad(ssid, password)) {
              connectToWiFi();
            }

            waterGuyState = RUNNING;
          }
          break;
      case RUNNING:
          irrigationLoop();
          telegramLoop();
          networkLoop();
          break;
  }
}
