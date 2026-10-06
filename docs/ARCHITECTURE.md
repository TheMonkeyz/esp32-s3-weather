# Architecture


**Where the code lives (v1.14.0).** The infrastructure is espforge's (github.com/TheMonkeyz/espforge), taken at a
release tag by the component manager (`main/idf_component.yml` → `managed_components/`): forge_core (diag, test
console, i18n core, NVS helpers, version, png_rows, textfit, http_once, utf8), forge_net (`net.c`, `web.c`, `svc.c`,
`tlscert.c`), forge_ota and dns_server. Their sections below describe how this display uses them; espforge's
`docs/COMPONENTS.md` and the headers are the reference. This project's glue: `routes.c` (the page's app routes),
`console.c` (its console commands, "where", the "diag: display" line), `services.c` (its outside services),
`i18n.c` (texts, Inuktitut), Kconfig `CONFIG_FORGE_*` in `sdkconfig.defaults` (names, update site). CLAUDE.md,
"Shared with espforge", says how to change shared code.
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
| USB | USB serial/JTAG (COM5 on the dev PC) | |

The panel's init sequence and pin map come from Waveshare's BSP
(`waveshareteam/Waveshare-ESP32-components`, `bsp/esp32_s3_touch_amoled_1_75`). We don't use the BSP itself;
`display.c` is a small standalone driver.

## Tasks

