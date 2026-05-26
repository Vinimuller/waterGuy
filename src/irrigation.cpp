#include "irrigation.h"
#include "events.h"
#include <ArduinoJson.h>
#include <FS.h>
#include <time.h>

const int   relayPin   = 2;
bool        relayState = LOW;

unsigned long lastReconcileMills = 0;
const unsigned long RECONCILE_INTERVAL = 10000; // 10s — idempotent, frequent is fine

// Free-running week-minute clock. Seeded from NTP if/when available, otherwise
// free-runs from millis() starting at 0 (≈ Sunday 00:00) on boot. NTP, once
// it arrives, snaps it to true wall time and takes over.
long currentMinuteOfWeek    = 0;
unsigned long minuteAnchorMills = 0;
bool clockSeeded            = false;
bool ntpEverSynced          = false;

// 0 (LOW) matches the safe default of every pin before the scheduler has
// touched it, so the first reconcile only fires for pins that should be ON.
int  desiredState[17];
bool desiredStateInit = false;


void irrigationSetup() {
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, relayState);

  if (!SPIFFS.exists(CONFIG_FILE)) {
    File f = SPIFFS.open(CONFIG_FILE, "w");
    if (f) {
      f.print("[]");
      f.close();
      Serial.println("Created empty config file");
    } else {
      Serial.println("Failed to create config file");
    }
  }
}


// Advance the week-minute clock. Prefers NTP when available; otherwise
// free-runs from millis(). Safe across millis() overflow (unsigned math).
static void advanceClock() {
  unsigned long nowMills = millis();

  time_t now = time(nullptr) - (3 * 3600); // hardcoded UTC-3
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  bool ntpValid = (timeinfo.tm_year > (1970 - 1900));

  if (ntpValid) {
    long ntpMinute = (long)timeinfo.tm_wday * 24L * 60L
                   + (long)timeinfo.tm_hour * 60L
                   + (long)timeinfo.tm_min;
    if (!ntpEverSynced) {
      Serial.print("NTP acquired, snapping clock to week-minute ");
      Serial.println(ntpMinute);
      events::emit({IrrigationEventType::ClockSynced, 0, 0, ntpMinute, ""});
    }
    currentMinuteOfWeek = ntpMinute;
    minuteAnchorMills = nowMills - (unsigned long)timeinfo.tm_sec * 1000UL;
    ntpEverSynced = true;
    clockSeeded = true;
    return;
  }

  if (!clockSeeded) {
    currentMinuteOfWeek = 0;
    minuteAnchorMills = nowMills;
    clockSeeded = true;
    Serial.println("No NTP — free-running clock from boot (minute 0)");
    return;
  }

  while ((unsigned long)(nowMills - minuteAnchorMills) >= 60000UL) {
    minuteAnchorMills += 60000UL;
    currentMinuteOfWeek++;
    if (currentMinuteOfWeek >= EVENT_LOOP_MAX) currentMinuteOfWeek = 0;
  }
}


// Desired state for `pin` at `currentMinute`: val of the schedule entry with
// the largest `time` <= currentMinute. If none exists, fall back to the entry
// with the largest `time` overall (last week's final action, persists across
// the Sunday wrap). Returns -1 if the pin has no events at all.
static int desiredStateForPin(JsonArray config, int pin, long currentMinute) {
  int  bestVal      = -1;
  long bestTime     = -1;
  int  fallbackVal  = -1;
  long fallbackTime = -1;

  for (JsonObject obj : config) {
    if ((int)obj["pin"] != pin) continue;
    long t = (long)(int)obj["time"];
    int  v = obj["val"];

    if (t <= currentMinute && t > bestTime) {
      bestTime = t;
      bestVal  = v;
    }
    if (t > fallbackTime) {
      fallbackTime = t;
      fallbackVal  = v;
    }
  }
  return (bestVal != -1) ? bestVal : fallbackVal;
}


static bool validateConfig(JsonArray config, String &errorMsg) {
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


static void reconcileSchedule() {
  advanceClock();
  long currentMinute = currentMinuteOfWeek;

  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) {
    Serial.println("Reconcile: failed to open config file");
    return;
  }
  if ((int)file.size() > MAX_JSON_SIZE) {
    Serial.println("Reconcile: config too large: " + String(file.size()));
    file.close();
    return;
  }

  DynamicJsonDocument doc(MAX_JSON_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.print("Reconcile: JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }
  JsonArray config = doc.as<JsonArray>();

  if (!desiredStateInit) {
    for (int i = 0; i < 17; i++) desiredState[i] = 0;
    desiredStateInit = true;
  }

  bool seenPin[17] = {false};
  for (JsonObject obj : config) {
    int pin = obj["pin"];
    if (pin >= 0 && pin < 17) seenPin[pin] = true;
  }

  Serial.print("Reconcile @ week-minute ");
  Serial.println(currentMinute);

  for (int pin = 0; pin < 17; pin++) {
    if (!seenPin[pin]) continue;

    int desired = desiredStateForPin(config, pin, currentMinute);
    if (desired < 0) continue;
    if (desired == desiredState[pin]) continue;

    pinMode(pin, OUTPUT);
    digitalWrite(pin, desired);
    desiredState[pin] = desired;

    Serial.print("Pin "); Serial.print(pin);
    Serial.print(" -> "); Serial.print(desired);
    Serial.print(" @ minute "); Serial.println(currentMinute);

    events::emit({IrrigationEventType::PinChanged, pin, desired, currentMinute, ""});
  }
}


void irrigationLoop() {
  unsigned long now = millis();
  if (now - lastReconcileMills >= RECONCILE_INTERVAL) {
    lastReconcileMills = now;
    reconcileSchedule();
  }
}


String irrigationSaveConfig(const String& json) {
  if ((int)json.length() > MAX_JSON_SIZE) {
    String err = "Config too large (" + String(json.length()) +
                 " bytes, max " + String(MAX_JSON_SIZE) + ")";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }

  DynamicJsonDocument doc(MAX_JSON_SIZE);
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    String err = "Invalid JSON";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }

  String validationErr;
  if (!validateConfig(doc.as<JsonArray>(), validationErr)) {
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, validationErr});
    return validationErr;
  }

  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (!file) {
    String err = "Could not open config file for writing";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }

  if (serializeJson(doc, file) == 0) {
    file.close();
    String err = "Could not write config file";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }
  file.close();

  events::emit({IrrigationEventType::ConfigSaved, 0, 0, 0, ""});
  return "";
}


String irrigationReadConfig() {
  if (!SPIFFS.exists(CONFIG_FILE)) return "[]";
  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) return "[]";
  String content = file.readString();
  file.close();
  return content;
}


long irrigationCurrentMinute() {
  return currentMinuteOfWeek;
}
