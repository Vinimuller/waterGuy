#ifndef EVENTLOOP_H
#define EVENTLOOP_H
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <FS.h>               // SPIFFS library


// Function declaration
void eventScheduleLoop();
void updateCounterLoop();

#endif // MYSOURCEFILE_H
