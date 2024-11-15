#include "eventLoop.h"
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>   // Universal Telegram Bot Library written by Brian Lough: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot
#include <FS.h>               // SPIFFS library
#include "credentials.h"

#ifdef ESP8266
  X509List cert(TELEGRAM_CERTIFICATE_ROOT);
#endif
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

const int   relayPin        = 2;
bool        relayState      = LOW;

unsigned long eventLoopLastMills  = 0;
unsigned long eventLoopCounter    = 0;

void eventScheduleSetup(){
  configTime(0, 0, "pool.ntp.org");      // get UTC time via NTP

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, relayState);
  
  client.setTrustAnchors(&cert); // Add root certificate for api.telegram.org
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
  updateCounterLoop();

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

String eventLoopNewFile(String json){
    // Deserialize JSON
    StaticJsonDocument<MAX_JSON_SIZE> jsonDoc; // Adjust size based on expected JSON data
    DeserializationError error = deserializeJson(jsonDoc, json);

    // Check if there was an error in parsing JSON
    if (error) {
      Serial.print("JSON parsing failed: ");
      Serial.println(error.c_str());
      String err = "Bad Request: Invalid JSON";
      return err;
    }

    // Open the file for writing
    File configFile = SPIFFS.open(CONFIG_FILE, "w");
    if (!configFile) {
      Serial.println("Failed to open file for writing");
      String err = "Internal Server Error: Could not open file";
      return err;
    }

    // Serialize JSON to the file
    if (serializeJson(jsonDoc, configFile) == 0) {
      Serial.println("Failed to write to file");
      String err = "Internal Server Error: Could not write to file";
      configFile.close();
      return err;
    }
    configFile.close(); // Close the file after writing

    return "";
}