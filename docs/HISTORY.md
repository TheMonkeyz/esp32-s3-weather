# Project history and retrospective

A look back over the whole project (September 29 to October 2, 2026), written for the next session: how it grew,
how the work is done now, what paid off, what cost time, and what is still open. The details live elsewhere
(ARCHITECTURE for how things work, TESTING for how to check them, CLAUDE.md for the lessons in short); this file is
the story that connects them.

## In numbers

- Four days to v1.11.1 (98 commits, 13 stable releases, 30 tags); then the October 3 fix plan (v1.12.0-rc.1..rc.8, released as v1.12.0).
- 23 C files, ~10,000 lines (`main/`), plus the settings page (`main/web/index.html`), the web flasher, CI, and the
  test harness (`tools/harness/`).
- One board (Waveshare ESP32-S3-Touch-AMOLED-1.75, COM5 on the user's Windows PC), one user so far.

## Timeline

### September 29: the first build, without USB

The project started in the Claude desktop app's cloud container: no USB access, no typing into Windows terminals. So
the **flash helper** was born (`start_flash_helper.bat`, `flash.request` → `flash.done`, `serial_log.txt`), and a
**host LVGL simulator** (gcc + PNG output) checked layouts on the round screen before any flashing. The first bugs
were format surprises (lodepng returning a draw buffer, CARTO tiles needing an API key, a truncated HTTPS page from a
failed DMA allocation) and stack sizes. Lessons 1–12 in CLAUDE.md come from this day.

### September 30: features at speed (v1.0.0 to v1.5.0)

Radar zoom with a flash cache of every zoom level, the captive-portal Wi-Fi setup, presence dimming from the
microphones, the hourly view, then **diagnostics** (`diag:` lines): internal RAM was silently exhausted (LVGL's small
allocations; fixed with its own PSRAM heap, `lvgl_mem.c`) and font kerning was 71% of rendering. Then the release
machinery: CI builds, the **web flasher** with Stable and Beta channels built from GitHub releases, a per-device TLS
certificate, **updates over Wi-Fi with rollback** (v1.3.0), alerts, rain nowcast, extras and status pages, the
temperature graph, several places, units.

### October 1: Claude Code on the PC, more of the board (v1.6.0 to v1.10.0)

Work moved to Claude Code on the user's PC (ESP-IDF 5.5.4 installed locally, test builds in `build\v55`). Settings on
the display (long-press), wake on pick-up (the IMU), French then an Inuktitut draft (with snapshot checks of every
label's fit), alert sounds, lightning on the radar. The **offline Wi-Fi setup bug** had three separate causes, each
found in the log rather than guessed (lesson 20). Then the **test harness** (v1.10.0): the board driven through a USB
test console, the PC's Wi-Fi card acting as a phone, performance against a baseline. Its first runs caught real
problems (a rollback caused by the harness itself, a flaky Easy Connect channel pick).

### October 2: performance (v1.10.1 to v1.11.1)

The user asked for 60 fps swipes. LVGL 9.2 can't redraw a full screen in less than ~65–85 ms, so moves became
**pictures sent straight to the panel** (`slide.c`): screens, places and days at 60–70 fps. Getting there took the
hardest debugging of the project: display hangs from a cross-core race in esp_lcd (found with breadcrumbs and a
lock-free `where` command), stray taps after drags, drags that never ended because the touch chip goes silent
instead of reporting a release, PSRAM fragmentation that broke the radar's PNG decoder (replaced by `png_rows.c` with
the ROM's inflate). Then the user's feel reports drove the rest:

- "place drags are buggy / quite a delay": redraws that changed nothing were making cached pictures stale, and
  every return to the first place wrote the radar map to flash twice (v1.10.1-rc.3);
- list scrolling by moving the picture of the screen (v1.10.1-rc.4, released as **v1.11.0**), which needed that
  picture to always match the panel: a **display flush hook** keeps it so;
- the minute tick re-rendering whole pages, an update check marking everything (v1.11.1-rc.1);
- the radar zoom drawn the same way, today's hourly view ready before the tap, and **QIO flash**: every LVGL render
  ~30% faster (v1.11.1-rc.2).

The same day the release loop was handed over: Claude tags and pushes release candidates, watches CI with the GitHub
CLI, and installs them on the display through its own updater (`harness.py --ota`). Stable releases still need the
user's word.

### October 3: the evaluation and its fix plan (v1.12.0-rc.2 to rc.8, v1.12.0)

A read-only review of v1.11.1 (`docs/EVALUATION-2026-10-02.md`) became a fix plan worked through in one session,
one release candidate per group, each installed by the display's own updater and tested by the harness:

- **rc.2** (bugs): the settings page's Install button, touch loops that could hold the display lock, parsers that
  crashed on partial replies, alerts capped before sorting, a harness gate that passed what it didn't measure.
  Moving 48 KB of radar palettes to PSRAM raised the internal RAM low point from 9 to 50 KB.
- **rc.3** (security and robustness, with the owner's answers): a settings key in the on-screen QR, a per-display
  setup-network password that opens by itself for 15 minutes only, update confirmation tied to Wi-Fi, task stacks
  sized from measurements, places in typed NVS keys.
- **rc.4** (data and first run): forecasts right after midnight, failures visible on the weather screen, a
  first-run "Choose your location" hint.
- **rc.5** (tests and hygiene): the portal's DNS server limited to the setup network, host unit tests in CI, docs.
- **rc.6** (the owner's reports and the deferred items): the page's "No speaker found" (the speaker was probed
  before the microphones opened the shared I2S bus), a place change from the page now slides at ~65 fps like a
  drag, the French 12-hour clock as "2 h 45 p.m.", a one-time gesture hint after the location hint, a core dump
  partition read at boot, hardware AES on again (snapshots 30 % faster), a host test for the radar's row decoder.
- **rc.7**: rc.6's over-the-air test met an Open-Meteo outage (10 of 16 forecasts timed out on the display) and
  showed that a failed place kept its backoff (up to 10 min) across a reconnect. Wi-Fi back now fetches at once;
  the harness waits out an outage on the message screen instead of failing every test.
- **rc.8**: the OpenStreetMap User-Agent carries the running version. The same day the whole series shipped as
  **v1.12.0**, installed by the display's updater and passing every harness suite; the old test key was purged
  from git history (all commit IDs changed).

What it taught: build exactly what you commit (a script splitting work dropped an include and rc.1's build failed);
an empty translation is not a missing one; a rule with a time window needs a test that crosses it.

## How the work is done now

1. **Change, then a test build** labelled above the current release (`vX.Y.Z-name.N` in `version.txt`; the
   firmware compares version numbers first; for the same X.Y.Z a test label counts below any rc, so a board on
   `v1.12.0-fix.N` is offered `v1.12.0-rc.N`, while `v1.12.1-x` would hide it).
2. **Flash and observe**: `harness.py --flash` or a probe script in the scratchpad (`Board`/`Log` from the harness):
   act through the test console, read `serial_live.txt`. For hangs: breadcrumbs + `where`. For "why is X slow":
   time the parts, in a throwaway build with temporary logging (removed before committing).
3. **Prove it on the device**: `pictest` and the `picture` snapshot for anything drawn from cached pictures, the
   harness's checks for behaviour, its baseline for speed and memory. Pixel-exact checks found a bug the user had
   seen (the smeared graph) and that no frame-rate number would have shown.
4. **The user tries it**: the harness's straight synthetic swipes passed while real ones were missed or felt slow.
   The user's words ("follow the finger", "0.1 s delay", "corruption on the graph") were the most valuable signal of
   the performance work.
5. **"document and commit"**: ARCHITECTURE (how, and why that way), TESTING (how to check), CLAUDE.md (the lesson),
   the changelog for people (one line each, no jargon).
6. **Release candidates**: Claude publishes, CI builds, `harness.py --ota vX` installs it with the display's updater
   and runs everything. **Stable**: ask first. New features bump the minor version.

## What paid off

- **Logs over guesses.** Every hard bug here was found by reading or adding a log line: the Wi-Fi offline causes,
  the esp_lcd race, the drag delays (a build logging each picture marked out of date with its caller and a
  backtrace), the radar's double flash save.
- **Measuring the parts before optimising a part.** Kerning (71%), LVGL's layout pass during scrolls (4.4 of 7.7 ms,
  while the icons the obvious suspect saved nothing), the flash mode (30%). The profiler showed there was no single
  hot spot in LVGL's rendering, which is what pointed to pictures instead of faster widgets.
- **Tools that check the device, not the eye**: the snapshot API, the harness, `pictest`, `--ota`. A rule from the
  start held: verify on the device before calling something done.
- **Small, exact fixes with the reason in a comment**: most comments in `slide.c` and `ui.c` say what broke before
  ("a drag back waited 0.2 s…"), which is what the next session needs.
- **Keeping LVGL as the fallback** of every custom path (slides, drags, scrolls, zoom): when memory is short or
  something is busy, the old behaviour still works.

## What cost time

- **Concurrency on the display**: esp_lcd isn't thread-safe; its interrupt on the other core hung raw frames. Days of
  symptoms, minutes of fix once the breadcrumbs pointed at it.
- **Stale pictures from invisible causes**: a theme transition, a label set to the same text, a flag set to its own
  value, a layout left pending, events bubbling. Each one was a ~0.1–0.5 s delay the user felt. The lesson: a redraw
  that changes nothing is not free here.
- **Assuming an API's behaviour**: `lv_obj_scroll_to_y()` clamps at the ends, `lv_obj_scroll_by()` sends events
  that bubble, esptool writes a QIO bootloader's header as "dio", a static array is internal RAM. Read the source
  (`managed_components/lvgl__lvgl`, the ESP-IDF Kconfig) before relying on it.
- **Reporting before checking**: relaying a harness line ("Installing…") that meant something else. Say what the
  log shows, not what a message suggests.
- **Shell-escaped edits**: Python edit scripts in heredocs mangled `\n` and `\d` more than once; write such scripts
  with the Write tool.
- **Version labels and the updater**: test builds labelled above a release candidate never get offered it.

## Design rules that emerged

- **LVGL for content, `slide.c` for motion**: pictures in PSRAM, frames straight to the panel, LVGL's state kept in
  step so it can take over at the end.
- **The picture of the screen shown always equals the panel** (flush hook); other pictures are marked out of date by
  whoever changes them, only where they changed, and only when something really changed.
- **Background work yields to the user**: picture rendering in strips when the screen is quiet, flash writes paused
  while a finger is down or a move runs, no automatic benchmark.
- **Internal RAM is the scarce resource** (9–10 KB at the low point): anything big or static goes to PSRAM
  (`EXT_RAM_BSS_ATTR`, LVGL's heap); watch the harness's `internal_min_kb`.
- **Every visible text through i18n**, checked by snapshot in each language; French is Canadian French.
- **Mark only what changed, test what you changed**: every fix in this project added a harness check or a probe.

## Open threads

- Easy Connect's first setup should open the settings page on the phone (docs/IDEAS.md); the first-run hint (rc.4)
  covers part of it.
- Left from the October 3 fix plan: saving a place on the captive
  portal before the Wi-Fi restart (D3); heap poisoning in test builds; the harness's non-English Windows detection
  and separate render limits for QIO/DIO boards (E3); a real phone install from the settings page. (Old pull-request refs still reach
  pre-purge commits; the key was rotated, so no GitHub Support request was made.)
- The Inuktitut draft needs a fluent speaker (docs/translations/iu-review.md).
- Radar zoom: ~80 ms before the first frame (the overlays' alpha, mostly the range ring); could be cached between
  zooms.
- A data change (new forecast, alerts) still makes neighbours' pictures out of date: a drag in the next second or
  two waits ~0.13 s. The picture cache has 5 slots: with 2 places, Settings and the hourly view's other days aren't
  kept ready.
- Boards updated over the air keep a DIO bootloader (no QIO speed-up) until a USB or web-flasher install.
- GitHub's `ubuntu-latest` moves to Ubuntu 26 from October 19, 2026 (pin `ubuntu-24.04` if the build breaks).
