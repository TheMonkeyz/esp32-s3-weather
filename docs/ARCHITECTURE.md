# Architecture

## Hardware (Waveshare ESP32-S3-Touch-AMOLED-1.75)

| Part | Details | Pins |
|---|---|---|
| MCU | ESP32-S3 (QFN56) rev 0.2, 16 MB flash, 8 MB octal PSRAM | |
| Display | CO5300 AMOLED 466×466, QSPI, RGB565 | CS 12, CLK 38, D0–D3 4/5/6/7, RST 39; column offset **+6** |
| Touch | CST9217, I2C addr `0x5A` | SDA 15, SCL 14, RST 40, INT 11 (unused) |
| Microphones | 2× digital mics via ES7210 ADC (I2C `0x40`, shared bus with touch), I2S 16 kHz stereo 16-bit | MCLK 42, BCLK 9, WS 45, DIN 10 |
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
| `radar` | 0 / 3, 10 KB stack | Basemap, latest radar frame, history frames; sleeps unless the radar screen is visible |
| `presence` | 0 / 2 | Reads 100 ms of audio, computes the level, runs the dim/off state machine, fades brightness |
| `diag` | 0 / 1 | Every 60 s logs heap, frame timing, CPU and stack per task; starts `bench` once at 45 s |
| `bench` | 1 / 4, one-shot | Times full-screen renders of each screen without showing them (UI blocked ~1.5 s) |
| httpd (HTTPS :443, HTTP :80) | – | Settings page + JSON API. Stacks 7 KB (TLS handshake peaks ~3.3 KB) / 4 KB |

Main task stack is 6 KB. Stack sizes come from the measured high-water marks in `diag: tasks` lines; re-check
them there after adding work to a task.

**Rule:** any LVGL call from outside the `lvgl` task must be wrapped in `display_lock(-1)` / `display_unlock()`.
Keep lock holds short: `diag: display` reports the longest hold and which task did it (the radar holds it up to
~65 ms while swapping frames).
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
- Unused swipes call `lv_indev_wait_release()`; otherwise their release is also delivered as a `SHORT_CLICKED`.

## Hourly view (`ui.c`, `weather.c`)

- Open-Meteo `hourly=temperature_2m,weather_code,precipitation_probability,wind_speed_10m,is_day` with
  `forecast_days=3` → `weather_t.hour[72]` (starts at 00:00 local today). Response is about 4 KB.
- `SHORT_CLICKED` on the weather screen with y ≥ 296 → forecast column by x → `scr_hour` (move-top animation).
  `ui_weather()` keeps a copy of the forecast (`wx`) for this screen.
- `scr_hour` holds a pager: a horizontally scrollable object with three full-screen day pages,
  `LV_SCROLL_SNAP_CENTER` + `LV_OBJ_FLAG_SCROLL_ONE`, so pages follow the finger, snap, and bounce at the ends.
  Each page has its own vertically scrollable hour list; LVGL picks the scroll direction from the drag.
  Page dots update on `LV_EVENT_SCROLL`. A tap closes the view.
- **Rows are drawn, not created:** each list has one tall object with an `LV_EVENT_DRAW_MAIN` callback that draws
  only the rows inside the clip area (`lv_draw_label` with `text_local`, `lv_draw_rect`). The weather icons have a
  painter mode (`P_layer`) that draws the same shapes straight into the layer. 72 rows as real objects would have
  been several hundred small allocations in internal RAM.
- Today's page is refilled at each new hour and when new data arrives.

## Radar (`radar.c`)

- **Projection:** Web Mercator. Zoom levels 4–10 give view radii of about 1560 / 780 / 390 / 195 / 98 / 49 / 24 km
  at 47°N (zoom 7 = ~837 m/px is the default). Zoom 10 is the useful limit, because GeoMet radar is 1 km/px. The range
  ring picks the round distance (5 km … 800 km) closest to half the radius. The view is centred on the saved location (`apply_view()`). Only doubling steps are used, so
  map tiles are shown at native resolution and labels stay sharp.
- **Zoom gestures:** swipe down = zoom in, up = zoom out (`radar_zoom()`, called from the LVGL gesture handler).
  Zoom in grows the current picture 2× (`lv_anim` on `lv_image_set_scale`, 300 ms, nearest-neighbour), then the sharper
  map replaces it. Zoom out first loads the wider map (usually from the flash cache), then shrinks it from 2× into
  place (`reveal_map()`), so no black border shows. The task waits for a running zoom-in animation (`wait_zoom_anim()`)
  before swapping images. Requests interrupt waits (`ulTaskNotifyTake`), and a basemap download for a zoom level the
  user has already left is cancelled.
