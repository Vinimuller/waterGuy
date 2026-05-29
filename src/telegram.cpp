#include "telegram.h"
#include "events.h"
#include "irrigation.h"
#include "uptime.h"
#include "credentials.h"
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

static WiFiClientSecure client;
static UniversalTelegramBot bot(BOTtoken, client);

static bool   configEdit = false;
static String configBuffer = "";

static bool    skipConfirmPending = false;
static uint8_t skipConfirmN       = 0;

static unsigned long lastTelegramCheck = 0;
static unsigned long lastCommandMs    = 0;
static const unsigned long POLL_INTERVAL_IDLE   = 3000;   // 3s
static const unsigned long POLL_INTERVAL_ACTIVE = 1000;   // 1s while a conversation is active
static const unsigned long ACTIVE_WINDOW_MS     = 300000; // 5 min since last command


void telegramSetup() {
  client.setInsecure();
}


String buildDeviceInfoMessage() {
  String msg = "💧 Water Guy Online\n\n";

  msg += "📡 IP: ";
  if (WiFi.status() == WL_CONNECTED) {
    msg += WiFi.localIP().toString();
  } else {
    msg += "not connected";
  }
  msg += "\n";

  msg += "📶 WiFi configured: ";
  msg += (WiFi.SSID().length() > 0) ? "yes\n" : "no\n";

  msg += uptimeBuildStatus();

  msg += "\n🤖 Telegram commands:\n";
  msg += "/status\n";
  msg += "/stop\n";
  msg += "/skip <n> — skip next n scheduled starts (with confirm)\n";
  msg += "/set <pin> <state [0,1]> <duration>\n";
  msg += "/config_read\n";
  msg += "/config_set\n";
  msg += "/config_save\n";
  msg += "/config_cancel\n";

  msg += "\n📄 Config file:\n";
  String content = irrigationReadConfig();
  if (content.length() > 800) {
    content = content.substring(0, 800);
    content += "\n... (truncated)";
  }
  msg += content + "\n";

  return msg;
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
                     " @ " + formatWeekSecond(ev.minute));
        break;
      case IrrigationEventType::ConfigSaved:
        telegramSend("✅ Config saved");
        break;
      case IrrigationEventType::ConfigError:
        telegramSend("❌ " + ev.detail);
        break;
      case IrrigationEventType::ClockSynced:
        telegramSend("🕒 NTP synced @ " + formatWeekSecond(ev.minute));
        break;
      case IrrigationEventType::ProgramStarted:
        telegramSend("▶️ Program started @ " + formatWeekSecond(ev.minute));
        break;
      case IrrigationEventType::ProgramEnded:
        telegramSend("⏹ Program ended @ " + formatWeekSecond(ev.minute));
        break;
      case IrrigationEventType::StartSkipped:
        telegramSend("⏭ Skipped scheduled start @ " + formatWeekSecond(ev.minute) +
                     " (" + String(ev.val) + " skip(s) remaining)");
        break;
    }
  }
}


static void sendCurrentConfig(const String& chat_id) {
  bot.sendMessage(chat_id, "📄 Current config:\n\n" + irrigationReadConfig(), "");
}


static void handleSkipCommand(const String& chat_id, const String& text) {
  String args = text.substring(5);
  args.trim();
  if (args.length() == 0) {
    bot.sendMessage(chat_id, "Usage: /skip <n>  (1.." + String(MAX_STARTS) + ")", "");
    return;
  }
  int n = args.toInt();
  if (n == 0) {
    irrigationSetSkip(0);
    skipConfirmPending = false;
    skipConfirmN = 0;
    bot.sendMessage(chat_id, "✅ Skip queue cleared", "");
    return;
  }
  if (n < 0 || n > MAX_STARTS) {
    bot.sendMessage(chat_id,
      "❌ /skip n out of range — must be 0.." + String(MAX_STARTS), "");
    return;
  }

  long upcoming[MAX_STARTS];
  uint8_t got = irrigationPeekNextStarts(upcoming, (uint8_t)n);
  if (got == 0) {
    bot.sendMessage(chat_id, "❌ No program loaded — nothing to skip", "");
    return;
  }

  String body = "Skip the next " + String(got) + " scheduled start(s)?\n\n";
  for (uint8_t i = 0; i < got; i++) {
    body += String(i + 1) + ". " + formatWeekSecond(upcoming[i]) + "\n";
  }

  const String keyboard =
    "[[{\"text\":\"\xE2\x9C\x85 Confirm\",\"callback_data\":\"skip_confirm\"},"
    "{\"text\":\"\xE2\x9D\x8C Cancel\",\"callback_data\":\"skip_cancel\"}]]";

  skipConfirmPending = true;
  skipConfirmN       = (uint8_t)got;

  bot.sendMessageWithInlineKeyboard(chat_id, body, "", keyboard);
}


