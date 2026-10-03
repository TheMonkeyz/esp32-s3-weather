# Feature ideas

Backlog of possible features (2026-09-30). ⭐ = suggested first. Move an item to "Done" when it ships.

## Using more of the board

The board also has a speaker (ES8311), a motion sensor (QMI8658), a real-time clock (PCF85063) and battery
charging (AXP2101).

- [ ] **Night clock** — when dimmed, a very dim large clock instead of a black screen.
- [ ] **Battery level** — when running on a battery.

## Everyday use

- [ ] **First setup with Easy Connect opens the settings page** — when a brand-new display gets its Wi-Fi from
  Android Easy Connect, take the phone to the settings page (place, units) on its own, as the setup network's
  sign-in page does (the user's request, October 2). Easy Connect gives no channel back to the phone, so it needs
  another way: e.g. a QR code with the page's address on the display right after it connects for the first time.
- [ ] **Home Assistant (MQTT)** — publish room presence from the microphones; show an indoor temperature.

## Done

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
