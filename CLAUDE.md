# Notes for AI-assisted sessions on this project

Read `README.md` and `docs/ARCHITECTURE.md` first. This file collects the practical lessons from the first build
session (Sept 2026): what worked, what cost time, and how to avoid repeating it.

## Working setup (how the first session ran)

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75 on **COM5** of a Windows PC. Project folder:
  `C:\Users\lmathieu\ESPDEV\weather_amoled`.
- **Two ways to work:**
  1. **Claude Code on Windows** (preferred): run it in this folder. It can use `idf.py` and the COM port directly.
     ESP-IDF v5.4 may still need installing on the PC (look in `~\.espressif`).
  2. **Claude desktop app (Cowork):** builds happen in a cloud container. Its shell on the PC is a Linux VM with **no
     USB access**, and it can't type into Windows terminals. Flashing then goes through the **flash helper**: the user
     starts `start_flash_helper.bat` once, Claude writes `flash.request` (containing the log seconds) and polls for
     `flash.done`, then reads `serial_log.txt`.
- **Talk to the user:** post a one-line progress note before anything that takes more than a minute (build, flash,
  log window). Long silences read as "stuck". Don't take control of the user's screen; ask first, and prefer the flash
  helper. Ask the user to interact with the board (swipe, tap, open the page) *during* the log window, and say when.

## Cloud build recipe (when no local IDF)

The network allowlist blocked PyPI, dl.espressif.com, Docker Hub and the component registry; only GitHub worked:

```bash
git clone --depth 1 --branch v5.4.2 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git
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

With the component manager off, LVGL is vendored at `components/lvgl`, and `esp_codec_dev` (v1.5.2, sparse-cloned
from `espressif/esp-adf`) at `components/esp_codec_dev`. Its Kconfig warns that `ESP_IDF_VERSION` isn't set; that's
harmless and keeps `CODEC_I2C_BACKWARD_COMPATIBLE` off, which we need because we use the new I2C driver. LVGL is (a v9.2.2 clone). Keep its `examples/` and
`demos/` directories, because the CMake file adds them as include paths. Neither vendored directory is in this repo; the
Windows build gets both from `main/idf_component.yml`. `components/dns_server` *is* in the repo.

## Verification tricks that paid off

- **Host LVGL simulator:** compile LVGL plus `ui.c`/`radar.c` pieces with gcc against a small `lv_conf.h`, render
  into a framebuffer, write PNGs. It caught layout clipping on the round screen and proved the clock timer logic.
  Stub the ESP headers (`esp_log.h`, `net.h`, `config` functions). Use `-Wl,--wrap=time` to fake the clock.
- **On-device framebuffer dump:** print a 1/3-scale RGB565 snapshot as base64 between `IMGDUMP name w h` and
  `IMGEND` in the serial log, then decode it to PNG. This found the PNG decoding bug and the CARTO "API KEY REQUIRED"
  tiles. The code was removed after use; re-add it when a screen looks wrong.
- **Headless browser test of the settings page:** Playwright + a tiny Python mock server (mock `/api/*`, mock
  geolocation, route the geocoding APIs). It proved the page's JS was fine, which pointed at the device.
- **Always check file transfers:** `md5sum` the firmware on the PC against the build output. **Twice**, pushing a
  rebuilt file from the *same* staging path delivered the *previous* version. Use a fresh staging directory for every
  push and compare checksums before writing `flash.request`.
- **Flash helper feedback:** read `flash.status`, then `flash.done` (it includes error, warning and reset counts),
  then grep `serial_log.txt`. A plain-text `flash_log.txt` shows exactly what esptool wrote.

## Bugs hit, and their fixes (don't repeat these)

1. **LVGL's bundled lodepng returns an `lv_draw_buf_t*`, not raw pixels.** Use `db->data` and `db->header.stride`;
   bytes are R,G,B,A. Free it with `lv_draw_buf_destroy`.
2. **CARTO basemap tiles now need an API key.** They return grey "API KEY REQUIRED" images, so we switched to OSM tiles
   with a proper User-Agent, attribution and a flash cache.
3. **Stack overflow in the radar task:** arrays of `frame_t` (about 1 KB each) were on the stack. Keep big structs
   `static`.
4. **HTTPS page arrived truncated, so the buttons "did nothing":** hardware AES failed to allocate internal DMA
   memory. We disabled `CONFIG_MBEDTLS_HARDWARE_AES`, reduced the LVGL buffers to 32 lines, and send the page in
   1 KB chunks.
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

## User preferences learned

- Swipe **down** = zoom in, **up** = zoom out (the opposite felt wrong).
- Prefers doubling zoom steps (sharp map) over exact 50 km steps.
- Wants short progress notes, no screen takeover, and changes verified on the device (log + snapshot) before
  being called done.

## Useful facts

- GeoMet `GetCapabilities&layer=RADAR_1KM_RRAI` holds `start/end/PT6M` in the time Dimension. `GetMap` with
  `crs=EPSG:3857`, an exact bbox and 466×466 lines up pixel-perfectly with zoom-7 tiles.
- Open-Meteo `timezone=auto` returns `utc_offset_seconds`; `config_local_time()` uses it.
- The display's IP address appears in the log line `web: Settings page: https://<ip>/`.
