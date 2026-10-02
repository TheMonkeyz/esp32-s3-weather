# Architecture

## Hardware (Waveshare ESP32-S3-Touch-AMOLED-1.75)

| Part | Details | Pins |
|---|---|---|
| MCU | ESP32-S3 (QFN56) rev 0.2, 16 MB flash, 8 MB octal PSRAM | |
| Display | CO5300 AMOLED 466×466, QSPI, RGB565 | CS 12, CLK 38, D0–D3 4/5/6/7, RST 39; column offset **+6** |
| Touch | CST9217, I2C addr `0x5A` | SDA 15, SCL 14, RST 40, INT 11 (unused) |
| Microphones | 2× digital mics via ES7210 ADC (I2C `0x40`, shared bus with touch), I2S 16 kHz stereo 16-bit | MCLK 42, BCLK 9, WS 45, DIN 10 |
| Speaker | ES8311 codec (I2C `0x18`, 8-bit `0x30`, shared bus) + amplifier enabled on GPIO 46; same I2S port as the microphones (DOUT 8) | DOUT 8, PA 46 |
| Motion sensor | QMI8658 6-axis IMU, I2C `0x6B` (shared bus with touch); only the accelerometer is used, polled at 10 Hz (its INT pins aren't used). Waveshare's BSP doesn't drive it (`BSP_CAPS_IMU 0`) | SDA 15, SCL 14 |
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
| `bench` | 1 / 4, one-shot, 10 KB stack (6 KB overflowed) | Times full-screen renders of each screen without showing them (UI blocked ~1.5 s); only while the weather screen is idle |
| `svc_probe` | any / 2, one-shot, 8 KB stack | Status page opened: one small request to each service idle for 5 min, then exits |
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
- The detail line is a flex row (`p->detail`): feels-like, then a blue droplet (`drop_draw`: circle + triangle, the
  raindrops' `0x4DA3FF`) before the humidity and a light-grey wind mark (`wind_draw`: three staggered strokes,
  `0xC9D1DA`) before the speed. The TinyTTF Montserrat has no symbol glyphs, so small icons are drawn
  (`LV_EVENT_DRAW_MAIN`), not typed.
  Decorative objects are made non-clickable so presses bubble up to the screen.
- Gestures: `LV_EVENT_GESTURE` on both screens → `lv_screen_load_anim` (move left/right).
- **Places:** the weather widgets live on one page per place (`place_page_t pp[MAX_PLACES]`) in a vertical pager
  (`pager.c`) on `scr_main`; the alert pill, update pill, page dots, place dots and settings overlay are siblings
  above it. `passthrough(scr_main)` makes everything non-clickable, then the pager gets `CLICKABLE` back (a
  non-clickable object can't start a scroll). `ui_place(i, name, w)` fills page i (NULL = "Loading...") and keeps a
  copy (`pw[i]`) for redraws; `ui_places(n, active)` hides unused pages, moves the dots, and scrolls to the active
  page when it was chosen on the settings page. When the pager settles on another page, `place_select_cb` →
  `main.c` selects it. Each page's clock uses its place's `utc_offset`. Icon objects need unique bolt-point slots:
  page × 4 + icon.
- `main.c` keeps every place's forecast (`wx[i]`, tagged with the coordinates it was fetched for, so edits and
  deletions never show one place's weather under another's name), refreshes all of them every 10 min (the place
  shown first) and fetches new or edited places at once. Alerts, air quality and the radar are for the place shown:
  a switch clears them and `radar_relocate()`s. `config.c` stores the places (place 1 in NVS `loc` as before, the
  others as blobs in `places`, plus the count and the place shown).
- The radar only caches the **first** place's maps (`cache_save()` returns otherwise, and no preload): switching
  between places would rewrite up to 3.5 MB of flash and fetch 63 OSM tiles each time.

## Pager (`pager.c`)

- Full-screen pages side by side or stacked in a scroller with `LV_SCROLL_SNAP_CENTER` + `LV_OBJ_FLAG_SCROLL_ONE`:
  the page follows the finger, snaps, and bounces at the ends (elastic scrolling). Callbacks: `on_change` while
  dragging (dots), `on_settle` at `SCROLL_END`. Used by the hourly view (days, horizontal) and the weather screen
  (places, vertical).
- Its scroll handler must ignore bubbled events (`target != current_target`): the hourly lists scroll vertically
  inside the pages and their `LV_EVENT_SCROLL` bubbles up; reading pager state from the list crashed the board.
- Long-press opens the Settings screen (below). Its *More on your phone* row shows the overlay with a QR code
  (`lv_qrcode`) for `https://<ip>`.

## Languages (`i18n.c`, `i18n_strings.h`)

- Every display text is a `tid_t` id from `main/i18n_strings.h`: one `X(id, "English", "French")` line per string,
  expanded into the id enum (`i18n.h`) and the text table (`i18n.c`). `tr(id)` returns the current language's text
  (English when a language has none). Format strings keep the same conversions in every language.
- Dates: `tr_weekday()`, `tr_date_long()` ("Wednesday, September 30" / "Mercredi 1er octobre"), `tr_date_ymd()` (the
  release notes' headers). French: lowercase weekday and month inside a date, **1er** for the first of the month,
  the plain number otherwise (OQLF); a standalone label starts with a capital. Weather conditions: `tr_weather()`.
- The language is a unit (`units_t.lang`, NVS `units/lang`); `config.c` calls `i18n_set()` on load and save.
  Changing it (Settings row *Langue*, or `POST /api/units {lang}`) calls `ui_units_changed()`, which also re-sets the
  fixed labels registered with `tlabel()` (titles, row names, buttons) and rebuilds the notes header; the release
  notes are fetched again (`ota_check_now()`, dates in the new language).
- Alerts keep both languages (`alert_t.name/area/text[ALERT_LANGS]`, EC's `_en` / `_fr` fields), so switching is
  instant; `AL` picks the one shown.
- French is **Canadian French** (Québec usage): balayer un code QR, appuyer longuement, tamiser (dim), endroit(s)
  (places), herbe à poux, micrologiciel; air quality Bonne / Acceptable / Mauvaise. Standard written French, no
  slang.
- The settings page has its own dictionary (`I18N` in `index.html`, same keys idea: `data-i18n`,
  `data-i18n-html`, `data-i18n-ph`, `t('key', args)`); it follows `GET /api/config` → `units.lang` and lists
  `languages`. City search and reverse geocoding ask for names in the page's language; French numbers use a decimal
  comma (`num()`).
- **Adding a language:** a column in `i18n_strings.h`, its row in `LANG_CODE` / `LANG_NAME` / `WD_FULL` /
  `WD_SHORT` / `MONTH` and the date rules in `i18n.c`, `LANG_COUNT`, and a block in the page's `I18N`. EC alerts
  only exist in English and French (others fall back to English).
- **Inuktitut (`LANG_IU`, "iu") is a draft awaiting a fluent reviewer** (docs/translations/README.md). Its column in
  `i18n_strings.h` and its `I18N.iu` block are **generated** by `tools/i18n_iu.py build` from
  `docs/translations/iu.tsv`: edit the TSV, never the generated syllabics. Glyphs come from `main/syllabics.ttf`, the
  `fallback` font of every TinyTTF font (`mkfont()`), drawn 5/4 larger. Dates use English order with the
  Inuktitut names. Syllabic words run 1.2 to 2.8 times wider than English: check every screen with snapshots
  after any change.

## Settings screen (`ui.c`, `cfg_*`)

- `scr_cfg`, opened by a long-press on the weather screen (offline: the Wi-Fi setup screen instead). Rows in a
  scrolling box (y 70–360) under a fixed *Done* button: Dim when quiet and Wake on pick-up (`lv_switch`, the whole
  row is the button), Timing (cycles Short / Normal / Long; the same presets as the page's `PRESETS`, *Custom* if
  none matches), Temperature / Wind / Clock (cycle), More on your phone (the QR overlay), Wi-Fi network
  (`ui_wifi_setup`), Updates (*Check now* → `ota_check_now()`, shows *Checking...* then the result for 6 s; with an
  update offered, opens the update screen), Restart (a second tap within 4 s). Every change goes through
  `presence_set_config()` / `presence_set_motion()` / `config_set_units()` + `ui_units_changed()`, the same calls
  as the web API, so the page and the display agree. A 1 s timer refreshes the rows while the screen is shown.
- **Brightness:** `cfg_arc` along the bottom edge is display-only; the band under it (`cfg_zone`, y ≥ 364) maps the
  finger's x to 5–100 % (`BR_X0`..`BR_X1`). While dragging, `presence_preview_brightness()` applies it at once
  (the presence task skips its fade for 300 ms); the value is saved on release. A clickable full-screen `lv_arc`
  caught every touch on the screen (rows, Done, scrolling): keep it non-clickable.
- The QR overlay and the Wi-Fi setup screen return to Settings when opened from it (`back_to_cfg`), else to the
  weather screen. *Done* or a swipe right closes Settings.
- Unused swipes call `lv_indev_wait_release()`; otherwise their release is also delivered as a `SHORT_CLICKED`.

## Updates over Wi-Fi (`ota.c`)

- **Flash layout** (`partitions.csv`): nvs 0x9000 and phy 0xF000 stay where v1.0–1.2 had them (settings survive),
  `ota_0` 0x10000 and `ota_1` 0x310000 (3 MB each), `otadata` 0x610000, `mapcache` 0x620000. Web flasher, flash
  helper and `flash.bat` also write `ota_data_initial.bin` (blank boot selection) so a USB flash always boots `ota_0`.
- **Source**: the Pages site. `channels.json` → the chosen channel (NVS `ota/channel`, "stable" default; beta falls
  back to stable when absent) → its `manifest.json` → the part at 0x10000 = app image URL. No GitHub API (rate
  limits, redirects), and the site only ever holds released files.
- **Versions**: `vX.Y.Z[-rc.N | -N-gHASH | -other]`; rc < release < dev build (git describe) of the same X.Y.Z; other
  suffixes count as pre-releases. Offered only if strictly newer; an unparsable local version (plain hash) is offered
  any release.
- **Release notes**: once an update is found, `notes.json` (from `CHANGELOG.md`, built by `make_flasher_site.py
  site`) is fetched and filtered to the releases newer than the running version, up to the offered one; when a
  release is offered, `-rc` sections are skipped (the release's section repeats them). Result: up to 3 KB of
  "vX|Month D, YYYY" header lines and one line per change, in PSRAM; `notes_id` in the status changes when it does.
  The update screen (`update_notes()`) builds an accent header and a bulleted label per release in its scrolling
  column; `GET /api/update` adds `notes` while an update is available. Missing notes don't block an update.
- **Task** `ota` (core 0, prio 2): first check 60 s after boot, then every 6 h, or on request. `ota_install()` →
  `esp_https_ota` (begin / perform / finish), progress to the listener, project name must match, restart 2.5 s later.
- **Rollback**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A new image boots `PENDING_VERIFY`; after 60 s running the
  task calls `esp_ota_mark_app_valid_cancel_rollback()`. A reset before that makes the bootloader return to the
  previous slot. (Needs the new bootloader: one USB flash.) `GET /api/update` reports `pending_verify` and
  `uptime_s` (since v1.10.0-rc.3) so tools don't restart a board during those 60 s: the test harness did once and
  tested the rolled-back firmware.
- **UI**: `ui_ota()` from the OTA task: pill at the bottom of the weather screen (tap region y > 408), `scr_update`
  with Install button and progress bar; `GET/POST /api/update` (`channel`, `action: check|install`) for the settings
  page's Firmware card.

## Extras page (`ui.c`, `weather.c`)

- Screens left to right: status, extras, weather, radar (page dots show 4, `N_PAGES`). `gesture_cb` handles every
  move between them.
- Data: the weather request adds `current=uv_index` and `daily=sunrise,sunset,uv_index_max`; a second request goes to
  `air-quality-api.open-meteo.com` (`current=us_aqi,pm2_5,alder_pollen,birch_pollen,grass_pollen,ragweed_pollen`), same
  10-minute cycle. Pollen comes from CAMS Europe: `null` elsewhere, and the row is hidden.
- Sun arc: `lv_arc` 180°→360°, indicator = fraction of daylight elapsed, a glowing dot on the arc at that angle.
  At night the arc is dim and the centre shows the next sunrise.
- Moon: phase from the mean synodic month (29.530589 d) since the 2000-01-06 18:14 UTC new moon; % lit =
  (1 − cos φ)/2. The picture is drawn row by row in a draw callback: dark disc, then the lit span from the terminator
  (`w·cos φ`) to the limb, on the right while waxing and the left while waning.
- UV and AQI levels use the standard category colours. The weather request's URL is long enough to need
  `buffer_size_tx = 1024` (the default 512 logged "Buffer length is small to fit all the headers").

## Status page (`svc.c`, `ui.c`)

- Two swipes right of the weather screen. Header: firmware version, channel and running slot
  (`esp_ota_get_running_partition()`); Wi-Fi RSSI, IP, uptime. Then one row per external service, in a list box at
  y 120–400 (clear of the round edge and the page dots) that scrolls.
- `svc.c` keeps the last outcome per service (`svc_id_t`): time of the last try and last success (`esp_timer`),
  duration, failures in a row, reason (`HTTP 503`, `Can't connect`, `Timed out`, `No reply`, `Bad response`…). Each
  fetch reports right after `esp_http_client_perform`: `weather.c` (forecast, air), `alerts.c` (list, region
  shape), `radar.c` `http_fetch` (GeoMet or OSM, by host, retries included), `ota.c` `get_json` (site JSON). The
  SNTP `sync_cb` reports the time server. Only transitions are logged (`svc: X: HTTP 503`, `svc: X: OK again`).
- Dot: green = last try OK, amber = one failure, red = failing again or never OK, grey = not used yet / checking.
  Each row also shows the API version called (Open-Meteo API v1, GeoMet WMS 1.3.0…); the GitHub Pages row shows the
  version the channel offers.
- Opening the page calls `svc_probe_stale()`: a short-lived task (8 KB stack) sends one small request to each
  service not contacted for 5 min (a 1-line forecast, `limit=1` alerts, GetCapabilities, OSM tile 0/0/0 with the
  User-Agent the tile policy asks for, `channels.json`) and reports it the same way. NTP isn't probed. The rows
  refresh every second while the page is shown.
- Labels are single-line `LV_LABEL_LONG_DOT` with a fixed height. Without the height, LVGL wraps them, and that
  overlapped the next line on the first try.

## Weather alerts (`alerts.c`, `ui.c`)

- Names, areas and texts are kept in English and French (see Languages).

- List: `https://api.weather.gc.ca/collections/weather-alerts/items?f=json&skipGeometry=true&bbox=<±0.005° around
  the location>`. The server intersects the box with the real region shapes, so a tiny box is a point query.
  Properties used: `alert_name_en`, `risk_colour_en` (yellow/orange/red), `event_end_datetime`,
  `expiration_datetime`, `status_en` (skip `ended`/`cancelled`), `feature_name_en`, `alert_text_en` (the standard
  "Please continue to monitor…" closing paragraph is cut). Same alert in two regions = one entry. Sorted red first.
  Fetched with the weather (every 10 min); a failed request keeps the previous alerts.
- UI: a pill in the alert colour replaces the city name; a tap in the top half opens `scr_alert` (title fixed; map,
  when/where and text in one scrolling column).
- **Region map** (`alerts_map()`): `items/<id>?f=json` gives the shape (≈4 KB for a county). Coordinates are pulled
  out with a small scanner instead of cJSON (thousands of points would mean thousands of small allocations). The zoom
  is the closest level (4–10) where the region fits around the location in the 300×200 crop, then one level out for
  context. Background: the radar's cached basemap for that zoom (`radar_basemap_read()`, under the new `cache_mux`),
  or OSM tiles fetched directly (`radar_osm_render()`) when it isn't cached. Region filled at 35 % plus outline,
  white dot at the location. Rebuilt only when the top alert changes. `ui_alert_map()` swaps the buffer under the
  display lock.
- Tested with a throw-away build using Athabasca, AB (frost advisory at the time). Large static buffers
  (`alerts_t`, `weather_t`, the alert text) use `EXT_RAM_BSS_ATTR` (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`),
  which also freed ~15 KB of internal RAM.

## Rain nowcast (`weather.c`, `ui.c`)

- Open-Meteo `minutely_15=precipitation,snowfall&forecast_minutely_15=9`: slot 0 is the current quarter hour, then
  2 hours. Each value is the sum over the *preceding* 15 minutes (native 15-min data in North America, from HRRR).
- `nowcast()`: wet = ≥ 0.1 mm. "Wet now" looks at slots 0–1. The first later slot that differs gives the change;
  the time shown is the start of that slot (the previous slot's timestamp). Snow if that slot has snowfall.
  Only the first change is reported; rain for the whole 2 h (or none) shows nothing.
- Shown in accent blue under the details line: "Rain around 14:45" / "Snow until about 15:30"; hidden otherwise.
  Refreshed with the weather (every 10 min).

## Hourly view (`ui.c`, `weather.c`)

- Open-Meteo `hourly=temperature_2m,weather_code,precipitation_probability,wind_speed_10m,is_day` with
  `forecast_days=7` → `weather_t.day[WX_DAYS]` and `hour[WX_DAYS*24]` (`WX_DAYS` = 7, hours start at 00:00 local
  today). Response is about 7 KB (receive buffer 48 KB in PSRAM). The weather screen shows `day[0..2]`.
- `SHORT_CLICKED` on the weather screen with y ≥ 296 → forecast column by x → `scr_hour` (move-top animation).
  `ui_weather()` keeps a copy of the forecast (`wx`) for this screen.
- `scr_hour` holds a horizontal pager (`pager.c`) with one full-screen page per forecast day (`WX_DAYS`), so pages
  follow the finger, snap, and bounce at the ends.
  Each page has its own vertically scrollable hour list; LVGL picks the scroll direction from the drag.
  Page dots update on `LV_EVENT_SCROLL`. A tap closes the view.
- **Rows are drawn, not created:** each list has one tall object with an `LV_EVENT_DRAW_MAIN` callback that draws
  only the rows inside the clip area (`lv_draw_label` with `text_local`, `lv_draw_rect`). The weather icons have a
  painter mode (`P_layer`) that draws the same shapes straight into the layer. 72 rows as real objects would have
  been several hundred small allocations in internal RAM.
- Today's page is refilled at each new hour and when new data arrives.
- **Temperature graph** at the top of each day's list (it scrolls away with the column headers): hours 0–24 (the
  last point is the next day's 00:00; the last forecast day stops at 23:00), filled curve, a 1-px line every hour
  (brighter every 3 h, which are labelled), the day's high and low (00:00–23:00) labelled. The vertical scale fits
  every point, with at least 4° of range so a flat day stays flat. On today's page the past hours are grey and a
  white dot marks now.
- The graph is drawn **once into a canvas** per day (`graph_render()`: 316×120 RGB565, about 76 KB each, 7 pages ≈
  530 KB of PSRAM) and redrawn only when `wx_gen` (bumped by each forecast) or the current hour changes. Drawn on
  every frame it was about 200 shapes and dragging fell from 14.9 to 12.1 fps; from the canvas it's back to 15 fps.
  The fill colours are pre-mixed with the black background instead of using transparency.

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
  in PSRAM. `compose()` blends the frame over the basemap at alpha×0.86 into `out565`, which an `lv_image` displays,
  then draws the frame's lightning bolts on top.
- **Lightning** (`fetch_lightning()`, called by `fetch_frame()` after each radar image): GeoMet layer
  `Lightning_2.5km_Density` (Canadian Lightning Detection Network: flashes of the last 10 min on a 2.5 km grid, every
  10 min, kept 3 h, Canada + 250 km), same bbox and size as the radar. Time = the radar time rounded down to 10 min;
  if that window isn't published yet (GeoMet answers XML, detected by the PNG signature) the one before. The image
  (density colours on 2–3 px squares) would vanish under the rain, so each 20×20 px block with any flash becomes one
  bolt (13×17 px, yellow, dark outline) at the flashes' centre, at most `LTG_MAX` (64) per frame. Marks are stored as
  2 px units **after the pixels in the frame's PSRAM buffer** (`LTG_X()` / `LTG_Y()`, buffer W×H + 128), so they move
  with it in `plan_frames()`; putting them in `frame_t` cost ~25 KB of internal RAM (it is copied ~46 times in
  static arrays). Optional: a failed request leaves the frame without bolts. Being requested for the view's bbox, the
  bolts follow the map on zoom and relocation like the rain. Adds ~0.3–0.5 s per frame (one more GetMap + decode).
  Testing without storms: a throwaway build that fills empty frames with marks at fixed lat/lon (converted with the
  view's `zoom` / `view_x` / `view_y`); fixed *screen* positions don't follow zooms and only test the drawing.
- **Animation:** 15 frames. The latest frame, plus 14 history frames on a fixed 12-minute grid (so refreshes reuse
  most of them). They download newest first while the radar screen is visible. A tap plays at 3 fps via an LVGL timer,
  holds the last frame about 1 s and loops for `PLAY_LOOP_MS` (60 s), then returns to live. A tap while playing
  stops it, and so does a zoom or location change. A new radar time doesn't: `plan_frames()` swaps the list under
  the display lock, playback skips frames not loaded yet, and the new frame joins the loop once downloaded.
  The live view (`show_live()`) shows the newest frame that loaded, so a failed download keeps the previous image
  (with its time in the label) rather than a map without rain.

## Wi-Fi setup / captive portal (`net.c`, `ui.c`, `web.c`)

- **Boot** (`main.c`): no saved network → `portal()` (AP only, until credentials are saved and the board
  restarts). Saved network → `net_begin()` + `web_start()` right away, then `net_wait(30000)`. If that fails,
  `offline_setup()` loops: open the Wi-Fi setup screen; when it closes (a tap, or 5 idle minutes), show
  *Connecting to …* and wait 30 s for the saved network; still nothing → setup again. New credentials restart the board.
- **Reconnects pause while a setup mode is on** (`net.c`, `setup_on()` = first-time portal, setup AP or Easy
  Connect): each disconnect schedules `esp_wifi_connect()` on an `esp_timer` (1 s for the first 8 tries, then 3 s,
  then 30 s). `ap_up()` and `net_dpp_start()` stop the timer and cancel an attempt in progress (`pause_saved()`);
  `ap_down()` and `net_dpp_stop()` call `resume_saved()`, which retries after 1 s (switching setup pages stops one
  mode and starts the other in between). **Why:** an attempt makes the radio hop channels, so phones couldn't join
  the setup network and Easy Connect failed (October 1 bug). The old rule (wait only while a phone was joined) wasn't
  enough. `BIT_FAIL` only ends `net_wait()`. SNTP starts on the first `GOT_IP`, whenever that happens.
- **Status screens** (`scr_msg`) take a long-press too: it starts the AP and shows the Wi-Fi QR (not in first-boot
  setup, which already shows it). A 10-minute timer stops that AP only once the board is online.
- On the weather screen, a long-press while offline skips the Settings screen (useless without a network) and opens
  the Wi-Fi setup screen directly.
- **Wi-Fi setup screen** (`scr_setup` in `ui.c`, `ui_wifi_setup(note)`): used by first-time setup, offline setup,
  status-screen long-press, the Settings screen's *Wi-Fi network* row and the QR overlay's long-press. Page 1 = setup AP QR (`net_setup_ap_start()`),
  page 2 = Wi-Fi Easy Connect; swipe switches. A tap closes it (except in first-time setup); offline the note says
  *Tap to try again*, because closing it lets the saved network be tried. Timer: 10 min online; 5 min offline unless a
  phone is on the setup AP (so a display whose router was rebooting gets back online by itself). `ui_wifi_setup_open()`
  tells `main.c` when it closed.
- **Easy Connect (DPP enrollee, `net.c`)**: `CONFIG_ESP_WIFI_DPP_SUPPORT=y`, `wpa_supplicant` in REQUIRES. The radio
  can't serve the AP and listen at once, so page 2 stops the AP (`net_setup_ap_stop_any()`, even in first-time setup),
  disconnects the station and pauses our reconnects (`dpp_active`). It then scans (~2 s) and listens on **one
  channel**: the saved network's if in range, else the strongest network's (`dpp_pick_channel()`). The saved network
  is looked for **by name** first (probe requests carrying its SSID, 120 ms per channel): a broadcast scan at 40-80 ms
  missed a router on a busy channel and keeps only the 16 strongest records (v1.10.0: picked channel 11 instead
  of 1). The broadcast scan is only the fallback. The phone stays on
  its own network's channel and the display needs ~0.3 s to answer its Authentication Request: on another channel the
  phone had already left and the exchange ended in `ESP_ERR_DPP_AUTH_TIMEOUT` (with "1,6,11" or "6"). It only worked
  online because the station was already on the router's channel. `esp_supp_dpp_bootstrap_gen(channel, QR)` is
  **asynchronous**: `esp_supp_dpp_start_listen()` must be called from the `ESP_SUPP_DPP_URI_READY` callback (calling
  it right after bootstrap_gen returns `ESP_FAIL`, which was the first bug). `ESP_SUPP_DPP_CFG_RECVD` gives a
  `wifi_config_t`: saved with `net_save_creds()`, restart after 2.5 s. `ESP_SUPP_DPP_FAIL` re-listens; its data is
  the error code on IDF 5.4 but a `wifi_event_dpp_failed_t *` on 5.5 (`failure_reason`). Leaving page 2
  deinitialises DPP and resumes reconnects.
- **Settings overlay state machine** (`ui.c`): 0 = hidden, 1 = settings QR (`https://<ip>`), 2 = Wi-Fi setup.
  The Settings screen's *More on your phone* goes to 1; a long-press on the overlay opens the Wi-Fi setup screen.
  Gestures are ignored while the overlay is open.
- **Access point:** `ap_up()` switches to APSTA, so the station connection stays up and the AP follows its channel.
  It sets DHCP option 114 (captive-portal URI `http://192.168.4.1/`) and starts the DNS server
  (`components/dns_server`), which answers every name with the AP's IP. The DNS server is started once and **never
  stopped**: `stop_dns_server()` deletes its task without closing the socket, so port 53 stayed taken and every later
  setup network had no DNS, hence no captive portal (bind `errno 112` in the log). First-boot setup (`net_start_portal()`) uses
  the same function.
- **HTTP :80 decides by interface** (`from_setup_ap()` looks at the socket's local address):
  - On the setup AP it *is* the portal. `/` serves the page over plain HTTP, because phone sign-in browsers reject the
    self-signed certificate. `/api/*` works, and every other URL (OS connectivity checks such as `/generate_204` or
    `/hotspot-detect.html`) gets a 302 to `http://192.168.4.1/` with a small HTML body, which iOS requires.
  - On the home network, everything redirects to HTTPS.
- On the plain-HTTP page the GPS button can't work (browsers only allow geolocation on secure pages). The page shows a
  link to the HTTPS version instead, and puts the Wi-Fi card first when opened on 192.168.4.1.

## Alert sounds (`sound.c`)

- **I2S shared with the microphones:** `presence.c` opens I2S0 in both directions (`i2s_new_channel(&cc, &tx,
  &rx)`, 16 kHz stereo 16-bit, DOUT 8, `auto_clear` so the speaker gets silence between sounds) and keeps one
  `audio_codec_data_if_t` (`presence_audio_data_if()`) for the ES7210 (in) and the ES8311 (out). `sound.c` opens the
  speaker device only while a sound plays (`esp_codec_dev_open` / `close`), so the amplifier is off otherwise. The
  microphones keep working during and after a sound (checked).
- **Sounds:** synthesised beeps, by alert level: yellow / statement 2 beeps (1.5 kHz), orange 3 + 3 quick beeps
  (1.8 kHz), red a hi-lo siren (1.8 / 1.35 kHz, ~3 s). Sine + 30 % 3rd harmonic, 6 ms raised-cosine edges, amplitude
  0.32 (headroom); the codec volume (0–100) scales it. The whole sound is **rendered into PSRAM first, then
  streamed** in 4 KB writes: computing it while playing crackled (the I2S DMA starved). Deliberately unlike
  Canada's Alert Ready attention signal.
- **When:** `main.c` `chime_new_alerts()` after each successful alert fetch for the place shown. An alert sounds once
  (ids remembered, 16 max); the first fetch for a place (start-up, place switch) only records what is already
  there. The most severe new alert decides the sound. `sound_alert()` applies the level setting (0 off, 1 red, 2
  orange and red, 3 all) and quiet hours (local time of the place shown; red always sounds). Tested with fake
  alerts (see CLAUDE.md): silent in quiet hours, a known alert doesn't sound again, a new one does.
- **Settings:** NVS `sound` (`level`, `vol`, `qfrom`, `qto`); defaults orange and red, 60 %, 22:00–07:00. Display
  Settings rows (SOUND: alert chime cycles Off / Red / Orange+ / All, volume cycles 20–100 % and plays the orange
  sound, test). `GET /api/sound`, `POST /api/sound` (any of `level`, `volume`, `quiet_from`, `quiet_to` "HH:MM",
  or `test`: true / 1–3); the page's Sound card.

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
  - Movement (`moved`): → ACTIVE from DIM or OFF; while ACTIVE it resets the quiet timer like noise.
- **Wake on pick-up** (`imu.c` + the presence loop): every 100 ms the accelerometer is read (QMI8658: ±2 g,
  62.5 Hz, gyroscope off). `rest` is a slow average of the acceleration vector (5 % per tick, ~2 s); movement =
  distance from it in g, so lifting or tilting counts and lying still in any position doesn't. Above `motion_thr` →
  `moved`. Measured on the board: still 0.001–0.005 g, a firm bump on the table ~0.07 g, picking it up 0.14–0.33 g.
  Sensitivity: High 0.05 g (a firm bump wakes it), Normal 0.10 g (default), Low 0.20 g (a clear lift). The first
  second of samples after boot is skipped (a 3.7 g junk reading came out before the sensor settled).
- **Settings storage:** `presence/cfg` is a blob of `presence_cfg_t`; a blob of another size is ignored on load,
  so **don't add fields to `presence_cfg_t`** (an update would reset everyone's screen settings). The pick-up
  settings are separate keys: `presence/motion` (u8) and `presence/motion_mg` (u16, threshold in milli-g).
- **Brightness:** CO5300 command `0x51`, faded in 10% steps per tick (about 1 s full ↔ off), under `display_lock()`.
  Rendering continues while the screen is off.
- **API:** `GET /api/presence` (config and live status: level, threshold, state, wake_progress, quiet_s, calibrating,
  brightness, `imu_ok`, `motion_g` (recent peak, decays in ~1 s, for the page's meter), `motion_thr`,
  `motion_wake`), `POST /api/presence` (config, including `motion_wake` and `motion_thr`), `POST /api/calibrate
  {seconds}`. The page polls status every 700 ms
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
  - `POST /api/location {name, lat, lon, index}`: `index` = which place (count = add one); without it, the place
    shown. `GET /api/config` returns `places [{name, lat, lon}]`, `active`, `max_places`.
  - `POST /api/places {select: i}` shows place i on the display; `{delete: i}` removes it (not the last one).
  - `POST /api/units {temp:"c"|"f", wind:"kmh"|"mph"|"ms", clock:24|12}` (any subset); `GET /api/config` returns
    `units` in the same form. Both servers; `max_uri_handlers` is 14 (12 were all used).
  - `POST /api/wifi {ssid, pass}`: restarts the device.
  - `GET /api/snapshot?screen=weather|extras|status|radar|update|alert|hourly0..hourly6|current` (HTTPS only): the
    screen rendered
    off-display (`ui_snapshot()` → `lv_snapshot_take`, RGB565 in PSRAM, needs `CONFIG_LV_USE_SNAPSHOT`), streamed as
    a top-down 24-bit BMP in 16-row chunks. Used by `tools/snapshot.py` (docs/TESTING.md).
- The page runs the phone's geolocation, reverse geocoding (Nominatim) and city search (Open-Meteo geocoding) in the
  **browser**; the device only stores the result.
- **Places card:** a list (tap a place to edit it; *Show* puts it on the display) and an editor that replaces the
  list while open: name, city search, a map with a draggable pin (tap to move it), phone location, coordinates
  folded under *Coordinates*, Cancel / Save, Delete. A spot picked on the map or by GPS suggests a name (reverse
  geocoding) unless the name was typed; editing an existing place keeps its name.
- **Map:** Leaflet 1.9.4 with OpenStreetMap tiles, loaded from unpkg the first time the editor opens, pinned and
  integrity-checked (SRI hashes in the page; `tools/webtest` checks them against the npm package). The phone needs
  internet for it, as for the city search; without it the editor says so and opens the coordinate fields.
- Location is stored in NVS namespace `loc`. `config_local_time()` uses Open-Meteo's `utc_offset_seconds`, which
  handles any time zone and DST; before the first fetch it falls back to the `EST5EDT` TZ rule.
- **Units** (NVS namespace `units`: `temp`, `wind`, `h12`; default °C, km/h, 24 h). Data is always fetched in metric;
  every screen formats through `config.c`: `config_temp()` (rounded °C or °F), `config_fmt_wind()`,
  `config_fmt_time()` / `config_fmt_hour()` / `config_fmt_hhmm()` (the forecast's "HH:MM" strings),
  `config_miles()` (radar distances in miles when the wind is in mph). The forecast's 0.1 °C and 0.1 km/h are
  converted exactly and rounded only for display, so °F can differ by 1° from a source that rounds the unrounded
  model value, which the forecast's own rounding does anyway. The graph's shape doesn't change (linear), only its
  labels (12-hour: 12a 3a … 12p … 12a).
- A change calls `ui_units_changed()`: re-renders the weather screen from the kept copy (`ui_weather(&wx)`, which
  also bumps `wx_gen` so the graph canvases redraw), the alerts' "Until", and `radar_units_changed()` (clock, frame
  time, range ring in km or mi, radius). No refetch.

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

1. **build** (every push, PR and tag): `espressif/esp-idf-ci-action` (ESP-IDF v5.5.4, component manager), then
   `make_flasher_site.py dist` → release files + `flash-parts.json` (chip, version, offsets from
   `build/flasher_args.json`), plus a merged full image. Uploaded as a workflow artifact.
2. **release** (`v*` tags): GitHub Release with those files; `prerelease` when the tag contains `-`.
3. **pages** (after a release, or *Run workflow* on `main`): `gh release list` → newest stable release and the newest
   pre-release if it's newer than that; downloads their assets; `make_flasher_site.py site` → `stable/`, `beta/`,
   `channels.json`, `notes.json` (from the checked-out `CHANGELOG.md`); deploys to GitHub Pages. The site always reflects released files, never a branch build.

ESP Web Tools resolves part paths relative to the manifest, so each channel folder is self-contained. The page swaps
the install button's `manifest` attribute when the channel changes. `new_install_prompt_erase` offers an erase on
the first install. The firmware version (`esp_app_get_description()->version`) is logged by `diag.c` and returned
by `GET /api/config` as `version`.

## Test console (`testcon.c`)

- Line commands on the USB serial port (USB-Serial-JTAG driver installed for input; the log keeps using the
  secondary console for output). Answers are `test:` log lines. Task `testcon`: 3 KB stack in internal RAM, core 0,
  priority 3 (a PSRAM stack crashed when a command read NVS, and heavy work such as the render bench must run in its
  own task: `diag_bench_request()`).
- Simulated finger: `touch_inject()` / `touch_inject_end()` in `touch.c` replace the controller's report, upstream
  of the wake-swallow and gesture logic.
- Wi-Fi test switches (`net_test_*` in `net.c`): a fake network name in the station config, never the saved
  credentials; `offline-boot` uses an `RTC_NOINIT` flag (one boot). `web_test_windows_quiet()` answers
  `/connecttest.txt` on the setup AP until restart.
- In every build on purpose: USB access can already reflash the board, and the harness tests what ships.
  See docs/TESTING.md §6.

## Memory budget (approximate)

| Item | Where | Size |
|---|---|---|
| LVGL draw buffers | internal DMA | 2 × 30 KB |
| LVGL heap (objects, styles, glyph cache) | PSRAM (`lvgl_mem.c`, `LV_USE_CUSTOM_MALLOC`) | ~40–50 KB |
| Basemap + composed screen | PSRAM | 2 × 434 KB |
| Hourly temperature graphs (7 canvases) | PSRAM (LVGL heap) | 7 × 76 KB |
| 15 radar frames | PSRAM | 3.3 MB |
| PNG decode (466×466 ARGB) | PSRAM (transient) | ~0.9 MB + zlib |
| TLS (client and server) | PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`) | ~40–60 KB per session |

Build: `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (debug `-Og` made LVGL rendering noticeably slow) and
`CONFIG_LV_DEF_REFR_PERIOD=15`.

Measured: internal RAM ~69 KB free steady, 30 KB min; PSRAM ~3.3 MB free, 1.1 MB min (v1.4.0, with the graph
canvases). See `docs/DIAGNOSTICS.md` for
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
