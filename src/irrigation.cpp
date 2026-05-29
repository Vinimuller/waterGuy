#include "irrigation.h"
#include "events.h"
#include <ArduinoJson.h>
#include <FS.h>
#include <time.h>

static const uint8_t ALLOWED_PINS[] = {2, 4, 5, 12, 13, 14, 16};
static const uint8_t ALLOWED_PIN_COUNT = sizeof(ALLOWED_PINS) / sizeof(ALLOWED_PINS[0]);

static const long SECONDS_PER_DAY  = 86400L;
static const long SECONDS_PER_WEEK = 7L * SECONDS_PER_DAY;
static const int  UTC_OFFSET_SEC   = -3 * 3600;
static const unsigned long TICK_INTERVAL_MS = 1000;

enum class State : uint8_t {
  IDLE,
  RUNNING_STEP,
};

struct ManualPin {
  bool          active;
  uint8_t       pin;
  unsigned long startMs;
  uint32_t      durationMs;
};
static ManualPin     g_manual[ALLOWED_PIN_COUNT] = {};

static Program       g_program       = {0, {}, 0, {}};
static State         g_state         = State::IDLE;
static uint8_t       g_stepIndex     = 0;
static unsigned long g_stepStartMs   = 0;
static long          g_currentSecond = 0;
static unsigned long g_secondAnchorMs = 0;
static bool          g_clockSeeded   = false;
static bool          g_ntpEverSynced = false;
static unsigned long g_lastTickMs    = 0;
static long          g_prevSecond    = -1;  // last tick's g_currentSecond; -1 = not yet sampled
static uint8_t       g_skipRemaining = 0;

// Match-window cap. Any forward gap between ticks bigger than this is treated
// as a clock discontinuity (NTP snap, long stall, post-step resume) and the
// tick contributes no start matches — we just resync g_prevSecond and move on.
static const long MAX_MATCH_GAP_SEC = 300;


String formatWeekSecond(long ws) {
  if (ws < 0) ws = ((ws % SECONDS_PER_WEEK) + SECONDS_PER_WEEK) % SECONDS_PER_WEEK;
  else        ws %= SECONDS_PER_WEEK;

  static const char* WKD[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  uint8_t dayIdx = (uint8_t)(ws / SECONDS_PER_DAY);   // 0=Sun..6=Sat
  long    tod    = ws - (long)dayIdx * SECONDS_PER_DAY;
  uint8_t hh = (uint8_t)(tod / 3600);
  uint8_t mm = (uint8_t)((tod % 3600) / 60);
  uint8_t ss = (uint8_t)(tod % 60);

  char buf[20];
  snprintf(buf, sizeof(buf), "%s %02u:%02u:%02u", WKD[dayIdx], hh, mm, ss);
  return String(buf);
}


static bool isAllowedPin(uint8_t pin) {
  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) {
    if (ALLOWED_PINS[i] == pin) return true;
  }
  return false;
}

static String allowedPinsList() {
  String out;
  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) {
    if (i > 0) out += ",";
    out += String(ALLOWED_PINS[i]);
  }
  return out;
}

static long timeOfDaySec(const ProgramStart& s) {
  return (long)s.hour * 3600L + (long)s.minute * 60L + (long)s.second;
}

static long startToWeekSecond(const ProgramStart& s) {
  return ((long)s.weekday - 1L) * SECONDS_PER_DAY + timeOfDaySec(s);
}

