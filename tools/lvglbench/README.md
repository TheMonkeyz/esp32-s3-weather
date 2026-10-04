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
- The app's weather screen takes ~41 ms, this benchmark's 23: not an overhead in the app but a heavier screen (the
  benchmark's icons are plain circles). Ruled out on October 4: the LVGL heap in PSRAM (`v9psram`: 22.9 vs 23.0 ms),
  the app's 400 px centred labels (+0.9 ms per screen), the syllabics fallback fonts (+0), the other core's work (app
  tasks paused and Wi-Fi off: 41.4 -> 41.2 ms). The hourly screens match (app 34.3, benchmark 35). Per object (hidden
  one at a time, throwaway build): the detail row (5 labels, 2 drawn icons) 8.0 ms, the temperature with its icon 5.6,
  each day's icon ~2.5, each label ~1-1.4.
- Two draw units (both cores) and newer LVGL, same screens (`v9os`, `v96`, `v96os`), median ms weather / hourly / radar:

  | Build | TTF | pre-rendered |
  |---|---|---|
  | 9.2.2, one core (the app) | 22.8 / 34.9 / 33.7 | 21.4 / 34.0 / 33.2 |
  | 9.2.2, two draw units | 23.7 / 35.9 / 34.3 | 19.0 / 31.9 / 32.7 |
  | 9.6.0, one core | 28.2 / 40.6 / 33.6 | 27.2 / 40.0 / 33.2 |
  | 9.6.0, two units (tiles) | 48.7 / 76.6 / 49.9 | 39.0 / 67.2 / 47.3 |

  9.6 is slower than 9.2.2 here (a blank screen 4.5 ms against 2.7), and splitting the 32-row strips into tiles for
  two cores costs more than it gains; bigger strips would need internal RAM the app hasn't got. Stay on 9.2.2.
- In v8, `lv_tiny_ttf_create_data_ex()`'s last argument is the cache size in bytes (v9: glyphs); 96 bytes crashed
  it (division by zero in `lv_lru_hash`).
