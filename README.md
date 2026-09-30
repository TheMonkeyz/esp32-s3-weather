# Weather display: Waveshare ESP32-S3-Touch-AMOLED-1.75

Firmware for the Waveshare 1.75" round AMOLED board (ESP32-S3, 466×466 CO5300 panel, CST9217 touch).
It shows the local weather and a live rain radar with a 3-hour animation, and has a phone-friendly
settings page.

Built with **ESP-IDF v5.4** and **LVGL 9.2**. No API keys needed.

## Features

| Screen | What it shows | Interaction |
|---|---|---|
| **Weather** | Clock, city, icon and temperature, conditions, feels-like / humidity / wind, 3-day high/low with icons | **Tap a day** of the forecast for its hourly view. Swipe **left** for the radar. **Long-press** for the settings QR code; **long-press again** for Wi-Fi setup |
| **Radar** | Area around the location: dimmed OpenStreetMap map, Environment Canada radar, range ring, clock, radar time and radius | **Tap** to play the last 3 h (15 frames, 3 fps). Swipe **down** to zoom in, **up** to zoom out (≈25 km up to ≈1,550 km radius in 7 doubling steps, animated). Swipe **right** to go back |
| **Hourly view** | Weekday, conditions and high/low; one row per hour: time, icon, temperature, chance of rain, wind. Today starts at the current hour ("Now") | Drag **up/down** to scroll hours. Drag **left/right** to change day (the page follows the finger and snaps). **Tap** to close |
| **Settings page** (phone) | Location: *Use my phone's location* (GPS), city search, or manual lat/lon. Screen & presence (live sound meter, calibration, delays, brightness). Wi-Fi network | Opened from the QR code; served over HTTPS |
| **Presence dimming** | The microphones act as a presence sensor: quiet room → dim → screen off; sustained sound (not a single bang) or a touch → back on | Configured on the settings page |

Data sources:

- Weather: [Open-Meteo](https://open-meteo.com) (current conditions, 3-day daily and 72-hour hourly forecast, `timezone=auto`).
- Radar: [ECCC MSC GeoMet](https://eccc-msc.github.io/open-data/msc-data/obs_radar/readme_radar_geomet_en/) WMS, layer `RADAR_1KM_RRAI` (North America, 1 km, every 6 min, last 3 h).
- Basemap: OpenStreetMap standard tiles (zoom 4–10, one level per radar zoom step). After boot, any level that isn't cached yet downloads in the
  background (about 45 s for all 7) and is saved in flash, so zooming is instant afterwards. If you open the radar before it's done, a
  "Preparing maps" panel shows the progress. Attribution is shown on screen.

## First-time setup

1. Flash the firmware (see below). With no Wi-Fi saved, the screen shows **Wi-Fi setup** and a QR code.
2. Scan the QR code to join the display's network **Weather-Setup** (password `meteo1234`).
3. The phone's **"Sign in to network"** page opens by itself (captive portal) and shows the setup page with the Wi-Fi
   section on top. If it doesn't, open **http://192.168.4.1**.
4. The page scans automatically and lists nearby networks (strongest first, 🔒 = password needed). Tap yours, enter
   the password (**Show** reveals it while typing) and save. The display restarts and connects. **Scan again** refreshes the list.

## Presence dimming (microphones)

The two onboard microphones measure the room's sound level every 0.1 s.

- **Quiet** for *Dim after* → the screen dims. Quiet for *Turn off after* (total quiet time) → the screen turns off.
- **Waking** from dim/off needs *Wake after* seconds of **sustained** sound. Sound fills a wake meter and silence drains
  it at half speed, so talking with pauses wakes it but a door slam doesn't. Touching the screen always wakes it; the
  touch that wakes a dark screen is ignored, so it doesn't also swipe or tap.
- **Calibrate** on the settings page while the room is quiet: 5 s of measurement set the background level (90th
  percentile). "Loud" means background + *Sensitivity* dB.
- The settings card shows a live meter (orange mark = trigger level), the state (Active / Dimmed / Screen off), the
  wake progress and the quiet timer, which is handy for tuning.
- **Presets** (durations can be entered in s / min / h; editing any value switches to *Custom*):

  | Preset | Dim after | Turn off after (total quiet) | Wake after |
  |---|---|---|---|
  | Testing | 10 s | 30 s | 2 s |
  | Short | 2 min | 15 min | 2 s |
  | **Normal** (default) | 10 min | 60 min | 3 s |
  | Long | 30 min | 3 h | 3 s |

  Other defaults: sensitivity 10 dB, brightness 100% / dimmed 15%. Defaults only apply when nothing is saved in NVS.

## Changing settings later

- **Location:** long-press the weather screen, scan the **Settings** QR code and open the page. Your phone must be on
  the same Wi-Fi. The phone will warn that the certificate isn't trusted. That's expected, because the display signs
  its own certificate; choose *Advanced → Proceed*. HTTPS is what allows **Use my phone's location**. Location changes
  apply immediately: the map and radar reload within seconds.
- **Wi-Fi:** long-press the weather screen, then **long-press again** on the Settings screen. The display starts
  **Weather-Setup** alongside its current connection and shows a QR code to join it. The sign-in page then opens on
  the phone as during first-time setup. Tap the display to cancel; the setup network also switches off after 10 min.
- **Reset Wi-Fi from the buttons:** press **RESET**, then hold **BOOT** for about 2 s while the screen says
  *Starting…*. Don't hold BOOT *while* pressing RESET, because that puts the chip into flashing mode.

## Flashing (Windows)

The prebuilt binaries go in `firmware/` (they're ignored by git, so you get them from a build). The flashing tool is
`tools/esptool.exe`: standalone esptool **v4.8.1**, downloaded from
https://github.com/espressif/esptool/releases (`esptool-v4.8.1-win64.zip`) and also ignored by git.

- `flash.bat`: flashes `firmware\*.bin` (COM port auto-detected), then logs serial output to `serial_log.txt`.
  - `flash.bat`: interactive, logs for 40 s, then waits for a key press.
  - `flash.bat auto 90`: no pause at the end (for scripts or Claude Code), logs for 90 s.
- `monitor.ps1 -Port COM5 -Seconds 60`: serial log only.
- `start_flash_helper.bat`: opens the **flash helper** window (`flash_helper.ps1`). Whenever a file named
  `flash.request` appears in this folder, it flashes `firmware\*.bin` and logs serial output for the number of seconds
  written in the file (default 60). This lets tools that can only write files (like the Claude desktop app) trigger a
  flash. The window shows each step live: the request, the esptool upload, **FLASH OK on COMx in N s** (with a beep)
  or **FLASH FAILED** with the last esptool lines, the board's serial output, and a summary (errors, warnings,
  resets). It also writes:
  - `flash.status`: `idle`, `flashing`, `logging` or `flash_failed`.
  - `flash.done`: exit code, port, timings and counts.
  - `flash_helper.log`: a running history.

  Close the window to stop it.

Flash layout: bootloader at `0x0`, partition table at `0x8000`, app at `0x10000`. Flash settings: DIO, 80 MHz, 16 MB.

### Diagnostics

`echo 300 > reboot.request` restarts the board through the helper without flashing and records 300 s of log;
`python3 tools/diag_summary.py` then summarises memory, render timing and per-task CPU/stack. Details and
reference numbers: [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

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
  ui.c          weather screen, hourly view, message/QR screens, settings overlay, swipe handling
  radar.c       radar screen: basemap tiles + flash cache, GeoMet frames, animation
  weather.c     Open-Meteo fetch/parse, WMO code -> text/icon
  net.c         Wi-Fi station, setup access point + captive portal (DNS, DHCP option 114), credentials in NVS
  web.c         settings server: HTTPS on the home network, plain-HTTP captive portal on the setup AP, JSON API
  config.c      saved location (NVS) and local-time helper (UTC offset from Open-Meteo)
  presence.c    microphones (ES7210 over I2S) -> presence state machine -> screen brightness
  diag.c        "diag:" log lines: heap, frame timing, lock contention, CPU/stack per task, render bench
  lvgl_mem.c    LVGL's allocator, in PSRAM (keeps internal RAM for Wi-Fi, DMA and stacks)
  web/index.html  settings page (embedded)
  certs/        self-signed TLS certificate + key (embedded; see Security)
  montserrat.ttf  font, rendered at runtime with LVGL TinyTTF (supports accents like "é")
partitions.csv  nvs, phy, factory app (3 MB), mapcache (4 MB: one 512 KB basemap slot per zoom level)
sdkconfig.defaults
components/dns_server/  captive-portal DNS (from the ESP-IDF captive_portal example, CC0)
docs/ARCHITECTURE.md  how the pieces fit together, memory budget, known issues
docs/DIAGNOSTICS.md   how to measure memory/CPU/render speed, reference numbers, findings
tools/diag_summary.py summarises the diag: lines of serial_log.txt
flash_helper.ps1      flash.request = flash + log, reboot.request = restart + log (no flashing)
CLAUDE.md       notes for AI-assisted development sessions
```

## Security notes

- The TLS certificate and private key in `main/certs/` are shared by every build of this firmware. They only protect
  traffic on your own network and exist so the phone lets the page use GPS. Don't reuse them anywhere else.
- The setup access point password (`meteo1234`) is in `main/net.h`.
