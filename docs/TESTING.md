# Testing on the device

How a change gets built, flashed, observed and shown before it is called done. Every change is verified on the
board (log, snapshot, or both). The Windows PC drives the board on its USB serial port (COM5 on the dev PC); the
board is on the home network. `<ip>` below is the display's address (`net: Connected, IP …` in the log).

## 1. Build

| Where | ESP-IDF | Components | Use |
|---|---|---|---|
| CI (`.github/workflows/firmware.yml`) | v5.5.4 (v5.4.2 until v1.9.0) | component manager (`main/idf_component.yml`) | releases, the web flasher |
| Windows PC, Claude Code | v5.5.4 (`C:\Espressif\esp-idf`) | component manager | test builds |
| Cowork cloud container | v5.5.4 (clone that tag) | vendored (see CLAUDE.md) | test builds when no PC shell |

Test build on the PC, in its own folder so the repo's `sdkconfig` is never touched (PowerShell):

```powershell
Set-Content version.txt "v1.4.0-status.3" -NoNewline -Encoding ascii   # label; git-ignored
(Get-Item CMakeLists.txt).LastWriteTime = Get-Date                      # version.txt is read at configure time
. C:\Espressif\esp-idf\export.ps1
idf.py -B build\v55 -D SDKCONFIG=build\v55\sdkconfig build
```

- **Label test builds above the current stable release** (`v1.4.0-name.N` while v1.3.1 is out). A lower label is
  offered the stable release as an update. Delete `version.txt` when done, or later builds keep the label.
- **To test a release candidate over Wi-Fi,** the board must run something *below* it. After a test build labelled
  higher (e.g. `v1.5.0-graph.3` vs `v1.4.0-rc.2`), flash an older test build from `firmware\in\` first (with a new
  unique name) and let the board offer the rc.
- **Settings can be driven from the PC** for tests, e.g.
  `curl -sk -X POST -H 'Content-Type: application/json' -H "X-Key: $KEY" -d '{"temp":"f"}' https://<ip>/api/units`,
  then snapshot. Since v1.12.0 a change needs the display's key: `echo key > serial.send` and read `test: key …` in
  `serial_live.txt` (the harness does it and saves it in `tools/harness/.display_key`, git-ignored). Put the user's
  settings back afterwards.
- After changing `sdkconfig.defaults`, delete `build\v55\sdkconfig` and run `idf.py ... reconfigure`: an existing
  sdkconfig keeps its old values (an option that exists as "not set" ignores the new default).
- **Keep the PC and CI on the same ESP-IDF.** Until v1.9.0 CI used v5.4.2 while the PC had v5.5.4, so test builds
  didn't match releases (Easy Connect's failure event differs between them). Change both together.
- **Bootloader:** the flash helper writes whatever is in `firmware\bootloader.bin` with the app. Since v1.11.1 that is
  the QIO bootloader from `build\v55\bootloader\bootloader.bin` (copy it there after a build that changes it); the
  render-time baselines assume QIO (~30 % faster than DIO). `firmware\bootloader-idf542-dio.bin` is the old DIO one:
  flash it to reproduce a board updated over the air (they keep the bootloader they had). The harness prints the
  mode it finds in the boot log next to "Testing vX".

## 2. Flash and log (flash helper)

The user starts `start_flash_helper.bat` once (restart it after changing `flash_helper.ps1`; `monitor.ps1` is
reloaded on every run). Then:

1. Copy the build to a **new, unique name** and compare checksums. A reused path has delivered a stale file twice.
   ```bash
   cp build/v55/weather_amoled.bin firmware/in/weather_amoled_v64_status3.bin
   cp firmware/in/weather_amoled_v64_status3.bin firmware/weather_amoled.bin
   md5sum build/v55/weather_amoled.bin firmware/weather_amoled.bin    # must match
   ```
2. `echo 300 > flash.request` (seconds of serial log). `echo 300 > reboot.request` restarts and logs without
   flashing.
