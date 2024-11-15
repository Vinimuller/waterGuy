#ifndef EVENTLOOP_H
#define EVENTLOOP_H

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>   // Universal Telegram Bot Library written by Brian Lough: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot
#include <FS.h>               // SPIFFS library
#include "credentials.h"

#define EVENT_LOOP_TICK   60000 // 1 minute
#define EVENT_LOOP_MAX    10080 // 10080 minutes -> 1 week
#define CONFIG_FILE       "/config.json"
#define MAX_JSON_SIZE     400

// Function declaration
void    eventScheduleSetup();
void    eventScheduleLoop();
String  eventLoopNewFile(String json);
void    msgTelegram(String msg);

#endif // MYSOURCEFILE_H
