# Diagnostics and performance

The firmware always logs `diag:` lines (`main/diag.c`). They cost well under 1% CPU and add no visible
behaviour, so they stay on in normal builds.

## How to run it again

1. The flash helper must be running (`start_flash_helper.bat`).
2. Restart the board and record its log **without flashing**:
   `echo 300 > reboot.request` (seconds to record, up to 9999). Use `flash.request` instead to flash
   `firmware\*.bin` first. Wait for `flash.status` to be `idle` again; `flash.done` has the summary line.
3. Summarise: `python3 tools/diag_summary.py serial_log.txt` (any Python 3; no packages needed).
4. For numbers about real use, do the **scenario** while it records, about 30 s per step so each step gets its own
   60 s window or half of one:
   1. idle for the first minute (boot, background map download, render bench at 45 s);
   2. radar: tap to animate, zoom in and out a few times;
   3. hourly view: drag between days, scroll the hours;
   4. settings page open on the phone (live sound meter polls every 700 ms);
   5. idle until the end.

Change `diag_start(60)` in `main.c` for another report period, or `BENCH_AT_S` in `diag.c` (0 disables the bench).

## What the lines mean

| Line | Content |
|---|---|
| `diag: reset reason …, flash …, PSRAM …` | once at boot; also app partition size and NVS entries used/free |
| `diag: mark <stage> internal N KB free (largest N), DMA N KB, PSRAM N KB` | heap after each boot stage (`diag_mark()` calls in `main.c`) |
| `diag: bench render-only full screen: blank … weather … hourly … radar …` | once, 45 s after boot: time to render each whole screen, not sent to the panel (invisible to the user; blocks the UI ~1.5 s) |
| `diag: bench weather incl. panel transfer` | same for the weather screen repainted *with* the SPI transfer; minus the render-only time = cost of the panel |
| `diag: heap: internal … (min ever …, largest block now … / worst …) \| DMA … \| PSRAM …` | every period. *min ever* is since boot. *worst* largest block is sampled every second |
| `diag: display: N frames, render avg/max, Mpx sent \| animation fps (frames, worst gap) \| LVGL lock wait max, longest hold by <task>` | per period. *Animation* counts frames rendered less than 250 ms apart. *Lock wait* is how long the LVGL task waited for `display_lock()`; *hold* is the longest time another task kept it |
| `diag: tasks: name(c<core> p<prio>) CPU% stackB` | per period, busiest first; CPU % of one core over the window, *stack* = free stack high-water mark (bytes) |
| `diag: cpu: core0 N% busy, core1 N% busy` | 100 − idle task time |

Needs `CONFIG_FREERTOS_USE_TRACE_FACILITY`, `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` and
`CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID` (in `sdkconfig.defaults`).

## Findings (2026-09-29)

Reference numbers. Re-run and compare after changes that touch memory or rendering.

### 1. Internal RAM was exhausted (fixed)

Before: internal RAM free was **10 KB steady, 0 KB "min ever"**. Boot marks showed where it went:

| stage | before | after |
|---|---|---|
| after network init | 123 KB | 131 KB |
| after `ui_init` (LVGL objects, fonts) | 61 KB (−48) | 107 KB (−10) |
| after web servers | 22 KB (−28) | 81 KB (−21) |
| after first weather fetch | 10 KB | 71 KB |
| steady state / min ever | 10 / **0** KB | 69 / 30 KB |

