# Notes for AI-assisted sessions on this project

Read `README.md`, `docs/HISTORY.md` (how the project grew, how the work is done now, what is still open) and
`docs/ARCHITECTURE.md` first. **On a Mac, read `docs/MACOS.md` first**: it replaces this file's Windows setup (COM5,
PowerShell, WSL, `C:\` paths) and sets a designer's scope (her branch only: no tags, releases or `main`). This file collects the practical lessons, September 29 to October 2, 2026: what worked,
what cost time, and how to avoid repeating it.

**The loop, in short** (details in docs/HISTORY.md, "How the work is done now"): test build labelled above the
current release → flash and probe through the harness (`tools/harness/`) → prove it on the device (`pictest`,
snapshots, the harness's checks and baseline) → the user tries it → "document and commit" → Claude publishes the
release candidate and tests it with `harness.py --ota` → a stable release only with the user's OK.

## Working setup (how the first session ran)

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75 on **COM5** of a Windows PC. Project folder:
  `C:\Users\lmathieu\ESPDEV\weather_amoled`.
- **Two ways to work:**
  1. **Claude Code on Windows** (preferred): run it in this folder. It can use `idf.py` and the COM port directly.
     ESP-IDF v5.5.4 is installed at `C:\Espressif\esp-idf` (see below).
  2. **Claude desktop app (Cowork):** builds happen in a cloud container. Its shell on the PC is a Linux VM with **no
     USB access**, and it can't type into Windows terminals. Flashing then goes through the **flash helper**: the user
     starts `start_flash_helper.bat` once, Claude writes `flash.request` (containing the log seconds) and polls for
     `flash.done`, then reads `serial_log.txt`.
- **Claude Code on this PC (September 30):** ESP-IDF **v5.5.4** is installed at `C:\Espressif\esp-idf` (CI uses
  v5.5.4 too since v1.10.0; before that CI used v5.4.2 and the mismatch went unnoticed, see docs/TESTING.md). Test
  builds go to `build\v55` with their own sdkconfig; see docs/TESTING.md.
- **Releases (since October 2):** Claude tags and pushes **rc** releases itself, then watches CI (`gh`, GitHub CLI,
  signed in as TheMonkeyz; in Git Bash call `"/c/Program Files/GitHub CLI/gh.exe"`) and tests the published build on
  the display with `harness.py --ota vX.Y.Z-rc.N` (the display's own updater installs it). **Stable** releases: ask
  the user before pushing the tag. Pick the number by content: new features → minor bump (the v1.10.1-rc series
  shipped as v1.11.0).
- **Talk to the user:** post a one-line progress note before anything that takes more than a minute (build, flash,
  log window). Long silences read as "stuck". Don't take control of the user's screen; ask first, and prefer the flash
  helper. Ask the user to interact with the board (swipe, tap, open the page) *during* the log window, and say when.

## Shared with espforge

[espforge](https://github.com/TheMonkeyz/espforge) (`C:\Users\lmathieu\ESPDEV\espforge`) is this project's framework,
extracted on October 4 (its `docs/LESSONS.md` L1-L184 generalize the bugs below, plus newer ones). The two drifted
within hours (Easy Connect was fixed there while this display still failed with the user's phone), so since v1.14.0
the display **takes its infrastructure from espforge**: forge_core (diag, test console, i18n core, NVS helpers,
version, png_rows, textfit, http_once, utf8), forge_net (Wi-Fi, setup network, Easy Connect, web server, svc,
certificate), forge_ota (updates) and dns_server, **at a release tag** (the user's choice, October 4).

- **Where:** `main/idf_component.yml` pins the four at the same espforge tag; ESP-IDF's component manager fetches them
  into `managed_components/` (git-ignored, like `dependencies.lock`). If `idf.py` stops on "'git init --bare' failed
  ... unable to get current working directory", set `IDF_COMPONENT_CACHE_PATH` to a folder that exists (this PC:
  `C:\Users\lmathieu\.espressif\cm_cache`). Builds outside idf.py (host tests, the emulator, CI) use
  managed_components, or `python tools/fetch_forge.py` (clones the pinned tag into `.espforge/`).
- **What stays here:** display, touch, `slide.c` (larger than espforge's trimmed fork: port ideas, not the file), pager,
  lvgl_mem, imu, presence, sound, ui, radar, weather, alerts, config, and the app's glue: `routes.c` (the settings
  page's app routes), `console.c` (the display's console commands, "where", "diag: display"), `services.c` (the five
  outside services, their probe URLs, the reasons' texts), `i18n.c` (the texts, Inuktitut's descriptor). Kconfig
  `CONFIG_FORGE_*` in `sdkconfig.defaults` keep the display's names (Weather-Setup, the certificate, the User-Agent,
  the update site).
- **A fix to shared code** is made in espforge: test it with this app first (`python tools/forge_local.py`: a build
  against the espforge checkout, nothing committed), release an espforge rc, then bump the four tags here. Before
  debugging Wi-Fi setup, OTA or the tools, read espforge's LESSONS for that topic.
- **After bumping the tags, delete `dependencies.lock`** (git-ignored) before building: on October 6 the component
  manager said it re-solved, yet kept the locked commit for the new tag (v0.2.1 built v0.2.0's `forge_core`, without
  its PNG fix). Check the build log's `NOTICE: [n/7] forge_core (<commit>)`. CI starts clean.
- **Still twins** (same code in both, nothing shares them yet): `lvgl_mem.c`, `pager.c`, `imu.c`, `display.c`,
  `touch.c`, the harness's `board.py` / `harness.py`, the flash helper. A fix in one gets ported or a task chip.

## Cloud build recipe (when no local IDF)

The network allowlist blocked PyPI, dl.espressif.com, Docker Hub and the component registry; only GitHub worked:

```bash
git clone --depth 1 --branch v5.5.4 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git
./install.sh esp32s3            # toolchains download from GitHub; the python-env step fails, which is OK
python3 -m venv --system-site-packages idfenv && . idfenv/bin/activate
pip install --no-deps git+https://github.com/pyserial/pyserial.git@v3.5 \
  git+https://github.com/tomerfiliba-org/reedsolomon.git@v1.7.0 \
  git+https://github.com/python-intelhex/intelhex.git@2.3.0 \
  git+https://github.com/espressif/esptool.git@v4.8.1 \
  git+https://github.com/espressif/esp-idf-kconfig.git@v2.5.0
