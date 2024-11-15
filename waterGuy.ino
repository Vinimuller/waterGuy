#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>   // Universal Telegram Bot Library written by Brian Lough: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot
#include <ArduinoJson.h>
#include <FS.h>               // SPIFFS library
#include "credentials.h"

#define EVENT_LOOP_TICK   60000 // 1 minute
#define EVENT_LOOP_MAX    10080 // 10080 minutes -> 1 week
#define MAX_JSON_SIZE     400
#define CONFIG_FILE       "/config.json"
#define WIFI_FILE         "/configWifi.json"

#ifdef ESP8266
  X509List cert(TELEGRAM_CERTIFICATE_ROOT);
#endif

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Initialize the ESP8266 server on port 80
ESP8266WebServer server(80);

String ssid;
String password;

const int   relayPin        = 2;
bool        relayState      = LOW;

unsigned long eventLoopLastMills  = 0;
unsigned long eventLoopCounter    = 0;
wl_status_t lastWifiState         = WL_DISCONNECTED;
void connectWifiLoop(){
  // Reconnect if Wi-Fi connection is lost
  if (WiFi.status() != WL_CONNECTED && !WiFi.softAPgetStationNum()) {
    startAccessPoint();
  }
}

void msgTelegram(String msg){
  if(WiFi.status() == WL_CONNECTED){
    Serial.println("Sending Telegram");
    bot.sendMessage(CHAT_ID, msg, "");
  }
}

unsigned long lastUpdateCounterLoop = 0;
void updateCounterLoop(){
  unsigned long nowMills = millis();

  if(nowMills - lastUpdateCounterLoop >= 60000){
    lastUpdateCounterLoop = nowMills;
    
    // Get current time
    time_t now = time(nullptr) - (3*3600);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo); // Convert time_t to struct tm in UTC

    if (timeinfo.tm_year > (1970 - 1900)) { // Check if valid time received
      Serial.printf("Current UTC date and time: %04d-%02d-%02d %02d:%02d:%02d wday: %d\n",
                    timeinfo.tm_year + 1900,
                    timeinfo.tm_mon + 1,
                    timeinfo.tm_mday,
                    timeinfo.tm_hour,
                    timeinfo.tm_min,
                    timeinfo.tm_sec,
                    timeinfo.tm_wday);

      unsigned long newEventLoopCounter = (timeinfo.tm_wday * 24 * 60) + (timeinfo.tm_hour)*60 + timeinfo.tm_min;

      Serial.print("New event loop counter: ");
      Serial.println(newEventLoopCounter);

      eventLoopCounter = newEventLoopCounter;
    } else {
      Serial.println("Time not set yet.");
    }
  }
}

void eventScheduleLoop(){
  unsigned long now = millis();

  if(now - eventLoopLastMills >= EVENT_LOOP_TICK){
    eventLoopLastMills = now;
    Serial.println("-----------------");
    Serial.print("Event loop tick: ");
    Serial.println(eventLoopCounter);

    // Open the file for reading
    File file = SPIFFS.open(CONFIG_FILE, "r");
    if (!file) {
      Serial.println("Failed to open config file");
      return;
    }

    // Allocate the JSON document
    StaticJsonDocument<MAX_JSON_SIZE> doc;  // Adjust size according to your JSON size
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
      Serial.print("Failed to parse JSON: ");
      Serial.println(error.c_str());
      return;
    }

    // Get the array from the JSON document
    JsonArray array = doc.as<JsonArray>();
    for (JsonObject obj : array) {
      int time    = obj["time"];
      int pin     = obj["pin"];
      int val     = obj["val"];
      // int att     = obj["att"];

      if(time == eventLoopCounter){
        Serial.print("Time: ");
        Serial.print(time);
        Serial.print(" pin: ");
        Serial.print(pin);
        Serial.print(" value: ");
        Serial.print(val);
        Serial.println("");

        digitalWrite(pin, val);

        String msg = "Time: " + String(time) + " Pin: " + String(pin) + " Value: " + String(val);
        msgTelegram(msg);
      }
    }
    
    eventLoopCounter++;
    if(eventLoopCounter > EVENT_LOOP_MAX) {
      eventLoopCounter = 0;
      String msg = "Reset evenLoopCounter to 0";
      msgTelegram(msg);
    }
  }
}

