#ifndef IRRIGATION_H
#define IRRIGATION_H

#include <Arduino.h>

#define CONFIG_FILE       "/program.json"
#define MAX_JSON_SIZE     1024
#define MAX_STEPS         8
#define MAX_STARTS        16
#define MAX_STEP_DURATION 900  // 15 min cap, hard safety bound

struct ProgramStep {
  uint8_t  pin;
  uint16_t durationSec;
};

struct ProgramStart {
  uint8_t weekday;  // 1 = Sunday .. 7 = Saturday
  uint8_t hour;     // 0..23
  uint8_t minute;   // 0..59
  uint8_t second;   // 0..59
};

struct Program {
  uint8_t      stepCount;
  ProgramStep  steps[MAX_STEPS];
  uint8_t      startCount;
  ProgramStart starts[MAX_STARTS];
};

void   irrigationSetup();
void   irrigationLoop();

// Validates + writes /program.json. Returns "" on success, error string on
// failure. Emits ConfigSaved / ConfigError events accordingly.
String irrigationSaveConfig(const String& json);

// Returns the raw contents of /program.json, or an empty-program JSON if missing.
String irrigationReadConfig();

// True while a step is actively driving a pin. Used to gate NTP snaps.
bool   irrigationIsRunning();

// Aborts any running step and drives every allowed pin LOW. Safe to call
// while IDLE — pins are forced LOW unconditionally.
void   irrigationStop();

// Manually drives a pin HIGH/LOW for `durationSec` seconds, after which the
// pin is auto-reverted to LOW. Independent of the program state machine —
// does not interrupt or alter a running program. Returns "" on success, or
// a human-readable error string (disallowed pin, bad state, duration out of
// 1..MAX_STEP_DURATION range).
String irrigationManualSet(uint8_t pin, int state, uint16_t durationSec);

// Renders a week-second (0..604799) as "Wkd HH:MM:SS (ws=N)".
// Day 1=Sun..7=Sat to match the program.json schema.
String formatWeekSecond(long ws);

// Multi-line human-readable runtime status: clock + NTP source, IDLE vs
// running step (with remaining seconds), program summary, next scheduled start.
String irrigationBuildStatus();

// Skip the next `n` scheduled program starts. Overwrites any previous counter
// (not additive). n==0 clears. RAM-only; reboot resets to 0.
void    irrigationSetSkip(uint8_t n);
uint8_t irrigationGetSkip();

// Fills outSeconds[] with the next `count` scheduled start week-seconds in
// chronological order from the current clock. Returns the number written
// (0 if no program is loaded; capped at `count`).
uint8_t irrigationPeekNextStarts(long* outSeconds, uint8_t count);

#endif