export IDF_PATH=... IDF_COMPONENT_MANAGER=0 PATH=<xtensa-esp-elf/bin>:$PATH
cmake -S . -B build -G Ninja -DIDF_TARGET=esp32s3 -DPYTHON=$(which python) -DPYTHON_DEPS_CHECKED=1
ninja -C build
```

With the component manager off, LVGL (a v9.2.2 clone) is vendored at `components/lvgl`, and `esp_codec_dev` (v1.5.11, as pinned in `main/idf_component.yml`; sparse-cloned
from `espressif/esp-adf`) at `components/esp_codec_dev`. Its Kconfig warns that `ESP_IDF_VERSION` isn't set; that's
harmless and keeps `CODEC_I2C_BACKWARD_COMPATIBLE` off, which we need because we use the new I2C driver. Keep LVGL's `examples/` and
`demos/` directories, because the CMake file adds them as include paths. Neither vendored directory is in this repo; the
Windows build gets both from `main/idf_component.yml`. `components/dns_server` *is* in the repo.

## Verification tricks that paid off

- **Host LVGL simulator:** compile LVGL plus `ui.c`/`radar.c` pieces with gcc against a small `lv_conf.h`, render
  into a framebuffer, write PNGs. It caught layout clipping on the round screen and proved the clock timer logic.
  Stub the ESP headers (`esp_log.h`, `net.h`, `config` functions). Use `-Wl,--wrap=time` to fake the clock.
- **Screenshots: `python tools/snapshot.py <ip> <screen>`.** The firmware renders the screen off-display
  (`GET /api/snapshot`) and the tool saves a PNG with the area outside the round panel tinted red. Claude Code on the
  PC can reach the board over HTTPS (the Cowork cloud and Linux shell can't). Use it to check every layout change
  before asking the user for a photo. The full routine is in **docs/TESTING.md**. (The first session dumped a
  1/3-scale framebuffer as base64 between `IMGDUMP` and `IMGEND` in the serial log; it found the PNG decoding bug and
  the CARTO "API KEY REQUIRED" tiles. That code is gone; the endpoint replaces it.)
- **Harness: `python tools/harness/harness.py`** (docs/TESTING.md §6; from a git worktree, run the main
  checkout's harness with `--flash <absolute path>`, and use its `Board` class for ad-hoc scripts) runs everything without the user: screens by
  simulated touch, the settings page, performance against `tools/harness/baseline.json`, and the offline Wi-Fi
  setup paths with the PC's Wi-Fi card acting as a phone. Run it before calling a change done, and add a test when
  a bug is fixed. Only Easy Connect's final phone scan needs a person (`--phone`). Testing a release: `--expect
  vX.Y.Z-rc.N`, and read the "Testing vX" line: a restart within 60 s of an update rolls it back (it happened).
- **Settings page tests:** `cd tools/webtest && npm test` (Playwright + mock display, see docs/TESTING.md §5). Run
  them and look at `shots/` before building firmware with a page change. In the first session a headless test
  proved the page's JS was fine, which pointed at the device.
- **Translations: check fit with snapshots, not by guessing.** The Inuktitut draft (docs/translations/) looked fine
  in a width estimate but six labels collided or wrapped on the board. Set the language with `POST /api/units`,
  snapshot every screen (including `settings1..3`, `phone`, `setup0/1`), shorten in the TSV, regenerate, reflash.
  Unit letters (`s`, `min`, `h`) must be in backticks in `iu.tsv`, or the converter turns them into syllabics ("56 ᐢ").
- **Always check file transfers:** `md5sum` the firmware on the PC against the build output. **Twice**, pushing a
  rebuilt file from the *same* staging path delivered the *previous* version. Use a fresh staging directory for every
  push and compare checksums before writing `flash.request`.
- **Flash helper feedback:** read `flash.status`, then `flash.done` (it includes error, warning and reset counts),
  then grep `serial_log.txt`. A plain-text `flash_log.txt` shows exactly what esptool wrote.
- **Don't wait out a long log window:** ask for a generous one (e.g. 330 s), then `echo > stop.request` as soon as
  the line you need has appeared (the user can press Q or Esc in the helper window). The log is saved within ~1 s.

## Bugs hit, and their fixes (don't repeat these)

1. **LVGL's bundled lodepng returns an `lv_draw_buf_t*`, not raw pixels.** Use `db->data` and `db->header.stride`;
   bytes are R,G,B,A. Free it with `lv_draw_buf_destroy`.
2. **CARTO basemap tiles now need an API key.** They return grey "API KEY REQUIRED" images, so we switched to OSM tiles
   with a proper User-Agent, attribution and a flash cache.
3. **Stack overflow in the radar task:** arrays of `frame_t` (about 1 KB each) were on the stack. Keep big structs
   `static`.
4. **HTTPS page arrived truncated, so the buttons "did nothing":** hardware AES failed to allocate internal DMA
   memory. We disabled `CONFIG_MBEDTLS_HARDWARE_AES`, reduced the LVGL buffers to 32 lines, and send the page in
   1 KB chunks. Hardware AES is on again since v1.12.0-rc.6 (internal RAM is no longer short; measured: snapshots
   30 % faster, the page whole): if pages ever arrive truncated again, look at internal DMA memory first.
5. **Two httpd servers:** the HTTPS default control port is already 32769, so don't also set the HTTP server to 32769.
6. **CST9217 touch:** after each read you must write `D0 00 AB` (ack), or the controller stops reporting. NACKs while
   idle are normal, so we silence the `i2c.master` logs. Coordinates are mirrored in both X and Y.
7. **Long-press never fired:** decorative `lv_obj`s (icon blobs, boxes) are clickable by default and swallow presses.
   Use `passthrough()` and non-clickable blobs. The overlay then closed on the same finger release; now it ignores
   presses within 800 ms and calls `lv_indev_wait_release()`.
8. **"Clock not updating"** on the radar screen was really the radar *data* timestamp in the title. The pill now shows
   the live clock, with the radar time in a sub-label.
9. **Reading the BOOT button:** holding BOOT while pressing RESET enters download mode. The firmware checks BOOT
   about 1 s *after* boot.
10. **Task watchdog (IDLE0) warnings:** caused by one big `esp_partition_erase_range` of the map cache. Erase and
    write sector by sector with `vTaskDelay(1)` in between.
11. **Zoom-out showed a small square:** shrinking the old picture exposes black borders. Load the wider map first,
    then animate it from 2× down to 1×.
12. **Radar ignored swipes for 20 s** after a dropped GeoMet connection (a fixed `vTaskDelay`). Waits in the radar task
    must use `ulTaskNotifyTake` so user requests wake it; long downloads check `zoom_target`/`relocate_pending` and bail.
13. **Choppy animations:** the project was compiled with `-Og`. Use `CONFIG_COMPILER_OPTIMIZATION_PERF`; keep
    `lv_image_set_antialias(img, false)` for full-screen scaling.
14. **Captive portal and HTTPS don't mix:** sign-in browsers won't accept a self-signed certificate. Serve the portal
    over HTTP on the AP interface (decide with `getsockname`) and keep HTTPS for the home-network page (GPS).
15. **Git on the PC folder from the Linux VM** needs delete permission (index.lock, temporary objects). Set
    `core.fileMode false` and `core.autocrlf false`.
16. **LVGL events bubble from children:** a page's scrolling list sends `LV_EVENT_SCROLL` up through the pager. A
    handler that reads state from `lv_event_get_target()` crashed (null user data). Use
    `lv_event_get_current_target()` and ignore events whose target is someone else.
17. **Sending non-ASCII through curl from Git Bash** mangles it (Montréal stored as Latin-1). Put the JSON in a file
    (`printf` with `\u00e9`) and use `--data-binary @file`; a phone sends proper UTF-8.
18. **A full-size `lv_arc` is a full-size touch target:** on the Settings screen it caught every tap and drag (rows,
    Done, scrolling did nothing; only "brightness" appeared in the log). Draw arcs non-clickable and give them a
    separate touch zone. Snapshots can't show this: ask the user to tap through a new screen during a log window.
19. **Sliders must act while dragging:** the user expects the screen to change under the finger
    (`presence_preview_brightness()`); save on release.
20. **Wi-Fi setup while offline (October 1):** three separate causes, each found from the log, not by guessing.
    (a) Reconnect attempts to the saved network made the radio hop channels, so phones dropped off the setup AP and
    Easy Connect: now no attempts while any setup mode is on. (b) `stop_dns_server()` leaks its socket: the DNS
    server now runs forever, else the next setup AP has no captive portal. (c) Easy Connect timed out
    (`ESP_ERR_DPP_AUTH_TIMEOUT`) unless the display listened on the phone's own channel: scan and listen on the saved
    (or strongest) network's channel. To see DPP steps, set `CONFIG_ESP_WIFI_DEBUG_PRINT=y` and
    `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` in `build\v55\sdkconfig` only (lines `wpa: DPP: …`; the old
    `CONFIG_WPA_DEBUG_PRINT` does nothing on IDF 5.5). Reproduce with `wifi offline-boot` (harness `wifi_setup`).
    (d) v1.10.0's harness run caught the channel pick guessing: the broadcast scan missed the router (busy channel),
    so the display scans for the saved network by name first. A flaky check needs several runs before calling it
    fixed (4/4 after the fix).
    (e) **The real Easy Connect failure (v1.13.0, found in espforge, October 4):** with the station off every network
    nothing held the radio on the channel; a Pixel 8 Pro's confirmation arrived 7 ms after the display's answer and
    was never received (`no-ACK` in the phone's log, Auth Confirm timeout here), 7 times out of 7. That is also why it
    "only worked online" (c). The setup network now stays up on Easy Connect's channel (`dpp_hold_channel`). Found
    with the phone's own log (docs/TESTING.md, "Easy Connect: the phone's side"), after three wrong theories.
    (f) The radio work of a setup page (a 1.6-2.6 s scan) ran in the swipe's handler and froze the screen; Easy
    Connect stopped before its listen started crashed (assert). A task does it now (`su_radio_task`), and
    `net_dpp_stop()` waits for the listen: never call it under the display lock (`su_dpp_uri` takes it).
    (g) **October 5: the Pixel on 5 GHz failed again, every time, with every firmware** (the Oct 4 one too); on
    2.4 GHz it worked. The display answered in 235 ms and its answer was ACKed; the phone's kernel log (`adb
    bugreport`, with the user's OK, deleted after) showed its Wi-Fi driver receiving the answer and not passing it on.
    Not fixable here (a resend patch and faster crypto changed nothing): the failure text now says to swipe right and
    join the setup network, which works on any band. espforge LESSONS L188: A/B the old firmware first (L172), then
    read the phone's kernel log; a harness failure the same evening was the forecast service (HTTP 503).
21. **Frame rate (v1.11.0, October 2):** LVGL 9.2 can't redraw a full screen in less than ~65–85 ms, and profiling
    found no single hot spot (docs/TESTING.md §8). Moves between screens, places and days are now pictures sent
    straight to the panel by `slide.c` (ARCHITECTURE "Moves"). What it took, in order of cost:
    (a) **Hangs:** esp_lcd isn't thread-safe. Its transfer-done interrupt ran on the other core and raced with the
    raw frames: the interrupt now runs on core 1, and no esp_lcd call is made while a band is in flight. Found
    with breadcrumbs and the console's `where` (no lock), not by guessing; keep both when touching `display.c`.
    (b) **Drawing outside LVGL hides the touch from LVGL.** After a drag, reset its input (`touch_resync`:
    `touch_forget()`, `wait_until_release`, `lv_indev_reset`). Otherwise the next swipe is ignored, or `touch.c`'s
    NACK guard replays the last point as a stray tap (the hourly view opened after place drags).
    (c) **The CST9217 NACKs instead of reporting "up".** A loop waiting for the finger to lift must count read errors
    as a release (5 in a row), or it never ends. And it lies the other way under a fast finger: see 23.
    (d) **Anything that blocks LVGL for more than ~50 ms loses quick flicks:** cache pictures are rendered in
    64-row strips, the neighbour is rendered before the first frame, and the automatic render bench is off.
    (e) **Big PSRAM users fragment PSRAM for the others:** with the 2.2 MB picture cache, lodepng's 2–3 MB radar
    decodes failed. The radar now decodes row by row (`png_rows.c`, ROM inflate, ~50 KB). After a change that holds
    PSRAM, check the `radar: Frame` lines and the harness's `psram_min_kb`.
    (f) **Content changed while its screen isn't shown** must call `slide_cache_dirty()`, or a drag shows the old
    picture for a moment. The reverse costs as much: **a redraw that changes nothing** (same label text, a flag already
    set, a style set again, a theme transition, a layout left pending) also makes the picture out of date, and the
    next drag waits 0.2–0.5 s. The user felt it at once on place drags after rc.2. Compare before setting, use
    `set_hidden()`, and find the culprits with a throwaway build that logs each invalidation with a backtrace
    (ARCHITECTURE "Moves", Cache).
    (g) **Flash writes stall the whole chip** (PSRAM and code caches are off during an erase): the radar's map save
    made drags crawl. Background flash writes wait for `slide_screen_busy()`. And check what a save stores: a
    download for one place finishing after a switch was saved as the other place's map.
    (h) A drag must decide its axis like LVGL (larger axis after 10 px since v1.12.1, see 23): a 2:1 rule missed curved swipes on the
    round screen. Check drags with the harness (`navigation`, `perf`), then ask the user to try them: the
    harness's straight synthetic drags passed while real swipes were still missed.
    (i) **List scrolls** (v1.10.1-rc.4) move the picture of the screen shown and render only the new rows, so the
    picture must equal the screen: the display's flush hook keeps it so, `pictest` and the `picture` snapshot check
    it. `lv_obj_scroll_to_y()` silently stops at the ends: past them the list didn't move while the picture did (the
    user saw the hourly graph smeared). Use `lv_obj_scroll_by_raw()` (see (k)). Recognise drags in the touch read, before LVGL
    handles it, or LVGL's own scroll starts first on a flick.
    (j) **Mark only what changed, where it changed:** a clock marks its rows (`slide_cache_dirty_rows`), an update
    check only the screens that show the update. "Everything" each minute and at each update check made the next
    drag render a whole page (~0.13–0.2 s). Probe drags right after the event (`ui: clock`, a forced check through
    `POST /api/update {"action":"check"}`) and read `pictures rendered` in the drag line.
    (k) **Measure the parts before optimising a part:** the hourly list's per-icon pictures saved nothing visible;
    timing `render_rows()`'s layout and draw showed 4.4 ms of 7.7 in LVGL's layout, which `lv_obj_scroll_by()`'s
    bubbling events re-triggered every frame (`lv_obj_scroll_by_raw()`). And a static array in a `.c` file is internal
    RAM: the zoom's 3.8 KB of tables dropped its low point to 3 KB (the harness caught it): `EXT_RAM_BSS_ATTR`.
    (l) **QIO flash** made all LVGL rendering ~30 % faster (the code runs from flash through a 16 KB cache). ESP-IDF
    writes a QIO bootloader's header as "dio" and the bootloader switches itself: what matters is the bootloader
    binary, which only a USB / web-flasher install writes.
22. **Fixes from the October 2-3 evaluation (v1.12.0, docs/FIX-PLAN-2026-10-02.md):**
    (a) A JavaScript parameter named `t` hid the page's translation function `t()`: the Install button never showed
    (shipped in two releases; nothing tested the "update offered" state). The mock display now scripts every update
    state (`POST /__update`); give the mock every state the page can be in.
    (b) Every loop that reads the touch chip directly goes through `slide.c`'s `finger()` (5 failed reads = up) and
    has a cap; one loop that waited for a clean "up" could hold the display lock forever.
    (c) **Any** esp_lcd call from outside LVGL waits for LVGL's last band (`lvgl_inflight`), not only raw frames:
    `display_brightness()` from the presence task (core 0) didn't.
    (d) `esp_http_client_init()` can return NULL: use `http_once()`. Every array a parser indexes must be checked
    (a missing `daily.time` meant a reboot loop the rollback couldn't catch: the image was already confirmed).
    (e) Cap after sorting, never before (a red warning listed fifth was dropped); "the same alert" is its
    `alert_code`, not the feature id, which changes at every Environment Canada re-issue.
    (f) A release gate must fail on what it didn't measure: MISSING and NEW metrics fail like regressions unless the
    test says `ctx.skip()`; tests keep their own log positions (`log.mark()` is the harness's crash check).
    (g) Host tests (`tests/host`, gcc in WSL) reproduce parser bugs off the board; check the test fails on the old code.
    (h) **Build exactly what you commit.** Group A was split out of a tree that already held the next group's work by
    a script; it dropped an `#include` and v1.12.0-rc.1's CI build failed (no release). To set later work aside, use
    `git stash push -u`, build the tree as it will be committed, commit, then `git stash pop`. And run the host
    tests (`tests/host`, `make` in WSL) before tagging when a C file they compile changed: v1.12.1-rc.2's release
    failed on their HTTP client stand-in (`tests/host/shim/`), which lacked an event name the firmware started using.
    (i) **Changes need the display's key** (v1.12.0-rc.3, ARCHITECTURE "Settings / web"): every POST and
    `/api/snapshot` carries `X-Key`. Get it with `echo key > serial.send` (`test: key …`); the harness and
    `tools/snapshot.py` do it themselves. A cached "no key" from the firmware before a flash made the harness skip
    its checks once: anything cached about the board is reset after a flash or an install.
    (j) The setup network's password is per display (`wifi status` shows `ap_pass`); `meteo1234` is only older firmware.
    (k) A rule with a time window needs a test that crosses the window: the 15-minute setup limit passed review and
    failed on the board (the window ended while setup was open, and nothing closed it). `wifi offline-boot-short`.
    (l) An empty translation ("") is not "missing": it showed as nothing. `tr()` now falls back on empty too, and
    `tests/host/test_i18n.c` checks every text in every language (and the same printf conversions as English).
    New French text: snapshot it (`hint next-boot` + restart for the first-run hint); long lines wrap badly on the
    round screen, so give them explicit `\n` breaks.