| Task | Core / priority | Job |
|---|---|---|
| `main` (app_main) | 0 / 1, 10 KB stack | Boot flow, then weather loop: fetch every 10 min; woken early by a location change. Also draws the alert region map (TLS tile downloads + inflate when the basemap isn't cached): `app: alert map … stack N B spare` |
| `lvgl` | 1 / 4 | `lv_timer_handler()` loop under a recursive mutex (`display_lock()`) |
| `radar` | 0 / 3, 10 KB stack | Basemap, latest radar frame, history frames; sleeps unless the radar screen is visible |
| `presence` | 0 / 2 | Reads 100 ms of audio, computes the level, runs the dim/off state machine, fades brightness |
| `diag` | 0 / 1 | Every 60 s logs heap, frame timing, CPU and stack per task |
| `bench` | 1 / 4, one-shot, 10 KB stack (6 KB overflowed) | Times full-screen renders of each screen without showing them (UI blocked ~1.5 s); only on request (test console `bench`, the harness's `perf`). Until v1.11.0 it also ran by itself 45 s after boot and swallowed the swipes made meanwhile (`BENCH_AT_S` in `diag.c`, now 0) |
| `svc_probe` | any / 2, one-shot, 8 KB stack | Status page opened: one small request to each service idle for 5 min, then exits |
| httpd (HTTPS :443, HTTP :80) | – | Settings page + JSON API. Stacks 10 KB (TLS handshake ~3.3 KB, and a snapshot renders a whole screen here: 992 B spare was seen with 7 KB; `web: snapshot … stack N B spare`) / 6 KB (portal page: 1.1 KB spare with 4 KB) |
| `ota` | 0 / 2, 8 KB stack | Update checks and the install (TLS + flash writes); `ota: Update installed, restarting (… B of stack spare)` |
| `sound` | 0 / 3, 4 KB stack | Renders a warning sound into PSRAM and streams it to the speaker |
| `testcon` | 0 / 3, 4 KB stack | USB test console (568 B spare with 3 KB, 664 B with 3.5 KB); heavy commands start their own task |
| `sys_evt` (ESP-IDF) | 0 / 20, 3.5 KB stack | Wi-Fi / IP events, and Easy Connect's success path (log, NVS write, display labels): 2.3 KB left ~600 B; `net: Easy Connect: event task stack N B spare` |

Stack sizes come from the measured high-water marks in `diag: tasks` lines; re-check them there after adding work
to a task. Paths that end in a restart (an update's install, Easy Connect's success) or run rarely (a snapshot, the
alert map) log their own high-water mark, because the 60 s `diag` report never sees them.

**Rule:** any LVGL call from outside the `lvgl` task must be wrapped in `display_lock(-1)` / `display_unlock()`.
Keep lock holds short: `diag: display` reports the longest hold and which task did it (the radar holds it up to
~65 ms while swapping frames).
LVGL timer and event callbacks already run inside the lock.

## Display pipeline

- LVGL renders in partial mode into two internal DMA buffers (466 × 32 lines).
- `flush_cb` byte-swaps RGB565, sets the window (`0x2A` with +6 column offset, then `0x2B`) and sends pixels with
  QSPI command `0x32 / 0x2C`.
- A rounder callback makes every area start on an even pixel and end on an odd one (a CO5300 requirement).
- QSPI at **80 MHz** since v1.11.0 (40 MHz before): a full frame takes ~11 ms on the bus instead of 22.
- The transfer-done interrupt runs on **core 1** (`isr_cpu_id`), the LVGL task's core. esp_lcd isn't thread-safe:
  with the interrupt on core 0, a raw frame (below) hung for good every few slides.
- **Raw frames** (`display_raw_frame(fill, user)`, for `slide.c`): with LVGL paused (display lock held), `fill`
  writes each 32-line band into LVGL's own two buffers and the band goes straight to the panel; band k is filled
  while band k−1 is on the bus (~15 ms a frame). Rules that came from hangs: first wait until LVGL's last band is
  out (`lvgl_inflight`: it outlives the refresh), and never call esp_lcd (the window commands) while a band is still
  in flight; every wait gives up after 200 ms. If it ever sticks, the test console's `where` prints the breadcrumbs
  (`slide_phase`, `raw_phase`, `raw_band`, `lvgl_inflight`). After a raw frame, LVGL must redraw before it flushes
  again (the slide invalidates the screen).
- **Any esp_lcd call from outside LVGL** follows the same rule: `display_brightness()` (the presence task, core 0,
  display lock held) first waits (bounded, 100 ticks) until LVGL's last band is out, as raw frames do. Before
  v1.12.0 it wrote the brightness command while LVGL's last band could still be on the bus, from the other core
  (`raw_phase` 7 = waiting, 8 = sending).
- `display_raw_area()`: the same for a rectangle only (a list's), several rows per band when it is narrow, bands top
  down or bottom up (a fill that moves rows down in place needs the bottom ones first).
- **Flush hook** (`display_set_flush_hook()`): every area LVGL sends is also handed, before the byte swap, to
  `slide.c`, which copies it into its picture of the screen shown (see Moves, Cache).
- Fonts: Montserrat TTF embedded and rendered by TinyTTF at 15–96 px, so accents and "°" render correctly.
- **Core dump** (since v1.12.0-rc.6): a crash is written to the `coredump` partition (128 KB at 0xA20000, ELF,
  CRC32) and the next boot logs `diag: coredump: last crash in task …, PC …, backtrace …` and erases it (decode
  with `xtensa-esp32s3-elf-addr2line -pfC -e build/v55/weather_amoled.elf <addresses>`). The partition comes with a
  USB / web-flasher install (the table isn't updated over the air); without it the firmware logs that once and
  carries on.

## Weather screen (`ui.c`)

- Layout was tuned for the round screen using the host simulator (see CLAUDE.md). Everything stays inside the
  circle; the forecast row ends at about y=408.
- Icons are built from LVGL primitives (circles, rounded rectangles, one line for the lightning bolt), scaled per use.
- The detail line is a flex row (`p->detail`): feels-like, then a blue droplet (`drop_draw`: circle + triangle, the
  raindrops' `0x4DA3FF`) before the humidity and a light-grey wind mark (`wind_draw`: three staggered strokes,
  `0xC9D1DA`) before the speed. The TinyTTF Montserrat has no symbol glyphs, so small icons are drawn
  (`LV_EVENT_DRAW_MAIN`), not typed.
  Decorative objects are made non-clickable so presses bubble up to the screen.
- Moves between screens (status | extras | weather | radar), between places and between days follow the finger:
  they are drawn as pictures by `slide.c` ("Moves" below), started by `drag_read` in `ui.c` (in the touch read).
  `LV_EVENT_GESTURE` (`gesture_cb`) is left with the radar's zoom swipes (up / down). Every other
  screen change (hourly view, Settings, alerts, update) calls `slide_screen()`, which takes the same arguments as
  `lv_screen_load_anim()`.
- **Places:** the weather widgets live on one page per place (`place_page_t pp[MAX_PLACES]`) in a vertical pager
  (`pager.c`) on `scr_main`; the alert pill, update pill, page dots, place dots and settings overlay are siblings
  above it. `passthrough(scr_main)` makes everything non-clickable, then the pager gets `CLICKABLE` back (a
  non-clickable object can't start a scroll). `ui_place(i, name, w)` fills page i (NULL = "Loading...") and keeps a
  copy (`pw[i]`) for redraws; `ui_places(n, active)` hides unused pages, moves the dots, and scrolls to the active
  page when it was chosen on the settings page. When the pager settles on another page, `place_select_cb` →
  `main.c` selects it. Each page's clock uses its place's `utc_offset`. Icon objects need unique bolt-point slots:
  page × 4 + icon.
- `main.c` keeps every place's forecast (`wx[i]`, tagged with the coordinates it was fetched for, so edits and
  deletions never show one place's weather under another's name). Each place is due 10 min after its last success
  (the place shown first); a failure is retried after 30 s, 1, 2, 5, then every 10 min (HTTP 429: 10 min), per
  place (until v1.12.0 every place was refetched every 30 s during an outage: ~11,500 requests a day, over
  Open-Meteo's free quota). New or edited places (`tried[i]` ≠ their coordinates) are fetched at once, and so are
  failed ones when Wi-Fi comes back, from the first delay again (rc.6 kept a 2–10 min wait across a reconnect: after
  an Open-Meteo bad patch the weather screen came back minutes after Wi-Fi did). Alerts, air
  quality and the radar are for the place shown: every 10 min, and at once after a switch, an edit or a language
  change (`extras_now`).
- **Open-Meteo sometimes stalls:** it accepts the connection and the request, then answers nothing for 15 s and more
  (`ESP_ERR_HTTP_EAGAIN`), for a minute or so. Seen from the display (rc.6's update test, a debug run) and, on
  October 3, from the PC too: 4 of 448 requests over two hours (connected in 0.06 s, no first byte in 20 s), the rest
  in 0.07-0.5 s. On October 4 it also answered HTTP 200 with only its own error, "Unexpected error while streaming
  data: allEndpointsUnavailable" (`weather: Unexpected response (HTTP 200, N bytes): …`; that line is only ever a
  200, any other status, 429 included, is a `forecast HTTP failed … status N` line). Not a rate limit: no 429, and
  the PC on the same connection got answers. It is the server, not the display; the retries above cover it. A failed forecast or air-quality
  fetch logs how far it got (`connected N ms, request sent N, first byte N, N bytes`) and internal RAM.
- **When the forecast can't be had** (`ui_place_state(i, ok)`): a page without a forecast says "Can't reach the
  forecast service. Retrying." instead of "Loading..." forever, and so does the start-up message after a failure. A
  line above the clock (`p->age`, ~220 px wide there) says "No connection" while offline and "Updated N min ago" once
  the forecast is over 30 min old; nothing otherwise. It is updated on the minute tick, and only its rows of the
  page's picture are marked (as the clock's).
- **After local midnight** the forecast still starts yesterday until the next fetch (10 min online, the whole outage
  offline). `ui_place()` drops the days before the place's local date (`weather_from_today()`, host-tested), and the
  minute tick re-sends a place's forecast when its midnight has passed: the hourly view's "Now", the graph's dot, the
  day names and the "Today" column are right from 00:00. Until v1.12.0 they showed yesterday.
- **Gesture hint:** once per display (NVS `ui/gest`, every display once since v1.12.0-rc.6), the overlay without its
  QR code says how to get around (sideways, up/down, a day, the long-press), `ov_state` 2; after the location hint
  when both are due.
- **A place chosen on the settings page** slides like the end of a drag (`slide_page()`: the two pages' pictures, then
  `pager_go()` without an animation). LVGL's scroll of the place pager redrew the whole screen each frame (~10 fps).
- **First run:** a display still on the built-in place (no place saved, one place) shows the settings QR by itself
  once, the first time a forecast is on screen, titled "Choose your location" (`ui_first_run()`; NVS `ui/hint`). A
  tap closes it. The test console's `hint next-boot` asks for it on the next boot without touching the places or the
  flag (RTC memory). The Settings row that opens the QR says "Location & more (phone)".
- `config.c` stores the places: see Settings / web (typed NVS keys since v1.12.0).
- The radar only caches the **first** place's maps (`cache_save()` returns otherwise, and no preload): switching
  between places would rewrite up to 3.5 MB of flash and fetch 63 OSM tiles each time.

## Pager (`pager.c`)

- Full-screen pages side by side or stacked in a scroller with `LV_SCROLL_SNAP_CENTER` + `LV_OBJ_FLAG_SCROLL_ONE`:
  the page follows the finger, snaps, and bounces at the ends (elastic scrolling). Callbacks: `on_change` while
  dragging (dots), `on_settle` at `SCROLL_END`. Used by the hourly view (days, horizontal) and the weather screen
  (places, vertical).
- Its scroll handler must ignore bubbled events (`target != current_target`): the hourly lists scroll vertically
  inside the pages and their `LV_EVENT_SCROLL` bubbles up; reading pager state from the list crashed the board.
- **Both pagers are frozen since v1.11.0** (`pager_freeze`: scroll direction `LV_DIR_NONE`). LVGL's elastic scroll
  redrew every widget for every frame (10–15 fps). The drag is drawn by `slide.c` from pictures instead:
  `pager_peek(i)` moves the pager to page i and back within one LVGL cycle to take its picture (no callbacks, the
  `quiet` flag), and `pager_switch(i)` ends the drag (calls `on_change` and `on_settle`, like a scroll that settled
  there). The hourly lists inside the pages still scroll with LVGL.

## Moves: slides and drags (`slide.c`)

Why: LVGL 9.2 redraws every widget for every frame of a move. A full screen costs 65–85 ms (each of the 15 bands
walks the object tree, and TinyTTF draws the glyphs), so swipes ran at 10–15 fps. The profiler (docs/TESTING.md §8)
found no single hot spot to fix. Copying finished pictures is cheap: ~8 ms per screen from PSRAM into panel byte
order, and ~11 ms on the bus.

- **Pictures:** a screen is rendered off-display into a 466×466 RGB565 draw buffer (434 KB, PSRAM through LVGL's
  heap). `slide_picture_rows()` renders only rows y0..y1, so a picture can be made one strip at a time. `render_rows()`
  also clears and draws up to 8 rows on each side (`ROW_MARGIN`, as far as the buffer has room): LVGL skips a label
  whose box misses the rows drawn, and Inuktitut's syllabics (the fallback font, 5/4 larger) reach a few rows past
  their box, so a strip ending just above a label lost the glyphs' tips (v1.14.1). Painting saves
  and restores the display's redraw list (`inv_p`) and ignores the invalidations it causes.
- **Slides** (`slide_screen(to, MOVE_*, ms)`, run on the next LVGL cycle via `lv_async_call`): pictures of both
  screens, then frames sent with `display_raw_frame()`. `fill()` composes each row from the two pictures with a byte
  swap; the motion is a cubic ease-out with as many frames as fit (64–70 fps). Then the real screen is loaded and
  LVGL redraws it. Not enough PSRAM, or a picture failed: plain `lv_screen_load_anim()`.
- **Drags** (`slide_drag()`) are recognised in the touch read itself (`touch_set_read_hook()`: `ui.c`'s `drag_read`
  sees each read before LVGL handles it, and `lv_indev_wait_release()` there keeps LVGL from acting on that same
  read). A 10 ms timer watching LVGL's point (until v1.10.1-rc.3) came too late on flicks: the finger moves 30–60 px
  between two reads, and LVGL had already begun its own scroll of a list. After 16 px the larger axis decides, as
  LVGL picks a scroll direction; a 2:1 rule missed curved swipes on the round screen. From then on slide.c reads the finger
  itself (`touch_get()`, LVGL paused) and draws the current picture with the neighbour coming in under the finger.
  At an end (no neighbour) the screen resists (a third of the movement, at most a fifth of the screen) and bounces
  back. On release it goes on to the neighbour past a third of the screen, or after a flick (more than 24 px and
  0.35 px/ms that way), else back. `commit(side)` loads the screen or `pager_switch`es the page.
  - Five touch read errors in a row count as a release (after the 60 ms hold, see "List scrolls"): the CST9217 often
    stops answering (NACK) when nothing touches it instead of reporting "up", and the drag never ended. Every loop in `slide.c` that reads the finger
    goes through `finger()`, which keeps that rule (and "up stays up until a press"): since v1.12.0 also the
    PSRAM-busy drag (it waited for a clean "up" forever, holding the display lock) and the zoom (a failed read kept
    the swipe that started it "on", so a second swipe during the zoom was ignored).
  - While a release is being confirmed (those 60 ms, or read errors) the page goes on at the finger's last speed, at
    most 80 ms worth (v1.13.1, from espforge): it used to stand still, then snap, at the end of every swipe. The drag
    line's part after " | " says what a real finger did: the longest gap between frames, reads held by an error or an
    unconfirmed "up" (and the longest run of them), how long the finger itself stood still, samples, speed. The
    harness reads only the part before " | ".
  - Safety caps: a pure wait for the lift gives up after 3 s, a loop following the finger (drag, list scroll) after
    20 s (a slow scroll while reading is legitimate); then the move ends as if the finger had lifted and LVGL takes
    the touch over. Both log a warning, which the harness reports.
  - The neighbour in the detected direction is rendered before the first frame, and a finger lifted before that
    frame counts as a flick that way: a quick flick could be over before anything was drawn.
- **After a drag** (`touch_resync()`): `touch_forget()`, LVGL's `wait_until_release` cleared and `lv_indev_reset()`.
  Without this, LVGL took the next touch for the old one still going (the next swipe was ignored), or `touch.c`'s
  NACK guard replayed the last point LVGL had seen as a press: a stray tap where the drag had started, which opened
  the hourly view after place drags.
- **Cache** (`slide_cache_*`): 5 pictures keyed by screen, or by page for the two pagers (`key_of()` in `ui.c`).
  `pictures_tick` (every 30 ms) keeps the shown screen and its neighbours (screens, places, days), and
  `slide_cache_idle_work(800)` renders the first missing or out-of-date one, one 64-row strip per tick (~15–25 ms):
  once nothing has changed on screen for 0.8 s and no finger is down, or when a picture has been out of date for 2 s.
  When nobody has touched the display for 2 s (new data: a forecast, alerts), the wait is 0.15 s and the strips
  follow each other (the timer runs every 1 ms while there is work; LVGL still reads the touch between strips).
  A whole picture at once blocked LVGL for 60–180 ms, and a quick flick could start and end unseen.
  - **The picture of the screen shown follows the panel** (since the list scrolls): the flush hook copies every area
    LVGL draws into it (`flushed()`), so a redraw of the screen shown never makes its picture out of date, and a drag
    or a list scroll never waits for it. Only when the screen shown changes before LVGL has drawn its last redraw (a
    screen load right after a change) does that redraw miss the picture: `invalidated()` / `rendered()` track the
    key with redraws pending (`inv_key`, `inv_pending`) and mark it. Ignored: the redraws of painting (pager peeks)
    and of a drag's switch (the pager's scroll redraws the page being left, which didn't change). `pictures_tick`
    keeps the picture of every screen shown (any screen can have a list), plus the neighbours of the four drag screens.
  - For the screens not shown, the `ui_*` functions mark what they change: `ui_place` its page (and, for the place
    shown, extras and the hourly days via `place_current`), `ui_air` the extras page, `ui_ota` the update, status
    and Settings screens (the places only when the weather screen's update pill changes: every update check used to
    mark everything), `ui_alerts` the alert screen and the pill's rows on the place shown's page (`pill_rows_dirty`),
    `ui_alert_map` the alert screen, `ui_units_changed` and a new place count everything (`slide_cache_dirty(NULL)`);
    the radar marks its own screen in `show_live()`. New code that changes a screen while it isn't shown must do the
    same, or a drag shows stale content for a moment. And no more than it changes: until v1.14.1 `ui_alerts` marked
    every picture, so with an alert at home the "none" sent at each switch (below) made both places' pictures out of
    date, and the drag back 2 s later waited 124-155 ms for one (October 5; harness `perf.alert_active`).
  - **Rows only** (`slide_cache_dirty_rows(key, y0, y1)`): a picture is out of date in a row range (`d0`..`d1`,
    widened by later marks), and only those rows are rendered again (`get()`, the idle strips). A range of one strip
    or less renders at once, without waiting for a quiet screen. The minute tick: `clock_tick` marks each other
    place's clock rows (from the label's position in its page), the radar its pill's rows when `radar_clock` changes
    it, and `slide_cache_dirty_hidden(minute_marks_own)` marks every other hidden picture whole (extras: the sun moves
    and `extras_refresh()` only runs before a whole render). A drag 0.6 s after the minute now starts in 15 ms (it
    rendered a whole neighbour, ~130 ms; at the very start of this work two). `drag_paint` moves the pager's dots
    only when the rows it renders include them (twice a whole-screen layout: ~50 ms for 36 clock rows).
  - **Redraws that change nothing still make a picture out of date, and the next drag waits for it** (0.2–0.5 s:
    long enough for a quick drag to end before its first frame). A place switch sends the place shown, "no alerts"
    and air quality again; each used to redraw. So: `ui_place`, `ui_alerts` and `ui_air` return early when nothing
    changed; `set_hidden()` only touches a flag that changes (LVGL redraws an object even when the flag is already
    right); `clock_tick` only sets labels whose text changed; `place_dots` remembers what it drew; `drag_paint` applies
    the layout before it returns (the dots moved back after a neighbour's picture were redrawn at LVGL's next refresh,
    for real); `CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0` (the theme's 80 ms transition on our screens redrew the
    whole weather screen ~0.1 s after every drag). Found with a throwaway build that logged each picture marked out of
    date with `__builtin_return_address(0)`, and each redraw of the shown screen with its area and
    `esp_backtrace_print()` (decoded with `addr2line`). Measured with 10 place drags 1.5–20 s apart: before, 4 of 10
    waited 0.2–0.6 s; after, all started within 14–18 ms.
  - **After new data** (a forecast, alerts) the pictures are out of date, and a drag before they are rendered again
    renders its neighbour first (~0.12 s). Until v1.12.1 that lasted 1.5 s for a place drag and 1 s for a screen
    drag (0.8 s of quiet, then a strip per 30 ms); untouched, a place drag is now ready after ~0.6 s and a screen
    drag after ~0.3 s (console `dirty` + drags at fixed delays; harness `drag_place_after_data`). Other places' pictures never show the alert pill of
    the place shown (`drag_paint` hides it): a switch clears the alerts until the new place's are fetched. So the
    place left had the pill in its picture and is drawn without it from then on: `ui_places` marks those rows (the
    pill and the city name, ~40: rendered again at once, as a clock's).
- **List scrolls** (`slide_scroll()`, from `drag_read` for a vertical drag on a scrollable object other than the
  weather screen's places and the radar's zoom swipes: `slide_scroll_target()`): LVGL redrew all of a scrolling list
  for every frame (35–50 ms: 17–22 fps). Here each frame moves the list's rows within the picture of the screen shown
  (which matches the panel, see Cache), has LVGL render only the rows coming into view (into a 102 KB strip buffer, 96 rows and the margins),
  and sends just the list's rectangle (`display_raw_area`); the move and the send are one pass
  (`scroll_move_fill`), bottom up when the content goes down. LVGL's own scroll position is kept up to date with
  `lv_obj_scroll_by()` so it renders those rows right; its redraw requests are dropped at the end (`inv_p` restored:
  the panel and the picture already show the result).
  - **`lv_obj_scroll_to_y()` stops at the ends**: past them (pulled, springing back) the picture moved while the list
    didn't, and the rows rendered for it repeated the edge (the user saw the hourly graph's hour labels smeared and
    a doubled last row). `lv_obj_scroll_by()` isn't bounded.
  - Like LVGL: the list follows the finger, resists past an end (a third of the movement, at most a fifth of the
    list) and springs back (ease out, 220 ms); after a flick it goes on and slows down (`v × e^(−t/300 ms)`: ~v ×
    300 ms, as LVGL's 10% per frame; past an end it brakes in ~30 ms, then springs back). The flick speed is
    measured over the last ~80 ms of fresh readings, starting with the press and the 10 px that made it a scroll
    (passed by `drag_read`): a quick flick can be over before the first frame.
  - **Deciding a touch:** `drag_read` makes a move a drag or a scroll after 10 px along the larger axis; LVGL's own
    scroll limit is 20 px (`lv_indev_set_scroll_limit`), so it never starts first (it did at 10 vs 16: flicks went to
    LVGL's slow scroll). A touch during the coast or the spring-back stops the list and is decided the same way:
    vertical follows from where it went down, sideways is handed to `slide_scroll_on_sideways()` (the hourly view's
    day drag). A finger already down when a drag or scroll gives LVGL the touch back is a new press for `drag_read`
    (`touch_forgotten()`).
  - **Reading the finger** (`finger()`, every loop that follows it): the chip is read at most every 10 ms
    (`touch_get()`, `touch_fresh()`); polled every millisecond it answered "up" for long stretches with the finger
    on it. A reported "up" or 5 silent reads count after 60 ms (`UP_HOLD_US`, -2 meanwhile); the scroll starts
    coasting at once and follows again if the finger is still there, and never ends on an undecided "up". The
    scroll's log line counts lifts, silences, re-touches, bridged "ups" and the longest gap between reads.
  - At the start LVGL lets go of the touch (`lv_indev_reset`: the row pressed under the finger, e.g. a Settings row
    with a pressed colour) and `lv_refr_now()` draws that, into the panel and the picture. No exact picture of the
    screen yet (just opened): `slide_scroll()` returns false and LVGL scrolls as before.
  - The list moves with `lv_obj_scroll_by_raw()`: `lv_obj_scroll_to_y()` stops at the ends (see above), and
    `lv_obj_scroll_by()` sends SCROLL_BEGIN / END, which bubble up the hourly view (list -> page -> pager -> screen,
    all with `EVENT_BUBBLE`) and made LVGL lay out the whole screen again on every frame: 4.4 ms of the hourly list's
    7.7 ms of rendering. Settings' box doesn't bubble events, which is why only the hourly list paid. Found by timing
    `render_rows()`'s parts (layout / draw) in a throwaway build.
  - Per frame (harness): hourly list 13.7 ms (render the new rows ~3, move and send ~11), Settings 13.2 ms, status
    page ~16 ms (QIO flash; 15.0 / 14.4 with DIO).
  - `pictest` (test console) compares the picture of the screen shown with a fresh rendering, after drawing pending
    redraws; the harness runs it after drags and scrolls. Snapshot `picture` (`GET /api/snapshot?screen=picture`)
    returns the picture itself, to compare with the `current` snapshot pixel by pixel (how the smear was found).
- **Radar zoom** (`slide_zoom()`, from radar.c's `start_scale_anim()`; LVGL's scale animation stays the fallback):
  LVGL transformed the whole 466×466 image for every frame of a zoom (~100 ms: ~10 fps). Here each frame is scaled
  straight into the panel bands (nearest neighbour through per-frame row and column maps, pivot at the centre, as
  LVGL with antialias off), ease out over 300 ms, then LVGL's own scale is set and it redraws the same last frame.
  - The screen's other objects (pill, labels, range ring, dot: 30–70 % opaque over the map) are rendered once at the
    start, with their alpha, into 32-row ARGB8888 strips on a transparent background, and kept as a list of the
    pixels they cover (~16k: position + alpha packed in a word, colour apart, 6 bytes each; `ovl_row[]` indexes the
    rows). Each frame blends them over the scaled rows. Per strip only the box the objects cover in those rows is
    cleared, drawn and scanned (32-bit reads): 52–79 ms before the first frame since v1.12.1, ~110 ms before.
    Measured per object (a throwaway build): drawing them all takes ~36 ms, the range ring 19 of it; clearing
    and scanning whole 466-px strips took as long again. (It used to say "mostly the ring": it wasn't measured.)
    Taller strips didn't help; LVGL's own zoom showed its first frame after ~100 ms.
  - LVGL doesn't read the touch during the zoom: the zoom reads it, and a swipe made meanwhile goes to the radar at
    the end (`zoom_swipe` -> the next zoom). A touch already down at the start is the swipe that triggered it (LVGL's
    gesture fires before the finger lifts) and is ignored until it lifts: counted, one swipe zoomed twice.
  - 40–43 fps (harness `radar_zoom_fps`). Tables in PSRAM (`EXT_RAM_BSS_ATTR`): as static internal arrays they took
    3.8 KB of internal RAM and its low point fell to 3 KB.
- **Ready to open:** on the weather screen, spare cache slots keep today's hourly page and Settings (with 2 places
  there's room for the hourly page only: 4 + 1). `drag_paint` first puts a hidden screen in the state it opens in
  (`fill_page` + list at the top for an hourly page, as `main_tap` does; `cfg_open_state()` for Settings), so the
  slide that opens it starts at once (hourly: 0 ms instead of ~170). `ui_settings_changed()` (web.c: presence,
  sound, update channel) marks Settings' picture; the minute tick marks both (they're hidden pictures).
- **Memory:** `room_for(n)`: free PSRAM above 1 MB + 440 KB per picture, and a 900 KB block. 5 pictures = 2.2 MB.
  A slide may reuse cache slots (`force`) rather than fall back to the slow animation.
- **Measured (v1.11.0, harness `perf`):** screen to screen 64–70 fps (was 10–15; one run measured 32 fps to the
  radar, the next 67), places 46 fps (66 with the pictures ready; was 10), days 58 fps (was 11), drag start ~15 ms.
  Lists (rc.4): hourly ~52 fps while moving (was 17.5), Settings ~70 (was 22), status page ~64. v1.11.1: hourly
  ~60 fps (raw scroll), radar zoom 40–43 fps (was ~10), today's hourly view opens at once.
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
- **Text from outside** (network names, place names typed on a phone) goes through `textfit()` (`textfit.c`, twin of
  espforge's, v1.13.1): characters neither font has (emoji) are left out, "…" when nothing is left. TinyTTF draws a
  box for a missing glyph and never says so; textfit reads both TTFs' character maps (`textfit_init` + `textfit_add`
  in `ui_init`). Applied before formatting, never to the app's own texts. Host test: `tests/host/test_textfit.c`.

## Settings screen (`ui.c`, `cfg_*`)

- `scr_cfg`, opened by a long-press on the weather screen (offline: the Wi-Fi setup screen instead). Rows in a
  scrolling box (y 70–360) under a fixed *Done* button: Dim when quiet and Wake on pick-up (`lv_switch`, the whole
  row is the button), Timing (cycles Short / Normal / Long; the same presets as the page's `PRESETS`, *Custom* if
  none matches), Temperature / Wind / Clock (cycle), Language (cycles English / Français / Inuktitut (draft)), Alert
  sound (Off / Red / Orange+ / All), Volume, Test the sound, Location & more (phone) (the QR overlay), Wi-Fi network
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

## Updates over Wi-Fi (espforge's forge_ota `ota.c`, `ota_web.c`; texts: `main.c` `ota_err_text`)

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
- **Task** `ota` (core 0, prio 2): first check 60 s after boot, then every 6 h, or on request, and whenever the
  station comes back online (a check missed during an outage waited up to 6 h). `ota_install()` → `esp_https_ota`
  (begin / perform / finish), progress to the listener; the image's project name **and version** must be the ones
  offered; restart 2.5 s later. A failed download goes back to "available" with the reason (Install stays) and the
  channel is checked again 2 min later (it hid the pill and Install until the next check, up to 6 h).
- **Rollback**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A new image boots `PENDING_VERIFY`; once it has run 60 s
  **connected to Wi-Fi** (10 min without: a home without Wi-Fi keeps a working display) the task calls
  `esp_ota_mark_app_valid_cancel_rollback()`. Until v1.12.0 60 s were enough, counted before Wi-Fi had even started:
  an image whose network never worked would have been kept. A reset before that makes the bootloader return to the
  previous slot; that one finds the undone image (`esp_ota_get_last_invalid_partition()`) and says so once: log,
  update screen, `rolled_back` in `GET /api/update` (NVS `ota/rb_seen` remembers it was shown). Restarts asked for
  in that window (Settings → Restart, Save Wi-Fi, Easy Connect) wait for the confirmation (`ota_restart_when_safe()`). (Needs the new bootloader: one USB flash.) `GET /api/update` reports `pending_verify` and
  `uptime_s` (since v1.10.0-rc.3) so tools don't restart a board during those 60 s: the test harness did once and
  tested the rolled-back firmware.
- **UI**: `ui_ota()` from the OTA task: pill at the bottom of the weather screen (tap region y > 408), `scr_update`
  with Install button and progress bar; `GET/POST /api/update` (`channel`, `action: check|install`) for the settings
  page's Firmware card.

## Extras page (`ui.c`, `weather.c`)

- Screens left to right: status, extras, weather, radar (page dots show 4, `N_PAGES`). Moves between them are drags
  drawn by `slide.c`, started in the touch read (`drag_read`, see Moves).
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

## Status page (espforge's forge_net `svc.c`; `services.c`, `ui.c`)

- Two swipes right of the weather screen. Header: firmware version, channel and running slot
  (`esp_ota_get_running_partition()`); Wi-Fi RSSI, IP, uptime. Then one row per external service, in a list box at
  y 120–400 (clear of the round edge and the page dots) that scrolls.
- **One request, one connection:** `http_once()` (`http_once.h`) creates the client, performs and cleans up.
  `esp_http_client_init()` returns NULL when memory is short and `perform()` dereferences it: that is now a failed
  fetch (`ESP_ERR_NO_MEM`), not a reboot. `radar.c` keeps its own keep-alive client and checks it the same way.
  The parsers check every array they index: a forecast missing `daily.time` (or with a shorter array) crashed every
  fetch, and the region-shape scanner looped forever on a `-` that starts no number. `tests/host` replays such
  replies against the real code.
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
  "Please continue to monitor…" closing paragraph is cut). Same alert in two regions = one entry. At most four
  (`ALERTS_MAX`), the most severe kept: once four are held, a later feature replaces the least severe one if it is
  worse (`parse_features()`); then sorted red first. Until v1.12.0 the cap ran in the server's order before the sort,
  and a red warning listed fifth (the request asks for 20) was dropped: no pill, no sound.
  Fetched with the weather (every 10 min); a failed request keeps the previous alerts.
- UI: a pill in the alert colour replaces the city name; a tap in the top half opens `scr_alert` (title fixed; map,
  when/where and text in one scrolling column). The title wraps within 260 px (the round edge's width at y = 40) and
  `al_layout()` starts the column under its last line: up to v1.12.1 the column sat at a fixed y = 80, and a two-line
  title ("Wreckhouse wind warning", October 4) ran into "Until …" or hid its second line behind the map.
- **Region map** (`alerts_map()`): `items/<id>?f=json` gives the shape (≈4 KB for a county, 12.5 KB for Québec's
  frost advisory region). Coordinates are pulled out with a small scanner instead of cJSON (thousands of points would
  mean thousands of small allocations). The zoom is the closest level (4–10) where the region fits around the
  location in the 300×200 crop, then one level out for context. Background: the crop's rows of the radar's cached
  basemap for that zoom (`radar_basemap_crop()`, under `cache_mux`), or OSM tiles fetched directly
  (`radar_osm_render()`) when it isn't cached. Region filled at 35 % plus outline, white dot at the location.
  Rebuilt only when the top alert changes. `ui_alert_map()` swaps the buffer under the display lock.
  - **Memory, one step at a time** (v1.14.2): the reply grows as it arrives (16 KB first, up to 160 KB; a reply that
    doesn't fit is refused, it used to be cut and drawn), the points are sized from it (its `]` count), and it is
    freed before the 117 KB picture is taken; the crop is read row by row from flash. Up to v1.14.1 every buffer was
    taken at once: 160 KB for the reply, 96 KB of points, a 434 KB copy of the whole cached map to crop 117 KB from
    it, ~830 KB (818 KB measured by the host test, 134 KB now). On top of the picture cache (PSRAM ~0.9-1 MB free
    once it is full) that took PSRAM's low point to 11-190 KB with a frost advisory at home, and twice the map failed
    for lack of memory (`region map: no memory (176 KB of PSRAM free, largest 92 KB)`).
  - **Kept across place switches:** `main.c` keeps the map (`map_key` + the point it was drawn for) when another place
    is shown, hidden (`ui_alert_map_show(false)`), and shows it again when the alerts fetched on the way back have
    the same top alert: until v1.14.1 every return to the place downloaded and drew it again (4 times in 2 minutes
    of harness drags). One map is held at most (117 KB); a place without alerts frees it only if it is its own.
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
  `place_current()` (called by `ui_place()` for the place shown) keeps a copy of its forecast (`wx`) for this
  screen and the extras page.
- `scr_hour` holds a horizontal pager (`pager.c`) with one full-screen page per forecast day (`WX_DAYS`). Moving
  between days follows the finger, snaps, and bounces at the ends (drawn by `slide.c`, the pager is frozen).
  Each page has its own vertically scrollable hour list. Drags are recognised in the touch read (`drag_read`):
  sideways → `slide_drag()` (the neighbouring day's picture follows the finger, `pager_switch()` at the end);
  vertical on the list → `slide_scroll()` (the list scrolls by moving the picture of the screen, see Moves). The page
  dots follow the day shown. A tap closes the view.
- **Rows are drawn, not created:** each list has one tall object with an `LV_EVENT_DRAW_MAIN` callback that draws
  only the rows inside the clip area (`lv_draw_label` with `text_local`, `lv_draw_rect`). The weather icons have a
  painter mode (`P_layer`) that draws the same shapes straight into a layer: since v1.11.1 once per kind, day or
  night and row background (the "Now" row's), into 40×40 pictures (`hr_icon_make()`, made by `fill_page`, at most
  28, through a hidden canvas: not during a render), which `hr_draw` copies (`lv_draw_image`); a missing one is drawn
  as shapes. 72 rows as real objects would have been several hundred small allocations in internal RAM.
- Today's page is refilled at each new hour and when new data arrives.
- Hourly values the forecast left null are "no value": the temperature is `NAN` and the chance of rain
  `WX_POP_NONE` (255); the row shows `--` and the graph leaves the point out (its scale, fill and curve). They were
  stored as 0 and shown as real: 0° and 0 %.
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
  The animations are drawn by `slide.c` (`slide_zoom()`, ~40 fps; see Moves, Radar zoom), LVGL's `lv_anim` on
  `lv_image_set_scale` only as the fallback.
  Zoom in grows the current picture 2× (300 ms, nearest-neighbour), then the sharper
  map replaces it. Zoom out first loads the wider map (usually from the flash cache), then shrinks it from 2× into
  place (`reveal_map()`), so no black border shows. The task waits for a running zoom-in animation (`wait_zoom_anim()`)
  before swapping images. Requests interrupt waits (`ulTaskNotifyTake`), and a basemap download for a zoom level the
  user has already left is cancelled.
- **Basemap:** 3×3 OSM tiles (`tile.openstreetmap.org/<z>/x/y.png`, one keep-alive connection, 3 retries each),
  decoded with `png_rows.c` (below), dimmed and desaturated (`dim_map`, 55%), then saved to that zoom level's
  512 KB slot in the 4 MB `mapcache` partition (7 slots). The header (magic `MAP7`, zoom, view origin) makes a location change
  download fresh tiles. Bump the magic to force a full re-download (useful for testing the preload). `cache_save()`
  stores only the first place's view and waits between sectors while the screen is in use (see Known issues).
  `cache_load()` reads a map back in 4 KB pieces (~70 ms): a flash read into PSRAM goes through an internal buffer as
  large as the read (up to 16 KB), and read whole it was internal RAM's low point at each return to the first place
  (v1.14.2-rc.2, docs/DIAGNOSTICS.md §5). A level with a tile that fails to decode is not saved: OSM's zoom-4 tile
  `4/5/6.png` near the first place is a 4-bit PNG that `png_rows` refuses, so that level downloads again at every boot
  and return to the first place (October 5; an espforge fix).
- **Background preload:** `radar_preload_start()` (called by `main` after Wi-Fi connects) and a change of the first
  place (the only one cached; only once `radar_preload_start()` has run, so never in the browser emulator, which loads
  the zoom shown only) run `preload_all()` in the radar task. It checks each zoom level's cache header and downloads the missing levels
  (9 tiles each; all 7 take about 45 s). Meanwhile the weather screen works normally. The radar screen shows a
  "Preparing maps" panel (level x of n, tile bar), and zoom swipes answer "Maps still downloading", because the
  preload temporarily moves the task's `zoom`. If a level gets no tiles at all (no network), the preload stops.
  Afterwards the current level is loaded from flash and the latest frame is fetched.
- **Radar frames:** GeoMet WMS `GetMap` in EPSG:3857 with the exact view bbox at 466×466, `transparent=true`,
  `time=<ISO>`. The latest time comes from `GetCapabilities` (`<Dimension name="time">start/end/PT6M`).
- **PNG decoding** (`png_rows.c`, since v1.11.0): one row at a time with the ESP32-S3 ROM's inflate (`tinfl`,
  `#include "miniz.h"`, 32 KB window), ~50 KB of working memory whatever the image size. Tiles, radar frames and
  lightning images each pass a row callback (`tile_row`, `frame_row`, `lightning_row`). LVGL's lodepng decoded whole
  images (~2–3 MB for a radar frame, in two big blocks), and those allocations failed once the drag pictures had
  fragmented PSRAM. Handles 8-bit grey, RGB, palette (+tRNS), grey+alpha and RGBA, not interlaced: what GeoMet and
  OSM send. Checked against lodepng on the same frame (same pixel and colour counts).
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

## Wi-Fi setup / captive portal (espforge's forge_net `net.c`, `web.c`; `ui.c`)

- **Boot** (`main.c`): no saved network → `portal()` (AP only, until credentials are saved and the board
  restarts). Saved network → `net_begin()` + `web_start()` right away, then `net_wait(30000)`. If that fails,
  `offline_setup()` loops: open the Wi-Fi setup screen; when it closes (a tap, or 5 idle minutes), show
  *Connecting to …* and wait 30 s for the saved network; still nothing → setup again. New credentials restart the
  board. For 15 minutes only (`AUTO_SETUP_S`, the owner's choice): after that the setup network no longer opens by
  itself and the screen says *Still trying* (a long-press opens setup). `wifi offline-boot-short` on the test console
  makes it 60 s for one boot (harness `setup_stops_opening_by_itself`).
- **Reconnects pause while a setup mode is on** (`net.c`, `setup_on()` = first-time portal, setup AP or Easy
  Connect): each disconnect schedules `esp_wifi_connect()` on an `esp_timer` (1 s for the first 8 tries, then 3 s,
  then 30 s). `ap_up()` and `net_dpp_start()` stop the timer and cancel an attempt in progress (`pause_saved()`);
  `ap_down()` and `net_dpp_stop()` call `resume_saved()`, which retries after 1 s (switching setup pages stops one
  mode and starts the other in between). **Why:** an attempt makes the radio hop channels, so phones couldn't join
  the setup network and Easy Connect failed (October 1 bug). The old rule (wait only while a phone was joined) wasn't
  enough. `BIT_FAIL` only ends `net_wait()`. SNTP starts on the first `GOT_IP`, whenever that happens.
- **Status screens** (`scr_msg`) take a long-press too: it opens the Wi-Fi setup screen (not in first-boot setup,
  which already shows it); the setup screen's own timer closes it (see below).
- On the weather screen, a long-press while offline skips the Settings screen (useless without a network) and opens
  the Wi-Fi setup screen directly.
- **Wi-Fi setup screen** (`scr_setup` in `ui.c`, `ui_wifi_setup(note)`): used by first-time setup, offline setup,
  status-screen long-press, the Settings screen's *Wi-Fi network* row and the QR overlay's long-press. Page 1 = setup AP QR (`net_setup_ap_start()`),
  page 2 = Wi-Fi Easy Connect; swipe switches. A tap closes it (except in first-time setup); offline the note says
  *Tap to try again*, because closing it lets the saved network be tried. Timer: 10 min online; 5 min offline unless a
  phone is on the setup AP (so a display whose router was rebooting gets back online by itself). `ui_wifi_setup_open()`
  tells `main.c` when it closed. The radio work of a page runs in its own task (`su_radio_task`, a queue: only the
  latest page request counts, stops are never skipped), never in a touch handler (the scan froze the screen
  1.6-2.6 s) nor under the display lock (`net_dpp_stop()` waits for Easy Connect's callbacks, which take it).
  `ui_wifi_setup_end()` goes through the task too and waits for it (main.c, "Saved network is back").
- **Easy Connect (DPP enrollee, `net.c`)**: `CONFIG_ESP_WIFI_DPP_SUPPORT=y`, `wpa_supplicant` in REQUIRES. Page 2
  disconnects the station and pauses our reconnects (`dpp_active`), turns power save off, and keeps the **setup AP up,
  moved to Easy Connect's channel** (`dpp_hold_channel()`, v1.13.0): ESP-IDF stops listening when the phone's request
  arrives and answers with a short wait on the channel; the phone confirms ~7 ms later, and with nothing holding the
  radio there that confirmation was lost (a Pixel 8 Pro: 7 failures out of 7, `no-ACK` in the phone's log). An AP
  never leaves its channel. `CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM` makes the answer ~4x faster. It scans (~2 s) and listens on **one
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
  deinitialises DPP and resumes reconnects; `net_dpp_stop()` first waits for the listen to start (≤3 s + 300 ms):
  a deinit while it was still queued in the supplicant's task asserted (espforge, a page switched back at once).
  When the scan finds nothing and the station was connected, the router's channel is used.
- **Settings overlay** (`ui.c`, `ov_state`): 0 = hidden, 1 = settings QR (`https://<ip>/#k=<key>`, see Settings /
  web). The Settings screen's *Location & more (phone)* row and the first-run hint open it; a long-press on it opens
  the Wi-Fi setup screen (a screen of its own, not an overlay state).
  Gestures are ignored while the overlay is open.
- **Access point:** `ap_up()` switches to APSTA, so the station connection stays up and the AP follows its channel.
  WPA2/WPA3 mixed (`WIFI_AUTH_WPA2_WPA3_PSK`, PMF capable) with this display's own password (`net_setup_ap_pass()`:
  8 letters and digits without look-alikes, made at the first start, NVS `setup/pass`), shown on the setup screen and
  in its QR code. Until v1.12.0 every display used the published `meteo1234`. Names and passwords go into the driver's
  fixed fields with their length (`put_field()` / `get_field()`): a 32-byte name has no NUL there, and `strlcpy` kept
  31 bytes (a legal 32-character network was saved but never joined) while Easy Connect's handler read past it into
  the password. `net_creds_valid()`: a 1–32 byte name, a password empty, 8–63 characters or 64 hex digits.
  It sets DHCP option 114 (captive-portal URI `http://192.168.4.1/`) and starts the DNS server
  (`components/dns_server`), which answers every name with the AP's IP. The DNS server is started once and **never
  stopped**: `stop_dns_server()` deletes its task without closing the socket, so port 53 stayed taken and every later
  setup network had no DNS, hence no captive portal (bind `errno 112` in the log). First-boot setup (`net_start_portal()`) uses
  the same function.
- **HTTP :80 decides by interface** (`from_setup_ap()` looks at the socket's local address):
  - On the setup AP it *is* the portal. `/` serves the page over plain HTTP, because phone sign-in browsers reject the
    self-signed certificate. `/api/*` works, and every other URL (OS connectivity checks such as `/generate_204` or
    `/hotspot-detect.html`) gets a 302 to `http://192.168.4.1/` with a small HTML body, which iOS requires.
  - On the home network, everything redirects to HTTPS: to the device's own address, never the Host header. The API
    answers only setup-network clients here (GET → 302 to the HTTPS page, POST → 403; see Settings / web).
- On the plain-HTTP page the GPS button can't work (browsers only allow geolocation on secure pages). The page shows a
  link to the HTTPS version instead, and puts the Wi-Fi card first when opened on 192.168.4.1.
- On the setup network the phone has no internet: the map and the place search can't load. `GET /api/config`
  says `"setup": true` there and the page's Places card explains it (save the Wi-Fi first, then choose places on
  the secure page; or type coordinates). A place saved there is kept (no key needed on the setup network).

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
- **When:** `main.c` `chime_new_alerts()` after each successful alert fetch for the place shown. A warning sounds once,
  then again only if it gets worse (`alerts_to_sound()`): "the same warning" is the same `alert_code` for the place
  shown (16 remembered, with the worst colour heard). Environment Canada re-issues a warning every few hours under a
  new feature id (the CAP publication id with its timestamp): keyed on that id, every re-issue sounded again (red
  ignores quiet hours) and re-downloaded the region map, which is now keyed on code + region (`alerts_map_key()`),
  and a map that failed (a 404) isn't retried until the alert changes. The first fetch for a place (start-up,
  place switch) only records what is already there. The most severe new alert decides the sound. `sound_alert()` applies the level setting (0 off, 1 red, 2
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

## Settings / web (espforge's forge_net `web.c`; `routes.c`, `config.c`)

- HTTPS server (`esp_https_server`, per-device self-signed EC P-256 cert from `tlscert.c`) on 443. The plain HTTP
  server on 80 is the captive portal on the setup network and redirects to HTTPS on the home network. The HTTPS
  server uses control port 32769 and the HTTP one the default 32768; they must not share a port.
- **Who may change things** (`guarded()` wraps every API route on both servers, one table `api[]`):
  - the Host header must be the device's own address (192.168.4.1 on the setup network): another name is a
    DNS-rebinding attempt, 421;
  - on the home network the API is HTTPS only: on port 80, GET → 302 to `https://<own ip>/`, POST → 403;
  - a change (every POST) must be `Content-Type: application/json` (415) and carry `X-Key` = the device's key (401),
    as must `GET /api/snapshot`. The key: 16 hex digits from `esp_fill_random()` at the first start, NVS
    `web/key`, compared in constant time. It travels in the settings QR code as `https://<ip>/#k=<key>` (a fragment:
    never sent to a server, not in Referer); the page stores it in `localStorage`, strips it from the address bar and
    adds the header to every `/api/` request (a `fetch` wrapper); a 401 shows how to get it. A custom header also
    forces a CORS preflight, which this server never answers: another site's page can't send one;
  - on the setup network no key is needed (its password is on the screen), but `GET /api/config` leaves out the
    coordinates and the saved network's name and `/api/snapshot` answers 403.
  The test console's `key` prints it (USB = someone at the display); the harness and `tools/snapshot.py` use it.
- API:
  - `GET /api/config`
  - `GET /api/scan`: Wi-Fi scan, returning `[{ssid, rssi, secure}]` strongest first, one entry per name, without
    Weather-Setup. Scanning blocks the server for 2–3 s, so the page scans only on the **Scan** button, or
    automatically when opened on the setup AP.
  - `POST /api/location {name, lat, lon}`
  - `POST /api/location {name, lat, lon, index}`: `index` = which place (count = add one); without it, the place
    shown. `GET /api/config` returns `places [{name, lat, lon}]`, `active`, `max_places`.
  - `POST /api/places {select: i}` shows place i on the display; `{delete: i}` removes it (not the last one).
  - `POST /api/units {temp:"c"|"f", wind:"kmh"|"mph"|"ms", clock:24|12, lang:"en"|"fr"|"iu"}` (any subset);
    `GET /api/config` returns `units` in the same form.
  - `POST /api/wifi {ssid, pass}`: 400 unless `net_creds_valid()`; restarts the device (after an update's
    confirmation if one is pending).
  - `GET|POST /api/presence`, `POST /api/calibrate {seconds}`, `GET|POST /api/sound`, `GET|POST /api/update
    {channel, action: check|install}` (`pending_verify`, `uptime_s`, `rolled_back`, `notes`).
  - A setting that NVS refused answers 500 "not saved" (`nvs_check()`); the page shows *Not saved (500)*.
  - Routes: 14 on each server (`API_N`), plus `/` (and the port-80 wildcard); `max_uri_handlers` = `API_N + 2`.
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
- Places: the first in NVS namespace `loc` (`name`, `lat`, `lon` in micro-degrees), the others in `places` as typed
  keys `pNn`, `pNa`, `pNo` (N = 1..3) with the count `n` (written last) and the place shown `act`. Until v1.12.0
  places 2–4 were raw `location_t` blobs `p1..p3`, accepted only if their size matched: the first field added to
  `location_t` would have dropped them all. The blobs are read once and rewritten as typed keys. Names are cut to 47
  bytes without splitting a UTF-8 character (`utf8.h`); the page refuses longer ones (15 syllabics). `config_local_time()` uses Open-Meteo's `utc_offset_seconds`, which
  handles any time zone and DST; before the first fetch it falls back to the `EST5EDT` TZ rule.
- **Units** (NVS namespace `units`: `temp`, `wind`, `h12`; default °C, km/h, 24 h). Data is always fetched in metric;
  every screen formats through `config.c`: `config_temp()` (rounded °C or °F), `config_fmt_wind()`,
  `config_fmt_time()` / `config_fmt_hour()` / `config_fmt_hhmm()` (the forecast's "HH:MM" strings; 12-hour in
  French is Québec style, "2 h 45 p.m." and "3 p.m.", since v1.12.0-rc.6),
  `config_miles()` (radar distances in miles when the wind is in mph). The forecast's 0.1 °C and 0.1 km/h are
  converted exactly and rounded only for display, so °F can differ by 1° from a source that rounds the unrounded
  model value, which the forecast's own rounding does anyway. The graph's shape doesn't change (linear), only its
  labels (12-hour: 12a 3a … 12p … 12a).
- A change calls `ui_units_changed()`: re-renders every place page from its kept copy (`ui_place(i, name, &pw[i])`,
  which also bumps `wx_gen` through `place_current()` so the graph canvases redraw), the alerts' "Until", and `radar_units_changed()` (clock, frame
  time, range ring in km or mi, radius). No refetch.

## TLS certificate (espforge's forge_net `tlscert.c`)

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

## Test console (espforge's forge_core `testcon.c`; `console.c`)

- Line commands on the USB serial port (USB-Serial-JTAG driver installed for input; the log keeps using the
  secondary console for output). Answers are `test:` log lines. Task `testcon`: 3 KB stack in internal RAM, core 0,
  priority 3 (a PSRAM stack crashed when a command read NVS, and heavy work such as the render bench must run in its
  own task: `diag_bench_request()`).
- Simulated finger: `touch_inject()` / `touch_inject_end()` in `touch.c` replace the controller's report, upstream
  of the wake-swallow and gesture logic. Drags drawn by `slide.c` read the same injected finger (`touch_get()`).
- `screen` and `page` take the display lock for at most 2 s and answer `error … display busy`; `where` takes no lock
  and prints the display breadcrumbs, so it answers even when the display is stuck. `fps` uses its own counters
  (`display_get_test_stats()`), so a `diag` report in the middle of a measurement doesn't reset it.
- Wi-Fi test switches (`net_test_*` in `net.c`): a fake network name in the station config, never the saved
  credentials; `offline-boot` uses an `RTC_NOINIT` flag (one boot). `web_test_windows_quiet()` answers
  `/connecttest.txt` on the setup AP until restart.
- `alert at LAT LON|off` (main.c's `alert_at`, RAM only): the first place's alerts and region map are looked up at
  that point, handled like a switch (the alert found doesn't sound, the map held is dropped). `memlow start|stop`:
  ESP-IDF's local heap minimum (`heap_caps_monitor_local_minimum_free_size_start/stop`), so a test reads the low
  points of one step; the since-boot ones come back as the lower of both.
- In every build on purpose: USB access can already reflash the board, and the harness tests what ships.
  See docs/TESTING.md §6.

## Memory budget (approximate)

| Item | Where | Size |
|---|---|---|
| LVGL draw buffers | internal DMA | 2 × 30 KB |
| Radar frame structs (46 × ~1 KB of palettes) | PSRAM (`EXT_RAM_BSS_ATTR`, since v1.12.0) | 48 KB |
| cJSON parse trees (forecast ~10 KB of JSON) | PSRAM (`cJSON_InitHooks`, since v1.12.0) | transient, ~100 KB |
| Alert region map (shape reply, points, crop) | PSRAM (main task; the crop is kept) | 16–160 KB + ~10 KB, then 117 KB (~830 KB at once until v1.14.1) |
| LVGL heap (objects, styles, glyph cache) | PSRAM (`lvgl_mem.c`, `LV_USE_CUSTOM_MALLOC`) | ~40–50 KB |
| Basemap + composed screen | PSRAM | 2 × 434 KB |
| Hourly temperature graphs (7 canvases) | PSRAM (LVGL heap) | 7 × 76 KB |
| 15 radar frames | PSRAM | 3.3 MB |
| Drag and slide pictures (`slide.c` cache) | PSRAM (LVGL heap) | up to 5 × 434 KB |
| PNG decode (`png_rows.c`) | PSRAM (transient) | ~50 KB |
| TLS (client and server) | PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`) | ~40–60 KB per session |
| A TLS client's own part: `esp_tls_t`, the HTTP client's buffers (radar 4 + 0.5 KB, alerts 2 + 0.5, air and forecast 0.5 + 1), lwIP's control block and unsent segments, queued Wi-Fi frames (1.75 KB each) | internal (`malloc` under 16 KB; PSRAM when internal is full) | ~10–15 KB per download while it runs |
| Flash reads into PSRAM (`esp_partition_read`) | internal: the flash driver's buffer, as large as the read | up to 16 KB while reading (`cache_load()`: 4 KB pieces since v1.14.2-rc.2) |

Build: `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (debug `-Og` made LVGL rendering noticeably slow),
`CONFIG_LV_DEF_REFR_PERIOD=15`, and **QIO flash** since v1.11.1 (`CONFIG_ESPTOOLPY_FLASHMODE_QIO`; DIO before). The
program runs from flash through a 16 KB instruction cache (more would cost internal RAM), and LVGL's code is large:
full-screen renders went 67 → 45 ms (weather), 51 → 35 (hourly), 56 → 44 (radar). The bootloader switches the chip to
QIO itself (ESP-IDF writes its header as "dio" even then: the flash helper's `--flash_mode dio` is right), so it takes
a bootloader built with QIO: a USB or web-flasher install. A board updated over the air keeps its old bootloader and
runs the same app at DIO speed (checked: v1.11.1-qio.1 on the IDF 5.4.2 DIO bootloader boots and passes the harness).
The flash helper writes `firmware\bootloader.bin` (copy it from `build\v55\bootloader\` after a config change;
the old one is kept as `firmware\bootloader-idf542-dio.bin`).

Measured (v1.12.0, harness `perf` and `wifi_setup`): internal RAM ~84 KB free steady, 36–40 KB min ever in a normal
boot, 38–43 KB min after the reconnect path (rc.2 had 96 / 50 / 76 KB, before the task stacks grew by ~12.7 KB); PSRAM ~450 KB min ever with the picture cache. (v1.11.1: 48 KB steady,
8–10 KB min, 4–5 KB after a reconnect: the 46 radar frame structs were in internal RAM.) App image ~2.06 MB of the
3 MB slot (~23 KB more per release lately; the two TTF fonts are 318 KB of it).

`EXT_RAM_BSS_ATTR` data and anything in PSRAM is unreachable while the flash cache is off (a flash erase or write):
only tasks may touch it, never an interrupt handler. See `docs/DIAGNOSTICS.md` for
how to measure again (`reboot.request` + `tools/diag_summary.py`) and the reference numbers.

Internal DMA-capable RAM is the scarce resource. Everything under 16 KB that goes through plain `malloc` lands
there first (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`), which is why LVGL has its own PSRAM allocator. Fonts use
`LV_FONT_KERNING_NONE`: kerning lookups were 71% of the rendering time. `CONFIG_MBEDTLS_HARDWARE_AES` was **off** from v1.0 to
v1.12.0-rc.5: the AES peripheral allocates internal DMA bounce buffers, and that failed mid-response, which truncated
the settings page. With internal RAM no longer short (LVGL's heap and the radar palettes in PSRAM) it is on again
since rc.6: HTTPS snapshots 2.2 → 1.5 s, the page whole every time, internal RAM the same (measured on the board, then
the full harness).

Internal RAM is five heaps (October 5): main DRAM (~260 KB, full all the time), a 22 KB one (full), a 32 KB region
where the network buffers go, 8 KB of RTC RAM, and the 32 KB pool `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` keeps for
allocations that ask for internal or DMA memory explicitly. A plain `malloc` that finds no internal room takes PSRAM
instead of failing; what must be internal (task stacks, FreeRTOS objects, DMA buffers, the flash driver's read
buffer) depends on the small heaps and that pool. The low-point numbers (`memlow`, "min ever", the harness's floors)
add up each heap's own low point, so they sit below any real moment (docs/DIAGNOSTICS.md §5).

## Known issues / TODO

- ~~Task watchdog warnings~~ (fixed): writing the 450 KB basemap cache in one flash erase blocked core 0 for
  seconds. `cache_save()` now erases and writes one 4 KB sector at a time with `vTaskDelay(1)` in between, and the
  palette, compose and tile-copy loops yield every 32–64 rows. Keep new long loops on core 0 yielding.
- GeoMet drops idle keep-alive connections. `GetCapabilities` retries up to 3× and frames retry once, reconnecting each time.
- The frame count is fixed at 15 and the radar layer is rain rate only (`RADAR_1KM_RRAI`). `Radar_1km_SfcPrecipType`
  would colour snow and rain separately.
- ~~Flash writes stalled drags~~ (fixed after v1.10.1-rc.2): a map saved to flash (`cache_save()`) pauses both
  cores in bursts for ~3 s, and a drag then waited ~2 s for its first frame and crawled at 3–9 fps (the harness's
  hourly swipe failed once). Two causes. (1) Every return to the first place wrote flash twice: the other place's
  download, ending just after the switch, was saved in the first place's slot (the check was "which place is shown
  now"), so the first place's map was lost and the preload downloaded and wrote it again. `cache_save()` now checks
  the view the tiles were downloaded for against the first place's. (2) Writes didn't care about the user: each
  sector now waits while `slide_screen_busy()` (a finger down in the last 0.5 s, a slide or drag running; at most
  10 s per map). Left: the boot preload still writes all 7 levels (~45 s), pausing for the user as above.
- Taps can also be missed while LVGL redraws a whole screen (65–110 ms, e.g. when new data arrives).
- ~~Lists scrolled at 17–22 fps~~ (fixed in v1.10.1-rc.4: list scrolls by moving the picture, see Moves).
- Internal RAM is tight: about 6.5 KB free while serving the page with the AP running. Watch
  `web: GET / (page), free internal …` in the log after adding features.
- Phones hammer the portal with parallel connections (including HTTPS probes that fail the handshake, which is
  harmless). `CONFIG_LWIP_MAX_SOCKETS=16` leaves room for both servers, the DNS socket and several clients.
