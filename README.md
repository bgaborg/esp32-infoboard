# esp32-infoboard

Small always-on info screen for the Freenove ESP32-S3 Display (FNK0104B, 2.8" 320x240 ILI9341, FT6336U touch).

The first page shows live Budapest public transport departures from the BKK FUTAR API. The plan is to add more pages (weather and others) and to switch between them by touch.

Terms: **screen** = the physical display. **page** = one app (transit, weather). **view** = one layout inside a page.

## Current features

- Live departures for one stop, two directions, a configurable set of routes.
- Minutes until departure, counted down locally between API polls.
- Colors: green = live GPS time, orange `+N` = late by N minutes, white = timetable only, red `elmarad` = canceled.
- BKK alerts for the shown routes scroll in the footer (header and full description).
- The footer shows WiFi state, stale-data warning, or WiFi signal strength.
- Screen timeout (default 5 min). No API polls while the screen is off.
- Night dimming 22:00-06:00.

## Controls

| Input | Action |
|---|---|
| Tap (screen off) | Wake the screen |
| Tap (screen on) | Next view: both directions, direction A only, direction B only |
| Hold 1.5 s | Change the screen timeout: 5, 15, 30 min, always on. Saved in flash. |
| BOOT button | Wake the screen and refresh now |

## API usage and resources

- One HTTPS request per poll covers both stops.
- Poll interval: 30 s if a bus leaves within 20 min, else 90 s. Backoff up to 5 min on errors.
- The JSON response streams through an ArduinoJson filter, so the full response is never in RAM.
- The fetch runs in its own FreeRTOS task, so drawing never stops.
- Only changed rows are redrawn.
- TLS checks the server certificate against DigiCert Global Root G2 (`src/ca.h`, valid to 2038).

## Setup

1. Install PlatformIO: `uv tool install platformio`
2. Request a free BKK API key at https://opendata.bkk.hu
3. `cp .env.example .env` and fill it in. Git ignores `.env`. At build time, `config.py` writes `src/config.h` from `.env`.
4. Connect the board over USB and run `pio run -t upload`.
5. Serial log: `pio device monitor`

### Find stop and route IDs

```bash
curl -4 "https://futar.bkk.hu/api/query/v1/ws/otp/api/where/search.json?query=<stop name>&key=<API key>"
```

`data.references.stops` gives the stop IDs (one per direction), `data.references.routes` gives the route IDs.

### Tuning

Constants at the top of `src/main.cpp`: poll intervals, timeout options, backlight levels, dimming hours, hold time.

## Hardware notes

- ESP32-S3, 16 MB flash, 8 MB PSRAM (not enabled). WiFi is 2.4 GHz only.
- Display and touch pins are in `platformio.ini` and `src/main.cpp`. They come from the Freenove TFT_eSPI setup file for FNK0104AB.
- Built-in TFT_eSPI fonts are ASCII only. `asciify()` in `src/logic.h` converts Hungarian accented letters.

## Freenove kit resources

Freenove publishes the kit package at https://github.com/Freenove/Freenove_ESP32_S3_Display. It contains tutorials, Arduino example sketches, tested library versions and TFT_eSPI setup files for each display model. This project does not need it. Use it to run the Freenove examples in the Arduino IDE. Also use it to find pins for hardware this project does not use yet: speaker, SD card, battery voltage, RGB LED.

## Roadmap

- More transit info: vehicle details (if the API gives them), delay trend, more departures per view.
- Page system: each page (transit, weather, ...) is its own module with `fetch()` and `draw()`.
- Swipe to switch pages. Each page has its own poll interval. Only the visible page polls.
- Weather page: current conditions and forecast from a free API.
- Configuration over WiFi (web page on the device) instead of rebuilding for `.env` changes.
- OTA firmware updates.

## Tests

Host-side check of the text and time helpers:

```bash
c++ -std=c++17 -I src test/check.cpp -o test/check && test/check
```