23. **Touches lost on the hourly view (v1.12.1, October 3-4):** found from the user's reports, each cause from the log
    of a try with their finger (the harness's straight synthetic touches passed every time). Read
    `docs/ARCHITECTURE.md` "List scrolls" before touching `finger()`, `scroll_run()` or `drag_read()`.
    (a) **LVGL took quick flicks:** its scroll starts after 10 px, `drag_read` decided at 16: a read landing between
    went to LVGL, which scrolled the list at 17-28 fps and read the touch only between its 35-60 ms frames. Ours
    decides at 10 px now and LVGL's limit is 20. Found by logging every touch read with its time (reads 60 ms apart).
    (b) **Don't poll the CST9217 every millisecond:** read that fast (each read is acknowledged) it answered "not in
    contact" for long stretches with the finger on it, and every fix built on those false "ups" (a 60 ms hold, then
    waiting for the hold to settle) only moved the problem. `touch_get()` reads it at most every 10 ms (LVGL: 15);
    `touch_fresh()` says whether a reading is new. Take speed samples from fresh readings only, or a repeated point
    measures a flick as 0 px/ms (a day swipe snapped back).
    (c) A touch during a list's coast or spring-back is decided like a new one (10 px): sideways goes to the day drag
    (`slide_scroll_on_sideways`), vertical follows from where it went down. `touch_forget()` bumps a counter so
    `drag_read` takes a finger already down after a drag or scroll for a new press (it was ignored).
    (d) The scroll log line counts the finger's lifts, silences, re-touches and bridged brief "ups": read it first.
