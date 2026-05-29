# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP8266 (NodeMCU v2) irrigation controller. Controls GPIO-connected relays on a weekly schedule. Managed via a local HTTP web UI and a Telegram bot. Built with PlatformIO.

## Build & Flash Commands

```bash
pio run --target upload        # compile and flash firmware
pio run --target uploadfs      # upload SPIFFS filesystem (config files)
pio device monitor             # serial monitor at 115200 baud
pio run                        # compile only (no flash)
```

`pio` lives at `~/.platformio/penv/bin/pio` and is not on `$PATH` by default. Dependencies are managed by PlatformIO via `platformio.ini` (ArduinoJson 6.x, UniversalTelegramBot).

## Architecture

The firmware is split into six modules under `src/`. `main.cpp` is the shell; each other module owns one concern and is wired into `loop()` non-blockingly.

### `main.cpp` — boot, state machine, initial WiFi connect
- System state machine: `INIT` (first 15 s, AP open for setup) → `RUNNING`
- On boot: mounts SPIFFS, calls `irrigationSetup()` / `telegramSetup()` / `webserverSetup()`, and starts the AP (`waterGuy` / `~~~~~~~~`) so the config UI is reachable at `192.168.4.1`
- After 15 s with no AP client, calls `wifiConfigLoad()`; on success runs `connectToWiFi()` (blocking STA connect up to ~10 s, syncs NTP, sends Telegram boot message via `buildDeviceInfoMessage()`, hands credentials to `networkBegin()`)
- In `RUNNING`, each `loop()` iteration calls `webserverLoop()` → `irrigationLoop()` → `telegramLoop()` → `networkLoop()`. None of them block. (`webserverLoop()` also runs during `INIT` so the AP-mode config UI works.)

### `webserver.{h,cpp}` — HTTP server, route handlers, manual pin control
- Owns the `ESP8266WebServer` instance on port 80 and the allowed-pin tracking arrays (`ALLOWED_PINS = {2, 4, 5, 12, 13, 14, 16}`, `pinInitialized`, `pinState`) used by the manual-toggle and status endpoints.
- `webserverSetup()`: registers all routes and starts the server. `webserverLoop()`: services pending clients.
- HTTP routes: `GET/POST /` (WiFi config UI — POST delegates to `wifiConfigSave` then reboots), `GET/POST /saveConfig` (schedule editor — delegates to `irrigationSaveConfig`/`irrigationReadConfig`), `GET /toggle?pin=N`, `GET /status` (JSON of allowed-pin states), `GET /get-file?file=<path>`.
- `POST /saveConfig` accepts both shapes: the built-in HTML form posts `application/x-www-form-urlencoded` with the JSON in a `config` field; raw clients (e.g. `curl --data @file.json`) land in the `plain` arg. The handler tries `config` first, then falls back to `plain`.

### `wifi_config.{h,cpp}` — WiFi credential file I/O
- Owns `/configWifi.json` on SPIFFS. `wifiConfigLoad(ssid, password)` reads it (returns false if missing); `wifiConfigSave(ssid, password)` writes it.
- Loaded by `main.cpp` on boot. Written by the `/` POST handler in `webserver.cpp`, which then reboots to apply.

