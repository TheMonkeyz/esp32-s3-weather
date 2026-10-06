# Diagnostics and performance

The firmware always logs `diag:` lines (espforge's forge_core `diag.c`, plus this display's `diag: display` line from `main/console.c`). They cost well under 1% CPU and add no visible
behaviour, so they stay on in normal builds.

## How to run it again

1. The flash helper must be running (`start_flash_helper.bat`).
2. Restart the board and record its log **without flashing**:
   `echo 300 > reboot.request` (seconds to record, up to 9999). Use `flash.request` instead to flash
   `firmware\*.bin` first. Wait for `flash.status` to be `idle` again; `flash.done` has the summary line.
3. Summarise: `python3 tools/diag_summary.py serial_log.txt` (any Python 3; no packages needed).
4. For numbers about real use, do the **scenario** while it records, about 30 s per step so each step gets its own
   60 s window or half of one:
   1. idle for the first minute (boot, background map download);
   2. radar: tap to animate, zoom in and out a few times;
   3. hourly view: drag between days, scroll the hours;
   4. settings page open on the phone (live sound meter polls every 700 ms);
   5. idle until the end.

Change `diag_start(60)` in `main.c` for another report period. The render bench runs only on request (test console
`bench`, or the harness's `perf` suite): since v1.11.0 `BENCH_AT_S` in `diag.c` is 0, because the automatic run 45 s
after boot blocked the screen for 1.5 s and swallowed swipes.

## What the lines mean

| Line | Content |
|---|---|
| `diag: boot reset=… flash_mb=… psram_kb=…` | once at boot (after 4 s); also app partition size and NVS entries used/free; `diag: coredump: last crash …` after a crash (before v1.14.0: `reset reason …`, `last crash (core dump)`) |
| `diag: mark <stage> internal N KB free (largest N), DMA N KB, PSRAM N KB` | heap after each boot stage (`diag_mark()` calls in `main.c`) |
| `diag: bench render-only full screen: blank … weather … hourly … radar …` | on request (`bench`), when the weather screen is idle (otherwise "postponed"; the harness asks again; its last step repaints the weather screen, which flashed over the hourly view once): time to render each whole screen, not sent to the panel (invisible to the user; blocks the UI ~1.5 s) |
| `diag: bench weather incl. panel transfer` | same for the weather screen repainted *with* the SPI transfer; minus the render-only time = cost of the panel |
| `diag: heap: internal … (min ever …, largest block now … / worst …) \| DMA … \| PSRAM …` | every period. *min ever* is since boot, and it adds up each internal heap's own low point (see "Internal RAM at a place switch" below): it can be lower than any real moment. *worst* largest block is sampled every second |
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

Since then (from the harness, which is now the reference: `internal_free_kb`, `internal_min_kb`,
`internal_min_kb.reconnect`): by v1.11.1 the 5-picture cache, TLS and the hourly graphs had brought it to ~48 KB
steady and **8–10 KB min ever** (3 KB once), 4–5 KB on the reconnect path (three TLS clients and the forecast parse at
once). v1.12.0 moved the radar's 46 frame structs (48 KB of palettes) and cJSON's parse trees to PSRAM: **~96 KB
steady, 48–50 KB min ever, 76 KB after the reconnect path** (rc.2). rc.3 then gave six task stacks the room their
measured peaks asked for (~12.7 KB): **~84 KB steady, 36–40 KB min ever, 38–43 KB after the reconnect path**. The `heap` lines also count failed allocations and LVGL
blocks put in internal RAM (both 0 expected).

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
buffers). Since v1.11.0 the bus runs at 80 MHz: ≈ 11 ms.

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

  v1.4.0 temperature graph, hourly view dragging: drawn on every frame 12.1 fps (avg 73.5 ms); cached in a canvas
  15.4 / 14.8 fps (avg 55–59 ms, worst gap 175 ms), back to the reference.

  Radar zoom is the heaviest case: the scale animation transforms a full-screen image every frame.

  v1.11.1 with QIO flash, full-screen renders (bench): weather 45 ms, hourly 35, radar 44, weather with the panel
  69 (DIO: 67 / 51 / 56 / 94). Radar zoom 40–43 fps (LVGL: ~10); hourly list 13.7 ms a frame.

  v1.11.0, harness `perf` (`fps`, frames less than 250 ms apart): screen to screen 64–70 fps,
  places 46 fps, hourly days 58 fps (moves drawn as pictures, `slide.c`); the hourly list 17.5 fps and Settings
  21.9 fps still scroll with LVGL. PSRAM low point 452–517 KB with the 5-picture cache (2.2 MB; 1042 KB in v1.10.0).
- The radar task holds `display_lock()` for up to ~75 ms while it swaps in a composed frame, so the UI can miss
  a frame or two during radar loading. Candidate fix if it's ever visible: compose outside the lock, swap under it.
- `mbedtls_ssl_handshake returned -0x7780` on the HTTPS server = the phone's browser closed extra speculative
  connections after the self-signed certificate warning. Harmless.
- `GET failed (ESP_ERR_HTTP_WRITE_DATA / FETCH_HEADER)` from GeoMet = idle keep-alive connection closed by the
  server; the retry logic handles it.

### 5. Internal RAM at a place switch (2026-10-05, v1.14.2-rc.2)

