# Weather display: Waveshare ESP32-S3-Touch-AMOLED-1.75

Firmware for the Waveshare 1.75" round AMOLED board (ESP32-S3, 466×466 CO5300 panel, CST9217 touch).
It shows the local weather and a live rain radar with a 3-hour animation, and has a phone-friendly
settings page.

Built with **ESP-IDF v5.4** and **LVGL 9.2**. No API keys needed.

## Features

| Screen | What it shows | Interaction |
|---|---|---|
| **Weather** | Clock, city, icon and temperature, conditions, feels-like / humidity / wind, 3-day high/low with icons | Swipe **left** for the radar. **Long-press** for the settings QR code |
| **Radar** | ~200 km around the location: dimmed OpenStreetMap map, Environment Canada radar, 100 km ring, clock | **Tap** to play the last 3 h (15 frames, 3 fps). Swipe **right** to go back |
| **Settings page** (phone) | Location: *Use my phone's location* (GPS), city search, or manual lat/lon. Wi-Fi network | Opened from the QR code; served over HTTPS |

Data sources:

- Weather: [Open-Meteo](https://open-meteo.com) (current conditions plus 4-day forecast, `timezone=auto`).
- Radar: [ECCC MSC GeoMet](https://eccc-msc.github.io/open-data/msc-data/obs_radar/readme_radar_geomet_en/) WMS, layer `RADAR_1KM_RRAI` (North America, 1 km, every 6 min, last 3 h).
- Basemap: OpenStreetMap standard tiles at zoom 7, downloaded once and cached in flash. Attribution is shown on screen.

## First-time setup

1. Flash the firmware (see below). With no Wi-Fi saved, the screen shows **Wi-Fi setup** and a QR code.
2. Scan the QR code to join the display's network **Weather-Setup** (password `meteo1234`).
3. Open **https://192.168.4.1**. Your phone will warn that the certificate isn't trusted. That's expected, because the display signs its own certificate. Choose *Advanced → Proceed*.
4. Pick your Wi-Fi network, enter the password and save. The display restarts and connects.

**Changing settings later:** long-press the weather screen, scan the QR code and open the page (your phone must be on
the same Wi-Fi). Location changes apply immediately: the map and radar reload in about 20 s.

**Resetting Wi-Fi:** press **RESET**, then hold **BOOT** for about 2 s while the screen says *Starting…*.
Don't hold BOOT *while* pressing RESET, because that puts the chip into flashing mode.

## Flashing (Windows)

The prebuilt binaries go in `firmware/` (they're ignored by git, so you get them from a build). The flashing tool is
`tools/esptool.exe`: standalone esptool **v4.8.1**, downloaded from
https://github.com/espressif/esptool/releases (`esptool-v4.8.1-win64.zip`) and also ignored by git.

- `flash.bat`: flashes `firmware\*.bin` (COM port auto-detected), then logs serial output to `serial_log.txt`.
  - `flash.bat`: interactive, logs for 40 s, then waits for a key press.
  - `flash.bat auto 90`: no pause at the end (for scripts or Claude Code), logs for 90 s.
- `monitor.ps1 -Port COM5 -Seconds 60`: serial log only.
- `start_flash_helper.bat`: starts a minimized background watcher (`flash_helper.ps1`). Whenever a file named
  `flash.request` appears in this folder, it runs `flash.bat auto <seconds>`, using the number of seconds in the file,
  and writes `flash.done` when finished. This lets tools that can only write files (like the Claude desktop app)
  trigger a flash. Close its window to stop it.

Flash layout: bootloader at `0x0`, partition table at `0x8000`, app at `0x10000`. Flash settings: DIO, 80 MHz, 16 MB.

## Building from source

With an ESP-IDF **v5.4** environment (the "ESP-IDF 5.4 PowerShell" shortcut on Windows):

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

LVGL 9.2.2 is fetched automatically by the component manager (`main/idf_component.yml`). To refresh the prebuilt
files used by `flash.bat`, copy `build\bootloader\bootloader.bin`, `build\partition_table\partition-table.bin` and
`build\weather_amoled.bin` into `firmware\`.

## Project layout

```
main/
  main.c        boot flow, weather refresh loop, reacts to location changes
  display.c     CO5300 QSPI panel driver + LVGL display port (flush, rounder, LVGL task + mutex)
  touch.c       CST9217 I2C touch -> LVGL pointer
  ui.c          weather screen, message/QR screens, settings overlay, swipe handling
  radar.c       radar screen: basemap tiles + flash cache, GeoMet frames, animation
  weather.c     Open-Meteo fetch/parse, WMO code -> text/icon
  net.c         Wi-Fi station, setup access point, credentials in NVS
  web.c         HTTPS settings server (+ HTTP -> HTTPS redirect) and JSON API
  config.c      saved location (NVS) and local-time helper (UTC offset from Open-Meteo)
  web/index.html  settings page (embedded)
  certs/        self-signed TLS certificate + key (embedded; see Security)
  montserrat.ttf  font, rendered at runtime with LVGL TinyTTF (supports accents like "é")
partitions.csv  nvs, phy, factory app (3 MB), mapcache (512 KB basemap cache)
sdkconfig.defaults
docs/ARCHITECTURE.md  how the pieces fit together, memory budget, known issues
CLAUDE.md       notes for AI-assisted development sessions
```

## Security notes

- The TLS certificate and private key in `main/certs/` are shared by every build of this firmware. They only protect
  traffic on your own network and exist so the phone lets the page use GPS. Don't reuse them anywhere else.
- The setup access point password (`meteo1234`) is in `main/net.h`.
