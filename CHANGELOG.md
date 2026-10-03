# Changelog

What changed in each release. The display shows these notes on its update screen, and the settings page shows
them before you install. Only the entries newer than the version you have are shown. The web flasher site
publishes them as `notes.json`, built by `tools/make_flasher_site.py`.

How to write an entry: add a `## vX.Y.Z - YYYY-MM-DD` section at the top **before** tagging the release, with one
`- ` line per change. Write for the person holding the display, not for developers. Release candidates can get
their own `## vX.Y.Z-rc.N` section. Those are shown only to Beta users, while the final release's section should
list everything again. (Two tags have no section: v1.3.0-rc.1, and v1.12.0-rc.1, whose build failed and was published
as v1.12.0-rc.2.)

## v1.12.0-rc.7 - 2026-10-03
- After Wi-Fi comes back, the forecast is fetched again at once (it could wait several minutes when the forecast service had been slow).

## v1.12.0-rc.6 - 2026-10-03
- The settings page no longer says "No speaker found" on displays that have one.
- Choosing another place on the settings page slides to it smoothly on the display.
- In French, the 12-hour clock reads "2 h 45 p.m.".
- A one-time screen shows how to get around: swipe for the other screens, drag up or down for your places, tap a day for the hours, press and hold for settings.
- If the display ever crashes, the details are kept and reported at the next start (after a USB or web-flasher install).
- The settings page and its screenshots load faster (the chip's encryption hardware is used again).

## v1.12.0-rc.5 - 2026-10-03
- The setup network's name lookups (which open the setup page on your phone) are answered only on the setup network, no longer on your home network.
- A slightly smaller firmware (an unused image decoder removed), and the map requests name the project.

## v1.12.0-rc.4 - 2026-10-03
- The hourly view and the "Today" column are right just after midnight (they showed the day before until the next update).
- When the forecast can't be updated, the weather screen says so: "No connection", or "Updated 45 min ago" above the clock, and "Can't reach the forecast service" instead of "Loading..." forever. It retries less often during a long outage.
- A new display shows the settings code by itself the first time, to choose your location.
- Hours without data in the forecast show "--" instead of 0° and 0 %.
- On the radar, the zoom distance follows your units (miles), and "No Wi-Fi" goes away as soon as Wi-Fi is back.
- Clearer wording: "sound" instead of "chime", plain words on the status page, a note about the Beta channel, and Inuktitut marked as a draft.
- The map in an alert's details credits OpenStreetMap.
- Two update messages were blank in Inuktitut.

## v1.12.0-rc.3 - 2026-10-03
- Changing settings from your phone now needs the code shown on the display: press and hold the weather screen, tap More on your phone, and scan it. Your phone remembers it. Anyone on your Wi-Fi can still look at the settings page.
- The display's setup network has its own password, shown on the display next to its code (it was the same for every display).
- When your Wi-Fi is down for a long time, the setup network opens by itself for 15 minutes, then the display just keeps trying; press and hold to open it again.
- If an update is undone because the display restarted too early, the update screen says so. The display now waits until an update is confirmed before restarting on its own, and keeps it only once it has worked with your Wi-Fi.
- After an interrupted download the Install button stays, and the display checks again a few minutes later. It also checks for updates as soon as your Wi-Fi comes back.
- When a setting could not be saved, the settings page says so instead of "Saved".
- Network names of 32 characters and passwords are checked before they are saved.

## v1.12.0-rc.2 - 2026-10-03
- The Install button on the settings page shows again when an update is offered.
- A red warning is never hidden behind lesser alerts when more than four are in force.
- An alert beeps once. Environment Canada updates a warning every few hours; the display now beeps again only if the warning gets worse.
- Swipes made during a radar zoom are no longer lost, and a rare freeze after a drag is gone.
- The display keeps running when a weather service sends an incomplete answer or memory is short, instead of restarting.

## v1.11.1 - 2026-10-02
- Smooth radar zoom: zooming in and out runs at about 45 frames per second instead of 10, and a second swipe made during a zoom now zooms again instead of being lost.
- Tapping the forecast opens today's hourly view at once.
- The hourly list scrolls more smoothly (about 60 frames per second).
- Moves start at once in more cases: right after the clock changes minute, and after the display checks for updates.
- Everything on screen is drawn about 30% faster after an install from the web flasher. An update over Wi-Fi brings the other improvements but keeps the previous drawing speed.

## v1.11.1-rc.2 - 2026-10-02
- Smooth radar zoom: zooming in and out runs at about 45 frames per second instead of 10, and a second swipe made during a zoom now zooms again instead of being lost.
- Tapping the forecast opens today's hourly view at once.
- The hourly list scrolls more smoothly (about 60 frames per second).
- Everything on screen is drawn about 30% faster after an install from the web flasher. An update over Wi-Fi brings the other improvements but keeps the previous drawing speed.

## v1.11.1-rc.1 - 2026-10-02
- Moves start at once in more cases: right after the clock changes minute, and after the display checks for updates. They could wait about a tenth of a second.

## v1.11.0 - 2026-10-02
- Smoother moves: going from screen to screen, from place to place (drag up or down) and from day to day in the hourly view now runs at 45 to 70 frames per second instead of 10 to 15. The screen follows your finger, bounces at the first and last one, and a quick flick is enough to go to the next. Moves start at once, even right after the previous one.
- Lists scroll smoothly: the hourly view's hours, Settings, the status page, alert details and the update notes now scroll at 50 to 70 frames per second instead of about 20. They follow your finger, keep going after a flick and slow down, stop when you touch them, and spring back at the top and bottom.
- Android Easy Connect: the setup screen and the settings page now say to scan the code with the phone's camera or any QR app while the phone is on your Wi-Fi. Not every phone has a QR icon in its Wi-Fi settings.
- Fixed: Android Easy Connect could still fail when the display couldn't reach its Wi-Fi, if your router was on a busy channel. The display now looks for your network by name to find its channel.
- Fixed: going back to your first place made the radar save its map twice, and the screen stuttered for a few seconds. It saves once now, and waits while you're touching the screen.
- Behind the scenes: the screen is fed twice as fast, radar images and map tiles are decoded with much less memory, and the display no longer runs a speed test by itself a minute after starting (it froze the screen for about 1.5 seconds).

## v1.10.1-rc.4 - 2026-10-02
- Lists scroll smoothly: the hourly view's hours, Settings, the status page, alert details and the update notes now scroll at 50 to 70 frames per second instead of about 20. They follow your finger, keep going after a flick and slow down, stop when you touch them, and spring back at the top and bottom.
- Android Easy Connect: the setup screen and the settings page now say to scan the code with the phone's camera or any QR app while the phone is on your Wi-Fi. Not every phone has a QR icon in its Wi-Fi settings.
- Moves between screens and places start at once more often: the screen you're on never needs redrawing before a move.

## v1.10.1-rc.3 - 2026-10-02
- Moving between places starts at once, even right after the previous move. It could wait up to half a second before the screen followed your finger.
- Fixed: going back to your first place made the radar save its map twice, and the screen stuttered for a few seconds. It saves once now, and waits while you're touching the screen.

## v1.10.1-rc.2 - 2026-10-02
- Smoother moves: going from screen to screen, from place to place (drag up or down) and from day to day in the hourly view now runs at 45 to 70 frames per second instead of 10 to 15. The screen follows your finger, bounces at the first and last one, and a quick flick is enough to go to the next. Please report any swipe that gets missed or a screen that freezes.
- Scrolling the hourly list and the Settings screen is unchanged for now.
- Behind the scenes: the screen is fed twice as fast, radar images and map tiles are decoded with much less memory, and the display no longer runs a speed test by itself a minute after starting (it froze the screen for about 1.5 seconds).

## v1.10.1-rc.1 - 2026-10-01
- Fixed: Android Easy Connect could still fail when the display couldn't reach its Wi-Fi, if your router was on a busy channel. The display now looks for your network by name to find its channel.

## v1.10.0 - 2026-10-01
- Lightning on the radar: yellow bolts mark where Environment Canada detected lightning in the last 10 minutes, on the live radar and in the 3-hour animation. Lightning is detected over Canada and up to about 250 km beyond.
- Built with a newer version of the Espressif tools (ESP-IDF 5.5.4), the same one used for testing. Nothing should look different.
- Behind the scenes: an automatic test setup now checks the screens, the settings page, speed and memory, and the Wi-Fi setup (setup network, sign-in page, Easy Connect) before each release.

## v1.10.0-rc.3 - 2026-10-01
- Behind the scenes: the display now tells the automatic tests when an update has just been installed, so the tests wait for it to be confirmed instead of restarting it too early (which made it go back to the previous version).

## v1.10.0-rc.2 - 2026-10-01
- Built with a newer version of the Espressif tools (ESP-IDF 5.5.4), the same one used for testing. Nothing should look different; please report anything that behaves oddly, especially Wi-Fi, updates and the radar.
- Behind the scenes: an automatic test setup now checks the screens, the settings page, speed and memory, and the Wi-Fi setup (setup network, sign-in page, Easy Connect) before releases.

## v1.10.0-rc.1 - 2026-10-01
- Test version of lightning on the radar: yellow bolts mark where Environment Canada detected lightning in the last 10 minutes, on the live radar and in the 3-hour animation. Lightning is detected over Canada and up to about 250 km beyond.

## v1.9.0 - 2026-10-01
- New language: Inuktitut, in syllabics. Choose ᐃᓄᒃᑎᑐᑦ in Settings on the display (Language) or in the Units card of the settings page. It is a first draft that a fluent speaker has not checked yet, so some words may be wrong. Weather alerts and the "What's new" notes stay in English.
- Fixed: when the display couldn't reach its Wi-Fi, the setup network and Android Easy Connect didn't work. Both work again: the sign-in page opens on the phone, and Easy Connect takes the network on the first scan.
- While the Wi-Fi setup screen is open, the display no longer tries the old network. Tap the screen to try it again, or wait: after 5 minutes without a phone on the setup network it tries again by itself.

## v1.9.0-rc.2 - 2026-10-01
- Fixed: when the display couldn't reach its Wi-Fi, the setup network and Android Easy Connect didn't work. Both work again: the sign-in page opens on the phone, and Easy Connect takes the network on the first scan.
- While the Wi-Fi setup screen is open, the display no longer tries the old network. Tap the screen to try it again, or wait: after 5 minutes without a phone on the setup network it tries again by itself.

## v1.9.0-rc.1 - 2026-10-01
- Test version of an Inuktitut interface, in syllabics: choose ᐃᓄᒃᑎᑐᑦ in Settings on the display (Language) or in the Units card of the settings page. It is a first draft that a fluent speaker has not checked yet, so some words may be wrong. Weather alerts and the "What's new" notes stay in English.

## v1.8.0 - 2026-10-01
- The display and the settings page now speak French: choose Français in Settings on the display (Langue) or in the Units card of the settings page. Weather alerts come in French too. The "What's new" notes stay in English for now.
- On the weather screen, a blue drop marks the humidity and a wind symbol marks the wind speed.
- Alert sounds: the display beeps when a new weather alert appears (two beeps for yellow, more for orange, a siren for red). Choose which alerts sound, the volume and quiet hours on the settings page (red alerts sound even during quiet hours); the level, the volume and a test are also in Settings on the display.

## v1.6.0 - 2026-10-01
- Settings right on the display: long-press the weather screen. Turn dimming and wake on pick-up on or off, pick the timing (Short, Normal, Long), set the brightness by sliding along the bottom, change the units, open the phone settings or the Wi-Fi setup, check for updates, or restart.
- Wake on pick-up: when the screen is dimmed or off, picking up or tilting the display wakes it. A bump on the table doesn't. On the settings page (Screen & presence) you can turn it off, choose the sensitivity, and watch a live movement meter.

## v1.6.0-rc.1 - 2026-10-01
- Test version of settings on the display: long-press the weather screen for dimming, wake on pick-up, timing, brightness, units, the phone settings, Wi-Fi, updates and restart.
- Test version of wake on pick-up: picking up or tilting the display wakes it (a bump on the table doesn't). Settings page: on/off, sensitivity and a live movement meter.

## v1.5.0 - 2026-09-30
- Several places: add up to 4 (home, cottage, work...) on the settings page, then drag up or down on the weather screen to switch. Each place shows its own local time; alerts, air quality, the hourly view and the radar follow the place shown.
- On the settings page, tap a place to change it, and pick the exact spot on a map: tap the map or drag the pin. Handy for a cottage or anywhere you aren't right now.
- Units on the settings page: temperature in °C or °F, wind in km/h, mph or m/s, and a 24-hour or 12-hour clock. The display switches right away. With mph, the radar shows distances in miles.

## v1.5.0-rc.2 - 2026-09-30
- Test version of several places: add up to 4 on the settings page, then drag up or down on the weather screen to switch. Each place has its own local time, and alerts, air quality, the hourly view and the radar follow it.
- Test version of the new place editor on the settings page: tap a place to change it, and pick the exact spot on a map.

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