- **Basemap:** 3×3 OSM tiles (`tile.openstreetmap.org/<z>/x/y.png`, one keep-alive connection, 3 retries each),
  decoded with LVGL's bundled lodepng, dimmed and desaturated (`dim_map`, 55%), then saved to that zoom level's
  512 KB slot in the 4 MB `mapcache` partition (7 slots). The header (magic `MAP7`, zoom, view origin) makes a location change
  download fresh tiles. Bump the magic to force a full re-download (useful for testing the preload).
- **Background preload:** `radar_preload_start()` (called by `main` after Wi-Fi connects) and every location change
  run `preload_all()` in the radar task. It checks each zoom level's cache header and downloads the missing levels
  (9 tiles each; all 7 take about 45 s). Meanwhile the weather screen works normally. The radar screen shows a
  "Preparing maps" panel (level x of n, tile bar), and zoom swipes answer "Maps still downloading", because the
  preload temporarily moves the task's `zoom`. If a level gets no tiles at all (no network), the preload stops.
  Afterwards the current level is loaded from flash and the latest frame is fetched.
- **Radar frames:** GeoMet WMS `GetMap` in EPSG:3857 with the exact view bbox at 466×466, `transparent=true`,
  `time=<ISO>`. The latest time comes from `GetCapabilities` (`<Dimension name="time">start/end/PT6M`).
- **Frame storage:** each frame is palette-indexed (1 byte/px, index 0 = no echo, up to 255 RGBA colours), about 217 KB
  in PSRAM. `compose()` blends the frame over the basemap at alpha×0.86 into `out565`, which an `lv_image` displays.
- **Animation:** 15 frames. The latest frame, plus 14 history frames on a fixed 12-minute grid (so refreshes reuse
  most of them). They download newest first while the radar screen is visible. A tap plays at 3 fps via an LVGL timer,
  holds the last frame about 1 s, then returns to live.

## Wi-Fi setup / captive portal (`net.c`, `ui.c`, `web.c`)

- **Boot** (`main.c`): no saved network → `portal()` (AP only, until credentials are saved and the board
  restarts). Saved network → `net_begin()` + `web_start()` right away, then `net_wait(30000)`. If that fails,
  `offline_setup()` shows the Wi-Fi QR, starts the AP next to the station and waits for a connection; once connected
  (and no phone is on the AP, max 2 min) it stops the AP and continues.
- **Reconnects never stop** (`net.c`): each disconnect schedules `esp_wifi_connect()` on an `esp_timer` (1 s for the
  first 8 tries, then 3 s, then 30 s; formerly a `vTaskDelay` inside the event handler). `BIT_FAIL` only ends
  `net_wait()`. A connection attempt makes the radio scan other channels, which drops phones on the setup AP, so the
  retry is postponed while `net_ap_clients() > 0`. SNTP starts on the first `GOT_IP`, whenever that happens.
- **Status screens** (`scr_msg`) take a long-press too: it starts the AP and shows the Wi-Fi QR (not in first-boot
  setup, which already shows it). A 10-minute timer stops that AP only once the board is online.
- On the weather screen, a long-press while offline skips the settings QR (useless without a network) and opens the
  Wi-Fi setup step directly.
- **Wi-Fi setup screen** (`scr_setup` in `ui.c`, `ui_wifi_setup(note)`): used by first-time setup, offline setup,
  status-screen long-press and the settings overlay's second long-press. Page 1 = setup AP QR (`net_setup_ap_start()`),
  page 2 = Wi-Fi Easy Connect; swipe switches. A tap closes it only if it was opened while online; a 10-minute timer
  closes it once online.
- **Easy Connect (DPP enrollee, `net.c`)**: `CONFIG_ESP_WIFI_DPP_SUPPORT=y`, `wpa_supplicant` in REQUIRES. The radio
  can't serve the AP and listen at once, so page 2 stops the AP (`net_setup_ap_stop_any()`, even in first-time setup),
  disconnects the station and pauses our reconnects (`dpp_active`). `esp_supp_dpp_bootstrap_gen("1,6,11", QR)` is
  **asynchronous**: `esp_supp_dpp_start_listen()` must be called from the `ESP_SUPP_DPP_URI_READY` callback (calling
  it right after bootstrap_gen returns `ESP_FAIL`, which was the first bug). `ESP_SUPP_DPP_CFG_RECVD` gives a
  `wifi_config_t`: saved with `net_save_creds()`, restart after 2.5 s. `ESP_SUPP_DPP_FAIL` re-listens. Leaving page 2
  deinitialises DPP and resumes reconnects.