// Free-running week-second clock. Boot = 0 (weekday 1, 00:00:00). NTP, when
// available AND the state machine is IDLE, snaps it to wall-clock truth.
// During RUNNING_STEP the snap is deferred — step duration is measured
// against millis() independently, so blocking the snap is safe.
static void advanceClock() {
  unsigned long nowMs = millis();

  if (g_clockSeeded) {
    while ((unsigned long)(nowMs - g_secondAnchorMs) >= 1000UL) {
      g_secondAnchorMs += 1000UL;
      g_currentSecond++;
      if (g_currentSecond >= SECONDS_PER_WEEK) g_currentSecond = 0;
    }
  } else {
    g_currentSecond = 0;
    g_secondAnchorMs = nowMs;
    g_clockSeeded = true;
    Serial.println("Clock: free-running from boot at " + formatWeekSecond(0));
  }

  if (g_state == State::RUNNING_STEP) return;

  time_t now = time(nullptr) + UTC_OFFSET_SEC;
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  bool ntpValid = (timeinfo.tm_year > (1970 - 1900));
  if (!ntpValid) return;

  long ntpSecond = (long)timeinfo.tm_wday * SECONDS_PER_DAY
                 + (long)timeinfo.tm_hour * 3600L
                 + (long)timeinfo.tm_min  * 60L
                 + (long)timeinfo.tm_sec;

  if (!g_ntpEverSynced || ntpSecond != g_currentSecond) {
    g_currentSecond = ntpSecond;
    g_secondAnchorMs = nowMs;
    if (!g_ntpEverSynced) {
      Serial.println("NTP acquired, snapping clock to " + formatWeekSecond(ntpSecond));
      events::emit({IrrigationEventType::ClockSynced, 0, 0, ntpSecond, ""});
    }
    g_ntpEverSynced = true;
  }
}


static int findManualSlot(uint8_t pin) {
  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) {
    if (g_manual[i].active && g_manual[i].pin == pin) return i;
  }
  return -1;
}

static int findFreeManualSlot() {
  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) {
    if (!g_manual[i].active) return i;
  }
  return -1;
}

static void checkManualExpiry() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) {
    if (!g_manual[i].active) continue;
    if ((unsigned long)(now - g_manual[i].startMs) >= g_manual[i].durationMs) {
      uint8_t pin = g_manual[i].pin;
      digitalWrite(pin, LOW);
      g_manual[i].active = false;
      events::emit({IrrigationEventType::PinChanged, pin, LOW, g_currentSecond, ""});
      Serial.print("Manual pin "); Serial.print(pin);
      Serial.println(" expired -> LOW @ " + formatWeekSecond(g_currentSecond));
    }
  }
}


static void writePin(uint8_t pin, int val) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, val);
  events::emit({IrrigationEventType::PinChanged, pin, val, g_currentSecond, ""});
  Serial.print("Pin "); Serial.print(pin);
  Serial.print(" -> "); Serial.print(val);
  Serial.println(" @ " + formatWeekSecond(g_currentSecond));
}


static void enterStep(uint8_t index) {
  g_stepIndex   = index;
  g_stepStartMs = millis();
  g_state       = State::RUNNING_STEP;
  writePin(g_program.steps[index].pin, HIGH);
}


static void endStepAndAdvance() {
  uint8_t pin = g_program.steps[g_stepIndex].pin;
  writePin(pin, LOW);

  uint8_t next = g_stepIndex + 1;
  if (next < g_program.stepCount) {
    enterStep(next);
  } else {
    g_state = State::IDLE;
    events::emit({IrrigationEventType::ProgramEnded, 0, 0, g_currentSecond, ""});
    Serial.println("Program ended");
  }
}


// True if startSec falls in the half-open interval (prev, current] modulo
// SECONDS_PER_WEEK, given a known small forward gap. This is edge-detection,
// not equality — so a tick gap of N seconds still catches any start whose
// time fell anywhere inside that gap. Required because blocking calls
// (Telegram HTTPS poll, internet probe) routinely stall the loop for 1-3 s.
static bool startInGap(long startSec, long prev, long fwdGap) {
  long fwdToStart = startSec - prev;
  if (fwdToStart < 0) fwdToStart += SECONDS_PER_WEEK;
  return fwdToStart > 0 && fwdToStart <= fwdGap;
}