static void handleCallback(const String& chat_id, const String& query_id,
                           const String& data) {
  if (data == "skip_confirm") {
    if (skipConfirmPending) {
      uint8_t n = skipConfirmN;
      skipConfirmPending = false;
      skipConfirmN = 0;
      irrigationSetSkip(n);
      bot.answerCallbackQuery(query_id, "Confirmed");
      bot.sendMessage(chat_id,
        "✅ Next " + String(n) + " scheduled start(s) will be skipped", "");
    } else {
      bot.answerCallbackQuery(query_id, "No pending /skip");
    }
    return;
  }
  if (data == "skip_cancel") {
    skipConfirmPending = false;
    skipConfirmN = 0;
    bot.answerCallbackQuery(query_id, "Cancelled");
    bot.sendMessage(chat_id, "❌ /skip cancelled", "");
    return;
  }
  bot.answerCallbackQuery(query_id, "");
}


static void handleCommand(const String& chat_id, const String& text) {
  if (configEdit) {
    if (text == "/config_save") {
      irrigationSaveConfig(configBuffer);
      configEdit = false;
      configBuffer = "";
      // Reply (success or error) arrives via the event drain.
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

  if (text == "/config_read") {
    sendCurrentConfig(chat_id);
  }
  else if (text == "/status") {
    bot.sendMessage(chat_id, irrigationBuildStatus() + uptimeBuildStatus(), "");
  }
  else if (text == "/stop") {
    irrigationStop();
    bot.sendMessage(chat_id, "🛑 Stopped — all pins forced LOW", "");
  }
  else if (text.startsWith("/set ") || text == "/set") {
    String args = text.substring(4);
    args.trim();
    int sp1 = args.indexOf(' ');
    int sp2 = (sp1 >= 0) ? args.indexOf(' ', sp1 + 1) : -1;
    if (sp1 < 0 || sp2 < 0) {
      bot.sendMessage(chat_id,
        "Usage: /set <pin> <state [0,1]> <duration>", "");
      return;
    }
    int pin = args.substring(0, sp1).toInt();
    int state = args.substring(sp1 + 1, sp2).toInt();
    int dur = args.substring(sp2 + 1).toInt();
    String err = irrigationManualSet((uint8_t)pin, state, (uint16_t)dur);
    if (err.length() > 0) {
      bot.sendMessage(chat_id, "❌ " + err, "");
    } else {
      bot.sendMessage(chat_id,
        "✅ Pin " + String(pin) + " -> " + String(state) +
        " for " + String(dur) + "s", "");
    }
  }
  else if (text.startsWith("/skip ") || text == "/skip") {
    handleSkipCommand(chat_id, text);
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
    lastCommandMs = millis();
    if (lastCommandMs == 0) lastCommandMs = 1; // 0 is the "never" sentinel
    for (int i = 0; i < numNewMessages; i++) {
      String chat_id = bot.messages[i].chat_id;
      String text    = bot.messages[i].text;

      // SECURITY: only allow owner
      if (chat_id != CHAT_ID) continue;

      if (bot.messages[i].type == "callback_query") {
        handleCallback(chat_id, bot.messages[i].query_id, text);
      } else {
        handleCommand(chat_id, text);
      }
    }
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}


void telegramLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  drainEvents();

  unsigned long now = millis();
  unsigned long interval = POLL_INTERVAL_IDLE;
  if (lastCommandMs != 0 && (now - lastCommandMs) < ACTIVE_WINDOW_MS) {
    interval = POLL_INTERVAL_ACTIVE;
  }
  if (now - lastTelegramCheck < interval) return;
  lastTelegramCheck = now;

  pollInbound();
}
