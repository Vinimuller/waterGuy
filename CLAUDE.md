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

Dependencies are managed by PlatformIO via `platformio.ini` (ArduinoJson 6.x, UniversalTelegramBot).

## Architecture

The firmware splits into two logical layers:

**`main.cpp`** — connectivity and HTTP layer
- State machine: `INIT` (first 60 s, AP open for setup) → `RUNNING`
- On boot: starts AP (`waterGuy` / `~~~~~~~~`), waits for WiFi credentials via web form at `192.168.4.1`
- After 60 s with no AP client: reads `/configWifi.json` from SPIFFS, connects STA, sends Telegram startup message
- HTTP routes: `GET/POST /` (WiFi config + GPIO toggle UI), `GET/POST /saveConfig` (schedule editor), `GET /toggle?pin=N`, `GET /status`, `GET /get-file?file=<path>`
- Calls `eventScheduleLoop()` every loop iteration in RUNNING state

**`evenLoop.cpp`** (note: filename typo — header is `eventLoop.h`) — schedule and Telegram layer
- `eventScheduleSetup()`: NTP sync, GPIO2 relay init, creates empty `/config.json` if missing
- `updateCounterLoop()`: every 60 s, reads NTP time, computes `eventLoopCounter` = minutes since Sunday 00:00 in UTC−3 (hardcoded offset)
- `eventScheduleLoop()`: every 60 s (`EVENT_LOOP_TICK`), reads `/config.json` and fires any GPIO event matching `eventLoopCounter`
- `telegramLoop()`: polls Telegram every 3 s; only accepts messages from `CHAT_ID` (defined in `credentials.h`)

## Schedule Format

`/config.json` on SPIFFS — array of `{time, pin, val}` objects. `time` is minutes from week start (0–10080). Events must be in ON/OFF pairs; max 60 min active per pin.

```json
[{"time": 480, "pin": 5, "val": 1}, {"time": 510, "pin": 5, "val": 0}]
```

## Credentials

`src/credentials.h` is git-ignored. It must define:
```cpp
#define BOTtoken  "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID   "YOUR_TELEGRAM_CHAT_ID"
```

## Key Constants (eventLoop.h)

| Constant | Value | Meaning |
|---|---|---|
| `EVENT_LOOP_TICK` | 60 000 ms | Schedule check interval |
| `EVENT_LOOP_MAX` | 10 080 | Minutes in a week |
| `CONFIG_FILE` | `/config.json` | SPIFFS path |
| `MAX_JSON_SIZE` | 1 024 bytes | ArduinoJson document cap |

## Known Issues

- `evenLoop.cpp` filename typo (should be `eventLoop.cpp`)
- Timezone is hardcoded as UTC−3 in `evenLoop.cpp:62`
- `/status` Telegram command is advertised in `buildDeviceInfoMessage()` but has no handler in `telegramLoop()`
- `checkInternetLoop()` exists but is commented out in `loop()`
- `waterGuy.ino`, `py-utils/`, and `html/` are legacy leftovers not used by the PlatformIO build

## Allowed GPIO Pins

`{4, 5, 12, 13, 14, 16}` — validated by `isAllowedPin()` in `main.cpp`. GPIO 2 is the hardcoded relay pin.