24. **A label that can wrap must not have a fixed-position neighbour below it** (v1.12.2, October 4): the alert title
    wrapped ("Wreckhouse wind warning") while the column under it stayed at y = 80, so it ran into "Until …" and hid
    behind the region map. Wrap to the round edge's width at the label's own height (260 px at y = 40) and lay what
    follows from the label's real height (`al_layout()`). Environment Canada's and French texts are longer than you
    expect; the harness's `alert_layout` checks it with sample names (console `alert sample`). Its first run caught
    a measuring slip: `lv_obj_get_y()` is the last layout's position until the next one (it said 80 after
    `lv_obj_set_y(111)`); compare with `lv_obj_get_style_y()`, or `lv_obj_update_layout()` before reading.
25. **A strip rendered alone misses what reaches into it from outside (v1.14.1, October 5):** LVGL skips a label
    whose box misses the clip, but Inuktitut's syllabics (fallback font, 5/4 larger) reach past their box: after a
    Settings scroll in Inuktitut, `pictest` found one row off (faint glyph tips missing). `render_rows()` draws an
    8-row margin around each strip (scroll strip, cache strips, pictest bands). Check new layouts with pictest in
    every language (harness `scroll_other_languages`), not only English.
26. **An alert at home failed the gates that passed that morning (v1.14.2, October 5):** with a frost advisory, PSRAM's
    low point fell to 11-190 KB (floor 300; twice the map itself failed for lack of memory) and the drag back to the
    first place took 124-155 ms. Two causes, both read in the log. (a) `alerts_map()` took all its buffers at once
    (the reply's 160 KB, 96 KB of points, a 434 KB copy of the whole cached map to crop 117 KB: ~830 KB), and every
    return to the place downloaded and drew it again. Now one step at a time (134 KB, host-tested with ASan's byte
    count) and the map is kept across switches. Size a buffer from what it holds, free it before the next step. (b)
    `ui_alerts()` marked every picture out of date (21(f) the other way), and the "none" sent at each switch made both
    places' pictures stale; it marks the pill's rows and the alert screen now. Test with a real alert any day: console
    `alert at LAT LON` (harness `perf.alert_active`); `memlow start|stop` gives one step's low points. Internal RAM
    fell too (23 KB, floor 25): its low point is a place switch, ~27 KB with or without an alert (the radar's
    downloads and the alerts and air fetches at once; `memlow`, 2 rounds each), right at the floors; before v1.14.2
    the map's download at each return added one more TLS connection to that moment. (Mostly a flash read: see 27.)
27. **Internal RAM's "27 KB" at a place switch (v1.14.2-rc.2, October 5)** was mostly one flash read, and partly a
    sum. Found with a throwaway probe (branch `probe/switch-ram`, local: a 100 ms timeline of the exact low point
    tagged with the downloads running, a diff of the internal heap's used blocks at the low point, their callers from
    `CONFIG_HEAP_USE_HOOKS`, per-heap low points). (a) `esp_partition_read()` into PSRAM borrows an internal buffer as
    large as the read, up to 16 KB, for the whole read: `cache_load()` read the 434 KB map in one call at each return
    to the first place, while air quality loaded. It reads 4 KB pieces now (+20 ms). Writes don't (a 32-byte stack
    buffer). (b) Each TLS download holds ~10-15 KB internal while it runs (`esp_tls_t` 1.75 KB, the HTTP client's
    buffers, queued Wi-Fi frames of 1.75 KB each); air quality's 4 KB reply buffer was `calloc`'d (under 16 KB =
    internal): PSRAM now. Together 23-28 -> 42-43 KB. (c) `memlow` and the "min ever" numbers **add each internal
    heap's own low point** (5 heaps, reached at different times): the real moment was ~15 KB higher. Main DRAM is full
    all the time, and a plain `malloc` falls back to PSRAM when internal RAM is full (`failed_allocs` stays 0); what
    can fail is what must be internal (task stacks, FreeRTOS objects, DMA, flash reads), served from the 32 KB
    `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` pool. (d) A by-catch: OSM's zoom-4 tile `4/5/6.png` near home is a 4-bit
    PNG that espforge's `png_rows` refused, so that level was never cached and downloaded again at every boot and
    return home (espforge v0.2.1-rc.1 decodes 1/2/4-bit PNGs, its LESSONS L190). Harness:
    `internal_min_kb.place_switch` (`perf.swipes`, floor 36: 31 on rc.1, 39-47 since).
