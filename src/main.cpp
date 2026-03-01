#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <FS.h>               // SPIFFS library
#include "credentials.h"
#include "eventLoop.h"
#include "html.h"

#define MAX_JSON_SIZE     400
#define WIFI_FILE         "/configWifi.json"

const uint8_t ALLOWED_PINS[] = {4, 5, 12, 13, 14, 16};
const uint8_t ALLOWED_PINS_COUNT = sizeof(ALLOWED_PINS) / sizeof(ALLOWED_PINS[0]);
bool pinInitialized[17] = {false};   // GPIO 0..16
bool pinState[17] = {false};         // last known output state


// Initialize the ESP8266 server on port 80
ESP8266WebServer server(80);

void startAccessPoint();

String ssid;
String password;

int loadWifiSettings(){
  Serial.println("Load wifi settings");
  File file = SPIFFS.open(WIFI_FILE, "r");
  if (file) {
    ssid      = file.readStringUntil('\n');
    password  = file.readStringUntil('\n');
    ssid.trim();
    password.trim();
    file.close();

    Serial.println("Wi-Fi credentials loaded from file.");
    Serial.println("SSID: ");
    Serial.println(ssid);
    Serial.println("password: ");
    Serial.println(password);
    return 1;
  } else {
    Serial.println("Failed to open Wi-Fi credentials file.");
    return 0;
  }
}

bool isAllowedPin(int pin) {
  for (uint8_t i = 0; i < ALLOWED_PINS_COUNT; i++) {
    if (ALLOWED_PINS[i] == pin) return true;
  }
  return false;
}

void initOutputPin(int pin) {
  if (pinInitialized[pin]) return;

  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);        // default safe state
  pinState[pin] = LOW;

  pinInitialized[pin] = true;
}


void handleTogglePin() {

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

  // Toggle
  pinState[pin] = !pinState[pin];
  digitalWrite(pin, pinState[pin]);

  String msg = "GPIO ";
  msg += pin;
  msg += " is now ";
  msg += (pinState[pin] ? "ON" : "OFF");

  server.send(200, "text/plain", msg);
}


void handleWifiConfig() {
  if (server.method() == HTTP_POST) {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      ssid      = server.arg("ssid");
      password  = server.arg("password");

      // Save credentials to SPIFFS file
      File file = SPIFFS.open(WIFI_FILE, "w");
      if (file) {
        file.println(ssid);
        file.println(password);
        file.close();
        Serial.println("Wi-Fi credentials saved.");

        // Reboot to apply new settings
        ESP.restart();
      } else {
        server.send(500, "text/plain", "Failed to save Wi-Fi credentials.");
      }
    } else {
      server.send(400, "text/plain", "Missing ssid or password");
    }
  } else {
    server.send(200, "text/html", wifiHtml);
  }
}

// Function to serve a file via HTTP
void handleFileRequest() {
  String path = server.arg("file"); // Get 'file' parameter from the query string

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

  String contentType = "text/plain"; // Default content type
  if (path.endsWith(".html")) contentType = "text/html";
  else if (path.endsWith(".css")) contentType = "text/css";
  else if (path.endsWith(".js")) contentType = "application/javascript";
  else if (path.endsWith(".json")) contentType = "application/json";

  server.streamFile(file, contentType);
  file.close();
}

void handleSaveConfig() {
   if (server.method() == HTTP_POST) {
    // Check if the request has a JSON payload
    if (server.hasArg("plain") == false) {
      server.send(400, "text/plain", "Bad Request: No JSON payload found");
      return;
    }

    // Get JSON data from the request body
    String json = server.arg("plain");
    String err  = eventLoopNewFile(json);
    if(err.length() > 0){
      server.send(500, "text/plain", err);
    }

    Serial.println("Config saved successfully!");
    // Respond to the client
    server.send(200, "application/json", "{\"status\": \"success\"}");
  }else{
    // GET method → return html page with file content
    String fileContent = "[]";
    File file = SPIFFS.open(CONFIG_FILE, "r");
    if (file) {
      fileContent = file.readString();
      file.close();
    }

    String page = settingsHtml;
    page.replace("{{FILE_CONTENT}}", fileContent);

    server.send(200, "text/html", page);
  }
}

void handleStatus() {
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

  if (!SPIFFS.exists(CONFIG_FILE)) {
    msg += "❌ Not found\n";
  } else {
    File file = SPIFFS.open(CONFIG_FILE, "r");
    if (!file) {
      msg += "❌ Failed to open\n";
    } else {
      String content = file.readString();
      file.close();

      // Avoid Telegram overflow
      if (content.length() > 800) {
        content = content.substring(0, 800);
        content += "\n... (truncated)";
      }

      msg += content + "\n";
    }
  }

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
    msgTelegram(buildDeviceInfoMessage());
  } else {
    Serial.println("\nFailed to connect to Wi-Fi.");
    startAccessPoint();
  }
}

// Start the access point for configuring Wi-Fi credentials
void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("waterGuy", "~~~~~~~~");  // AP with SSID and password
  Serial.println("Access point started.");
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());
  delay(500);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Initialize SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }

  eventScheduleSetup();
  startAccessPoint();

  // Set up the server to handle POST requests to /saveConfig
  server.on("/get-file", HTTP_GET, handleFileRequest);
  server.on("/saveConfig",  handleSaveConfig);
  server.on("/",            handleWifiConfig);
  server.on("/toggle", HTTP_GET, handleTogglePin);
  server.on("/status", HTTP_GET, handleStatus);

  // Start the server
  server.begin();
  Serial.println("Server started!");
}

bool hasInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;

  http.setTimeout(3000);
  http.begin(client, "http://clients3.google.com/generate_204");
  int code = http.GET();
  http.end();

  return (code == 204);
}

void handleInternetFailure(){
  WiFi.mode(WIFI_OFF);
  delay(1000);
  connectToWiFi();
}

unsigned long lastInternetCheck = 0;
const unsigned long INTERNET_CHECK_INTERVAL = 60 * 1000; // 1 min
void checkInternetLoop(){
  if (millis() - lastInternetCheck > INTERNET_CHECK_INTERVAL) {
  lastInternetCheck = millis();

    if (!hasInternet()) {
      handleInternetFailure();
    }
  }

}

// Define an enum with typedef (or using alias)
typedef enum {
    INIT,
    RUNNING
} SystemState;
SystemState waterGuyState = INIT;
void loop() {
  server.handleClient();

  switch (waterGuyState) {
      case INIT:
          if(millis() > (60000) && !WiFi.softAPgetStationNum()){
            // Check if Wi-Fi credentials file exists
            Serial.println("Getting out init state");
            if (SPIFFS.exists(WIFI_FILE)) {
              loadWifiSettings();
              connectToWiFi();
            }

            waterGuyState = RUNNING;
          }
          break;
      case RUNNING:
          //normal operation
          eventScheduleLoop();
          // checkInternetLoop();
          break;
  }
}
