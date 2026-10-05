# Feature ideas

Backlog of possible features (2026-09-30). ⭐ = suggested first. Move an item to "Done" when it ships.

## Using more of the board

The board also has a speaker (ES8311), a motion sensor (QMI8658), a real-time clock (PCF85063) and battery
charging (AXP2101).

- [ ] **Night clock** — when dimmed, a very dim large clock instead of a black screen.
- [ ] **Battery level** — when running on a battery.

## Everyday use

- [ ] **Home Assistant (MQTT)** — publish room presence from the microphones; show an indoor temperature.
- [ ] **Wi-Fi setup pages that follow the finger** — the two setup pages switch on an LVGL gesture; espforge's are a
  pager dragged as pictures (its `main/ui.c`). Weather's `slide_drag` could do the same (v1.13.0 left it out).
- [ ] **Online setup: the home network back on the setup network's page** — opened while connected, the setup network
  starts alongside the connection; after a visit to the Easy Connect page (which leaves the home network) the display
  stays off it until setup closes (10 min at most). Not new in v1.13.0 (before, `net_setup_ap_start()` paused the
  reconnect `net_dpp_stop()` had just scheduled). Reconnect when page 1 is shown and a saved network is in range.

## To evaluate

- [ ] **LVGL 8 instead of 9** (the owner, October 3: "I've read that LVGL 8 is better than 9"). Test it rather than
  assume: LVGL 9.2 can't redraw a full screen in less than ~65-85 ms here (docs/TESTING.md §8), and v8 is said to be
  faster on small MCUs. A port touches every `lv_*` call (v9 renamed many), `slide.c`'s use of LVGL internals
  (layers, draw buffers, `lv_display_t`), `lvgl_mem.c` and the fonts. Measure first: the render bench and a
  full-screen redraw of each screen on v8 against v9 in a throwaway build, before deciding.
  Measured October 4 (`tools/lvglbench/`): with pre-rendered fonts v8 draws these screens 19-26 % faster than v9;
  with the app's TTF fonts 3-4x slower. v8.4's support ended in March 2025. The bigger lead: the app's v9 weather
  screen takes twice the benchmark's 24 ms, so something in the app (the LVGL heap in PSRAM?) costs more than the
  version. Also on v9: LVGL 9.3+ can draw on both cores (`LV_DRAW_SW_DRAW_UNIT_CNT=2`, needs `LV_USE_OS`), and 9.6's
  software renderer is reported 4-25 % faster.
  Followed up the same day (`tools/lvglbench/README.md`): the gap was the benchmark's simpler screen, not the app (heap
  in PSRAM, labels, fonts and the other core all ruled out); two draw units gain little on 9.2.2 and lose on 9.6, and
  9.6 is slower than 9.2.2 on these screens. Decision: stay on LVGL 9.2.2.
- [ ] **The display in the browser, on the flasher site** (the owner, October 3). The firmware binary can't run in a
  browser (no emulator covers this board's AMOLED panel, touch chip and Wi-Fi), but the same UI code could: LVGL +
  `ui.c` built to WebAssembly from the stable tag, drawing to a canvas, with the mouse as the finger and the browser
  fetching the forecast and radar (Open-Meteo, GeoMet, the alerts API and OSM tiles all send
  `Access-Control-Allow-Origin: *`, checked October 3). Mind OSM's tile policy for a public demo (cache, low zoom).
  Cirkit Designer's ESP32-S3 simulator (Rust/WASM, runs real firmware) was looked at on October 3: Arduino sketches
  only (ESP-IDF "coming soon"), plain SPI/I2C devices but not this board's QSPI AMOLED, CST9217 touch or I2S audio,
  nothing said about embedding. Worth a second look once it runs ESP-IDF projects.


- [x] **First setup with Easy Connect opens the settings page** — when a brand-new display gets its Wi-Fi from
  Android Easy Connect, take the phone to the settings page (place, units) on its own, as the setup network's
  sign-in page does (the user's request, October 2). Easy Connect gives no channel back to the phone, so it needs
  another way: e.g. a QR code with the page's address on the display right after it connects for the first time. Done in v1.12.0: after the first forecast the display shows the
  settings code by itself ("Choose your location"), then a gesture hint.

- [x] **Rain starting soon** — "Rain around 14:45" / "Rain until about 15:30" on the weather screen, from
  Open-Meteo's 15-minute forecast (2026-09-30).
- [x] **Weather warnings** — Environment Canada alerts: coloured pill on the weather screen, details screen
  with a map of the affected region (2026-09-30).
- [x] **Extras page** — swipe right: sun arc (sunrise/sunset, sun position), UV index, moon phase with picture,
  air quality (US AQI), pollen where available (Europe only) (2026-09-30).
- [x] **Updates over Wi-Fi (OTA)** — Stable/Beta channels from the flasher site, pill + update screen, settings
  page card, rollback (v1.3.0).
- [x] **Status page** — firmware version and the health of every online service, two swipes right (v1.4.0).
- [x] **Temperature graph** — the day's temperature curve, 0–24 h, at the top of each hourly-view page (v1.4.0).
- [x] **Units and formats** — °C/°F, km/h / mph / m/s (radar in miles with mph), 24- or 12-hour clock, on the
  settings page (v1.5.0).
- [x] **Several places** — up to 4, one weather page each, drag up/down to switch; everything follows the place
  shown (v1.5.0).
- [x] **Alert sounds** — warning beeps for new weather alerts, by level; level / volume / quiet hours (v1.8.0).
  An alarm clock or an hourly chime could reuse `sound.c`.
- [x] **French interface** — English / Canadian French for the display and the settings page, one setting;
  built so more languages are a column in `i18n_strings.h` (v1.8.0).
- [x] **Inuktitut interface (draft)** — syllabics, generated from `docs/translations/iu.tsv` (v1.9.0). Still needs
  a fluent speaker's review: see `docs/translations/iu-review.md`.
- [x] **Settings on the display** — long-press: screen (dimming, pick-up, timing, brightness), units, phone QR,
  Wi-Fi, updates, restart (v1.6.0).
- [x] **Wake on pick-up** — the motion sensor wakes the screen when it's lifted or tilted (a table bump doesn't, at
  Normal sensitivity); settings page: on/off, sensitivity, live meter (v1.6.0).
- [x] **Lightning on the radar** — Environment Canada lightning layer over the rain: yellow bolts, in the
  animation too (v1.10.0).