Causes and fixes:

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` puts every `malloc` under 16 KB in internal RAM first. LVGL
  (objects, styles, label text, TinyTTF glyph cache) made thousands of those. **Fix:** `main/lvgl_mem.c` with
  `CONFIG_LV_USE_CUSTOM_MALLOC` routes LVGL's heap to PSRAM (+52 KB internal). Rendering did not get slower.
  The linker needs `-u lv_malloc_core …` (in `main/CMakeLists.txt`) because only liblvgl references it.
- Oversized stacks (from the high-water marks): radar 16→10 KB (used ~4 KB), HTTPS server 10→7 KB (TLS
  handshake peak ~3.3 KB), HTTP server 8→4 KB (~1.2 KB), main 8→6 KB (~3.6 KB). About +14 KB.

Why it mattered: Wi-Fi RX buffers, DMA bounce buffers and new task stacks need internal RAM. Running out
earlier truncated the settings page (the hardware-AES bug) and can drop packets.

### 2. Rendering was dominated by font kerning (fixed)

Host profile (valgrind/callgrind of the LVGL simulator, weather + hourly screens): **71% of all instructions**
were `stbtt_GetGlyphKernAdvance`, a linear scan of Montserrat's kerning table for every character pair, on every
render and every text-width measurement. **Fix:** fonts are created with `LV_FONT_KERNING_NONE` (the screens look
the same side by side).

Full-screen render time on the device (ms, render only / with panel):

| screen | before | kerning off | + LVGL in PSRAM |
|---|---|---|---|
| blank | 6 / 28 | 7 / 28 | 6 / 28 |
| weather | 72 / 83 | 49 / 60 | 46–51 / 55–61 |
| hourly | 101 / 108 | 63 / 72 | 60–61 / 69 |
| radar | 68 / 78 | 55 / 65 | 54–55 / 64 |

Panel transfer of a full frame ≈ 22 ms (QSPI 40 MHz, 434 KB) and mostly overlaps with rendering (two 32-line
buffers).

### 3. Tried and reverted: two LVGL draw threads

`CONFIG_LV_OS_FREERTOS` + `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`: no gain (both threads landed on core 1; LVGL's
FreeRTOS port uses `xTaskCreate`, and the registry copy of LVGL can't be patched), and it cost 16 KB of internal
RAM for the two 8 KB draw stacks. Reverted.

### 4. Other observations (no action needed now)

- After-fix scenario run (300 s): internal RAM 61–69 KB free, min ever 28 KB; PSRAM min ever 1.7 MB; no resets;
  the only errors were the harmless TLS `-0x7780` below.
- CPU at idle: core 0 ~4–6% (presence 2.8%, Wi-Fi ~1%), core 1 ~1–2% (LVGL). Radar download/decoding peaks at
  ~35% of core 0; hourly-view dragging kept LVGL at ~31% of core 1.
- PSRAM: ~4 MB free steady, **1.7 MB min ever** (during radar frame loading at a new zoom level). Plenty, but
  that is the real headroom, not 2.8 MB.
- Animations measured with the scenario (60 s windows, frames rendered < 250 ms apart):

  | window | before | after |
  |---|---|---|
  | radar animation + zoom | 7.3 fps, frames avg 88 ms, worst 235 ms | 10.2 fps, avg 72 ms, worst 263 ms |
  | hourly view dragging | 9.9 fps, avg 90 ms | 14.9 fps, avg 58 ms, worst gap 174 ms |

  Radar zoom is the heaviest case: the scale animation transforms a full-screen image every frame.
- The radar task holds `display_lock()` for up to ~75 ms while it swaps in a composed frame, so the UI can miss
  a frame or two during radar loading. Candidate fix if it's ever visible: compose outside the lock, swap under it.
- `mbedtls_ssl_handshake returned -0x7780` on the HTTPS server = the phone's browser closed extra speculative
  connections after the self-signed certificate warning. Harmless.
- `GET failed (ESP_ERR_HTTP_WRITE_DATA / FETCH_HEADER)` from GeoMet = idle keep-alive connection closed by the
  server; the retry logic handles it.

### Candidate optimisations (only if a symptom appears)

| Symptom | Candidate | Expected gain |
|---|---|---|
| Radar zoom feels choppy (10 fps) | render the zoom animation from a half-resolution copy, or fewer steps | fewer pixels transformed per frame |
| Swipes/animations feel choppy | profile rendering on the device per object type; raise SPI clock to 80 MHz if the CO5300 accepts it | panel 22→11 ms (only matters when it doesn't overlap) |
| UI hiccup while the radar loads | compose radar frames outside `display_lock()` | removes 75 ms stalls |
| Internal RAM low again | check `diag: mark` lines to find the stage; stacks from the task table | — |