static void tick() {
  bool wasRunning = (g_state == State::RUNNING_STEP);
  if (wasRunning) {
    unsigned long elapsed = millis() - g_stepStartMs;
    uint32_t durationMs = (uint32_t)g_program.steps[g_stepIndex].durationSec * 1000UL;
    if (elapsed >= durationMs) {
      endStepAndAdvance();
    }
  }

  advanceClock();

  if (g_state != State::IDLE) return;
  if (g_program.stepCount == 0 || g_program.startCount == 0) return;

  long prev = g_prevSecond;
  long cur  = g_currentSecond;

  // First tick ever, or first tick after a program finished, or any
  // out-of-band discontinuity: just resync prev and skip matching.
  if (prev < 0 || wasRunning) {
    g_prevSecond = cur;
    return;
  }

  long fwdGap = cur - prev;
  if (fwdGap < 0) fwdGap += SECONDS_PER_WEEK;

  if (fwdGap == 0) return;

  if (fwdGap > MAX_MATCH_GAP_SEC) {
    Serial.print("Clock discontinuity (gap "); Serial.print(fwdGap);
    Serial.println("s) — skipping start matching for this tick");
    g_prevSecond = cur;
    return;
  }

  for (uint8_t i = 0; i < g_program.startCount; i++) {
    long startSec = startToWeekSecond(g_program.starts[i]);
    if (startInGap(startSec, prev, fwdGap)) {
      g_prevSecond = cur;
      if (g_skipRemaining > 0) {
        g_skipRemaining--;
        Serial.println("Program start SKIPPED @ " + formatWeekSecond(cur) +
                       " (scheduled " + formatWeekSecond(startSec) +
                       "), " + String(g_skipRemaining) + " skip(s) left");
        events::emit({IrrigationEventType::StartSkipped, 0,
                      (int)g_skipRemaining, startSec, ""});
        return;
      }
      Serial.println("Program start matched @ " + formatWeekSecond(cur) +
                     " (scheduled " + formatWeekSecond(startSec) + ")");
      events::emit({IrrigationEventType::ProgramStarted, 0, 0, cur, ""});
      enterStep(0);
      return;
    }
  }

  g_prevSecond = cur;
}


// ---------------- JSON parse / validate / save ----------------

