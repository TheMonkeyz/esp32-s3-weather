# Working on a Mac

For a designer improving the display's look on a MacBook, and for the AI agent helping her. The rest of the docs were
written on the owner's Windows PC (COM5, PowerShell, WSL, `C:\` paths). This file says what changes on a Mac.
Where it disagrees with CLAUDE.md or docs/TESTING.md, **this file wins on a Mac**.

**Status (October 4, 2026):** prepared on Windows. **Nobody has run it on a real Mac yet.** GitHub's macOS runner
(`.github/workflows/macos.yml`, Apple Silicon) runs the setup script on a fresh machine, then builds the firmware, runs
the host tests, the harness's unit tests, the settings page tests and builds the emulator, on every push to a branch
other than main. First green run on October 5 (macOS 15.7, arm64, 8 min): setup, build, 1,283 host checks with Apple's
clang, the flash helper's 16 tests, 30 page tests, the emulator. What no runner can check: the board on USB, the
serial log and the harness against a real display.
Section 5 lists those untried pieces, most likely to break first.

| | On the Mac | Checked by |
|---|---|---|
| Firmware build (ESP-IDF v5.5.4) | yes | macOS CI |
| Flash + serial log (`tools/flash_helper.py`) | yes | loopback test in CI; **never on a real board from a Mac** |
| Emulator (the display in the browser) | yes | macOS CI |
| Settings page tests (Playwright) | yes | macOS CI |
| Host unit tests (`tests/host`) | yes | macOS CI |
| Harness on the board | all suites except `wifi_setup` | not yet |
| Screenshots from the board (`tools/snapshot.py`) | yes | not yet |
| `wifi_setup` suite | **no**: it puts the computer's Wi-Fi on the display's setup network; a MacBook has no Ethernet, so it would lose the display and the internet. The owner's PC runs it | |
| Releases, tags, `harness.py --ota` | **no**: the owner's job | |

---

## 1. One-time setup (about an hour, mostly waiting)