- **Settings overlay state machine** (`ui.c`): 0 = hidden, 1 = settings QR (`https://<ip>`), 2 = Wi-Fi setup.
  A long-press on the weather screen goes to 1; a long-press on the overlay opens the Wi-Fi setup screen above.
  Gestures are ignored while the overlay is open.
- **Access point:** `ap_up()` switches to APSTA, so the station connection stays up and the AP follows its channel.
  It sets DHCP option 114 (captive-portal URI `http://192.168.4.1/`) and starts the DNS server
  (`components/dns_server`), which answers every name with the AP's IP. First-boot setup (`net_start_portal()`) uses
  the same function.
- **HTTP :80 decides by interface** (`from_setup_ap()` looks at the socket's local address):
  - On the setup AP it *is* the portal. `/` serves the page over plain HTTP, because phone sign-in browsers reject the
    self-signed certificate. `/api/*` works, and every other URL (OS connectivity checks such as `/generate_204` or
    `/hotspot-detect.html`) gets a 302 to `http://192.168.4.1/` with a small HTML body, which iOS requires.
  - On the home network, everything redirects to HTTPS.
- On the plain-HTTP page the GPS button can't work (browsers only allow geolocation on secure pages). The page shows a
  link to the HTTPS version instead, and puts the Wi-Fi card first when opened on 192.168.4.1.

## Presence dimming (`presence.c`)

- **Audio:** the ES7210 is driven through `esp_codec_dev` (I2C control on the touch controller's bus via
  `touch_i2c_bus()`; I2S0 RX, 16 kHz, 2 channels, 16-bit, 30 dB mic gain). Every 100 ms: RMS of both channels → dBFS.
- **Calibration:** `presence_calibrate(5)` collects 5 s of levels. The baseline is the 90th percentile, saved in NVS
  (`presence/cfg` blob, with the rest of the settings).
- **Sustained-noise score:** +0.1 per loud tick, −0.05 per quiet tick, clamped to `[0, wake_s]`. It wakes when it
  reaches `wake_s`, so short bangs don't wake it, while speech with pauses does.
- **States:**
  - ACTIVE: any loud tick resets the quiet timer; `dim_s` of quiet → DIM.
  - DIM: the score reaching `wake_s` → ACTIVE. A loud tick restarts the off countdown. `dim_s + off_s` of quiet → OFF.
  - OFF: the score reaching `wake_s` → ACTIVE.
  - Touch: `presence_touch()` → ACTIVE; the waking touch is swallowed in `touch.c` if the screen was off.
- **Brightness:** CO5300 command `0x51`, faded in 10% steps per tick (about 1 s full ↔ off), under `display_lock()`.
  Rendering continues while the screen is off.
- **API:** `GET /api/presence` (config and live status: level, threshold, state, wake_progress, quiet_s, calibrating,
  brightness), `POST /api/presence` (config), `POST /api/calibrate {seconds}`. The page polls status every 700 ms
  while visible.
- **Presets & units:** firmware stores `dim_s` and `off_s` (off is *after* dim). The page shows "Turn off after" as
  total quiet time (`dim_s + off_s`) with s/min/h unit selectors and converts back on save. Presets
  (Testing/Short/Normal/Long) live only in `index.html` (`PRESETS`); loading the config picks the matching preset or
  *Custom*. Firmware default = Normal (`dim_s 600`, `off_s 3000`, `wake_s 3`).

## Settings / web (`web.c`, `config.c`)

- HTTPS server (`esp_https_server`, per-device self-signed EC P-256 cert from `tlscert.c`) on 443. The plain HTTP server on 80 sends
  everything to HTTPS with a 302 redirect. The HTTPS server uses control port 32769 and the HTTP one the default
  32768; they must not share a port.
- API:
  - `GET /api/config`
  - `GET /api/scan`: Wi-Fi scan, returning `[{ssid, rssi, secure}]` strongest first, one entry per name, without
    Weather-Setup. Scanning blocks the server for 2–3 s, so the page scans only on the **Scan** button, or
    automatically when opened on the setup AP.
  - `POST /api/location {name, lat, lon}`
  - `POST /api/wifi {ssid, pass}`: restarts the device.
- The page runs the phone's geolocation, reverse geocoding (Nominatim) and city search (Open-Meteo geocoding) in the
  **browser**; the device only stores the result.
- Location is stored in NVS namespace `loc`. `config_local_time()` uses Open-Meteo's `utc_offset_seconds`, which
  handles any time zone and DST; before the first fetch it falls back to the `EST5EDT` TZ rule.

## TLS certificate (`tlscert.c`)

- `tlscert_get()` loads `cert` / `key` (PEM strings) from NVS namespace `tls` and checks that the certificate
  parses. If they're missing, a short-lived task (`tlsgen`, 8 KB internal stack: key generation needs stack and
  NVS writes need an internal-RAM stack) generates an EC P-256 key with mbedtls (`ctr_drbg` seeded from the
  hardware RNG, Wi-Fi is up by then) and a self-signed X.509 v3 certificate: CN `Weather Display <last 3 MAC
  bytes>`, random 16-byte serial, fixed validity 2024-01-01 → 2099-12-31 (the clock may not be set, and there's no
  CA to renew with). Takes ~180 ms on the S3.