static bool parseProgram(JsonObject obj, Program& out, String& errorMsg) {
  out.stepCount  = 0;
  out.startCount = 0;

  bool hasSteps  = obj.containsKey("steps");
  bool hasStarts = obj.containsKey("starts");
  if (!hasSteps || !hasStarts) {
    errorMsg = "Missing top-level key(s):";
    if (!hasSteps)  errorMsg += " 'steps'";
    if (!hasStarts) errorMsg += " 'starts'";
    return false;
  }

  JsonArray steps = obj["steps"].as<JsonArray>();
  if (steps.isNull() || steps.size() == 0) {
    errorMsg = "Empty 'steps' (need 1.." + String(MAX_STEPS) + " step objects with 'pin' and 'dur')";
    return false;
  }
  if (steps.size() > MAX_STEPS) {
    errorMsg = "Too many steps: got " + String(steps.size()) + ", max " + String(MAX_STEPS);
    return false;
  }

  uint8_t idx = 0;
  for (JsonObject s : steps) {
    if (!s.containsKey("pin") || !s.containsKey("dur")) {
      errorMsg = "steps[" + String(idx) + "] missing required field(s): need both 'pin' and 'dur'";
      return false;
    }
    int pin = s["pin"];
    int dur = s["dur"];
    if (pin < 0 || pin > 255 || !isAllowedPin((uint8_t)pin)) {
      errorMsg = "steps[" + String(idx) + "].pin=" + String(pin) +
                 " not allowed (allowed pins: " + allowedPinsList() + ")";
      return false;
    }
    if (dur < 1 || dur > MAX_STEP_DURATION) {
      errorMsg = "steps[" + String(idx) + "].dur=" + String(dur) +
                 "s out of range (must be 1.." + String(MAX_STEP_DURATION) + "s)";
      return false;
    }
    out.steps[out.stepCount].pin         = (uint8_t)pin;
    out.steps[out.stepCount].durationSec = (uint16_t)dur;
    out.stepCount++;
    idx++;
  }

  JsonArray starts = obj["starts"].as<JsonArray>();
  if (starts.isNull() || starts.size() == 0) {
    errorMsg = "Empty 'starts' (need 1.." + String(MAX_STARTS) + " start objects with 'd' and 't')";
    return false;
  }
  if (starts.size() > MAX_STARTS) {
    errorMsg = "Too many starts: got " + String(starts.size()) + ", max " + String(MAX_STARTS);
    return false;
  }

  uint8_t sidx = 0;
  for (JsonObject s : starts) {
    if (!s.containsKey("d") || !s.containsKey("t")) {
      errorMsg = "starts[" + String(sidx) + "] missing required field(s): need 'd' and 't'";
      return false;
    }
    int day = s["d"];
    const char* tstr = s["t"];
    if (tstr == nullptr) {
      errorMsg = "starts[" + String(sidx) + "].t must be a string \"HH:MM:SS\"";
      return false;
    }
    if (day < 1 || day > 7) {
      errorMsg = "starts[" + String(sidx) + "].d=" + String(day) + " out of range (1=Sun..7=Sat)";
      return false;
    }
    int h = -1, m = -1, sec = -1;
    if (sscanf(tstr, "%d:%d:%d", &h, &m, &sec) != 3) {
      errorMsg = "starts[" + String(sidx) + "].t=\"" + String(tstr) +
                 "\" malformed (expected \"HH:MM:SS\")";
      return false;
    }
    if (h < 0 || h > 23) {
      errorMsg = "starts[" + String(sidx) + "].t hour=" + String(h) + " out of range (0..23)";
      return false;
    }
    if (m < 0 || m > 59) {
      errorMsg = "starts[" + String(sidx) + "].t minute=" + String(m) + " out of range (0..59)";
      return false;
    }
    if (sec < 0 || sec > 59) {
      errorMsg = "starts[" + String(sidx) + "].t second=" + String(sec) + " out of range (0..59)";
      return false;
    }
    out.starts[out.startCount].weekday = (uint8_t)day;
    out.starts[out.startCount].hour    = (uint8_t)h;
    out.starts[out.startCount].minute  = (uint8_t)m;
    out.starts[out.startCount].second  = (uint8_t)sec;
    out.startCount++;
    sidx++;
  }

  // Non-overlap check per weekday. Brute force O(n^2) is fine — MAX_STARTS = 16.
  uint32_t total = 0;
  for (uint8_t i = 0; i < out.stepCount; i++) total += out.steps[i].durationSec;
  if (total > (uint32_t)SECONDS_PER_DAY) {
    errorMsg = "Program total duration " + String(total) + "s exceeds 24h (" +
               String((long)SECONDS_PER_DAY) + "s)";
    return false;
  }

  for (uint8_t day = 1; day <= 7; day++) {
    long sorted[MAX_STARTS];
    uint8_t n = 0;
    for (uint8_t i = 0; i < out.startCount; i++) {
      if (out.starts[i].weekday == day) sorted[n++] = timeOfDaySec(out.starts[i]);
    }
    for (uint8_t i = 1; i < n; i++) {
      long key = sorted[i];
      int8_t j = i - 1;
      while (j >= 0 && sorted[j] > key) { sorted[j+1] = sorted[j]; j--; }
      sorted[j+1] = key;
    }
    for (uint8_t i = 0; i + 1 < n; i++) {
      if (sorted[i+1] < sorted[i] + (long)total) {
        long a = sorted[i], b = sorted[i+1];
        errorMsg = "Overlapping starts on day " + String(day) +
                   ": " + String(a/3600) + ":" + String((a%3600)/60) +
                   ":" + String(a%60) + " + program " + String((long)total) +
                   "s overlaps next start " + String(b/3600) + ":" +
                   String((b%3600)/60) + ":" + String(b%60);
        return false;
      }
    }
    if (n > 0 && sorted[n-1] + (long)total > SECONDS_PER_DAY) {
      long a = sorted[n-1];
      errorMsg = "Start on day " + String(day) + " at " +
                 String(a/3600) + ":" + String((a%3600)/60) + ":" + String(a%60) +
                 " + program " + String((long)total) + "s runs past midnight";
      return false;
    }
  }

  return true;
}


static bool loadProgramFromFile() {
  if (!SPIFFS.exists(CONFIG_FILE)) return false;
  File f = SPIFFS.open(CONFIG_FILE, "r");
  if (!f) return false;
  if ((int)f.size() > MAX_JSON_SIZE) { f.close(); return false; }

  DynamicJsonDocument doc(MAX_JSON_SIZE);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("Program JSON parse failed on load: ");
    Serial.println(err.c_str());
    return false;
  }

  Program parsed;
  String errorMsg;
  if (!parseProgram(doc.as<JsonObject>(), parsed, errorMsg)) {
    Serial.print("Stored program invalid: ");
    Serial.println(errorMsg);
    return false;
  }
  g_program = parsed;
  Serial.print("Program loaded: "); Serial.print(g_program.stepCount);
  Serial.print(" steps, "); Serial.print(g_program.startCount);
  Serial.println(" starts");
  return true;
}


