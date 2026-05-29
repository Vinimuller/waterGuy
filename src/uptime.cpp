#include "uptime.h"
#include <FS.h>

static const uint32_t PERSIST_INTERVAL_SEC = 3600;  // write record at most once per hour
static const uint32_t SEC_PER_HOUR  = 3600UL;
static const uint32_t SEC_PER_DAY   = 86400UL;
static const uint32_t SEC_PER_WEEK  = 7UL  * SEC_PER_DAY;
static const uint32_t SEC_PER_MONTH = 30UL * SEC_PER_DAY;
static const uint32_t SEC_PER_YEAR  = 365UL * SEC_PER_DAY;

static unsigned long g_lastMs         = 0;
static uint32_t      g_msAccumulator  = 0;   // sub-second leftover
static uint32_t      g_uptimeSec      = 0;
static uint32_t      g_recordSec      = 0;
static uint32_t      g_lastPersistSec = 0;
static bool          g_recordIsNew    = false;


static uint32_t readRecordFile() {
  if (!SPIFFS.exists(UPTIME_FILE)) return 0;
  File f = SPIFFS.open(UPTIME_FILE, "r");
  if (!f) return 0;
  String s = f.readString();
  f.close();
  return (uint32_t)strtoul(s.c_str(), nullptr, 10);
}


static void writeRecordFile(uint32_t sec) {
  File f = SPIFFS.open(UPTIME_FILE, "w");
  if (!f) {
    Serial.println("Uptime: failed to open record file for write");
    return;
  }
  f.print(sec);
  f.close();
}


static String formatDuration(uint32_t sec) {
  uint32_t y  = sec / SEC_PER_YEAR;  sec %= SEC_PER_YEAR;
  uint32_t mo = sec / SEC_PER_MONTH; sec %= SEC_PER_MONTH;
  uint32_t w  = sec / SEC_PER_WEEK;  sec %= SEC_PER_WEEK;
  uint32_t d  = sec / SEC_PER_DAY;   sec %= SEC_PER_DAY;
  uint32_t h  = sec / SEC_PER_HOUR;

  String out;
  if (y > 0)                            out += String(y)  + "y ";
  if (y > 0 || mo > 0)                  out += String(mo) + "mo ";
  if (y > 0 || mo > 0 || w > 0)         out += String(w)  + "w ";
  if (y > 0 || mo > 0 || w > 0 || d > 0) out += String(d) + "d ";
  out += String(h) + "h";
  return out;
}


void uptimeSetup() {
  g_lastMs         = millis();
  g_msAccumulator  = 0;
  g_uptimeSec      = 0;
  g_recordSec      = readRecordFile();
  g_lastPersistSec = 0;
  g_recordIsNew    = false;
  Serial.print("Uptime: record loaded = ");
  Serial.print(g_recordSec);
  Serial.println("s");
}


void uptimeLoop() {
  unsigned long now = millis();
  uint32_t deltaMs  = (uint32_t)(now - g_lastMs);  // wrap-safe via unsigned modulo
  g_lastMs = now;

  g_msAccumulator += deltaMs;
  if (g_msAccumulator >= 1000) {
    g_uptimeSec     += g_msAccumulator / 1000;
    g_msAccumulator %= 1000;
  }

  if (g_uptimeSec > g_recordSec) {
    g_recordSec   = g_uptimeSec;
    g_recordIsNew = true;
  }

  if (g_recordIsNew && (g_uptimeSec - g_lastPersistSec) >= PERSIST_INTERVAL_SEC) {
    writeRecordFile(g_recordSec);
    g_lastPersistSec = g_uptimeSec;
    g_recordIsNew    = false;
  }
}


uint32_t uptimeCurrentSec() { return g_uptimeSec; }
uint32_t uptimeRecordSec()  { return g_recordSec; }


String uptimeBuildStatus() {
  String msg = "⏱ Up for ";
  msg += formatDuration(g_uptimeSec);
  msg += "\n🏆 Record: ";
  msg += formatDuration(g_recordSec);
  msg += "\n";
  return msg;
}
