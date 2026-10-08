# The display in the browser

The firmware's own screens, compiled to WebAssembly: LVGL 9.2.2 with the display's settings (`lv_kconfig.h`, generated
from its sdkconfig), `ui.c`, `slide.c`, `pager.c`, `config.c`, `i18n.c`, the forecast, air-quality and alerts code
(`weather.c`, `alerts.c`), the radar (`radar.c`, `png_rows.c` with miniz's tinfl as the chip's ROM has), the alert
sounds (`sound.c`), the brightness, dimming and wake on pick-up (espforge's `forge_presence`, with `audio.c`) and the settings page's routes
(`routes.c`, forge_ota's `ota_web.c`), all unchanged. Only the hardware is replaced:

| File | Stands in for |
|---|---|
| `emu_display.c` | the AMOLED panel: a 466x466 RGB565 framebuffer that `index.html` copies to a round canvas |
| `emu_touch.c` | the touch chip: the mouse or a finger on the canvas |
| `emu_http.c` | `esp_http_client`: `fetch()`, awaited with ASYNCIFY (Open-Meteo, GeoMet and the alerts API allow it) |
| `emu_nvs.c` | NVS: the settings, kept in the page's `localStorage` |
| `emu_stubs.c` | Wi-Fi, updates (Restart reloads the page), service statuses |
| `emu_audio.c` | the speaker and the microphones (`esp_codec_dev`): sound.c's PCM, collected between open and close, played through Web Audio at the codec volume (the first touch unlocks audio, browsers' rule); forge_presence's 100 ms reads (through `main/audio.c`), from the browser's microphone once the visitor turns it on, else silence at the same pace |
| `emu_imu.c` | the motion sensor (QMI8658): a phone's accelerometer (`devicemotion`), or the page's "Pick it up" button; a computer lies still |
| `emu_web.c` | the web server: the settings page below the emulator (`build/settings.html`, the display's `main/web/index.html` with `emu-settings.js` first in its head) queues its `/api/` requests, served here between LVGL frames by the display's own handlers; Wi-Fi scan and save answer that they need the real display |
| `emu_tasks.c` | FreeRTOS tasks: each one an Emscripten fiber, run by the main loop between LVGL frames; a wait inside a task (`vTaskDelay`, `ulTaskNotifyTake`, a request) goes back to the main loop. radar.c's task runs as is |
| `emu_partition.c` | the radar's map cache partition, in memory |
| `emu_main.c` | `main.c` after Wi-Fi is up: fetch every place, alerts (with the region map) and air quality, then run LVGL |
| `shim/` | ESP-IDF and FreeRTOS headers; `vTaskDelay` hands control back to the browser (`emscripten_sleep`) |

The firmware itself has one `#ifdef EMU_BUILD`: `ui.c` finds its fonts as arrays here (`build/fonts.c`).

## Build (WSL)

```bash
git clone --depth 1 https://github.com/emscripten-core/emsdk.git ~/emsdk && ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
source ~/emsdk/emsdk_env.sh
cd web/emu && make -j8      # build/emu.js, build/emu.wasm (~1.1 MB), build/index.html; ~1 min the first time
```

It needs LVGL where ESP-IDF's component manager puts it (`managed_components/lvgl__lvgl`, from a firmware build) and
ESP-IDF's cJSON (`IDF_PATH`, default `/mnt/c/Espressif/esp-idf`). Serve `build/` over HTTP (`.claude/launch.json`:
"emulator", or `python -m http.server -d web/emu/build`); `file://` can't load the WebAssembly.

From a git worktree (no `managed_components` of its own): `make -j8 LVGL=<main checkout>/managed_components/lvgl__lvgl`.
Port 8765 (`.claude/launch.json`) may already be serving another session's build: serve this one on another port
(`python -m http.server 8767 -d web/emu/build`), or the page shows the other checkout's firmware.

After changing LVGL options in `sdkconfig.defaults`: `python web/emu/gen_lv_kconfig.py build/v55/sdkconfig > web/emu/lv_kconfig.h`.

## On the flasher site

`python tools/make_flasher_site.py site --stable dist --emu web/emu/build` copies `index.html`, `emu.js` and
`emu.wasm` to the site's `try/` and adds `"try": "try/"` to `channels.json`; the flasher page then shows *Try it in
your browser* under its screenshots (hidden on a site built without `--emu`). `index.html` uses the site's font from
`../fonts/` and its device mockup.

CI (`.github/workflows/firmware.yml`, job `emulator`) builds it on every push, from the latest stable release (the tag
itself when a stable tag is pushed; a release older than the emulator has no `web/emu`, and then the pushed commit is
built, said in the log), with LVGL from GitHub at the firmware's version, cJSON 1.7.19 (ESP-IDF 5.5.4's) and
Emscripten 6.0.11. The `pages` job adds it to the site; if the emulator build fails, the site is published without it.