void irrigationSetup() {
  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) {
    pinMode(ALLOWED_PINS[i], OUTPUT);
    digitalWrite(ALLOWED_PINS[i], LOW);
  }

  if (!SPIFFS.exists(CONFIG_FILE)) {
    File f = SPIFFS.open(CONFIG_FILE, "w");
    if (f) {
      f.print("{\"steps\":[],\"starts\":[]}");
      f.close();
      Serial.println("Created empty program file");
    } else {
      Serial.println("Failed to create program file");
    }
  }

  loadProgramFromFile();
}


void irrigationLoop() {
  checkManualExpiry();

  unsigned long now = millis();
  if (g_state == State::RUNNING_STEP) {
    tick();
    g_lastTickMs = now;
    return;
  }
  if ((unsigned long)(now - g_lastTickMs) >= TICK_INTERVAL_MS) {
    g_lastTickMs = now;
    tick();
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
    String err = String("Invalid JSON: ") + error.c_str() +
                 " (payload " + String(json.length()) + " bytes)";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }

  if (!doc.is<JsonObject>()) {
    String err = "Invalid JSON: top-level value must be an object with 'steps' and 'starts'";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }

  Program parsed;
  String validationErr;
  if (!parseProgram(doc.as<JsonObject>(), parsed, validationErr)) {
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, validationErr});
    return validationErr;
  }

  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (!file) {
    String err = "Could not open program file for writing";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }
  if (serializeJson(doc, file) == 0) {
    file.close();
    String err = "Could not write program file";
    events::emit({IrrigationEventType::ConfigError, 0, 0, 0, err});
    return err;
  }
  file.close();

  g_program = parsed;
  g_prevSecond = -1;
  events::emit({IrrigationEventType::ConfigSaved, 0, 0, 0, ""});
  return "";
}


String irrigationReadConfig() {
  if (!SPIFFS.exists(CONFIG_FILE)) return "{\"steps\":[],\"starts\":[]}";
  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) return "{\"steps\":[],\"starts\":[]}";
  String content = file.readString();
  file.close();
  return content;
}


bool irrigationIsRunning() {
  return g_state == State::RUNNING_STEP;
}


String irrigationBuildStatus() {
  String msg;

  msg += "🕒 ";
  msg += formatWeekSecond(g_currentSecond);
  msg += g_ntpEverSynced ? "  (NTP)" : "  (virtual)";
  msg += "\n";

  if (g_state == State::RUNNING_STEP) {
    uint32_t durSec     = g_program.steps[g_stepIndex].durationSec;
    uint32_t elapsedSec = (uint32_t)((millis() - g_stepStartMs) / 1000UL);
    uint32_t remaining  = (elapsedSec >= durSec) ? 0 : (durSec - elapsedSec);
    msg += "⚙️ Running step " + String(g_stepIndex + 1) + "/" + String(g_program.stepCount);
    msg += ": pin " + String(g_program.steps[g_stepIndex].pin);
    msg += ", " + String(remaining) + "s remaining (of " + String(durSec) + "s)\n";
  } else {
    msg += "⚙️ Idle\n";
  }

  if (g_program.stepCount == 0 || g_program.startCount == 0) {
    msg += "📋 No program loaded\n";
    return msg;
  }

  msg += "📋 Program: " + String(g_program.stepCount) + " steps, ";
  msg += String(g_program.startCount) + " starts/week\n";

  // Walk past any queued skips so "Next start" reflects the next start that
  // will actually fire.
  uint8_t lookahead = (uint8_t)(g_skipRemaining + 1);
  long upcoming[MAX_STARTS + 1];
  uint8_t got = irrigationPeekNextStarts(upcoming, lookahead);
  long nextStart = upcoming[got - 1];
  long nextDelta = nextStart - g_currentSecond;
  if (nextDelta <= 0) nextDelta += SECONDS_PER_WEEK;

  char buf[48];
  if (nextDelta >= SECONDS_PER_DAY) {
    long dd = nextDelta / SECONDS_PER_DAY;
    long hh = (nextDelta % SECONDS_PER_DAY) / 3600;
    long mm = (nextDelta % 3600) / 60;
    snprintf(buf, sizeof(buf), " (in %ldd %02ldh %02ldm)", dd, hh, mm);
  } else {
    long hh = nextDelta / 3600;
    long mm = (nextDelta % 3600) / 60;
    long ss = nextDelta % 60;
    snprintf(buf, sizeof(buf), " (in %02ldh %02ldm %02lds)", hh, mm, ss);
  }
  msg += "📅 Next start: " + formatWeekSecond(nextStart) + String(buf) + "\n";

  if (g_skipRemaining > 0) {
    msg += "⏭ Skips queued: " + String(g_skipRemaining) + "\n";
  }

  return msg;
}


