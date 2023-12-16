/*
  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/telegram-control-esp32-esp8266-nodemcu-outputs/
  
  Project created using Brian Lough's Universal Telegram Bot Library: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot
  Example based on the Universal Arduino Telegram Bot Library: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot/blob/master/examples/ESP8266/FlashLED/FlashLED.ino
*/

#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>   // Universal Telegram Bot Library written by Brian Lough: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot
#include <ArduinoJson.h>

// Replace with your network credentials
const char* ssid = "Salame2G";
const char* password = "Um2tres4cinco";

// Initialize Telegram BOT
#define BOTtoken "6907833879:AAH1WprNyoWBfXKkSisYvLP7OVu1qkLw3aU"  // your Bot Token (Get from Botfather)

// Use @myidbot to find out the chat ID of an individual or a group
// Also note that you need to click "start" on a bot before it can
// message you
#define CHAT_ID "6264510964"

#ifdef ESP8266
  X509List cert(TELEGRAM_CERTIFICATE_ROOT);
#endif

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Checks for new messages every 1 second.
int botRequestDelay = 1000;
unsigned long lastTimeBotRan;

const int     relayPin        = 2;
bool          relayState      = LOW;
unsigned long lastRelayActivation;
unsigned long relayOnTime     = 1000;
bool          autoOff         = false;

// Handle what happens when you receive new messages
void handleNewMessages(int numNewMessages) {
  Serial.println("handleNewMessages");
  Serial.println(String(numNewMessages));

  for (int i=0; i<numNewMessages; i++) {
    // Chat id of the requester
    String chat_id = String(bot.messages[i].chat_id);
    // if (chat_id != CHAT_ID){
    //   bot.sendMessage(chat_id, "Unauthorized user", "");
    //   continue;
    // }
    
    // Print the received message
    String text = bot.messages[i].text;
    Serial.println(text);

    String from_name = bot.messages[i].from_name;

    if (text == "/start") {
      String welcome = "Welcome, " + from_name + ".\n";
      welcome += "Use the following commands to control your outputs.\n\n";
      welcome += "/on<minutes> to turn ON for X minutes \n";
      welcome += "/off to turn OFF \n";
      welcome += "/state to request current state \n";
      bot.sendMessage(chat_id, welcome, "");
    }

    if (text.startsWith("/on")) {
      Serial.println(text.substring(3));
      relayOnTime  = (text.substring(3).toInt()) * 60 * 1000;
      Serial.println(relayOnTime);
      
      relayState = HIGH;
      digitalWrite(relayPin, relayState);
      lastRelayActivation = millis();
      autoOff = true;

      String msg = "Turning irrigation ON for " + String(relayOnTime/1000/60) + " minutes.";
      bot.sendMessage(chat_id, msg, "");
    }
    
    if (text == "/off") {
      relayState = LOW;
      digitalWrite(relayPin, relayState);
      
      bot.sendMessage(chat_id, "Turning irrigation OFF", "");
    }
    
    if (text == "/state") {
      if (digitalRead(relayPin)){
        bot.sendMessage(chat_id, "state is ON", "");
      }
      else{
        bot.sendMessage(chat_id, "state is OFF", "");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  #ifdef ESP8266
    configTime(0, 0, "pool.ntp.org");      // get UTC time via NTP
    client.setTrustAnchors(&cert); // Add root certificate for api.telegram.org
  #endif

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, relayState);
  
  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  #ifdef ESP32
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Add root certificate for api.telegram.org
  #endif
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi..");
  }
  // Print ESP32 Local IP Address
  Serial.println(WiFi.localIP());
}

void loop() {
  if (millis() > lastTimeBotRan + botRequestDelay)  {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    while(numNewMessages) {
      Serial.println("got response");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }

  if ((millis() > (lastRelayActivation + relayOnTime)) && autoOff)  {
      autoOff = false;
      relayState = LOW;
      digitalWrite(relayPin, relayState);
    
      String msg = "Irration turned off after " + String(relayOnTime/1000/60) + " minutes.\n";
      bot.sendMessage(CHAT_ID, msg, "");
  }






}