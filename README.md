# Québec City weather display (Waveshare ESP32-S3-Touch-AMOLED-1.75)

Shows current temperature, conditions, feels-like, humidity, wind, a clock, and a 3-day high/low,
from Open-Meteo (free, no API key). Refreshes every 10 minutes.

## Screens
- **Swipe left** for the rain radar (~200 km around the city, 100 km ring). Radar from Environment Canada (GeoMet),
  dark map © OpenStreetMap contributors © CARTO. Refreshes every 6 min while shown.
- **Swipe right** to go back to the weather.

## First run: Wi-Fi setup
1. The screen shows "Wi-Fi setup".
2. On your phone, join Wi-Fi **Weather-Setup** (password **meteo1234**).
3. Open **http://192.168.4.1**, pick your network, type its password, tap Save.
4. The display restarts, connects and shows the weather.

To change Wi-Fi later: press **RESET**, then as soon as the screen shows "Starting..." press and hold **BOOT** for ~2 s. (Don't hold BOOT *while* pressing RESET — that enters flashing mode.)

## Files
- `flash.bat` – flash the prebuilt firmware and capture 40 s of serial log (`serial_log.txt`).
- `main/` – source: `display.c` (CO5300 QSPI + LVGL), `net.c` (Wi-Fi + setup page), `weather.c` (Open-Meteo), `ui.c` (weather screen), `radar.c` (radar screen), `touch.c` (CST9217 touch).
- Change the city in `main/weather.h` (name, latitude, longitude).
- Rebuild on Windows in an ESP-IDF v5.4 shell: `idf.py build flash monitor` (LVGL 9.2.2 is fetched automatically).
