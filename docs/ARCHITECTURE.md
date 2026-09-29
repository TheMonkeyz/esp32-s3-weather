# Architecture

## Hardware (Waveshare ESP32-S3-Touch-AMOLED-1.75)

| Part | Details | Pins |
|---|---|---|
| MCU | ESP32-S3 (QFN56) rev 0.2, 16 MB flash, 8 MB octal PSRAM | |
| Display | CO5300 AMOLED 466×466, QSPI, RGB565 | CS 12, CLK 38, D0–D3 4/5/6/7, RST 39; column offset **+6** |
| Touch | CST9217, I2C addr `0x5A` | SDA 15, SCL 14, RST 40, INT 11 (unused) |
| Buttons | BOOT = GPIO0 (also the strapping pin) | |
| USB | COM5 on the dev PC | |

The panel's init sequence and pin map come from Waveshare's BSP
(`waveshareteam/Waveshare-ESP32-components`, `bsp/esp32_s3_touch_amoled_1_75`). We don't use the BSP itself;
`display.c` is a small standalone driver.

## Tasks

| Task | Core / priority | Job |
|---|---|---|
| `main` (app_main) | 0 / 1 | Boot flow, then weather loop: fetch every 10 min; woken early by a location change |
| `lvgl` | 1 / 4 | `lv_timer_handler()` loop under a recursive mutex (`display_lock()`) |
| `radar` | 0 / 3, 16 KB stack | Basemap, latest radar frame, history frames; sleeps unless the radar screen is visible |
| httpd (HTTPS :443, HTTP :80) | – | Settings page + JSON API |

**Rule:** any LVGL call from outside the `lvgl` task must be wrapped in `display_lock(-1)` / `display_unlock()`.
LVGL timer and event callbacks already run inside the lock.

## Display pipeline

- LVGL renders in partial mode into two internal DMA buffers (466 × 32 lines).
- `flush_cb` byte-swaps RGB565, sets the window (`0x2A` with +6 column offset, then `0x2B`) and sends pixels with
  QSPI command `0x32 / 0x2C`.
- A rounder callback makes every area start on an even pixel and end on an odd one (a CO5300 requirement).
- Fonts: Montserrat TTF embedded and rendered by TinyTTF at 15–96 px, so accents and "°" render correctly.

## Weather screen (`ui.c`)

- Layout was tuned for the round screen using the host simulator (see CLAUDE.md). Everything stays inside the
  circle; the forecast row ends at about y=408.
- Icons are built from LVGL primitives (circles, rounded rectangles, one line for the lightning bolt), scaled per use.
  Decorative objects are made non-clickable so presses bubble up to the screen.
- Gestures: `LV_EVENT_GESTURE` on both screens → `lv_screen_load_anim` (move left/right).
- Long-press opens a settings overlay with a QR code (`lv_qrcode`) for `https://<ip>`.

## Radar (`radar.c`)

- **Projection:** Web Mercator, zoom 7 (~837 m/px at 47°N), so the 466 px view covers about 195 km radius.
  The view is centered on the saved location (`apply_view()`).
- **Basemap:** 3×3 OSM tiles (`tile.openstreetmap.org/7/x/y.png`, one keep-alive connection, 3 retries each),
  decoded with LVGL's bundled lodepng, dimmed and desaturated (`dim_map`, 55%), then saved to the `mapcache` flash
  partition. The header (magic `MAP3`, zoom, view origin) makes a location change download fresh tiles.
- **Radar frames:** GeoMet WMS `GetMap` in EPSG:3857 with the exact view bbox at 466×466, `transparent=true`,
  `time=<ISO>`. The latest time comes from `GetCapabilities` (`<Dimension name="time">start/end/PT6M`).
- **Frame storage:** each frame is palette-indexed (1 byte/px, index 0 = no echo, up to 255 RGBA colours), about 217 KB
  in PSRAM. `compose()` blends the frame over the basemap at alpha×0.86 into `out565`, which an `lv_image` displays.
- **Animation:** 15 frames. The latest frame, plus 14 history frames on a fixed 12-minute grid (so refreshes reuse
  most of them). They download newest first while the radar screen is visible. A tap plays at 3 fps via an LVGL timer,
  holds the last frame about 1 s, then returns to live.

## Settings / web (`web.c`, `config.c`)

- HTTPS server (`esp_https_server`, self-signed EC P-256 cert embedded) on 443. The plain HTTP server on 80 sends
  everything to HTTPS with a 302 redirect. The HTTPS server uses control port 32769 and the HTTP one the default
  32768; they must not share a port.
- API:
  - `GET /api/config`
  - `GET /api/scan`: Wi-Fi scan; only called when the SSID field gets focus, because scanning blocks the server for 2–3 s.
  - `POST /api/location {name, lat, lon}`
  - `POST /api/wifi {ssid, pass}`: restarts the device.
- The page runs the phone's geolocation, reverse geocoding (Nominatim) and city search (Open-Meteo geocoding) in the
  **browser**; the device only stores the result.
- Location is stored in NVS namespace `loc`. `config_local_time()` uses Open-Meteo's `utc_offset_seconds`, which
  handles any time zone and DST; before the first fetch it falls back to the `EST5EDT` TZ rule.

## Memory budget (approximate)

| Item | Where | Size |
|---|---|---|
| LVGL draw buffers | internal DMA | 2 × 30 KB |
| Basemap + composed screen | PSRAM | 2 × 434 KB |
| 15 radar frames | PSRAM | 3.3 MB |
| PNG decode (466×466 ARGB) | PSRAM (transient) | ~0.9 MB + zlib |
| TLS (client and server) | PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`) | ~40–60 KB per session |

Internal DMA-capable RAM is the scarce resource. `CONFIG_MBEDTLS_HARDWARE_AES` is **off**: the AES peripheral
allocates internal DMA bounce buffers, and that failed mid-response, which truncated the settings page.

## Known issues / TODO

- ~~Task watchdog warnings~~ (fixed): writing the 450 KB basemap cache in one flash erase blocked core 0 for
  seconds. `cache_save()` now erases and writes one 4 KB sector at a time with `vTaskDelay(1)` in between, and the
  palette, compose and tile-copy loops yield every 32–64 rows. Keep new long loops on core 0 yielding.
- GeoMet sometimes resets keep-alive connections; each request retries once.
- The frame count is fixed at 15 and the radar layer is rain rate only (`RADAR_1KM_RRAI`). `Radar_1km_SfcPrecipType`
  would colour snow and rain separately.
- One TLS key is shared by all builds (see README, Security notes).
