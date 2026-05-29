#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <Arduino.h>

void telegramSetup();
void telegramLoop();

// Direct send used by non-irrigation callers (e.g. boot/startup messages
// from main.cpp). Irrigation should emit events instead.
void telegramSend(const String& msg);

// Renders the boot/status banner: IP, WiFi state, command list, current config.
String buildDeviceInfoMessage();

#endif
