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

bool telegramConfigEdit = false;
String telegramConfigBuffer = "";

unsigned long lastTelegramCheck = 0;
const unsigned long TELEGRAM_POLL_INTERVAL = 3000; // 3s


void eventScheduleSetup(){
  configTime(0, 0, "pool.ntp.org");      // get UTC time via NTP

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, relayState);
  
  client.setTrustAnchors(&cert); // Add root certificate for api.telegram.org
}

void msgTelegram(String msg){
  if(WiFi.status() == WL_CONNECTED){
    Serial.println("Sending Telegram");
    bot.sendMessage(CHAT_ID, msg, "Markdown");
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

bool validateConfig(JsonArray config, String &errorMsg) {

  const int MAX_PIN = 17;

  bool pinIsOn[MAX_PIN];
  int  pinOnTime[MAX_PIN];

  for (int i = 0; i < MAX_PIN; i++) {
    pinIsOn[i] = false;
    pinOnTime[i] = -1;
  }

  for (JsonObject entry : config) {

    if (!entry.containsKey("pin") ||
        !entry.containsKey("val") ||
        !entry.containsKey("time")) {
      errorMsg = "Missing pin/val/time field";
      return false;
    }

    int pin  = entry["pin"];
    int val  = entry["val"];
    int time = entry["time"];

    if (pin < 0 || pin >= MAX_PIN) {
      errorMsg = "Invalid pin number: " + String(pin);
      return false;
    }

    if (val == 1) {
      if (pinIsOn[pin]) {
        errorMsg = "Pin " + String(pin) + " turned ON twice without OFF";
        return false;
      }
      pinIsOn[pin] = true;
      pinOnTime[pin] = time;
    }
    else if (val == 0) {
      if (!pinIsOn[pin]) {
        errorMsg = "Pin " + String(pin) + " turned OFF without ON";
        return false;
      }

      int duration = time - pinOnTime[pin];
      if (duration > 60) {
        errorMsg = "Pin " + String(pin) + " active more than 60 minutes";
        return false;
      }

      pinIsOn[pin] = false;
      pinOnTime[pin] = -1;
    }
    else {
      errorMsg = "Invalid val for pin " + String(pin);
      return false;
    }
  }

  for (int pin = 0; pin < MAX_PIN; pin++) {
    if (pinIsOn[pin]) {
      errorMsg = "Pin " + String(pin) + " never turned OFF";
      return false;
    }
  }

  return true;
}



void saveTelegramConfig(String chat_id) {
  StaticJsonDocument<MAX_JSON_SIZE> doc;
  DeserializationError err = deserializeJson(doc, telegramConfigBuffer);

  if (err) {
    bot.sendMessage(chat_id, "❌ Invalid JSON. Config NOT saved.", "");
    return;
  }

  String errorMsg;
  if (!validateConfig(doc.as<JsonArray>(), errorMsg)) {
    bot.sendMessage(chat_id, "❌ Config error: " + errorMsg, "");
    return;
  }

  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (!file) {
    bot.sendMessage(chat_id, "❌ Failed to write config file", "");
    return;
  }

  file.print(telegramConfigBuffer);
  file.close();

  telegramConfigEdit = false;
  telegramConfigBuffer = "";

  bot.sendMessage(chat_id, "✅ Config saved successfully", "");
}


void sendTelegramConfig(String chat_id) {
  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) {
    bot.sendMessage(chat_id, "❌ Failed to open config file", "");
    return;
  }

  String content = file.readString();
  file.close();

  bot.sendMessage(chat_id, "📄 Current config:\n\n" + content, "");
}


void telegramLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastTelegramCheck < TELEGRAM_POLL_INTERVAL) return;
  lastTelegramCheck = now;

  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

  while (numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {

      String chat_id = bot.messages[i].chat_id;
      String text    = bot.messages[i].text;

      // SECURITY: only allow owner
      if (chat_id != CHAT_ID) continue;

      // ----- CONFIG EDIT MODE -----
      if (telegramConfigEdit) {

        if (text == "/config_save") {
          saveTelegramConfig(chat_id);
          continue;
        }

        if (text == "/config_cancel") {
          telegramConfigEdit = false;
          telegramConfigBuffer = "";
          bot.sendMessage(chat_id, "❌ Config edit cancelled", "");
          continue;
        }

        telegramConfigBuffer += text + "\n";
        continue;
      }

      // ----- COMMANDS -----
      if (text == "/config") {
        sendTelegramConfig(chat_id);
      }
      else if (text == "/config_set") {
        telegramConfigEdit = true;
        telegramConfigBuffer = "";
        bot.sendMessage(chat_id,
          "✍️ Send new config JSON.\n"
          "Finish with /config_save\n"
          "Cancel with /config_cancel",
          ""
        );
      }
    }

    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}


void eventScheduleLoop(){
  telegramLoop();
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

        pinMode(pin, OUTPUT);
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