28. **Lesson 25's margin wasn't the whole fix (v1.14.2-rc.2, October 6):** the harness's Inuktitut `pictest` after a
    Settings scroll failed one time in four (rc.1 too), always 1-2 rows at the same two places in the list. (a) A
    list scroll moves the picture and renders only the new rows; the rows just before them were drawn while the next
    label was still past the list's edge (LVGL clips children to the list, and `lv_draw_label` returns unless the clip
    meets the label's own box), so without its syllabics' tops, then moved. A **slow drag** (2.5 s: each label comes in
    a row at a time) made it 6/6, and the snapshot diff showed which glyph (the dot over ᓈ). `scroll_move_fill` now
    takes the rows next to the new ones from the strip too. (b) How far: computed from the TTFs as TinyTTF places glyphs
    (rise = `ceil(yMax x fallback scale) + 1 - Montserrat's ascent`): syllabics 9 rows up at 28 px (7 at 20), 3 down;
    Montserrat alone 4 / 1. Lists use the syllabics' reach in Inuktitut only (Settings scrolls got faster in English:
    render 3.7 -> 3.2 ms a frame), whole screens always. (c) `render_rows` wrote its outermost margin rows, drawn
    without the labels past them: it puts them back now. (d) Settings re-set its labels every second (`cfg_tick`):
    `set_text()` compares first, as 21(f) says; the redraw was the 190-200 ms `swipe_gap_max_ms.scroll_settings`.
    An intermittent one-row miss: make it deterministic first (slowly, every time), and A/B the old firmware (it had
    it too). The first guess (the per-second redraw erasing tips) was wrong: the misses were there from the first
    `pictest` and didn't change across ticks.

