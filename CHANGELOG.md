# Changelog

What changed in each release. The display shows these notes on its update screen, and the settings page shows
them before you install. Only the entries newer than the version you have are shown. The web flasher site
publishes them as `notes.json`, built by `tools/make_flasher_site.py`.

How to write an entry: add a `## vX.Y.Z - YYYY-MM-DD` section at the top **before** tagging the release, with one
`- ` line per change. Write for the person holding the display, not for developers. Release candidates can get
their own `## vX.Y.Z-rc.N` section. Those are shown only to Beta users, while the final release's section should
list everything again.

## v1.5.0 - 2026-09-30
- Several places: add up to 4 (home, cottage, work...) on the settings page, then drag up or down on the weather screen to switch. Each place shows its own local time; alerts, air quality, the hourly view and the radar follow the place shown.
- On the settings page, tap a place to change it, and pick the exact spot on a map: tap the map or drag the pin. Handy for a cottage or anywhere you aren't right now.
- Units on the settings page: temperature in °C or °F, wind in km/h, mph or m/s, and a 24-hour or 12-hour clock. The display switches right away. With mph, the radar shows distances in miles.

## v1.4.0 - 2026-09-30
- The hourly view now starts with a graph of the day's temperature, from midnight to midnight, with the high and low marked. On today's page, the hours already past are greyed out and a dot shows the current hour.
- New status page: swipe right twice from the weather screen. It shows the firmware version, the Wi-Fi signal, and whether each online service the display uses is working, when it was last reached and why it failed.
- Opening the status page checks any service that hasn't been contacted in the last 5 minutes.

## v1.4.0-rc.2 - 2026-09-30
- Test version of the temperature graph at the top of the hourly view: the day's temperature from midnight to midnight, high and low marked, a dot at the current hour.

## v1.4.0-rc.1 - 2026-09-30
- Test version of the new status page (swipe right twice from the weather screen): firmware version, Wi-Fi signal, and the state of each online service the display uses.

## v1.3.1 - 2026-09-30
- The update screen and the settings page now show what's new before you install.

## v1.3.0 - 2026-09-30
- Updates over Wi-Fi: an "Update" pill appears on the weather screen when a new version is out. Tap it, then Install. Settings are kept.
- Choose Stable or Beta updates on the settings page (Firmware).
- If a new version doesn't start properly, the display goes back to the previous one by itself.
- Weather alerts from Environment Canada: tap the alert at the top for the details and a map of the area.
- Rain nowcast: "Rain around 14:45" when rain starts or stops in the next 2 hours.
- Extras page (swipe right from the weather): sunrise and sunset, UV, moon phase, air quality.

## v1.2.0 - 2026-09-30
- The hourly details view now covers 7 days.
- The radar animation keeps looping for a minute. Tap again to stop it.
- The radar loop no longer stops when a new radar image arrives, and the live view falls back to the newest image that loaded.
- MIT license.

## v1.1.0 - 2026-09-30
- Wi-Fi setup is reachable when the saved network is down: the setup QR code appears by itself after 30 seconds.
- Android phones can share their Wi-Fi network by scanning a QR code, no typing needed.
- The web flasher offers Stable and Beta versions.

## v1.0.0 - 2026-09-30
- First release: current weather, 3-day forecast, hourly details, radar with zoom, presence dimming, settings page.