### `irrigation.{h,cpp}` — schedule clock, reconcile, config I/O
- `irrigationSetup()`: drives every pin in `ALLOWED_PINS` LOW, creates an empty `/program.json` if missing, and loads the stored program into RAM.
- `irrigationLoop()`: 1 s tick when IDLE, every loop iteration when a step is running. Drives a two-state machine: `IDLE` ↔ `RUNNING_STEP`.
- `advanceClock()`: maintains `g_currentSecond` of week (0..604 799). Free-runs from `millis()` (boot = week-second 0 = virtual Sunday 00:00:00) until NTP is acquired. **NTP snaps are only applied while `IDLE`** — during `RUNNING_STEP` the wall clock is deliberately ignored so a step's OFF cannot be deferred or skipped. Overflow-safe across the 49.7-day `millis()` wrap. Hardcoded UTC−3 offset.
- Step duration is anchored to `millis()` at step entry (`g_stepStartMs`), not to a future week-second. This is the primary guarantee against leaving a pin on past its configured duration.
- The state machine evaluates step-end **before** any clock advance or start matching on each tick. A start match transitions IDLE → RUNNING_STEP at step 0; step completion advances to next step or returns to IDLE.
- Start matching is **edge-detected**, not equality: each tick fires any start whose scheduled week-second falls in the half-open interval `(g_prevSecond, g_currentSecond]` (mod week). This is mandatory because blocking calls elsewhere in the loop (Telegram HTTPS poll, internet probe) routinely stall ticks by 1–3 s, and an equality-based match would silently miss the start instant. Forward gaps larger than `MAX_MATCH_GAP_SEC` (300 s) are treated as a clock discontinuity (NTP snap, very long stall, or post-step resume after a long program) — `g_prevSecond` is resynced to `g_currentSecond` and matching is skipped for that tick. The first tick after boot and the first tick after a step completes are likewise skipped to avoid spurious fires.
- `irrigationSaveConfig(json)`: single owner of writes to `/program.json`. Parses + validates into a `Program` struct, then atomically swaps it in. Validation enforces allowed pins, per-step duration ≤ 900 s, non-overlapping starts per weekday (so N runs fit in 24 h), and total program length ≤ 24 h. Returns `""` on success / error string on failure, and emits `ConfigSaved` / `ConfigError` events. Called from both the HTTP `/saveConfig` route and the Telegram `/config_save` command.
- `irrigationReadConfig()`: returns the raw contents of `/program.json` (or an empty-program JSON if missing).
- `irrigationIsRunning()`: true while a step is actively driving a pin. Currently only consumed internally by the NTP gate.

### `telegram.{h,cpp}` — bot polling, command handlers, outbound notifier
- `telegramSetup()`: `client.setInsecure()` only — the bot is constructed at file scope.
- `telegramLoop()`: each tick (1) drains the event bus into outbound messages, (2) if 3 s have elapsed, polls Telegram for new commands.
- `telegramSend(msg)`: direct send used by `main.cpp` (boot message) and `network.cpp` (internet-restored notice). Irrigation does **not** call this — it goes through the event bus.
- Inbound commands (only accepted from `CHAT_ID`): `/config_read`, `/config_set` → enters multi-message edit mode, `/config_save`, `/config_cancel`, `/stop` (aborts any running step and forces every allowed pin LOW).
- Event drain emits human-readable messages for `PinChanged`, `ConfigSaved`, `ConfigError`, `ClockSynced`, `ProgramStarted`, `ProgramEnded`.

### `network.{h,cpp}` — non-blocking internet health watchdog
- `networkBegin(ssid, password)` called once after the initial STA connect succeeds. Stores credentials and disables `WiFi.persistent` (avoids flash wear from repeated `WiFi.begin()` on long outages).
- `networkLoop()` runs a five-state machine. Each call does at most one short step:
  - `NET_OK` — every 60 s tests `http://clients3.google.com/generate_204`. On failure records `outageStartMills`, calls `WiFi.disconnect(true)`, → `NET_DISCONNECTING`.
  - `NET_DISCONNECTING` — after 1500 ms (lets the SDK park the radio): `WiFi.mode(WIFI_STA); WiFi.begin(...)` → `NET_RECONNECTING`.
  - `NET_RECONNECTING` — polls `WiFi.status()`. Associated → `NET_TESTING`. Times out after 10 s → `NET_COOLDOWN`.
  - `NET_TESTING` — re-runs the upstream probe. Success → sends `🟢 Internet restored after <duration>` via Telegram and returns to `NET_OK`. Failure → `NET_COOLDOWN`.
  - `NET_COOLDOWN` — waits 60 s then retries from `NET_DISCONNECTING`. No AP fallback; retries forever.
- The 3 s `hasInternet()` HTTP GET is the only blocking call in the module, and it runs at most once per minute in steady state.

### `events.{h,cpp}` — fixed-size SPSC ring buffer
- 8-slot in-place buffer of `IrrigationEvent { type, pin, val, minute, detail }`. Drops oldest on overflow. No dynamic allocation.
- Producer: `irrigation.cpp` (via `events::emit`). Consumer: `telegram.cpp` (via `events::pop` in its drain loop).
- Event types: `PinChanged`, `ConfigSaved`, `ConfigError`, `ClockSynced`, `ProgramStarted`, `ProgramEnded`.
- The `minute` field is misnamed: it carries the current **week-second** (0..604 799). Kept for ABI inertia — rename later if desired.

