# LVGL 8 vs 9 render benchmark

`bench.c` builds four screens like the app's (weather, hourly, radar, blank) at 466x466 RGB565 with 32-row partial
buffers in internal DMA RAM, redraws each whole 20 times and logs the median (`bench: <fonts> <screen> median N ms`).
The flush returns at once: only LVGL's own rendering is timed. Two passes: the app's fonts (Montserrat through Tiny TTF,
96-glyph cache; v9 without kerning, v8 can't turn it off) and LVGL's pre-rendered Montserrat 20/28/48.

```powershell
. C:\Espressif\esp-idf\export.ps1
idf.py -C tools\lvglbench\v9 build      # LVGL 9.2.2 (the app's version)
idf.py -C tools\lvglbench\v8 build      # LVGL 8.4.0
```

Flash `v8\build\lvglbench_v8.bin` (or v9) with its partition table and bootloader, as `harness.py --flash` does, and
read the log. The settings store (NVS at 0x9000) is the app's; the benchmark overwrites the first app slot: put the
app back afterwards (e.g. the release files, laid out as a build folder, with `harness.py smoke --flash`).

## Results (October 4, 2026, QIO flash, 240 MHz, PSRAM 80 MHz, -O2)

| Screen | v9.2.2 TTF | v8.4.0 TTF | v9.2.2 pre-rendered | v8.4.0 pre-rendered |
|---|---|---|---|---|
| weather | 24.0 ms | 98.3 | 22.9 | 18.6 (-19 %) |
| hourly | 36.2 | 126.8 | 35.6 | 27.8 (-22 %) |
| radar | 35.3 | 51.7 | 34.6 | 25.7 (-26 %) |
| blank | 2.8 | 1.8 | 2.8 | 1.8 |

- v8 is ~20-26 % faster than v9 with pre-rendered fonts, and 3-4x slower with the app's TTF fonts (its Tiny TTF kerns
  and looks glyph metrics up on every draw).
- The app's own v9 weather screen takes 45-57 ms (harness `render_ms.weather`), twice this benchmark's 24: something
  in the app costs more than the LVGL version (suspects: the LVGL heap in PSRAM, `lvgl_mem.c`; more objects).
- In v8, `lv_tiny_ttf_create_data_ex()`'s last argument is the cache size in bytes (v9: glyphs); 96 bytes crashed
  it (division by zero in `lv_lru_hash`).