- Internal RAM ran out silently (10 KB free, 0 KB min ever) because LVGL's small allocations went to internal RAM
  first. Fixed with `lvgl_mem.c` (LVGL heap in PSRAM). Font kerning cost 71% of render time; fonts now use
  `LV_FONT_KERNING_NONE`. Before optimising anything, run the diagnostics (docs/DIAGNOSTICS.md) and compare with
  the reference numbers there.
- A committed firmware file can arrive stale even in a new staging dir: give the staged .bin a unique **name**
  (`weather_amoled_vNN.bin`) and refuse to flash unless the md5 on the PC matches.
- Changing sdkconfig makes CMake re-run; the cloud build then needs `IDF_COMPONENT_MANAGER=0` in the environment
  or it fails looking for `idf_component_manager`.

- `.github/` is a protected path for the remote file tools: Claude can't write workflow files into the PC folder.
  Hand the file to the user to save there (or edit it on GitHub).
- Releases: `vX.Y.Z-rc.N` tags feed the flasher's Beta channel, `vX.Y.Z` tags the Stable one; pushes to main only
  build. The Pages site is rebuilt from release assets (`flash-parts.json`), never from a branch.
- Before a release is tagged, `CHANGELOG.md` needs its `## vX.Y.Z - YYYY-MM-DD` section (user-facing wording): it
  becomes the "What's new" list on the display's update screen and the settings page.
