#include "webserver.h"

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <FS.h>

#include "html.h"
#include "irrigation.h"
#include "wifi_config.h"

static ESP8266WebServer server(80);

static const uint8_t ALLOWED_PINS[] = {2, 4, 5, 12, 13, 14, 16};
static const uint8_t ALLOWED_PINS_COUNT = sizeof(ALLOWED_PINS) / sizeof(ALLOWED_PINS[0]);
static bool pinInitialized[17] = {false};
static bool pinState[17] = {false};

static bool isAllowedPin(int pin) {
  for (uint8_t i = 0; i < ALLOWED_PINS_COUNT; i++) {
    if (ALLOWED_PINS[i] == pin) return true;
  }
  return false;
}

static void initOutputPin(int pin) {
  if (pinInitialized[pin]) return;

  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  pinState[pin] = LOW;

  pinInitialized[pin] = true;
}

static void handleTogglePin() {
  if (!server.hasArg("pin")) {
    server.send(400, "text/plain", "Missing pin parameter");
    return;
  }

  int pin = server.arg("pin").toInt();

  if (!isAllowedPin(pin)) {
    server.send(403, "text/plain", "GPIO not allowed");
    return;
  }

  initOutputPin(pin);

  pinState[pin] = !pinState[pin];
  digitalWrite(pin, pinState[pin]);

  String msg = "GPIO ";
  msg += pin;
  msg += " is now ";
  msg += (pinState[pin] ? "ON" : "OFF");

  server.send(200, "text/plain", msg);
}

static void handleWifiConfig() {
  if (server.method() == HTTP_POST) {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      String ssid     = server.arg("ssid");
      String password = server.arg("password");

      if (wifiConfigSave(ssid, password)) {
        ESP.restart();
      } else {
        server.send(500, "text/plain", "Failed to save Wi-Fi credentials.");
      }
    } else {
      server.send(400, "text/plain", "Missing ssid or password");
    }
  } else {
    String page = indexHtml;
    page.replace("{{FILE_CONTENT}}", irrigationReadConfig());
    server.send(200, "text/html", page);
  }
}

static void handleFileRequest() {
  String path = server.arg("file");

  if (path.isEmpty()) {
    server.send(400, "text/plain", "File parameter is missing");
    return;
  }

  if (!SPIFFS.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = SPIFFS.open(path, "r");
  if (!file) {
    server.send(500, "text/plain", "Unable to open file");
    return;
  }

  String contentType = "text/plain";
  if (path.endsWith(".html")) contentType = "text/html";
  else if (path.endsWith(".css")) contentType = "text/css";
  else if (path.endsWith(".js")) contentType = "application/javascript";
  else if (path.endsWith(".json")) contentType = "application/json";

  server.streamFile(file, contentType);
  file.close();
}

static void handleSaveConfig() {
  // Form posts from the index page arrive as application/x-www-form-urlencoded
  // with the JSON in the `config` field; raw clients (curl --data) land in `plain`.
  String json;
  if (server.hasArg("config")) {
    json = server.arg("config");
  } else if (server.hasArg("plain")) {
    json = server.arg("plain");
  } else {
    server.send(400, "text/plain", "Bad Request: No JSON payload found (expected 'config' field or raw body)");
    return;
  }

  String err = irrigationSaveConfig(json);
  if (err.length() > 0) {
    server.send(500, "text/plain", err);
  } else {
    Serial.println("Config saved successfully!");
    server.send(200, "application/json", "{\"status\": \"success\"}");
  }
}

static void handleStatus() {
  StaticJsonDocument<512> doc;
  JsonArray pins = doc.createNestedArray("pins");

  for (uint8_t i = 0; i < ALLOWED_PINS_COUNT; i++) {
    int pin = ALLOWED_PINS[i];

    JsonObject obj = pins.createNestedObject();
    obj["pin"] = pin;
    obj["initialized"] = pinInitialized[pin];
    obj["state"] = pinInitialized[pin] ? pinState[pin] : 0;
  }

  String response;
  serializeJson(doc, response);

  server.send(200, "application/json", response);
}

void webserverSetup() {
  server.on("/get-file",   HTTP_GET,  handleFileRequest);
  server.on("/saveConfig", HTTP_POST, handleSaveConfig);
  server.on("/",                      handleWifiConfig);
  server.on("/toggle",     HTTP_GET,  handleTogglePin);
  server.on("/status",     HTTP_GET,  handleStatus);

  server.begin();
  Serial.println("Server started!");
}

void webserverLoop() {
  server.handleClient();
}
