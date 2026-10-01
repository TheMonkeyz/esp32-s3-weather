# Feature ideas

Backlog of possible features (2026-09-30). ⭐ = suggested first. Move an item to "Done" when it ships.

## Weather

- [ ] **Lightning on the radar** — Environment Canada lightning layer over the rain.

## Using more of the board

The board also has a speaker (ES8311), a motion sensor (QMI8658), a real-time clock (PCF85063) and battery
charging (AXP2101).

- [ ] **Night clock** — when dimmed, a very dim large clock instead of a black screen.
- [ ] **Alarm / chime** — sound through the speaker, e.g. for a weather warning.
- [ ] **Wake on pick-up** — the motion sensor wakes the screen on a tap or when lifted, alongside the microphones.
- [ ] **Battery level** — when running on a battery.

## Everyday use

- [ ] ⭐ **French interface** — the font already handles accents.
- [ ] **Units and formats** — °F, mph, 12-hour clock as settings.
- [ ] **Several places** — home, cottage, work, each with its own weather screen.
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