- CI (`.github/workflows/firmware.yml`) builds with the component manager, unlike the cloud recipe above
  (vendored components). If CI fails but the cloud build works, suspect `main/idf_component.yml` versions.
- The app version comes from `version.txt` (CI) or `git describe`; a build outside a git checkout shows `1`.
  `version.txt` is read at CMake configure time: touch `CMakeLists.txt` after changing it.

- The HTTPS certificate is per device (`tlscert.c`, NVS namespace `tls`). After an erase the phone shows the
  certificate warning again; that's expected.

- Testing offline behaviour without touching the router: a throw-away build that calls `net_begin()` with a bogus
  SSID and restores the real one with `esp_wifi_set_config()` after N seconds (never commit it). Verified: long-press
  on *Connecting…* → AP + captive portal; auto setup QR after ~30 s; reconnect → AP stops.

- DPP (Easy Connect): `esp_supp_dpp_bootstrap_gen()` only queues work; start listening from the URI_READY callback.
  Verified with an Android phone: scan from Wi-Fi settings → credentials saved → restart → connected. Again on
  v1.10.1-rc.4 with the harness (`wifi_setup --phone`) and the new wording (scan with the camera or any QR app):
  QR code ready on the router's channel, the phone's network received 23 s later (the scan included), restart,
  connected. The harness prints `>>> ASK THE USER` when it's time to scan: relay it to the user at once.

- OTA layout since v1.3.0 (two app slots + otadata at 0x610000). USB flashes must include `ota_data_initial.bin`
  (the helper does when `firmware/ota_data_initial.bin` exists), otherwise a board that last updated over Wi-Fi keeps
  booting `ota_1`. The flash helper only picks up script changes after a restart.
- Test OTA end to end with the real site: flash a build labelled `vX.Y.Z-rc.0`, tag `vX.Y.Z-rc.1`, set the channel to
  Beta. A test build must be labelled above the current stable release or it will offer that release.
- Before an on-device OTA test, check the `ota: Running vX from ota_N` line in a fresh log. On September 30 the board
  was found on an older rc in `ota_1` instead of the test build that had been flashed, which invalidated a test.

## User preferences learned

- Swipe **down** = zoom in, **up** = zoom out (the opposite felt wrong).
- Prefers doubling zoom steps (sharp map) over exact 50 km steps.
- Wants short progress notes, no screen takeover, and changes verified on the device (log + snapshot) before
  being called done.