## Notes

- A click can start and end between two of LVGL's touch reads: `emu_touch.c` holds a press until LVGL has read it.
  Only a new press is held, never a move, and `touch_forget()` drops it: a drag (slide.c) reads the finger itself, and
  a press held from its moves reached LVGL after the drag as a tap (every day swipe closed the hourly view, until
  October 4).
- A new visitor gets three places: Québec City (the firmware's default), Vancouver and Iqaluit (`default_places()` in
  `emu_main.c`, added once, to a visitor with a single place; NVS `emu`/`places` remembers it), so dragging up and
  down between places works from the start, across three time zones.
- Fetches pause the screen while they wait (one thread); a forecast takes ~0.3-0.5 s.
- The radar loads the zoom shown only: the display preloads every zoom level's map once, which a public page would turn
  into bulk downloads against OpenStreetMap's tile policy (`radar_preload_start` is never called here, and radar.c
  preloads at a return to the first place only after it was).
- A place change calls `radar_relocate()` (`place_select()`, as main.c's `follow_active()`): until October 5 it didn't,
  and the radar kept the previous place's map and rain until a zoom made it look at the location again.
- A request starts a `fetch()` and polls for it: an await inside a fiber isn't safe with ASYNCIFY.
- OpenStreetMap's tile policy: a page can't set `User-Agent` (the firmware's isn't sent); for browser apps the policy
  asks for a valid `Referer` (sent explicitly: `strict-origin-when-cross-origin`, the site's origin) and the server's
  caching (the browser's default cache, never `no-cache`). Don't add a custom identifying header: it turns every tile
  request into a CORS preflight.
- `?place=lat,lon,Name` in the address makes that the place shown (kept), e.g. a link to a place with an alert:
  `try/?place=47.574,-59.137,Port%20aux%20Basques`.
- Alerts already there when the page opens are only recorded, as on the display: a sound plays when one is new or
  gets worse, or with Settings > Test the sound.
- The settings page (October 6): the display's own page in an iframe, unchanged. Its requests are queued by the page
  and served by the main loop (`emu_web_poll()`), never inside a call from JavaScript: a handler may wait
  (forge_ota's `update_post` does, `vTaskDelay`), and ASYNCIFY can't unwind a call made while the main loop is itself
  suspended. The frame can start before the page around it has run its script: `emu-settings.js` waits for
  `emuApi`. Loaded with `?test`, so the 10 s "Testing" timing is offered. A place changed there refetches and moves
  the radar, as main.c's location callback.
- Brightness (October 6): the canvas follows `display_brightness()` (a CSS `brightness()` filter; 0 is black, as the
  AMOLED off). The dimming runs only while the visitor's microphone is on (`emu_mic()`): with silence the room would
  always be "quiet" and the screen would dim for good. The sound never leaves the browser. Wake on pick-up: a phone's
  accelerometer (an iPhone asks permission, in the same tap as the microphone), or "Pick it up" (0.35 g for 0.7 s;
  forge_presence wakes above 0.10 g). A hidden tab pauses `requestAnimationFrame`, so the canvas (and its brightness)
  only updates when the page is shown.
- Screen dimming since v1.15.0 is espforge's `forge_presence` built from the same component as the firmware
  (`presence.c`, `presence_sm.c`, `presence_json.c`, `presence_web.c` from `$(FORGE)/forge_presence`), not a stand-in:
  `emu_main.c` gives it the hooks `main.c` does (the microphones through `main/audio.c`, which builds here on the I2S
  shim and `emu_audio.c`; `emu_imu.c`; `emu_touch.c`'s `touch_idle_ms`, so a touch now wakes a dimmed screen here too;
  `display_brightness()`). The console commands it registers go nowhere (`testcon_register` is a stub in
  `emu_stubs.c`); its settings are NVS typed keys in `localStorage` (`emu_nvs.c` gained `i16`), and a visitor's old
  `presence/cfg` blob is imported once as on the display.
