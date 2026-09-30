# Feature ideas

Backlog of possible features (2026-09-30). ⭐ = suggested first. Move an item to "Done" when it ships.

## Weather

- [ ] ⭐ **Rain starting soon** — "Rain around 14:40" on the weather screen from Open-Meteo's 15-minute
  precipitation forecast (`minutely_15`). *(in progress)*
- [ ] **Weather warnings** — Environment Canada watches/warnings for the location as a coloured banner.
- [ ] **Temperature graph** — the day's temperature curve at the top of each hourly-view page.
- [ ] **Extras page** — sunrise/sunset, UV index, air quality and pollen (Open-Meteo), as a third swipe screen.
- [ ] **Lightning on the radar** — Environment Canada lightning layer over the rain.

## Using more of the board

The board also has a speaker (ES8311), a motion sensor (QMI8658), a real-time clock (PCF85063) and battery
charging (AXP2101).

- [ ] ⭐ **Updates over Wi-Fi (OTA)** — check GitHub Releases (Stable or Beta channel), ask, update without USB.
  Needs an OTA partition layout (two app slots) instead of the single factory partition.
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

- (move items here)