## Schedule Format

`/program.json` on SPIFFS holds one irrigation program: a sequence of steps (pin + duration, run strictly sequentially) and a list of weekly start times at which the whole sequence fires.

```json
{
  "steps": [
    {"pin": 5,  "dur": 300},
    {"pin": 12, "dur": 180}
  ],
  "starts": [
    {"d": 2, "t": "06:00:00"},
    {"d": 4, "t": "06:00:00"}
  ]
}
```

- `d`: 1 = Sunday .. 7 = Saturday.
- `dur` is in seconds. `t` is local time of day as `"HH:MM:SS"`.
- Per-step `dur` capped at 900 s (15 min). The cap is a safety invariant, not a preference.
- Starts on the same `d` must be non-overlapping given the program's total duration, and must finish before midnight on that day. This is what enforces "N runs per day, as long as N × duration fits in a day."

When no NTP has ever been acquired, "day 1, 00:00:00" is whatever moment the device booted — the plant still gets watered at the configured cadence, just on a virtual week. NTP, once acquired, snaps the clock to the real wall time (only between programs, never mid-run).

## Credentials

`src/credentials.h` is git-ignored. It must define:
```cpp
#define BOTtoken  "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID   "YOUR_TELEGRAM_CHAT_ID"
```

## Key Constants

| Constant | Value | Where | Meaning |
|---|---|---|---|
| `TICK_INTERVAL_MS` | 1 000 ms | `irrigation.cpp` | IDLE tick cadence (running steps tick every loop iteration) |
| `SECONDS_PER_WEEK` | 604 800 | `irrigation.cpp` | Week-second clock modulus |
| `MAX_MATCH_GAP_SEC` | 300 s | `irrigation.cpp` | Forward tick gap above which start matching is skipped (treated as clock discontinuity) |
| `UTC_OFFSET_SEC` | −3 × 3 600 | `irrigation.cpp` | Local-time offset applied to NTP-derived week-second (hardcoded UTC−3) |
| `MAX_STEPS` | 8 | `irrigation.h` | Max steps per program |
| `MAX_STARTS` | 16 | `irrigation.h` | Max weekly start times |
| `MAX_STEP_DURATION` | 900 s | `irrigation.h` | Hard per-step duration cap |
| `CONFIG_FILE` | `/program.json` | `irrigation.h` | SPIFFS program path |
| `MAX_JSON_SIZE` | 1 024 bytes | `irrigation.h` | ArduinoJson document cap |
| `TELEGRAM_POLL_INTERVAL` | 3 000 ms | `telegram.cpp` | Inbound poll cadence |
| `CHECK_INTERVAL` | 60 000 ms | `network.cpp` | Internet probe / cooldown duration |
| `DISCONNECT_DELAY` | 1 500 ms | `network.cpp` | Pause after `WiFi.disconnect(true)` |
| `RECONNECT_TIMEOUT` | 10 000 ms | `network.cpp` | Give up on `WL_CONNECTED` after this |

## Known Issues

- Timezone is hardcoded as UTC−3 in `irrigation.cpp` (`advanceClock`).
- `/status` Telegram command is advertised in `buildDeviceInfoMessage()` (`main.cpp`) but has no handler in `telegramLoop()`.
- `connectToWiFi()` in `main.cpp` is still blocking (~10 s) and falls back to AP mode on failure. The non-blocking recovery in `network.cpp` only runs after the initial connect succeeds.
- Day-of-week is virtual until NTP syncs — there is no way to defer the program until "real Sunday" if that matters for a given install.
- The legacy `/config.json` from before the program refactor is no longer read; it stays on SPIFFS as dead data until manually removed (or until SPIFFS is reformatted).
- `waterGuy.ino`, `waterGuy2.inok`, `py-utils/`, `html/`, `doc/` are legacy leftovers not used by the PlatformIO build.

## Allowed GPIO Pins

`{2, 4, 5, 12, 13, 14, 16}` — validated by `isAllowedPin()` in `webserver.cpp` (manual toggle path) and by `parseProgram()` in `irrigation.cpp` (program-save path). `irrigationSetup()` initialises all of these as OUTPUT LOW on boot.