int loadWifiSettings(){
  Serial.println("Load wifi settings");
  File file = SPIFFS.open(WIFI_FILE, "r");
  if (file) {
    ssid = file.readStringUntil('\n');
    password = file.readStringUntil('\n');
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

void handleRequest() 
{
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
    const char * formHtml = "<html><head> <link rel=\"stylesheet\"</head><body> <div class=\"wrapper\"> <form class=\"form-signin\" method=\"POST\"> <h2 class=\"form-signin-heading\">~Water Guy Wi-Fi~</h2> <input type=\"text\" class=\"form-control\" name=\"ssid\" placeholder=\"SSID\" required=\"\" autofocus=\"\"/> <input type=\"password\" class=\"form-control\" name=\"password\" placeholder=\"Password\" required=\"\"/> <button class=\"btn btn-lg btn-primary btn-block\" type=\"submit\">Save</button> </form> </div></body></html> ";
    server.send(200, "text/html", formHtml);
  }
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

    // Deserialize JSON
    StaticJsonDocument<MAX_JSON_SIZE> jsonDoc; // Adjust size based on expected JSON data
    DeserializationError error = deserializeJson(jsonDoc, json);

    // Check if there was an error in parsing JSON
    if (error) {
      Serial.print("JSON parsing failed: ");
      Serial.println(error.c_str());
      server.send(400, "text/plain", "Bad Request: Invalid JSON");
      return;
    }

    // Open the file for writing
    File configFile = SPIFFS.open(CONFIG_FILE, "w");
    if (!configFile) {
      Serial.println("Failed to open file for writing");
      server.send(500, "text/plain", "Internal Server Error: Could not open file");
      return;
    }

    // Serialize JSON to the file
    if (serializeJson(jsonDoc, configFile) == 0) {
      Serial.println("Failed to write to file");
      server.send(500, "text/plain", "Internal Server Error: Could not write to file");
      configFile.close();
      return;
    }

    configFile.close(); // Close the file after writing
    Serial.println("Config saved successfully!");

    // Respond to the client
    server.send(200, "application/json", "{\"status\": \"success\"}");
  }else{
    const char * formHtml = "<html><head> <link rel=\"stylesheet\"</head><body> <div class=\"wrapper\"> <form class=\"form-signin\" method=\"POST\"> <h2 class=\"form-signin-heading\">~Water Guy Config~</h2> <input type=\"text\" class=\"form-control\" name=\"plain\" placeholder=\"config file\" required=\"\" autofocus=\"\"/> <button class=\"btn btn-lg btn-primary btn-block\" type=\"submit\">Save</button> </form> </div></body></html> ";
    server.send(200, "text/html", formHtml);
  }
}

// Connect to Wi-Fi using stored credentials
void connectToWiFi() {
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

  configTime(0, 0, "pool.ntp.org");      // get UTC time via NTP
  client.setTrustAnchors(&cert); // Add root certificate for api.telegram.org

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, relayState);

    // Check if Wi-Fi credentials file exists
  if (SPIFFS.exists(WIFI_FILE)) {
    loadWifiSettings();
    connectToWiFi();
  } else {
    startAccessPoint();
  }
  
  // Set up the server to handle POST requests to /saveConfig
  server.on("/saveConfig", handleSaveConfig);
  server.on("/", handleRequest);

  // Start the server
  server.begin();
  Serial.println("Server started!");
}

void loop() {
  connectWifiLoop();
  updateCounterLoop();
  eventScheduleLoop();
  server.handleClient();
}






