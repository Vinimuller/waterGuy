# Water Guy

ESP8266-based automated irrigation system. Controls water pumps/solenoids on a weekly schedule via GPIO pins. Managed remotely through a Telegram bot or a local web interface.

---

## Hardware

- **Board:** NodeMCU v2 (ESP8266 / ESP-12E)
- **Allowed GPIO pins:** 4, 5, 12, 13, 14, 16
- **Relay:** GPIO 2 (hardcoded in `evenLoop.cpp`)

---

## Project Structure

```
waterGuy/
├── src/
│   ├── main.cpp          # WiFi, web server, HTTP handlers, main loop
│   ├── evenLoop.cpp       # Event scheduler, Telegram bot, config logic
│   ├── eventLoop.h        # Shared constants and function declarations
│   ├── html.h             # Embedded HTML pages (settings + WiFi config)
│   └── credentials.h      # BOTtoken and CHAT_ID (git-ignored)
├── html/                  # Standalone HTML files (development reference)
├── py-utils/              # Legacy Python scripts (superseded by C++ impl)
│   ├── waterGuy.py
│   ├── telegram.py
│   └── weekdayToTime.xlsx
├── doc/
│   └── waterguy.drawio    # Architecture diagram
├── waterGuy.ino           # Original monolithic sketch (pre-refactor, unused)
└── platformio.ini
```

---

## How the Code Is Organized

### `main.cpp`
Owns everything related to connectivity and HTTP:
- SPIFFS initialization
- WiFi connection (STA mode) and fallback Access Point (`waterGuy` SSID)
- Web server on port 80 with these routes:
  - `GET /` — WiFi configuration form + GPIO toggle UI
  - `POST /` — Save WiFi credentials and reboot
  - `GET /saveConfig` — Irrigation schedule editor
  - `POST /saveConfig` — Save JSON schedule to SPIFFS
  - `GET /toggle?pin=N` — Toggle a GPIO output
  - `GET /status` — Returns GPIO states as JSON
  - `GET /get-file?file=/path` — Serve any SPIFFS file
- Startup Telegram notification with device IP and config summary
- Internet connectivity check (`checkInternetLoop`, currently disabled)
- State machine: `INIT` (first 60 s, AP mode open for setup) → `RUNNING`

### `evenLoop.cpp`
Owns the irrigation logic and Telegram interface:
- NTP time sync (UTC−3 hardcoded)
- Weekly minute counter (`eventLoopCounter`, range 0–10 080)
- Schedule execution: every 60 s reads `/config.json` and fires GPIO events that match the current minute
- Telegram polling every 3 s for commands:
  - `/config` — show current schedule
  - `/config_set` — enter multi-message edit mode
  - `/config_save` — validate and persist edited config
  - `/config_cancel` — discard edits
- Config validation: enforces matched ON/OFF pairs per pin, max 60 min active per pin
- Creates an empty `[]` config on first boot if the file is missing

### `eventLoop.h`
Shared constants:
| Constant | Value | Meaning |
|---|---|---|
| `EVENT_LOOP_TICK` | 60 000 ms | Schedule check interval |
| `EVENT_LOOP_MAX` | 10 080 | Minutes in a week |
| `CONFIG_FILE` | `/config.json` | SPIFFS path |
| `MAX_JSON_SIZE` | 400 bytes | ArduinoJson document size |

### `html.h`
Two minified HTML strings embedded directly in the binary:
- `settingsHtml` — JSON schedule textarea editor
- `wifiHtml` — WiFi setup form + GPIO toggle buttons

---

## Schedule Format

`/config.json` is a JSON array of events. Each event sets a GPIO pin high or low at a given minute offset from the start of the week (Sunday 00:00 UTC−3 = minute 0).

```json
[
  { "time": 480,  "pin": 5, "val": 1 },
  { "time": 510,  "pin": 5, "val": 0 },
  { "time": 1920, "pin": 4, "val": 1 },
  { "time": 1950, "pin": 4, "val": 0 }
]
```

Events must be in pairs (ON then OFF). Max active duration per pin: 60 minutes.

---

## Build & Flash

```bash
# Install PlatformIO, then:
pio run --target upload
pio run --target uploadfs   # upload SPIFFS (config files)
pio device monitor          # serial monitor at 115200
```

Dependencies are declared in `platformio.ini`:
- `bblanchon/ArduinoJson ^6.21.5`
- `witnessmenow/Universal-Arduino-Telegram-Bot` (GitHub)

---

## First Boot

1. Device starts as AP: SSID `waterGuy`, password `~~~~~~~~`
2. Connect and open `192.168.4.1` to enter WiFi credentials
3. After 60 s with no AP client connected, device switches to STA mode and connects to saved WiFi
4. Telegram receives a startup message with the local IP

---

## Code Organization Issues & Improvement Ideas

### Immediate (low effort)

- **Filename typo:** `evenLoop.cpp` should be `eventLoop.cpp` to match the header `eventLoop.h`.
- **Dead files:** `waterGuy.ino` and `waterGuy2.inok` are pre-refactor leftovers. `py-utils/` is superseded. `html/` is superseded by `html.h`. These can all be deleted.
- **Hardcoded timezone:** `-3 * 3600` in `evenLoop.cpp:49` should be a named constant or configurable via the web UI.
- **`checkInternetLoop()` is commented out:** Either remove it or re-enable it — dead code in the main loop is confusing.
- **`/status` command listed but not handled:** `buildDeviceInfoMessage()` advertises `/status` as a Telegram command but `telegramLoop()` has no handler for it.

### Structural

- **`main.cpp` is doing too much:** WiFi management, HTTP routing, HTML rendering, device info formatting, and the internet-check logic are all in one 380-line file. Extract into separate files:
  - `wifi.cpp/h` — `connectToWiFi()`, `startAccessPoint()`, `handleInternetFailure()`
  - `webServer.cpp/h` — all `server.on(...)` handlers
  - `device.cpp/h` — `buildDeviceInfoMessage()`, `handleStatus()`

- **HTML is embedded as raw strings:** `html.h` is hard to edit and can't be updated OTA. A better approach is to store HTML files in SPIFFS and serve them from disk — this also removes the need for `{{FILE_CONTENT}}` string replacement hacks.

- **Config validation lives in `evenLoop.cpp`:** `validateConfig()` is a pure function with no dependency on the event loop. It belongs in its own `config.cpp/h` alongside `eventLoopNewFile()` and `saveTelegramConfig()`.

- **`MAX_JSON_SIZE 400` is very tight:** A single schedule entry is ~30 bytes. 400 bytes allows ~13 entries. This should be increased or made dynamic with `DynamicJsonDocument`.

### Longer term

- **Timezone should be configurable:** Store UTC offset in SPIFFS, editable via web UI or Telegram.
- **Schedule editing via web UI:** The web settings page only has a raw JSON textarea. A table-based UI with day/time pickers would be far less error-prone.
- **No OTA update support:** `ArduinoOTA` or `ESP8266HTTPUpdateServer` would allow flashing without physical access.
- **WiFi credentials in plaintext:** The SPIFFS file at `/configWifi.json` stores the password as plain text. Not a concern on a local embedded device, but worth noting.
