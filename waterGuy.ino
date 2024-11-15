#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <FS.h>               // SPIFFS library
#include "src/credentials.h"
#include "src/eventLoop.h"
#include "src/html.h"

#define MAX_JSON_SIZE     400
#define WIFI_FILE         "/configWifi.json"

// Initialize the ESP8266 server on port 80
ESP8266WebServer server(80);

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
    // Get method. return html page
    server.send(200, "text/html", settingsHtml);
  }
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
    String msg = "Surfing at: " + WiFi.localIP().toString();
    msgTelegram(msg);
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

  // Start the server
  server.begin();
  Serial.println("Server started!");
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
          break;
  }
}






