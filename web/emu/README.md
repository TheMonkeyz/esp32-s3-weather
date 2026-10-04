# The display in the browser

The firmware's own screens, compiled to WebAssembly: LVGL 9.2.2 with the display's settings (`lv_kconfig.h`, generated
from its sdkconfig), `ui.c`, `slide.c`, `pager.c`, `config.c`, `i18n.c`, the forecast, air-quality and alerts code
(`weather.c`, `alerts.c`) and the radar (`radar.c`, `png_rows.c` with miniz's tinfl as the chip's ROM has), all
unchanged. Only the hardware is replaced:

| File | Stands in for |
|---|---|
| `emu_display.c` | the AMOLED panel: a 466x466 RGB565 framebuffer that `index.html` copies to a round canvas |
| `emu_touch.c` | the touch chip: the mouse or a finger on the canvas |
| `emu_http.c` | `esp_http_client`: `fetch()`, awaited with ASYNCIFY (Open-Meteo, GeoMet and the alerts API allow it) |
| `emu_nvs.c` | NVS: the settings, kept in the page's `localStorage` |
| `emu_stubs.c` | Wi-Fi, updates, speaker, microphones, service statuses |
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
- Fetches pause the screen while they wait (one thread); a forecast takes ~0.3-0.5 s.
- The radar loads the zoom shown only: the display preloads every zoom level's map once, which a public page would turn
  into bulk downloads against OpenStreetMap's tile policy (`radar_preload_start` is never called here).
- A request starts a `fetch()` and polls for it: an await inside a fiber isn't safe with ASYNCIFY.
- OpenStreetMap's tile policy: a page can't set `User-Agent` (the firmware's isn't sent); for browser apps the policy
  asks for a valid `Referer` (sent explicitly: `strict-origin-when-cross-origin`, the site's origin) and the server's
  caching (the browser's default cache, never `no-cache`). Don't add a custom identifying header: it turns every tile
  request into a CORS preflight.
- `?place=lat,lon,Name` in the address makes that the place shown (kept), e.g. a link to a place with an alert:
  `try/?place=47.574,-59.137,Port%20aux%20Basques`.
- Not yet: the settings page (the display's own web page), sounds.
