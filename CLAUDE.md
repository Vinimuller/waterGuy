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
- Owns the `ESP8266WebServer` instance on port 80 and the allowed-pin tracking arrays (`ALLOWED_PINS = {4, 5, 12, 13, 14, 16}`, `pinInitialized`, `pinState`) used by the manual-toggle and status endpoints.
- `webserverSetup()`: registers all routes and starts the server. `webserverLoop()`: services pending clients.
- HTTP routes: `GET/POST /` (WiFi config UI — POST delegates to `wifiConfigSave` then reboots), `GET/POST /saveConfig` (schedule editor — delegates to `irrigationSaveConfig`/`irrigationReadConfig`), `GET /toggle?pin=N`, `GET /status` (JSON of allowed-pin states), `GET /get-file?file=<path>`.

### `wifi_config.{h,cpp}` — WiFi credential file I/O
- Owns `/configWifi.json` on SPIFFS. `wifiConfigLoad(ssid, password)` reads it (returns false if missing); `wifiConfigSave(ssid, password)` writes it.
- Loaded by `main.cpp` on boot. Written by the `/` POST handler in `webserver.cpp`, which then reboots to apply.

### `irrigation.{h,cpp}` — schedule clock, reconcile, config I/O
- `irrigationSetup()`: inits GPIO 2 relay, creates empty `/config.json` if missing
- `irrigationLoop()`: every 10 s (`RECONCILE_INTERVAL`) calls `reconcileSchedule()`
- `advanceClock()`: maintains `currentMinuteOfWeek` (0–10079). Free-runs from `millis()` until NTP is acquired, then snaps to NTP truth. Overflow-safe across the 49.7-day `millis()` wrap. Hardcoded UTC−3 offset.
- `reconcileSchedule()`: reads `/config.json`, computes desired state per pin (most recent event ≤ current minute, with wrap-around fallback), writes any pin whose state needs to change, and **emits a `PinChanged` event** for each change.
- `irrigationSaveConfig(json)`: single owner of writes to `/config.json`. Validates JSON shape (every entry has pin/val/time), enforces ON/OFF pairing per pin and 60-min max active window. Returns `""` on success / error string on failure, and emits `ConfigSaved` / `ConfigError` events. Called from both the HTTP `/saveConfig` route and the Telegram `/config_save` command.
- `irrigationReadConfig()`: returns the raw contents of `/config.json` (or `"[]"` if missing).

### `telegram.{h,cpp}` — bot polling, command handlers, outbound notifier
- `telegramSetup()`: `client.setInsecure()` only — the bot is constructed at file scope.
- `telegramLoop()`: each tick (1) drains the event bus into outbound messages, (2) if 3 s have elapsed, polls Telegram for new commands.
- `telegramSend(msg)`: direct send used by `main.cpp` (boot message) and `network.cpp` (internet-restored notice). Irrigation does **not** call this — it goes through the event bus.
- Inbound commands (only accepted from `CHAT_ID`): `/config`, `/config_set` → enters multi-message edit mode, `/config_save`, `/config_cancel`.
- Event drain emits human-readable messages for `PinChanged`, `ConfigSaved`, `ConfigError`, `ClockSynced`.

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
- Event types: `PinChanged`, `ConfigSaved`, `ConfigError`, `ClockSynced`.

## Schedule Format

`/config.json` on SPIFFS — array of `{time, pin, val}` objects. `time` is minutes from week start (0–10079, Sunday 00:00 = 0). Events must be in ON/OFF pairs per pin; max 60 min active per pin.

```json
[{"time": 480, "pin": 5, "val": 1}, {"time": 510, "pin": 5, "val": 0}]
```

Reconcile is idempotent: it always re-derives each pin's desired state from the most recent event ≤ current minute (with wrap-around to last week's final event). NTP snaps that don't land on event boundaries are absorbed automatically.

## Credentials

`src/credentials.h` is git-ignored. It must define:
```cpp
#define BOTtoken  "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID   "YOUR_TELEGRAM_CHAT_ID"
```

## Key Constants

| Constant | Value | Where | Meaning |
|---|---|---|---|
| `RECONCILE_INTERVAL` | 10 000 ms | `irrigation.cpp` | Schedule reconcile cadence |
| `EVENT_LOOP_MAX` | 10 080 | `irrigation.h` | Minutes in a week |
| `CONFIG_FILE` | `/config.json` | `irrigation.h` | SPIFFS schedule path |
| `MAX_JSON_SIZE` | 1 024 bytes | `irrigation.h` | ArduinoJson document cap |
| `TELEGRAM_POLL_INTERVAL` | 3 000 ms | `telegram.cpp` | Inbound poll cadence |
| `CHECK_INTERVAL` | 60 000 ms | `network.cpp` | Internet probe / cooldown duration |
| `DISCONNECT_DELAY` | 1 500 ms | `network.cpp` | Pause after `WiFi.disconnect(true)` |
| `RECONNECT_TIMEOUT` | 10 000 ms | `network.cpp` | Give up on `WL_CONNECTED` after this |

## Known Issues

- Timezone is hardcoded as UTC−3 in `irrigation.cpp` (`advanceClock`).
- `/status` Telegram command is advertised in `buildDeviceInfoMessage()` (`main.cpp`) but has no handler in `telegramLoop()`.
- `connectToWiFi()` in `main.cpp` is still blocking (~10 s) and falls back to AP mode on failure. The non-blocking recovery in `network.cpp` only runs after the initial connect succeeds.
- `waterGuy.ino`, `waterGuy2.inok`, `py-utils/`, `html/`, `doc/` are legacy leftovers not used by the PlatformIO build.

## Allowed GPIO Pins

`{4, 5, 12, 13, 14, 16}` — validated by `isAllowedPin()` in `main.cpp`. GPIO 2 is the hardcoded scheduler relay pin (initialised in `irrigationSetup`).