void irrigationSetSkip(uint8_t n) {
  g_skipRemaining = n;
  Serial.print("Skip counter set to "); Serial.println(n);
}


uint8_t irrigationGetSkip() {
  return g_skipRemaining;
}


uint8_t irrigationPeekNextStarts(long* outSeconds, uint8_t count) {
  if (count == 0 || g_program.startCount == 0) return 0;

  long cursor = g_currentSecond;
  uint8_t produced = 0;
  while (produced < count) {
    long bestDelta = -1;
    long bestStart = 0;
    for (uint8_t i = 0; i < g_program.startCount; i++) {
      long s = startToWeekSecond(g_program.starts[i]);
      long d = s - cursor;
      if (d <= 0) d += SECONDS_PER_WEEK;
      if (bestDelta < 0 || d < bestDelta) { bestDelta = d; bestStart = s; }
    }
    outSeconds[produced++] = bestStart;
    cursor = bestStart;
  }
  return produced;
}


String irrigationManualSet(uint8_t pin, int state, uint16_t durationSec) {
  if (!isAllowedPin(pin)) {
    return "Pin " + String(pin) + " not allowed (allowed: " + allowedPinsList() + ")";
  }
  if (state != 0 && state != 1) {
    return "State " + String(state) + " invalid (must be 0 or 1)";
  }
  if (durationSec < 1 || durationSec > MAX_STEP_DURATION) {
    return "Duration " + String(durationSec) + "s out of range (1.." +
           String(MAX_STEP_DURATION) + "s)";
  }

  int slot = findManualSlot(pin);
  if (slot < 0) slot = findFreeManualSlot();
  if (slot < 0) return "No free manual slot";

  pinMode(pin, OUTPUT);
  digitalWrite(pin, state ? HIGH : LOW);
  g_manual[slot].active     = true;
  g_manual[slot].pin        = pin;
  g_manual[slot].startMs    = millis();
  g_manual[slot].durationMs = (uint32_t)durationSec * 1000UL;

  events::emit({IrrigationEventType::PinChanged, pin, state, g_currentSecond, ""});
  Serial.print("Manual pin "); Serial.print(pin);
  Serial.print(" -> "); Serial.print(state);
  Serial.print(" for "); Serial.print(durationSec);
  Serial.println("s @ " + formatWeekSecond(g_currentSecond));
  return "";
}


void irrigationStop() {
  bool wasRunning = (g_state == State::RUNNING_STEP);
  uint8_t activePin = wasRunning ? g_program.steps[g_stepIndex].pin : 0;

  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) {
    uint8_t pin = ALLOWED_PINS[i];
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    if (wasRunning && pin == activePin) {
      events::emit({IrrigationEventType::PinChanged, pin, LOW, g_currentSecond, ""});
    }
  }

  for (uint8_t i = 0; i < ALLOWED_PIN_COUNT; i++) g_manual[i].active = false;

  g_state = State::IDLE;
  g_prevSecond = -1;

  if (wasRunning) {
    events::emit({IrrigationEventType::ProgramEnded, 0, 0, g_currentSecond, "stopped"});
    Serial.println("Program stopped by request @ " + formatWeekSecond(g_currentSecond));
  } else {
    Serial.println("Stop requested while idle — all pins forced LOW");
  }
}