- Screen-to-screen moves should feel like the hourly view: follow the finger, snap, bounce at the ends, no
  wrap-around. Since v1.11.0 screens, places and days all go through `slide.c` (~60 fps). The user asked for 60 fps,
  noticed at once when a faster animation lost the finger-following and the bounce, and noticed a 0.1 s delay
  before a drag started. Frame rate alone isn't the goal: it has to follow the finger. October 4, v1.13.1-rc.1 (the
  page keeps the finger's speed while a release is confirmed, instead of stopping ~50 ms then snapping): "felt
  smooth like never before" (44 swipes: 57-71 fps, gaps 16-23 ms). The next thing to feel: 13 of those 44 drags
  rendered a picture first (78-116 ms before the first frame instead of ~17).
- **French = Canadian French (Québec), standard written:** "endroit(s)" for places (not "lieux"), "tamiser" for dim
  (not "atténuer"), "balayez le code QR", "appuyez longuement", **1er** for the first of the month. No joual, slang
  or anglicisms. Check grammar agreement when a noun changes (un endroit → "Nouvel endroit", "cet endroit").
- The font has no symbols or emoji: small icons are drawn shapes (see `drop_draw` / `wind_draw` in `ui.c`).
- New display text goes in `main/i18n_strings.h` (both languages) and page text in `I18N` in `index.html`; never a
  bare string literal on screen. Check French in snapshots: it is longer and wraps (widen the label, shorten, or
  use the smaller font for that value).
- Sliders follow the finger's horizontal movement within a band, even on the round screen (not drag-along-an-arc).
- Rows that open something need an obvious label and a `>` (a bare "QR" wasn't clear); a screen opened from
  Settings closes back to Settings.

## Useful facts

- **Speaker:** shares I2S0 with the microphones (presence.c opens it both ways). Render a whole sound into PSRAM
  before playing it (generating it on the fly crackled). Hear the levels from the PC with
  `POST /api/sound {"test":1|2|3}` (yellow / orange / red); ask the user to listen, there is no other check. The
  user wanted warning beeps, not a chime; keep them unlike Canada's official Alert Ready signal.
- **Testing the "new alert" path without a real alert** (done October 1 with `v1.8.0-alerttest.0`, never
  committed): in `main.c`, right after `alerts_fetch()` succeeds, append fake orange alerts (`id` "TEST-ORANGE-1"
  at 30 s of uptime, "-2" at 60 s) and shorten the loop's wait so it fetches at 31 s and 61 s. Between them, turn
  quiet hours off with `POST /api/sound {"quiet_from":"00:00","quiet_to":"00:00"}`. Expected log: at 30 s
  `sound: new alert (o): quiet hours` (if inside quiet hours), at 60 s only alert 2 sounds (`alert sound, level
  2`); the fake ids' region maps give HTTP 404 (expected). Then restore quiet hours, `git checkout main/main.c`, and
  flash a clean build.
- **The alert screen with a real alert** (October 4): no fakes needed. Find a place under a long-named alert with
  the Environment Canada API, open the emulator there (`?place=`), or, since v1.14.2, send the board's console
  `alert at LAT LON` (the first place's alerts and map from that point, RAM only, silent; `alert at off`): real texts
  in both languages and the real map (docs/TESTING.md §4). The harness's `perf.alert_active` does it every run.
- **Lightning on the radar** (October 1): GeoMet `Lightning_2.5km_Density`, see ARCHITECTURE "Radar". There was no
  lightning in Canada while it was built: test with fake marks at fixed **lat/lon** in a throwaway build (fixed
  screen positions don't follow zooms, which looked like a bug). Check `diag: heap` against a baseline build after
  touching `frame_t`: it is copied ~46 times in internal RAM.
- **Motion sensor (QMI8658 at I2C 0x6B):** register notes are at the top of `main/imu.c`. Real numbers from this
  board, for tuning: still 0.001–0.005 g, firm table bump ~0.07 g, pick-up 0.14–0.33 g (threshold 0.10 g). The
  presence log prints `motion peak X g` every 5 s and `picked up / moved (X g): wake`; the page shows a live meter.
- **Testing dim/off/wake without waiting 10 min:** read `GET /api/presence`, `POST {"dim_s":10,"off_s":20}`, ask
  the user to stay quiet and still, test, then **post the original values back**.
- **Don't grow `presence_cfg_t`:** it's saved as one NVS blob and a size change drops the user's settings. Add new
  presence settings as separate NVS keys (as `motion` / `motion_mg` do).

- Diagnostics without flashing: `echo 300 > reboot.request`, wait for `flash.status` = idle, then
  `python3 tools/diag_summary.py serial_log.txt` from the device shell.

- GeoMet `GetCapabilities&layer=RADAR_1KM_RRAI` holds `start/end/PT6M` in the time Dimension. `GetMap` with
  `crs=EPSG:3857`, an exact bbox and 466×466 lines up pixel-perfectly with zoom-7 tiles.
- Open-Meteo `timezone=auto` returns `utc_offset_seconds`; `config_local_time()` uses it.
- The display's IP address appears in the log line `web: Settings page: https://<ip>/`.