- `web_start()` starts the HTTPS server with it; if it fails, only the plain-HTTP server runs.
- The key is stored unencrypted in NVS (no flash encryption on this project). Anyone with the board and a USB
  cable can read it, which is acceptable for a LAN settings page.

## Release pipeline

`firmware.yml` (GitHub Actions):

1. **build** (every push, PR and tag): `espressif/esp-idf-ci-action` (ESP-IDF v5.4.2, component manager), then
   `make_flasher_site.py dist` → release files + `flash-parts.json` (chip, version, offsets from
   `build/flasher_args.json`), plus a merged full image. Uploaded as a workflow artifact.
2. **release** (`v*` tags): GitHub Release with those files; `prerelease` when the tag contains `-`.
3. **pages** (after a release, or *Run workflow* on `main`): `gh release list` → newest stable release and the newest
   pre-release if it's newer than that; downloads their assets; `make_flasher_site.py site` → `stable/`, `beta/`,
   `channels.json`; deploys to GitHub Pages. The site always reflects released files, never a branch build.

ESP Web Tools resolves part paths relative to the manifest, so each channel folder is self-contained. The page swaps
the install button's `manifest` attribute when the channel changes. `new_install_prompt_erase` offers an erase on
the first install. The firmware version (`esp_app_get_description()->version`) is logged by `diag.c` and returned
by `GET /api/config` as `version`.

## Memory budget (approximate)

| Item | Where | Size |
|---|---|---|
| LVGL draw buffers | internal DMA | 2 × 30 KB |
| LVGL heap (objects, styles, glyph cache) | PSRAM (`lvgl_mem.c`, `LV_USE_CUSTOM_MALLOC`) | ~40–50 KB |
| Basemap + composed screen | PSRAM | 2 × 434 KB |
| 15 radar frames | PSRAM | 3.3 MB |
| PNG decode (466×466 ARGB) | PSRAM (transient) | ~0.9 MB + zlib |
| TLS (client and server) | PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`) | ~40–60 KB per session |

Build: `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (debug `-Og` made LVGL rendering noticeably slow) and
`CONFIG_LV_DEF_REFR_PERIOD=15`.

Measured: internal RAM ~69 KB free steady, 30 KB min; PSRAM ~4 MB free, 1.7 MB min. See `docs/DIAGNOSTICS.md` for
how to measure again (`reboot.request` + `tools/diag_summary.py`) and the reference numbers.

Internal DMA-capable RAM is the scarce resource. Everything under 16 KB that goes through plain `malloc` lands
there first (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`), which is why LVGL has its own PSRAM allocator. Fonts use
`LV_FONT_KERNING_NONE`: kerning lookups were 71% of the rendering time. `CONFIG_MBEDTLS_HARDWARE_AES` is **off**: the AES peripheral
allocates internal DMA bounce buffers, and that failed mid-response, which truncated the settings page.

## Known issues / TODO

- ~~Task watchdog warnings~~ (fixed): writing the 450 KB basemap cache in one flash erase blocked core 0 for
  seconds. `cache_save()` now erases and writes one 4 KB sector at a time with `vTaskDelay(1)` in between, and the
  palette, compose and tile-copy loops yield every 32–64 rows. Keep new long loops on core 0 yielding.
- GeoMet drops idle keep-alive connections. `GetCapabilities` retries up to 3× and frames retry once, reconnecting each time.
- The frame count is fixed at 15 and the radar layer is rain rate only (`RADAR_1KM_RRAI`). `Radar_1km_SfcPrecipType`
  would colour snow and rain separately.
- One TLS key is shared by all builds (see README, Security notes).
- Internal RAM is tight: about 6.5 KB free while serving the page with the AP running. Watch
  `web: GET / (page), free internal …` in the log after adding features.
- Phones hammer the portal with parallel connections (including HTTPS probes that fail the handshake, which is
  harmless). `CONFIG_LWIP_MAX_SOCKETS=16` leaves room for both servers, the DNS socket and several clients.