**0. GitHub access.** You need a GitHub account. The owner (TheMonkeyz) invites you to the repository; accept the
invitation (email, or https://github.com/notifications). You can then push your own branches. Release tags are
protected: only the owner can make a release.

**1. Xcode command line tools.** In Terminal: `xcode-select --install`, accept, wait. (Already there? It says so.)

**2. Homebrew** (https://brew.sh). Paste its install line in Terminal. It asks for your Mac password. At the end it
prints "Next steps": run those two lines too. Close and reopen Terminal.

**3. Get the project.** Use a folder **without spaces** (ESP-IDF's build doesn't support spaces in paths) and **outside
Desktop / Documents** (iCloud may sync or offload build files there):

```bash
git clone https://github.com/TheMonkeyz/esp32-s3-weather.git ~/esp32-s3-weather
```

**4. Install everything else.** This takes 30-60 minutes and about 4 GB. Safe to run again if it stops:

```bash
cd ~/esp32-s3-weather
bash tools/mac/setup.sh
```

It installs build tools (Homebrew), ESP-IDF v5.5.4 in `~/esp/esp-idf-v5.5.4`, the page tests' browser, and
Emscripten in `~/emsdk` for the emulator. It also adds a `get_idf` command to your shell. At the end it runs the
doctor (below). **Open a new Terminal window afterwards** so `get_idf` exists.

**5. GitHub sign-in and your name on commits:**

```bash
gh auth login
git config --global user.name "Your Name"
git config --global user.email "you@example.com"
```

**6. Your own branch.** Never work on `main`:

```bash
cd ~/esp32-s3-weather
git switch -c design/ui-refresh
```

**7. Check:** `bash tools/mac/doctor.sh`. Every line should say PASS or INFO. A FAIL says what to run. Paste the whole
output when asking for help.

## 2. The daily loop

Two Terminal windows (or tabs), both in `~/esp32-s3-weather`:

- **Window 1, the flash helper** (only when the display is plugged in). It flashes the display and records its log
  whenever asked. It's the only program that may use the display's USB port. Leave it running:
  ```bash
  get_idf
  python tools/flash_helper.py
  ```
  Ctrl-C stops it. q or Esc ends a log early.
- **Window 2, your work.** Start every new window with `get_idf`.

### A. The emulator: fastest, no display needed

The display's real screens, compiled for the browser. A change shows in seconds instead of a flash:

```bash
get_idf
idf.py -B build/v55 -D SDKCONFIG=build/v55/sdkconfig build     # once: fetches LVGL, which the emulator uses
source ~/emsdk/emsdk_env.sh
make -C web/emu -j8                                           # again after each change (only the changed files)
python3 -m http.server 8765 -d web/emu/build                  # then open http://localhost:8765
```

Drag with the mouse to swipe; click a day for the hourly view; long-press for Settings. `?place=47.57,-59.13,Port%20aux%20Basques`
at the end of the address opens a place (see web/emu/README.md). Limits: the emulator's speed says nothing about the
display's (60 fps on the display is a separate check), and the phone's settings page isn't in it (see B).

### B. The settings page (the page your phone shows)

`main/web/index.html`. To look at it in a browser against a pretend display: `cd tools/webtest && node mock-server.js 8099`,
then open http://localhost:8099. Its tests (screenshots of every test land in `tools/webtest/shots/`):

```bash
cd tools/webtest && npm test
```

### C. On the display

1. **Label your build `v1.99.0-design.N`** (`.1`, then `.2`, `.3`… for each build you flash). A build labelled
   below the newest release is offered that release as an update, and installing it replaces your work. Releases
   come often (v1.14.2 two days after v1.12.3), so a label like "the next version up" goes stale within days;
   v1.99 stays above every release for a long time (`tests/host/test_version.c` checks it):
   ```bash
   printf v1.99.0-design.1 > version.txt && touch CMakeLists.txt
   ```
   It only labels the build on your display: the owner picks the real version number when your work is released.
2. **Build** (the first time ~5-10 min, then ~1 min):
   ```bash
   get_idf
   idf.py -B build/v55 -D SDKCONFIG=build/v55/sdkconfig build
   ```
3. **Flash** with the helper running in window 1:
   ```bash
   python tools/flash_helper.py stage build/v55     # copies the build into firmware/ and checks it
   echo 120 > flash.request                         # flash, then 120 s of log in window 1
   ```
   Without the helper running, the same in one go: `python tools/flash_helper.py flash 120`.
4. **First time on a display:** it shows "Wi-Fi setup" with a QR code. Follow README.md, "First-time setup". Use a
   2.4 GHz network that **your Mac is on too** (not a guest network: those keep devices apart).
5. **Screenshots from the display.** It renders the screen itself, so no photo is needed. The display's address is
   in the log (`net: Connected, IP …`):
   ```bash
   python tools/snapshot.py 192.168.x.y weather      # also: extras, status, radar, settings, hourly0 … (docs/TESTING.md §3)
   ```
6. **The full check, the harness.** It drives the display by itself (swipes, taps, screenshots, speed) for ~12 minutes
   and writes `tools/harness/reports/<date>/report.md`. Helper running in window 1:
   ```bash
   python tools/harness/harness.py --flash build/v55/weather_amoled.bin     # flash, then everything
   python tools/harness/harness.py smoke navigation                          # quicker: screens and gestures only
   ```

### Before you share a change

- Text on the display goes in `main/i18n_strings.h`, in **every** language (English, French, Inuktitut), never
  typed directly in the screen code. Check the French in screenshots: it is longer and wraps on the round screen.
- The font has no symbols or emoji: small icons are drawn shapes.
- Run what your change touches: `npm test` in `tools/webtest` (settings page), `make -C tests/host` (C code; after
  `get_idf`), and on the display `harness.py smoke navigation` at least.

## 3. Saving and sharing your work

```bash
git add -A && git commit -m "Weather screen: larger temperature, softer colours"
git push -u origin design/ui-refresh            # the first time; afterwards just: git push
gh pr create --draft --fill                     # once: a draft pull request the owner can follow
```

Each push runs the checks on GitHub: the firmware build, page tests and host tests on the pull request, and the Mac
run. A red ✗ on the pull request page says which failed. The owner reviews, merges and releases; **never push to
`main` and never create tags**.

## 4. Troubleshooting

Run `bash tools/mac/doctor.sh` first: most problems show up there as a FAIL with the fix.

| Symptom | Cause, fix |
|---|---|
| `idf.py: command not found`, `No module named serial`, `make: *** no cJSON` | This window didn't load ESP-IDF: `get_idf` (or `. ~/esp/esp-idf-v5.5.4/export.sh`) |
| `get_idf: command not found` | Open a new Terminal window after setup.sh; else `source ~/.zshrc` |
| setup.sh: `CERTIFICATE_VERIFY_FAILED` | A python.org Python lacks certificates: run its `Install Certificates.command` (Finder → Applications → Python 3.x), or use Homebrew's Python first in PATH (Espressif's macOS setup docs) |
| `tool xtensa-… has no installed versions` on Apple Silicon | `softwareupdate --install-rosetta --agree-to-license`, then `~/esp/esp-idf-v5.5.4/install.sh esp32s3` (Espressif's docs) |
| Build errors mentioning spaces in a path | Move the project to a path without spaces (`~/esp32-s3-weather`) |
| `flash_helper.py ports` shows no "ESP32-S3 (this board)" | A charge-only cable (try another), a hub (plug in directly), or the display is off. Last resort: hold BOOT, tap RESET, release BOOT, flash again |
| `could not open port`, `Resource busy` | Another program holds the port: only one helper; quit Arduino, `idf.py monitor`, `screen`. `lsof \| grep usbmodem` shows who |
| Flash OK but the log stays empty, or shows `waiting for download` | Press the display's RESET button. Tell the agent (section 5, item 2) |
| Harness: `the flash helper did not start logging` | Window 1's helper isn't running (or crashed: read its window) |
| Snapshot / harness: timeout, `No route to host` | The Mac can't reach the display. Same Wi-Fi, not a guest network? macOS 15 and later: **System Settings → Privacy & Security → Local Network**: allow your terminal app (Terminal, iTerm, VS Code). Test: `curl -sk https://<ip>/api/config` |
| The browser warns about the display's settings page | Expected: each display makes its own certificate (README, "Security notes") |
| Emulator page blank | Serve it (`python3 -m http.server …`), don't open the file. Port taken: use 8767 |
| `make -C web/emu`: `no LVGL` | Build the firmware once (it fetches LVGL into `managed_components/`) |
| The display offers an update / shows an old version | Version label (section 2C.1). The first log lines say what runs: `ota: Running vX from ota_N` |
| Build fails after pulling the owner's changes | If `sdkconfig.defaults` changed: `rm build/v55/sdkconfig` and build again. Else `idf.py -B build/v55 fullclean`, build |
| `git push` → 403 | The GitHub invitation isn't accepted yet, or `gh auth login` was for another account |
| `git push` → rejected (non-fast-forward) | `git pull --rebase`, then push |
| `git push` to `main` → rejected (`protect main` ruleset) | Expected: `main` only takes pull requests with the owner's approval. Push your branch and open a pull request |

---

## 5. For the AI agent helping her

Read CLAUDE.md, docs/TESTING.md and this file. You're helping a **designer**: she knows design, maybe not terminals,
ESP-IDF or git. Give one step at a time, a command to paste, what she should see, and what to send back. Post a short
progress note before anything long (a build, a flash, a harness run).

**What changes from the Windows docs**

| Windows docs say | On her Mac |
|---|---|
| `. C:\Espressif\esp-idf\export.ps1` | `. ~/esp/esp-idf-v5.5.4/export.sh`. Each Bash tool call is a fresh shell: prefix every `idf.py` / `python` / `make -C tests/host` command with `. ~/esp/esp-idf-v5.5.4/export.sh >/dev/null &&` |
| `build\v55`, `firmware\in\` | `build/v55`, `firmware/in/` |
| COM5 | `/dev/cu.usbmodem…` (the S3's own USB, VID 303A, no driver). `python tools/flash_helper.py ports` |
| `start_flash_helper.bat` (`flash_helper.ps1` + `monitor.ps1`) | `python tools/flash_helper.py` in its own terminal. Or run it yourself with `run_in_background` (then no q/Esc; `stop.request` works). Same files, same protocol |
| `flash.bat auto 90` | `python tools/flash_helper.py stage build/v55 && python tools/flash_helper.py flash 90` (only while no helper runs) |
| `wsl … make` (host tests, emulator) | Directly: `make -C tests/host`, `make -C web/emu -j8` (after export.sh; emulator also `source ~/emsdk/emsdk_env.sh`) |
| `"/c/Program Files/GitHub CLI/gh.exe"` | `gh` |
| `Set-Content version.txt …` | `printf v1.99.0-design.N > version.txt && touch CMakeLists.txt` (fixed label, section 2C) |
| Lesson 17 (curl mangles non-ASCII in Git Bash) | Not a problem on a Mac |
| adb at `C:\…\platform-tools` (Easy Connect phone log) | `brew install android-platform-tools`; rarely needed for UI work |

**Her scope.** Her branch (`design/…`), commits, pushes and a draft pull request. **Not** hers, whatever older notes
say: tags, releases, `harness.py --ota`, pushing `main`, `--update-baseline` copied over `baseline.json`, the
`wifi_setup` suite, and espforge. If she changes a file shared with espforge (CLAUDE.md, "Twin files": e.g.
`display.c`, `touch.c`, `textfit.c`, `i18n.c`), say so in the pull request for the owner to port; don't ask her to.

**UI work, the lessons that matter most** (CLAUDE.md): every display text in `i18n_strings.h` in all three languages
(22l, French wording rules in "User preferences"); check the fit on the round 466×466 screen with snapshots, French
included (`POST /api/units {"lang":"fr"}`, then put her language back); wrapping labels and their neighbours (24);
content changed off-screen needs `slide_cache_dirty()`, and a redraw that changes nothing still slows the next drag
(21f, 21j); decorative objects must not catch touches (7, 18); no symbols in the font. Moves between screens are
pictures made by `slide.c` (ARCHITECTURE "Moves"): after a layout change run `pictest` / `harness.py navigation`.
The user's taste is recorded in CLAUDE.md, "User preferences learned". It's the owner's taste; her redesign may
change it on purpose, but say what changes.

**Never tried on a real Mac: suspect these first, most likely first**

1. **The port after a restart.** The S3's USB disappears and comes back at every restart (flash, `reboot`, crash,
   the harness's own restarts). `flash_helper.monitor()` reopens it for up to 15 s; if the old name is gone it finds
   the board again by vendor id (`flash_helper.log`: `the board came back as …`). Untested on a Mac. If the log stops
   with `[flash_helper] … gone and not back`, compare `flash_helper.py ports` before and after a restart.
2. **DTR/RTS when the port opens.** The helper opens with DTR and RTS low, as ESP-IDF's monitor does. If the board
   resets every time the log starts, or stays in `waiting for download`, that's it. Compare with
   `idf.py -p <port> monitor` (it works on macOS).
3. **The first seconds of log after a reset.** A restart request (`reboot.request`, the harness's start) goes through
   the test console's `reboot` with the port kept open, so the boot log is whole from `ESP-ROM` on (checked on the
   board, Windows, October 6). After a **flash**, esptool's reset re-enumerates the USB and the first ~2.5 s are lost;
   the harness then prints `flash ?` instead of `flash QIO` (harmless: QIO limits apply). If a restart request falls
   back to esptool (`flash_helper.log` lacks "Restarted through the test console"), the console didn't answer: a hung
   board, or firmware without it.
4. **Local Network permission** (macOS 15+): HTTPS to the display (snapshots, harness API) fails from an app that
   wasn't allowed. `curl -sk https://<ip>/api/config` from her terminal tells.
5. **Performance limits.** `baseline.json` came from the owner's board. Hers is the same model, so they should hold.
   Wi-Fi differences show in `snapshot_ms.*`. If a limit fails on unchanged firmware (`main`), report it; don't move
   the limit.
6. **esptool**: the helper runs ESP-IDF's (4.12 with v5.5.4 on Windows), with v4 command names.

**Debugging.** `bash tools/mac/doctor.sh` first, then the smallest command that fails, then the files:
`flash_log.txt` (esptool), `flash_helper.log` (the helper's history), `flash.done`, `serial_log.txt` /
`serial_live.txt` (the display). Find the cause in a log before changing anything (CLAUDE.md's way: three wrong
theories cost a day on Easy Connect). A fix to the Mac tooling (`tools/flash_helper.py`, `tools/mac/`, `macos.yml`)
goes in its own commit starting `macOS:`, with a line in "Lessons from the Mac" below. Those commits help everyone
after her.

## Lessons from the Mac

What happened on the first real Mac. Add `- YYYY-MM-DD: what happened → the fix` lines here.

- (none yet)
