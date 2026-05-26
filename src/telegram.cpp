#include "telegram.h"
#include "events.h"
#include "irrigation.h"
#include "credentials.h"
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

static WiFiClientSecure client;
static UniversalTelegramBot bot(BOTtoken, client);

static bool   configEdit = false;
static String configBuffer = "";

static unsigned long lastTelegramCheck = 0;
static const unsigned long TELEGRAM_POLL_INTERVAL = 3000; // 3s


void telegramSetup() {
  client.setInsecure();
}


void telegramSend(const String& msg) {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Sending Telegram");
  if (!bot.sendMessage(CHAT_ID, msg)) {
    Serial.println("Telegram send FAILED");
  }
}


static void drainEvents() {
  IrrigationEvent ev;
  while (events::pop(ev)) {
    switch (ev.type) {
      case IrrigationEventType::PinChanged:
        telegramSend("Pin " + String(ev.pin) + " -> " + String(ev.val) +
                     " @ minute " + String(ev.minute));
        break;
      case IrrigationEventType::ConfigSaved:
        telegramSend("✅ Config saved");
        break;
      case IrrigationEventType::ConfigError:
        telegramSend("❌ " + ev.detail);
        break;
      case IrrigationEventType::ClockSynced:
        telegramSend("🕒 NTP synced @ minute " + String(ev.minute));
        break;
    }
  }
}


static void sendCurrentConfig(const String& chat_id) {
  bot.sendMessage(chat_id, "📄 Current config:\n\n" + irrigationReadConfig(), "");
}


static void handleCommand(const String& chat_id, const String& text) {
  if (configEdit) {
    if (text == "/config_save") {
      String err = irrigationSaveConfig(configBuffer);
      if (err.length() == 0) {
        configEdit = false;
        configBuffer = "";
      }
      // Reply for both success and error arrives via the event drain.
      return;
    }

    if (text == "/config_cancel") {
      configEdit = false;
      configBuffer = "";
      bot.sendMessage(chat_id, "❌ Config edit cancelled", "");
      return;
    }

    configBuffer += text + "\n";
    return;
  }

  if (text == "/config") {
    sendCurrentConfig(chat_id);
  }
  else if (text == "/config_set") {
    configEdit = true;
    configBuffer = "";
    bot.sendMessage(chat_id,
      "✍️ Send new config JSON.\n"
      "Finish with /config_save\n"
      "Cancel with /config_cancel",
      ""
    );
  }
}


static void pollInbound() {
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

  while (numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
      String chat_id = bot.messages[i].chat_id;
      String text    = bot.messages[i].text;

      // SECURITY: only allow owner
      if (chat_id != CHAT_ID) continue;

      handleCommand(chat_id, text);
    }
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}


void telegramLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  drainEvents();

  unsigned long now = millis();
  if (now - lastTelegramCheck < TELEGRAM_POLL_INTERVAL) return;
  lastTelegramCheck = now;

  pollInbound();
}
