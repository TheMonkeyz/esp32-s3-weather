# Changelog

What changed in each release. The display shows these notes on its update screen, and the settings page shows
them before you install. Only the entries newer than the version you have are shown. The web flasher site
publishes them as `notes.json`, built by `tools/make_flasher_site.py`.

How to write an entry: add a `## vX.Y.Z - YYYY-MM-DD` section at the top **before** tagging the release, with one
`- ` line per change. Write for the person holding the display, not for developers. Release candidates can get
their own `## vX.Y.Z-rc.N` section. Those are shown only to Beta users, while the final release's section should
list everything again.

## v1.3.1 - 2026-10-01
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
