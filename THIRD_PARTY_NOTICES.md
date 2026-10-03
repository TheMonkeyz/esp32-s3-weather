# Third-party notices

The code in this repository is MIT-licensed (see [LICENSE](LICENSE)), except for the parts below, which keep their
own licenses. Firmware images built from this repository contain all of them.

## Included in this repository

| Component | Where | License |
|---|---|---|
| Montserrat font (Julieta Ulanovsky and the Montserrat Project Authors) | `main/montserrat.ttf` | SIL Open Font License 1.1, see [`main/montserrat-OFL.txt`](main/montserrat-OFL.txt) |
| Noto Sans Canadian Aboriginal font, subset to the syllabics block (The Noto Project Authors) | `main/syllabics.ttf` | SIL Open Font License 1.1, see [`main/syllabics-OFL.txt`](main/syllabics-OFL.txt) |
| Captive-portal DNS server (from ESP-IDF's `captive_portal` example, Espressif Systems) | `components/dns_server/` | Unlicense OR CC0-1.0 |

## Fetched at build time

| Component | Version | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) (FreeRTOS, lwIP, mbedTLS, Wi-Fi drivers…) | 5.5.4 | Apache-2.0, with components under their own compatible licenses (see ESP-IDF's `COPYRIGHT.rst`) |
| [LVGL](https://github.com/lvgl/lvgl) (including its TinyTTF / stb_truetype and QR code modules) | 9.2.2 | MIT |
| [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) (microphone and speaker codec drivers, ES7210 / ES8311) | 1.5.11 (pinned in `main/idf_component.yml`) | Apache-2.0 |
| [miniz](https://github.com/richgel999/miniz) inflate (`tinfl`), in the ESP32-S3 ROM, used by `main/png_rows.c` | ROM | MIT |

## Tools used by the web flasher

| Component | License |
|---|---|
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) (loaded by the flasher page) | Apache-2.0 |
| [Leaflet](https://leafletjs.com) 1.9.4 (the settings page's map, loaded by the phone from unpkg) | BSD-2-Clause |
| [esptool](https://github.com/espressif/esptool) (Windows flashing scripts; not in the repository) | GPL-2.0-or-later |

## Data shown by the display

The firmware downloads data at run time; it is not part of this repository and has its own terms. The display and
the flasher page show the required credits.

- Weather forecast: [Open-Meteo](https://open-meteo.com), CC BY 4.0.
- Rain radar: Environment and Climate Change Canada, [MSC GeoMet](https://eccc-msc.github.io/open-data/),
  Open Government Licence – Canada.
- Map tiles: © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, ODbL; tiles served by the
  OpenStreetMap Foundation under its [tile usage policy](https://operations.osmfoundation.org/policies/tiles/). The
  firmware identifies itself with a User-Agent naming the project, its running version and its repository
  (`svc_user_agent()`), caches the tiles it uses in
  flash (each display downloads its maps once per place, about 60 tiles) and credits OpenStreetMap on every screen that
  shows them. The policy asks distributed apps for heavy use to arrange their own tile source: if this project ever
  has many users, switch to a tile provider that allows it.
- City search / reverse geocoding on the settings page (in the phone's browser): Open-Meteo geocoding and
  [Nominatim](https://nominatim.org) (OpenStreetMap data, ODbL), within the
  [Nominatim usage policy](https://operations.osmfoundation.org/policies/nominatim/) (one request when a spot is
  picked on the map, none while typing). These requests go from the phone straight to those services, with the
  coordinates or the text searched.