3. `flash.status` goes `flashing` → `logging` → `idle`. Ask the user to touch the board *during* `logging`, and say
   what to do.
4. Stop early once the lines you need are in: `echo > stop.request` (or the user presses Q / Esc in the helper
   window). `serial_log.txt` is only written when the window ends.
5. `flash.done` has `exit`, timings, `errors`, `warnings`, `resets` and `stopped_early`. Then grep `serial_log.txt`.

Read before testing: the first lines say what is actually running. `diag: firmware vX` and
`ota: Running vX from ota_N, channel …`. Once, the board was on an older release candidate than the build that had
been flashed, which invalidated a test.

Normal noise: `mbedtls_ssl_handshake returned -0x7780` (a phone rejecting the self-signed certificate),
`read error :-0x0050` (peer closed), `failed to load RF calibration data` (first boot after a flash).

## 3. Screenshots (`tools/snapshot.py`)

The firmware serves `GET /api/snapshot?screen=<name>` on the settings server (HTTPS, home network only). It renders
the screen **off-display** with `lv_snapshot_take` and streams a 24-bit BMP, so the board isn't disturbed and no one
has to swipe:

```bash
python tools/snapshot.py <ip> status            # -> snapshot_status.png
python tools/snapshot.py <ip> weather out.png
python tools/snapshot.py <ip> weather --key <key>   # the key: see §1 (else $WEATHER_KEY, else the harness's file)
```

- Screens: `weather`, `extras`, `status`, `radar`, `update`, `alert`, `settings`, `hourly0`…`hourly6` (the hourly view of that
  day, list scrolled to the top; unchanged if the view is open), `current` (the one shown). For text-fit checks of
  screens a test can't safely open: `settings1`…`settings3` (the Settings list scrolled down by one screen each),
  `phone` (the settings-page QR overlay), `setup0` / `setup1` (the Wi-Fi setup pages, texts only: no access point or
  Easy Connect is started; the QR is blank).
- Outside the round panel is tinted red, so anything the circle cuts off stands out (`--square` to skip).
- The IP is in the log: `web: Settings page: https://<ip>/`. Each capture logs `web: snapshot <name> 466x466 sent`.
- It shows the screen's state, not what a person would see after interacting. For example, the status page's checks
  only run when someone swipes to it. For gestures, scrolling and animations, ask the user to act during a log
  window and send a photo.
- Cost: about 434 KB of PSRAM while rendering, about 650 KB sent, 2–3 s.

## 4. Other checks

- **Update over Wi-Fi end to end:** flash a build labelled `vX.Y.Z-rc.0`, tag `vX.Y.Z-rc.1`, set the channel to Beta,
  install from the board, then look for `ota: Update installed, restarting`, `ota: Running … from ota_N` and,
  60 s later, `marked valid (no rollback)`.
- **Service health:** the status page (two swipes right of the weather screen) shows every external service. In
  the log, `svc:` lines record failures and recoveries, and `svc: probe <name>` lines record the checks made when
  the page opens.
- **Alert sounds:** `POST /api/sound {"test":1}` (2, 3) plays the yellow / orange / red sound; the log shows
  `sound: alert sound, level N`. Check the microphones still measure afterwards (`presence: level` lines). The
  "new alert" decision (once per alert, quiet hours, red exception) needs fake alerts: see CLAUDE.md.
- **Presence (dim / off / wake):** shorten the delays through the API for the test
  (`POST /api/presence {"dim_s":10,"off_s":20}`), have the user stay quiet and still, then restore the values read
  from `GET /api/presence` beforehand. Look for `presence: ACTIVE -> DIM`, `DIM -> OFF`, `picked up / moved`.
- **Memory, CPU, render times:** see [DIAGNOSTICS.md](DIAGNOSTICS.md) (`reboot.request` + `tools/diag_summary.py`).
- **Settings page:** the Playwright tests below, before every build that changes `main/web/index.html`.
- **Host LVGL simulator:** used in the first session for layout work (see CLAUDE.md). It isn't in the repo. The
  snapshot endpoint now covers most of what it was used for, with the real fonts and data.