`memlow` around "drag to the second place, wait, drag back, wait" gave 23-28 KB on v1.14.2-rc.1 (from ~72 KB free),
with or without an alert. A throwaway probe build (local branch `probe/switch-ram`, console `rp`) found the parts:

| What | Internal RAM | When |
|---|---|---|
| `cache_load()`: the radar's cached map read from flash into PSRAM in one call; the flash driver reads through an internal buffer as large as the read, up to 16 KB | 16 KB for ~50 ms | return to the first place (and every zoom change, the end of a preload) |
| A TLS download: `esp_tls_t` (1.75 KB), the HTTP client's buffers (radar 4 KB + 0.5, alerts 2 + 0.5, air and forecast 0.5 + 1), lwIP's control block and unsent segments (1.5 KB each), queued Wi-Fi frames (1.75 KB each, 3-8 seen) | ~10-15 KB each | the radar's tiles and GeoMet frame overlap the main task's alerts, then air quality |
| Air quality's reply buffer (`calloc(4096)`: under 16 KB, so internal) | 4 KB | the air request |

Fixed in v1.14.2-rc.2: `cache_load()` reads 4 KB pieces (16 KB pieces: 50 ms and the reserve below left at 15 KB;
8 KB: 59 ms, 24 KB; 4 KB: 70 ms, 27.5 KB; 2 KB: 87 ms, 29 KB) and the air reply goes to PSRAM. `memlow` over the same
round: 23-28 -> 42-43 KB. Holding the radar's switch work 3 s more (until alerts and air are done) measured 45-48 KB,
but the radar's map for a new place then comes later and the held download landed on the drag back (59-61 fps
instead of 68): not done.

**How the numbers add up.** Internal RAM is five heaps (`heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)`): the main
DRAM heap (260 KB, full all the time: a few KB free), a 22 KB one (full), a 32 KB region (where the network buffers
go), 8 KB of RTC RAM, and the 32 KB pool `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` keeps for allocations that ask for
internal or DMA memory explicitly. `memlow`, `heap`'s min and the diag line's "min ever" add up **each heap's own
low point**, reached at different times: over one switch round the sum was 23-28 KB while internal RAM sampled every
2 ms never went below 41-45 KB. The harness's internal floors use that sum; compare it with itself, and use a
timeline (below) for the real moment.

**What runs out first.** A plain `malloc()` under 16 KB tries internal RAM, then quietly takes PSRAM
(`heap_caps_malloc_default`): Wi-Fi's buffers, lwIP, esp-tls and the HTTP clients spill into PSRAM rather than fail,
and `failed_allocs` stays 0. What can fail is what must be internal: task stacks, FreeRTOS queues and semaphores,
DMA buffers (AES bounce buffers, SPI), and the flash driver's read buffer. They come from the small heaps and then the
reserve pool, so the reserve's own low point (16 KB taken by the map read before rc.2, ~4 KB since) is the margin
that matters.

**How it was found** (to do again after a change that touches the network or flash): a throwaway build where each
download, decode and flash save marks itself active, and a task (priority 10, every 2 ms) logs per 100 ms the exact
low point (`heap_caps_monitor_local_minimum_free_size_start/stop` around each bucket) with what was running; at the
sampled low it walks the internal heap's used blocks (`heap_caps_walk`) and diffs them against the start of the
window. With `CONFIG_HEAP_USE_HOOKS` (in `build\v55\sdkconfig` only), an `esp_heap_trace_alloc_hook` records the
callers of internal allocations of 480 B or more (`esp_backtrace_get_start`, an IRAM hook, a DRAM ring copied at the
walk), resolved with `xtensa-esp32s3-elf-addr2line`. Console delays (the radar's relocation, or alerts and air,
held 12 s) separate the two tasks: 43-53 KB each alone, 34-37 overlapped (the probe's own 100 ms lows).

### Candidate optimisations (only if a symptom appears)

| Symptom | Candidate | Expected gain |
|---|---|---|
| ~~Radar zoom feels choppy (10 fps)~~ (done, v1.11.1) | scaled frames straight to the panel, overlays blended (`slide_zoom`) | 40–43 fps |
| ~~Swipes/animations feel choppy~~ (done, v1.11.0) | profiled (docs/TESTING.md §8); 80 MHz SPI; moves drawn as pictures (`slide.c`, ARCHITECTURE "Moves") | screens 10–15 → 64–70 fps, places 10 → 46, days 11 → 58 |
| ~~Lists scroll at 17–22 fps~~ (done, v1.10.1-rc.4) | move the picture of the screen, render only the new rows (ARCHITECTURE "Moves", List scrolls) | hourly ~52 fps, Settings ~70 |
| UI hiccup while the radar loads | compose radar frames outside `display_lock()` | removes 75 ms stalls |
| ~~Internal RAM low at a place switch~~ (done, v1.14.2-rc.2) | the map read from flash in 4 KB pieces, air quality's buffer in PSRAM (5 above) | `memlow` 23-28 -> 42-43 KB |
| Internal RAM low at a place switch again | the radar's switch work after the alerts and air-quality requests (one TLS download at a time) | +3-5 KB; the radar map of a new place 1-3 s later |
| Internal RAM low again | check `diag: mark` lines to find the stage; stacks from the task table; for a moment, the probe in 5 | — |
