# Testing on the device

How a change gets built, flashed, observed and shown before it is called done. Every change is verified on the
board (log, snapshot, or both). The Windows PC drives the board on **COM5**; the board is on the home network.

## 1. Build

| Where | ESP-IDF | Components | Use |
|---|---|---|---|
| CI (`.github/workflows/firmware.yml`) | v5.4.2 | component manager (`main/idf_component.yml`) | releases, the web flasher |
| Windows PC, Claude Code | v5.5.4 (`C:\Espressif\esp-idf`) | component manager | test builds |
| Cowork cloud container | v5.4.2 | vendored (see CLAUDE.md) | test builds when no PC shell |

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
  `curl -sk -X POST -H 'Content-Type: application/json' -d '{"temp":"f"}' https://<ip>/api/units`, then snapshot.
  Put the user's settings back afterwards.
- After changing `sdkconfig.defaults`, delete `build\v55\sdkconfig` and run `idf.py ... reconfigure`: an existing
  sdkconfig keeps its old values (an option that exists as "not set" ignores the new default).
- The PC's v5.5.4 differs from CI's v5.4.2. A test build checks the change, not the release toolchain. Flashing it
  keeps the board's v5.4.2 bootloader (the helper flashes `firmware\bootloader.bin`), which is what boards updated over
  Wi-Fi have.

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
python tools/snapshot.py 192.168.1.156 status            # -> snapshot_status.png
python tools/snapshot.py 192.168.1.156 weather out.png
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