## 5. Settings page tests (`tools/webtest/`)

Browser tests of `main/web/index.html` on a phone-sized Chromium (Pixel 7), against a mock display, no board
needed:

```bash
cd tools/webtest
npm install                      # first time: Playwright + Leaflet 1.9.4
npx playwright install chromium  # first time
npm test
```

- `mock-server.js` serves the real page and answers `/api/*` like `web.c`, with the state in memory
  (`POST /__reset`, `GET /__state` for the tests). Started by `playwright.config.js` on port 8099. Keep it in step
  with `web.c` when an API changes.
- `tests/fixtures.js` answers the outside services locally, so runs are repeatable and need no internet: Leaflet
  from `node_modules/leaflet` (same version and integrity hashes as the page), a grey tile for OpenStreetMap, fixed
  answers for the city search and reverse geocoding. The `noInternet` option makes every outside request fail (a
  phone on the setup network). Any page error or console error fails the test.
- `language.spec.js` checks the French page (texts, the display's language, decimal comma) and saves French review
  shots. For the display: `POST /api/units {"lang":"fr"}` (put the user's language back after), then snapshot every
  screen; French text is longer and is where layouts break. *the page in Inuktitut fits* checks the draft
  Inuktitut page at phone width (no English left, no overflow). For the display, see docs/translations/README.md
  (`{"lang":"iu"}`, then snapshot every screen including `settings1..3`, `phone`, `setup0/1`).
- Every test saves a full-page screenshot in `tools/webtest/shots/`; the `review:` test saves the Places card (list
  and editor) for design review. Look at them before flashing a page change.
- Node.js is installed on the PC (`C:\Program Files\nodejs`); in PowerShell put it first in `PATH` if a shell
  started before the install doesn't find `npx`.
- Don't name a custom fixture option `offline`: it's Playwright's own option and takes the whole browser offline,
  mock display included.

## 6. Harness (`tools/harness/`): everything, without a person

One command tests the board end to end: screens, settings page, performance, and the Wi-Fi setup paths that broke
on October 1. Needs the flash helper running; the board's firmware must have the test console (every build since
v1.10.0).

```bash
python tools/harness/harness.py                       # all suites, on the firmware already on the board (~12 min)
python tools/harness/harness.py --flash build/v55/weather_amoled.bin   # flash a build first
python tools/harness/harness.py smoke wifi_runtime    # some suites
python tools/harness/harness.py wifi_setup --phone    # + Easy Connect with a real phone (asks the user)
python tools/harness/harness.py --ota v1.11.1-rc.1    # install a published release with the display's updater, test it
```

**Testing a release from GitHub** (`--ota vX`): the display's own updater installs it, as a person would from
Settings (*Check for updates*). The harness asks the display to check its channel every minute until it offers vX (CI
and the Pages site take ~5 min after a tag; `--ota-wait` minutes, default 20), installs, waits for `ota: Running vX`
and for `marked valid` (60 s: a restart before that rolls the update back), then runs the suites with `--expect vX`.
An rc needs the display on the Beta channel; the harness doesn't change the channel. The board must be running a
version below vX (a test build labelled above it, e.g. `v1.10.2-scroll.6` against `v1.10.1-rc.4`, is never offered
the release: the firmware compares the numbers first, then rc numbers, and test labels count as rc −1).
CI from the PC: `gh run list --repo TheMonkeyz/esp32-s3-weather --limit 5`, `gh run watch <id>` (GitHub CLI, signed
in as TheMonkeyz; Git Bash doesn't have it on its PATH: `"/c/Program Files/GitHub CLI/gh.exe"`, or PowerShell).

Exit code 0 = all passed and no performance regression. The report is `tools/harness/reports/<date>/report.md`
(git-ignored) with screenshots, `results.json`, and the log of each failed test. If the flash helper is already
logging, the harness reuses that window; otherwise it restarts the board (`reboot.request`, 40 min window) and stops
the window at the end.

| Suite | What it proves |
|---|---|
| `smoke` | console answers, firmware version, Wi-Fi up, settings API |
| `navigation` | swipes and taps land on the right screen (weather ↔ extras ↔ status, radar, Settings by long-press, hourly by tapping a day); the ends bounce back, a short slow drag snaps back; places (from the first place: drag up / down, the first one bounces) and hourly days (left / right) change by one (`page`); snapshot of every screen incl. `settings1..3`, `phone`, `setup0/1`, `update` |
| `web` | the Playwright suite (`tools/webtest`) and the live API on the board; the page must arrive whole; who may change things (`main/web.c`): 403 for a POST over plain HTTP, 302 to the device itself, 401 without or with a wrong key, 415 for a non-JSON POST, 421 for another Host, 401 for a snapshot without the key, 200 with it |
| `perf` | boot stage times and internal RAM, heap low points, full-screen render bench (best of 3), radar first frame and lightning, frame rate of each move (`fps`: screen to screen, places, days, the hourly list and Settings scrolling; for drags also `drag_fps` and `drag_start_ms` from slide.c's log line, a place drag back 2 s after a switch, and a place drag 0.8 s after new data: console `dirty`); compared with `tools/harness/baseline.json`. The radar animation plays at 3 fps by design: not measured |
| `presence` | dim, off and wake with short delays set through the API (the user's put back after, even on a failure): ACTIVE → DIM → OFF → `wake`; three fades up during a swipe (the brightness command from core 0 while LVGL sends bands from core 1), and `where` must show `raw_phase=0` after each |
| `firstrun` | `hint next-boot` + restart: the "Choose your location" settings QR comes up by itself after the first forecast, then the gesture hint; a tap closes each (places and the real once-only flags untouched) |
| `wifi_runtime` | network lost while running: retries go on; long-press opens setup and **pauses them**; tap closes it; reconnects, and an update check follows at once |
| `wifi_setup` | start-up with the network unreachable (the October 1 path): setup after 30 s, no retries while open, **the PC joins the setup network like a phone** (DNS answers every name with 192.168.4.1, the Android check gets the 302, the page and `/api/config` load without the places' coordinates, the Sound card's API answers, a snapshot is refused, the PC is not dropped for 15 s; it joins with this display's own password, read from `wifi status`), Easy Connect listens on the router's 2.4 GHz channel as the PC sees it, the setup network works again after Easy Connect (DNS socket bug), tap → 30 s retry → setup again, network back → weather screen. `--phone` adds the real Easy Connect scan |
| `wifi_setup` (2) | `setup_stops_opening_by_itself`: a boot that can't reach the network with a 60 s automatic-setup window (`wifi offline-boot-short`; 15 min normally): setup opens by itself, then no longer after the window (*Still trying*), a long-press still opens it, recovery |

How it works:

- **Test console** (`main/testcon.c`, USB only): `ping`, `screen`, `page` (place and day shown), `tap X Y`,
  `press X Y [ms]`, `swipe left|right|up|down`, `drag X1 Y1 X2 Y2 [ms]`, `wake`, `presence`, `wifi
  status|offline|offline-boot|online`, `portal windows-quiet`, `fps [reset]` (frames and animation fps since the
  reset), `where` (display breadcrumbs, takes no lock), `memspeed` (PSRAM / internal copy speeds), `heap`, `bench`,
  `profile` (§8, profiler builds only), `pictest` (slide.c's picture of the screen shown against a fresh rendering:
  `slide: pictest rows_differ=N first=Y`), `dirty` (what new data does to slide.c's pictures: every hidden one out
  of date and the screen shown redrawn), `reboot`, `help`. Answers are log lines `test: …`. Simulated touches enter at the
  touch controller read (`touch_inject()`), so wake-up, long-press and gestures run the real code. `wifi offline`
  points the station at a network that doesn't exist (saved credentials untouched); `offline-boot` does it for the
  next boot only (flag in RTC memory) so the real start-up path runs.
- **Flash helper link** (`monitor.ps1`): it writes `serial_live.txt` line by line and sends each line of
  `serial.send` to the board (echoed as `> command`). You can use it by hand:
  `echo screen > serial.send`, then read `serial_live.txt`.
- **PC Wi-Fi as a phone** (`board.PCWifi`): `netsh wlan` adds a profile for Weather-Setup, connects, and deletes it
  afterwards. The PC stays online through Ethernet. Before joining, the harness sends `portal windows-quiet` so
  the display answers Windows' own connectivity check: otherwise Windows opens a browser tab (msftconnecttest →
  msn.com, because the PC is online). Until restart, and only that one URL; phones still get the portal.
- **Boot metrics:** `boot_s.forecast_shown` (the active place's forecast on screen, limit 20 s) is what the user
  waits for. `boot_s.first_weather` is the `diag: mark first weather` line, logged after the whole first round (every
  place, alerts, air quality): it follows Open-Meteo's speed (~4 s per request on a slow evening, 17.8 s total
  once), so its limit is a loose 30 s.
- **Baseline:** metrics are checked against `baseline.json` (`min`/`max` per metric, `ref` = value when it was set,
  `note` = why that limit). Nothing passes silently (since v1.12.0): a baseline metric whose suite ran but which
  wasn't measured is **MISSING**, a measured metric without a limit is **NEW**, and both fail the run like a
  **REGRESSION** (in October 2026 a third of the metrics had no limit, and a reused log window dropped every boot
  metric without a word). A test that doesn't measure something on purpose says so with `ctx.skip(pattern,
  reason)` (one place: no place drag; no new radar frame; a reused log window: no boot metrics), and the report
  shows "skipped: reason". `--update-baseline` writes `baseline.proposed.json`: this run's numbers as `ref`, limits
  and notes kept, and a proposed limit (marked) for new metrics; review it and copy it over `baseline.json`.
- **Log cursors:** the harness marks the log at the start of each test (`log.mark()`) and checks everything after
  it for `rst:0x` (an unexpected restart fails the test, with the decoded backtrace) and for fallback warnings
  (`drag: PSRAM busy`, a touch loop's safety cap, a raw band timeout), which go to the report's notes. Tests keep
  their own positions (`at = len(ctx.log.lines())`, `wait(..., start=at)`, `count(..., start=at)`): a test that
  moved the mark hid a crash from the restart check.
- **Memory floors:** `internal_min_kb` (read in `perf`, floor 8 KB) and `internal_min_kb.reconnect` (read at the end
  of `wifi_setup`, after the reconnect path with three TLS clients and the forecast parse: 5 KB on October 2).
- The harness's own logic has unit tests: `python -m unittest discover -s tools/harness -p "test_*.py"`.

Pitfalls met while building it:

- **Microsoft Store Python** virtualises `AppData\Local` for itself and its child processes: Playwright started from
  the harness couldn't see its browsers. The web suite uses a copy in `tools/webtest/.browsers` (git-ignored; copy
  `%LOCALAPPDATA%\ms-playwright\chromium_headless_shell-*` there after a Playwright update).
- A console command must never run heavy work on the console task: `bench` on its 3 KB stack reset the board; it
  now starts the bench task. A **PSRAM stack** crashed on `wifi online` (NVS read = flash busy): keep it internal.
- `flash.status` says `logging` ~2 s before `serial_live.txt` is recreated: the harness deletes the old one first.
- An event can be logged *before* the console's reply to the command that caused it: replies are matched from the
  command's own position, events from the test's read position (`Log.wait(start=…)`).
- The setup screens can't be snapshot while the display is offline (snapshots use the home network); the
  navigation suite captures them with the `setup0/1` pseudo-screens.
- Unexpected restarts fail the test even if every check passed. The report decodes the backtrace with
  `addr2line` against `build/v55/weather_amoled.elf` (wrong for any other build).
- **A command that gets no answer, or answers `display busy`, makes the harness send `where`**, which takes no lock:
  the failure then says where the display stuck (`slide_phase`, `raw_phase`, `raw_band`, `lvgl_inflight`, the LVGL
  task's state). That is how the raw-frame hangs of the v1.11.0 work were found (see ARCHITECTURE, "Display
  pipeline").
- A simulated **tap lasts 120 ms**: a 60 ms tap could fall entirely inside a 100 ms redraw and never be seen.
- The board's **automatic render bench** (45 s after boot) blocked the screen for 1.5 s and swallowed the swipes the
  harness made meanwhile; it is off since v1.11.0 (`BENCH_AT_S` 0) and `perf` runs it on request, keeping the best of
  3 runs (a run is "postponed" while a redraw is going on).
- `fps` has its own counters: `diag`'s 60 s report used to reset them in the middle of a measurement.
- After a **place change**, the radar saves the new location's map to flash (both cores pause in bursts for ~3 s): a
  tap then can go unseen, and a drag measured then crawled (3 fps, first frame after 2 s). `perf` waits until the
  radar has been quiet for 5 s with no map preload running (`radar_settled`), and taps the forecast twice if needed.
- **A forecast outage is not a firmware failure:** with no forecast the display shows its message screen ("Can't
  reach the forecast service. Retrying."); `go_weather` waits up to 3 min for it to clear and notes it. On October 3
  an Open-Meteo bad patch (10 of 16 forecasts timed out on the display while the PC got answers in 0.07 s) failed
  five tests of rc.6's run that way; four restarts later 8 of 8 forecasts worked.
- **A wait must outlast the firmware's own retry:** after the network comes back the weather screen needs the first
  forecast, and a failed fetch is retried after 30 s. A 30 s wait failed v1.11.1's run when Open-Meteo timed out
  once; it now waits 75 s and notes the retry. Check the firmware's interval before choosing a timeout.
- Drag tests check the result with `page`, not by the screen name: a place or day change stays on the same screen.
  They must start where the move is possible: the board keeps the place shown across restarts, and from the last
  place "drag up" bounced and the check still passed (October 2). `navigation` first goes back to the first place.
- `swipe_fps` counts every frame less than 250 ms apart, LVGL's redraws after a move included (a new place's
  data: 30 fps for a 57 fps drag). `drag_fps` (from the first frame) and `drag_start_ms` are the drag alone.
  `drag_start_ms` is checked right after the minute too: the minute tick only marks the places' clock rows,
  rendered at once (in v1.11.0 the harness skipped the check for 6 s after the minute).
- List scrolls: `scroll_frame_ms` (from slide.c's `scroll:` line: render + move + send per frame) is the number to
  watch; their `swipe_fps` includes the slow end of the flick (less than a pixel per frame). After the drags and
  each list scroll the harness runs `pictest`: the picture of the screen must equal the screen.
- **Checking pictures pixel by pixel:** `GET /api/snapshot?screen=picture` (slide.c's picture of the screen shown)
  against `?screen=current` (a fresh rendering). Parse the BMPs and compare rows (no PIL on this PC: plain
  `struct`/`zlib`, `tools/snapshot.py`'s `bmp_to_png`). It found the smeared hourly graph after a bounce. On a
  live screen (the status page's ages tick every second) two snapshots differ anyway: use `pictest`, which compares
  at one instant.
- **Don't restart a board in the first 60 s after an update:** the new firmware is still "pending verify" and the
  bootloader rolls back to the previous one. On October 1 the harness restarted rc.2 right after the user installed
  it, then tested the old build and reported a pass. Now: `GET /api/update` has `pending_verify` and `uptime_s`; the
  harness waits until the update is confirmed (75 s blind wait for firmware without the field), fails if the version
  changed across its restart, prints the version it tests, and `--expect vX` fails on any other version. Use
  `--expect` when testing a release.

## 7. Host unit tests (`tests/host/`)

Firmware C files built with the PC's gcc (WSL Ubuntu here, plain Linux in CI) against small shims of the ESP-IDF
headers (`tests/host/shim/`) and a scripted HTTP client (`fake.c`: every request gets one reply, an HTTP status, a
transport error, or "no memory for a client"), with AddressSanitizer:

```bash
wsl -d Ubuntu --cd /mnt/c/Users/<you>/ESPDEV/weather_amoled/tests/host -- make     # IDF_PATH defaults to /mnt/c/Espressif/esp-idf (cJSON)
```

- `test_weather.c`: Open-Meteo replies whole, partial (a daily array missing or shorter: it crashed every fetch),
  null values, not JSON, HTTP errors, no client.
- `test_alerts.c` includes `alerts.c` itself (its static parsers): the severity cap (a red warning listed fifth is
  kept), the beep rule (once per warning, again if worse, re-issues silent), the region map key, the shape scanner on
  `-]` (it looped forever), failures.
- `test_utf8.c`: names and texts cut without splitting a character.
- `test_i18n.c` includes `i18n.c`: every text exists in every language (an empty one showed as nothing) with the
  same printf conversions as the English, and `tr()` falls back to English.
- `test_version.c`: what the updater offers (`version.c`: rc order, test labels below every rc, git-describe builds)
  and which alerts sound when (`sound_wanted()`: levels, quiet hours across midnight, equal times, red always).
- `test_png.c`: `png_rows.c` against PNGs built with miniz 3.0.2 (the ROM's `tinfl` is miniz's inflate; the
  Makefile downloads it once into `build/`): grey, RGB, palette + tRNS, grey + alpha, RGBA, all five row filters,
  data over two IDAT chunks, a wrapped chunk length, refused inputs. (The wrap only bit a 32-bit `size_t`: on the
  64-bit host that case guards the behaviour, not the original overflow.)
- Not covered yet: `web.c`'s handlers.
- CI runs them on every push (`host-tests` job, cJSON fetched at ESP-IDF's version); a release needs them to pass.
- A fix that can be reproduced off the board gets a case here; check that the case fails on the old code
  (`git show HEAD:main/x.c`) before calling it a test.

## 8. Profiling LVGL rendering (`profile`)

Used in the v1.11.0 frame-rate work to see where a frame's time goes, per function and per draw task type. A local
experiment, never committed in a build:

1. `python tools/harness/lvgl_profile_patch.py apply`: tags each draw task type (`t_fill`, `t_label`, `t_image`…) in
   `managed_components` (not in git; `revert` undoes it).
2. In `build\v55\sdkconfig` only: `CONFIG_LV_USE_PROFILER=y`, `CONFIG_LV_USE_PROFILER_BUILTIN=y`,
   `CONFIG_LV_PROFILER_INCLUDE="src/misc/lv_profiler_builtin.h"`. Build and flash.
3. Send `profile` to the test console (`echo profile > serial.send`): one render-only frame of each screen, the
   trace between `PROFILE-BEGIN <screen>` and `PROFILE-END <screen>` lines.
4. `python tools/harness/profile.py serial_live.txt`: per screen, each marker's count, inclusive and self time.
5. Revert the patch and the sdkconfig lines (the profiler slows rendering), rebuild.

What it showed (the weather screen, ~75 ms a frame): no single hot spot. The time is spread over the 15 bands of 32
lines, each walking the whole object tree, and TinyTTF glyphs. Two LVGL images of the whole screen still cost ~35 ms a
frame. Hence pictures copied straight to the panel (`slide.c`) rather than faster widgets. `memspeed` gives the copy
speeds that bound that approach (~8 ms per screen from PSRAM).
