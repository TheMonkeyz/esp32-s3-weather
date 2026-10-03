# Project evaluation, October 2-3, 2026 (v1.11.1, commit 28310af)

Read-only review of the `weather_amoled` firmware, tooling and docs by Claude (Fable 5.1): 15 area reviewers in
parallel, every medium/high finding checked by two independent verifiers (one re-reads the code and tries to refute
the claim, one judges the severity for *this* project), a completeness critic, and four extra readers for the gaps
the critic named. 144 agents, nothing changed, built, flashed or sent to the board. The companion file
`FIX-PLAN-2026-10-02.md` turns this into work items for a fresh session.

**Coverage.** All fifteen areas: weather/hourly/extras/status/settings screens (`ui.c`, `pager.c`), the picture
pipeline (`slide.c`, `display.c`, `touch.c`), radar and PNG decoder, Wi-Fi and DNS (`net.c`, `components/dns_server`),
settings page and API (`web.c`, `index.html`, `tlscert.c`), updates and release pipeline, data core (`main.c`,
`weather.c`, `alerts.c`, `svc.c`, `config.c`), peripherals (`presence.c`, `sound.c`, `imu.c`, `diag.c`, `lvgl_mem.c`,
`testcon.c`), i18n, tooling and tests, docs and repo hygiene, build configuration, a security pass, a mechanical
robustness sweep, and a product/UX pass. Gap round: internal RAM low point from the harness reports, task stack margins
from the logs on disk, time/timezone/data semantics end to end, persistence and power loss.

**Verification.** 183 distinct issues were found. The 62 rated medium or high were verified: 62 confirmed, 0 refuted.
The impact judge lowered 1 high to medium and 26 mediums to low, and raised none. The 149 low/info items were not
verified (listed at the end, by area). Line numbers are as of commit 28310af; a few drifted by a line or two.

## Verdict

For a four-day-old project this is in unusually good shape, and the second pass confirmed it: every outbound HTTPS
client verifies the certificate bundle; inputs from the phone are type-checked and clamped; the move pipeline's
documented numbers all match the code; the three languages are mechanically complete and the Inuktitut generator
round-trips byte for byte; the data path keeps per-place time zones right; OTA has real rollback; and the harness
drives the real gesture code and has caught real bugs. The critic judged the overall picture "right and well
grounded" and not skewed, with one caveat: all reviews were static reads, and the runtime evidence sitting in the
repo (harness reports, serial logs) answered several open questions and exposed two things no reviewer saw (the
internal RAM floor and the task stack margins), which the gap round then covered.

The weaknesses cluster in six places:

1. **One shipped bug** (the only high): the settings page's Install button never appears when an update carries
   release notes, which every release does. Users can still update from the display.
2. **Security model is "anyone on the LAN", undocumented, and leakier than the docs say.** The full API, Wi-Fi
   credentials and OTA install included, is served unauthenticated over plain HTTP on the home network (the
   architecture doc says port 80 only redirects); the setup access point, with a published password, comes up by
   itself during router outages and exposes the same API; the captive portal's DNS server now answers on the home
   LAN too.
3. **Touch-chip blind spots left in two loops.** The project learned that the CST9217 NACKs instead of reporting
   "up" and fixed the main drag and scroll loops, but the PSRAM-busy drag fallback can spin forever holding the
   display lock, and the zoom's swipe detection has the same gap. `display_brightness()` also breaks the project's
   own "no esp_lcd call while a band is in flight" rule from the other core.
4. **Memory and stack margins are thinner than the docs claim.** Internal RAM bottoms at 8-10 KB in a normal boot
   and reached 5 KB on the reconnect path (docs still say 28-30 KB); 46 radar palette structs hold 48 KB of internal
   RAM that could be in PSRAM; the HTTPS server renders whole screens for snapshots on a 7 KB stack (992 B spare
   seen); the alert region map runs TLS tile downloads on the 6 KB main task; the Easy Connect success path runs
   on the 2.3 KB system event task. Seven `esp_http_client_init()` results are unchecked.
5. **Data semantics at the edges:** alerts are capped at four *before* the severity sort (a red warning can be
   dropped); "new alert" keys on an id that changes at every Environment Canada re-issue; the hourly view and the
   "Today" column index yesterday's data between midnight and the next refresh; a forecast failure is invisible on
   the weather screen; a brand-new display shows Québec City with nothing leading to the settings page.
6. **The release gate has holes:** nothing runs in CI (not even the board-free page tests); the harness never fails
   on metrics without a baseline entry or on metrics a run did not produce; its internal RAM check never samples
   the lowest path and its floor equals the observed worst case; `--update-baseline` erases hand-tuned limits;
   in-test log marks hide crashes; the rollback window confirms any image that stays up 60 s before Wi-Fi is even
   started.

Documentation accuracy is otherwise high (about two dozen numeric claims checked, nearly all exact), with staleness
from the last two days: the QIO bootloader story (TESTING.md, README), a "shared TLS key" known issue false since
`tlscert.c`, the web section's API list, the Hourly/Settings/Extras sections naming a function that no longer exists,
and memory reference numbers from September 29.

## Strengths worth keeping

- Outbound TLS everywhere, OTA URL pinned to the project site, project-name check before switching, rollback on.
- Inbound validation systematic (1 KB body cap, `cJSON_Is*` before every use, clamps in the sinks, `textContent`
  only on the page, Leaflet pinned with SRI). Per-device TLS certificate generated correctly.
- `slide.c`/`display.c`: raw frames reuse LVGL's DMA buffers, every wait bounded, ISR pinned to the LVGL core with an
  atomic in-flight counter, "picture equals panel" enforced by the flush hook and checked by `pictest`; all
  documented constants match the code; `lv_draw_buf` lifecycle clean on every exit path.
- `radar.c`/`png_rows.c`: error paths free everything; `cache_save()` writes header-last, sector by sector, and
  never saves another place's map; the decoder is bounded (4096 px, 8-bit, 256-entry palette).
- Forecasts keyed by coordinates, not index; per-place UTC offsets; big buffers in PSRAM; `svc.c` under a proper
  critical section; nowcast matches its doc line for line.
- Presence state machine, pick-up detector, quiet-hours wrap, QMI8658 register map and esp_codec_dev pairing all
  match docs and vendor sources.
- i18n: X-macro table with English fallback; 174 firmware and 141 page strings complete in en/fr/iu; the Inuktitut
  generator reproduces the committed syllabics exactly; Québec date rules right ("1er", lowercase months).
- Harness: touches injected at the controller read; lock-free `where` on any hang; rollback-aware `--ota`; every
  log regex it waits on exists in the firmware; PC Wi-Fi card reproduces the October 1 bugs end to end.
- Docs layered with reading order and *why*; CHANGELOG dated sections match all 30 tags; no secrets in the tree;
  `.gitignore` thorough.

## Findings

Severity after verification. **High** = users hit it or a release risk. **Medium** = real but narrow, or debt that
will bite. Items the verifiers lowered to low are in the next section; unverified low/info items follow by area.

### High

| # | Where | Finding |
|---|---|---|
| H1 | `main/web/index.html:834-840` | `function fwNotes(t)` shadows the translation function `t()`; with any non-empty notes `t('whatsNew')` throws before line 827 shows the Install button. Since commit b4e862b (Oct 1); shipped in v1.11.0 and v1.11.1. The harness's `--ota` uses the display's updater, so it never exercised this path; the Playwright mock has no "available" state. |

### Medium (confirmed)

**Security / network**

| # | Where | Finding |
|---|---|---|
| M1 | `main/web.c:520-536`, `:557-571` | Every state-changing endpoint (`/api/wifi`, `/api/update`, places, presence, units, sound) and `/api/snapshot` is unauthenticated, with no Origin/Host/Content-Type check, and the same set is registered on the plain-HTTP port 80 server on the home LAN. `web.c:1` and ARCHITECTURE.md:577-580 say port 80 only redirects. Any LAN host, or a web page via no-cors POST / DNS rebinding, can replace Wi-Fi credentials and restart, switch channel and install. |
| M2 | `main/net.c:264-268`, `net.h:6` | Setup AP: fixed published PSK `meteo1234`, settings page and `/api/wifi` form over plain HTTP inside it (a captured handshake decrypts the home password), `/api/config` returns the home SSID and coordinates to AP clients, and `offline_setup()` raises the AP automatically after 30 s offline and keeps it up. WPA3-SAE is already compiled in. |
| M3 | `main/net.c:147`, `:197`, `:384-387` | 32-byte SSIDs: `strlcpy` into the 32-byte driver field keeps 31 bytes, so a legal 32-character network is saved but never joined (page accepts 32); the DPP handler logs and saves `wc->sta.ssid` as a C string, which at 32 bytes runs into the password (into the log and NVS). Password length never validated. |

**Correctness / robustness**

| # | Where | Finding |
|---|---|---|
| M4 | `main/slide.c:479-480` | PSRAM-busy drag fallback waits for finger-up with `while (touch_get() != 0)`: exits only on a clean 0, so a CST9217 that NACKs after the lift (documented as common) keeps the LVGL task in the loop holding the display lock forever. The animated loop at :503-505 has the 5-error rule; this one does not. |
| M5 | `main/slide.c:980-993` | Zoom keeps `old_touch` true while reads return -1, so a swipe made during the 300 ms zoom is folded into the old touch and ignored (`touched` never set, no `touch_resync`). Same missing error cap. |
| M6 | `main/display.c:326-329`, `presence.c:233-239` | `display_brightness()` issues `esp_lcd_panel_io_tx_param` from the presence task on core 0 under the display lock with no `lvgl_inflight` wait; LVGL 9.2.2 releases the lock with its last band still on the bus, which is the cross-core condition the docs say hung raw frames. |
| M7 | `main/radar.c:97-104` and 6 more | `esp_http_client_init()` result passed straight to `perform()`/`get_status_code()` (both dereference it in IDF 5.5.4) at radar.c:97, weather.c:88 and :199, alerts.c:73 and :223, svc.c:115, ota.c:115. NULL on allocation failure → LoadProhibited reset. |
| M8 | `main/weather.c:117-121` | Daily loop bounds on `temperature_2m_max` but dereferences `tmin`, `weather_code` and `time` items unchecked (the hourly loop uses `num_at()`); a partial reply crashes every fetch → reboot loop until the API recovers, and the app is already marked valid so rollback cannot help. Also `alerts.c:167-172`: `parse_shape()` spins forever on `-` not followed by a digit. |
| M9 | `main/alerts.c:107`, `:129` | Features are truncated to `ALERTS_MAX` (4) in server order and sorted by severity only afterwards (`limit=20` requested): with more than four alerts over the point, a red warning listed fifth is dropped: no pill, no sound, wrong region map. |
| M10 | `main/main.c:67-81` | "New alert" keyed on the GeoMet feature `id`, whose prefix is the CAP publication id with its timestamp (live: `1727293701526659719202610020504_fea1-1068`); every Environment Canada re-issue chimes again (red bypasses quiet hours) and re-downloads the region map. The per-fetch name dedup also keeps whichever region id the server listed first. ARCHITECTURE promises "sounds once". |
| M11 | `main/ui.c:664-669`, `:2456`; `weather.h:17` | Hourly and daily arrays are indexed by position only (hourly `time` never read): between local midnight and the next successful fetch (≤ 10 min online; the whole outage offline) the hourly "Now" row, graph dot, day title and the "Today" column show yesterday's data. |
| M12 | `main/main.c:244-283`, `ui.c:2493-2545` | Forecast failures are invisible on the weather screen: the boot "Fetching forecast…" screen never reports failure, a new place shows "Loading…" forever, later failures leave stale data with no age; only the status page, two swipes away, says why. |
| M13 | `main/config.c:25-27`; `ui.c:338-355` | First run ends on Québec City with nothing leading to the settings page (the only route is long-press → "More on your phone" → QR → certificate warning); applies to the captive-portal path as well as Easy Connect, though IDEAS.md scopes it to Easy Connect. |
| M14 | `main/config.c:44-50`, `:72-76` | Places 2-4 are raw `location_t` blobs accepted only when the length equals `sizeof(location_t)`; a mismatch breaks the loop and the next save erases the orphans. The first field added to `location_t` silently drops the user's extra places. Same trap CLAUDE.md records only for `presence_cfg_t`. |
| M15 | `main/ota.c:297-310`, `main.c:210` | Rollback confirms any image that has run 60 s since `ota_start()`, which is called before `net_begin()`: a firmware whose Wi-Fi, display or touch never works is confirmed and the previous slot is never returned to. Only a crash or watchdog reset rolls back; the task watchdog is set to warn, not panic. |

**Memory and stacks** (from the gap round, measured in the repo's own logs)

| # | Where | Finding |
|---|---|---|
| M16 | `tools/harness/reports/*`, `docs/DIAGNOSTICS.md:55` | Internal RAM "min ever" is 8-10 KB in every harness run since Oct 1, 3 KB once (Oct 2 21:49), and 5 KB on the reconnect path (three concurrent TLS clients + forecast parse, `wifi_setup.unreachable_at_startup.log.txt:243`), while DIAGNOSTICS.md and ARCHITECTURE.md still say 28-30 KB (September 29 figures). Steady state has ~48 KB free but a 19 KB largest block. |
| M17 | `tools/harness/baseline.json:70-73`, `suites.py:185-188` | `internal_min_kb` floor is 5 under a reference of 10 (a 4 KB regression, the size of the zoom tables that caused the 3 KB dip, passes), and it is sampled once in `perf`, before the `wifi_*` suites that provoke the lowest path, so the 5 KB reading never reaches the comparison. |
| M18 | `main/radar.c:384`, `:798`, `:825` | 46 static `frame_t` (1048 B each: four 256-byte palettes) = 48,208 B of `.bss` in internal DRAM (confirmed in the linker map, ~86 % of `main/`'s static internal RAM) while `EXT_RAM_BSS_ATTR` is used elsewhere. |
| M19 | `main/web.c:515`, `:361-366`; `ui.c:2703-2704` | The HTTPS httpd task (7168 B, comment "measured peak ~3.3 KB, TLS handshake") also renders a whole screen off-display for `GET /api/snapshot` (`lv_obj_update_layout` + `lv_snapshot_take`); harness logs show 992-1516 B spare. The HTTP server's "~1.2 KB" comment is its idle baseline; serving the portal leaves 1144 B of 4096. |
| M20 | `main/main.c:272`, `alerts.c:252-258` | `alerts_map()` runs on the 6 KB main task (measured floor 1.7-3 KB free) and, when the basemap is not cached (always for places other than the first), downloads OSM tiles over TLS and inflates them, the path the radar task gets 10 KB for; never sampled. It also allocates ~1.2 MB of PSRAM transiently (512 KB receive buffer for a ~4 KB shape, a 434 KB full basemap copy) and fails silently; not in the memory budget. |

**Tooling / process**

| # | Where | Finding |
|---|---|---|
| M21 | `.github/workflows/firmware.yml:49-55` | CI only compiles. The 18-test board-free Playwright suite and any host test never run automatically; a page regression builds green and ships as an rc or stable. Cloud sessions cannot run them at all. |
| M22 | `tools/harness/harness.py:70-83`, `:166` | `compare()` skips silently in both directions: a baseline key with no metric in the run yields no row (reused log window drops all 13 boot metrics; a regex drift in `slide.c`'s log lines drops the drag metrics), and a metric with no baseline entry gets `ok=None` and never fails (38 of 76 recorded metrics in the Oct 2 22:25 report, including `drag_start_ms` for the screen drags the user notices). |
| M23 | `tools/harness/suites.py:222`, `:234`, `:435` | In-test `ctx.log.mark()` moves the single cursor the harness uses to detect unexpected `rst:0x` and to save the failing test's log: a crash during the radar's first-frame wait or the phone check is not flagged. |
| M24 | `docs/ARCHITECTURE.md:387`, `:389-391`, `:616` | Hourly view / Settings / Extras sections name `ui_weather()` (does not exist; `ui_place()`/`place_current()` do), describe LVGL-driven scrolling replaced by `slide.c`, and omit four Settings rows. These are the sections CLAUDE.md tells a new session to read first. |

### Confirmed, lowered to low by the impact judge

Each is literally true of the code; the judge found the practical consequence small for a one-user device today.

- **Docs/process:** README `:326` "checked (image header, SHA-256, same project)" reads like authenticity; no
  image signing (TLS to GitHub Pages + GitHub account is the whole chain) and Security notes `:526-533` omit the
  trust model. ARCHITECTURE `:706` "one TLS key shared by all builds" (false since `tlscert.c`); `:577-610` web
  section: port 80 "redirects everything", API list missing presence/calibrate/update/sound, `units.lang`,
  `pending_verify`, handler counts 14 vs 18/14, NVS namespace `places` (`n`, `act`, `p1..p3`) and `lang` missing, TZ
  "from net.c" while `main.c:197` sets it; Tasks table missing `ota`, `sound`, `testcon`. DIAGNOSTICS `:55`, `:96`
  and ARCHITECTURE `:689` keep the 28-30 KB figures. TESTING `:33-36` says the helper keeps the v5.4.2 DIO
  bootloader while `firmware\bootloader.bin` is now QIO (render baselines assume QIO; an OTA-updated board renders at
  DIO speed, weather 67 ms > max 56.6); README `:363` "DIO". `notes.json` keeps 15 sections including rc ones, so a
  display on v1.8.0 or older gets no notes for v1.8.0 (`make_flasher_site.py:118`, `:137`).
- **NVS writes:** every saver except `tlscert.c` discards `nvs_set_*`/`nvs_commit` results and reports "Saved"
  (`config.c:60-80`, `:194-212`; `net.c:226-235`; `presence.c:75-84`; `sound.c:63-70`; `ota.c:362`).
- **Retry and data:** failure retry every 30 s refetches all places with no backoff (`main.c:252`, `:282`; ~11,500
  requests/day in a day-long outage, above Open-Meteo's free quota); the shared static `weather_t` in
  `fetch_place()` is never zeroed, so a change in another place's nowcast defeats the `memcmp` "unchanged" test
  (`main.c:147`, `weather.c:54-56`, `ui.c:2502`, `:2520-2521`); hourly null `precipitation_probability`/temperature
  becomes 0 and is shown as real (`weather.c:42-46`, `ui.c:640-641`).
- **Network:** DNS server runs forever on `INADDR_ANY` (`dns_server.c:201`; `net.c:305-306` comment says "only
  phones on the setup AP"); swiping to the Easy Connect page runs a 1.6-2.6 s blocking scan inside the LVGL task
  under the display lock (`net.c:339`, `:359-360`, `ui.c:1115`, `:1148`); the Easy Connect success path (NVS write +
  label update under `display_lock`) runs on the 2304 B `sys_evt` task, 564-676 B free before any of it ran, and
  restarts 2.5 s later so it can never be measured (`net.c:381-391`; the `ui.c:1075` comment says "Wi-Fi task");
  setup stays open and the saved network is never retried while any station is associated to Weather-Setup
  (`ui.c:1134-1139`, `net.c:45`).
- **OTA:** a failed download hides pill and Install until the next check, up to 6 h (`ota.c:274-278`, `:347`;
  `ui.c:1749`, `:1758`); the `ota` task's install path (6144 B) is never measured because it restarts 2.5 s after
  finishing (IDF examples use 8 KB).
- **Peripherals / UI:** a failing `esp_codec_dev_read` does `continue` before `apply_brightness()`, so a persistent
  mic error freezes brightness with no log (`presence.c:262-265`); hidden radar-screen changes (preload panel,
  status texts, ring) do not call `slide_cache_dirty()` (`radar.c:266-275`, `:645-655`, `:757-775`, `:865-869`);
  `ui_place()`, `place_current()`, `ui_alerts()`, `ui_air()`, `ui_places()`, `ui_units_changed()` mark the picture of
  the *shown* screen dirty although the flush hook keeps it current, so the next drag after a 10-minute refresh
  pays a whole render (`ui.c:2528`, `:2506-2507`, `:1318`, `:1497`, `:2470`, `:2624`); `ui.c` is one 2716-line unit
  with ~110 file-scope statics and nine forward declarations; the alert details map shows OSM tiles with no
  attribution (`ui.c:1245-1271`; README `:345` claims it is on screen).
- **Page / API:** `/api/sound` (and `/api/snapshot`) HTTPS-only while the captive portal serves the full page over
  HTTP: the Sound card is dead during setup (`web.c:534-535` vs `:557-571`); "Save Wi-Fi & restart" ignores the HTTP
  status and always says "Saved. The display is restarting." (`index.html:713-714`; `web.c:434-437` answers 400 for
  33+ byte SSIDs); place names cut at 47 bytes can split a UTF-8 sequence (`web.c:249`, `config.h:6`; page allows 40
  characters, bypassed by the geocoder); `png_rows.c:103` `p + 12 + n > len` wraps in 32-bit for a crafted chunk
  length (read past the PSRAM buffer; needs a compromised TLS-verified tile server).
- **i18n / UX:** "What's new" notes are English-only in all languages (documented); Inuktitut offered on-device and
  on the page with no draft marker although `docs/translations/README.md` says it must not be presented as finished
  (`i18n.c:14`, `index.html:82`); no gesture hints after boot (long-press, tap a day, zoom direction).
- **Build / harness:** `esp_codec_dev` floats (`~1.5.2`, lock git-ignored; local resolution 1.5.11, CLAUDE.md says
  1.5.2); the device harness never switches language, so fr/iu layout regressions are outside the release gate;
  `--update-baseline` replaces each entry and drops all 22 `note` fields and hand-set limits (`harness.py:72-75`),
  and guesses direction from the name (`page_kb` would get a minimum); Wi-Fi tests have no cleanup, so a failed
  check leaves the board on the bogus SSID and the setup profile on the PC (`suites.py:387`, `:456`, `:422`).

### Unverified low / info, by area (compact)

- **ui:** `cfg_refresh()` rewrites ~12 Settings labels every second (`ui.c:2107`); hand-formatted numbers bypass
  locale helpers (English `a`/`p` graph hours, bare `%d%%` three times, `:523`); place chosen on the phone scrolls
  the frozen pager with LVGL's slow animation (`:2479`); overlay and Wi-Fi setup return via
  `lv_screen_load_anim(FADE_IN)` (`:333`); alert and update pills not tappable while the place has no forecast
  (`:704`); `ui_wifi_setup_end()` leaves `su_open` true (`:1198`); `ex_sun`, `ex_moon` and page dots still clickable
  without `GESTURE_BUBBLE` (`:1525`); stale comments (`:338`, `pager.h`); four dot drawers / four label factories;
  `ui_snapshot()` mutates live state for `phone`/`setupN`; `hr_draw` reads LVGL's private `_clip_area` (`:599`); OTA
  progress re-dirties all hidden place pages per percent (`:1790`); `alerts.h:7-10` defines `ALERT_LANGS` four
  times and `config.c:201-204` writes `lang` four times (commit b4e862b).
- **slide/display/touch:** scroll flick-speed code reads `smp[-1]` after a re-touch followed by five NACKs
  (`slide.c:727`); forced picture allocation bypasses `room_for()` while slots are empty (`:223`); stale "freed when
  the radar decodes" comment (`:153`); `slide_screen_busy()` reads a 64-bit timestamp cross-core without atomicity
  (`touch.c:72`); `touch_inject()` writes point and state non-atomically (`touch.c:29`); late band completion after a
  `raw_wait` timeout can be credited to LVGL's next band (`display.c:146`); flush hook assumes stride = width × 2
  (`display.c:187`); `lcd_cmd()` uses `ESP_ERROR_CHECK`, so any panel command error aborts (`display.c:67`); LVGL
  task stack 8192 while `slide.c:377` says a full render needs ~8 KB (open question).
- **radar:** zoom status label hard-codes "km" when the display is set to miles (`radar.c:1175`); `on_http`'s
  realloc failure is ignored by `esp_http_client`, so a truncated body counts as 200 (`:77`); "No Wi-Fi" shown for
  up to 6 min after Wi-Fi returns (`:934`); `cache_save()` holds `cache_mux` through up to 10 s of waits, blocking
  the alert map (`:208`); a zoom arriving between the last tile and `reveal_map` is served after a flash save
  (`:310`); zoom range constants duplicated in `radar.h` and `radar.c`; ARCHITECTURE `:428` says every location
  change preloads (only the first place does) and misstates the ring's smallest distance; no checksum on map
  cache slots; `sizeof(frame_t)` on target unconfirmed by `diag: heap` (open).
- **net:** DNS parser copies up to 255 bytes from a 128-byte stack buffer without checking `req_len`, never
  advances past the first question, double-closes the socket (`dns_server.c:78`, `:283`, `:357`), logs every packet
  at INFO under the upstream example tag; reconnect chain re-arms only from a DISCONNECTED event, a rejected
  `esp_wifi_connect()` ends retries silently (`net.c:50`); SNTP init result ignored, lost-IP not handled (`:92`);
  first-boot log says "not trying the saved network" when there is none (`:58`); `su_dpp_done` writes labels
  without checking the page (`ui.c:1086`); `from_setup_ap()` hard-codes 192.168.4.0/24 (`web.c:452`); ARCHITECTURE
  `:503` describes an overlay state 2 and an online-only AP timer that no longer exist.
- **web-api:** `read_json`/`send_json` dereference unchecked `calloc`/`cJSON_PrintUnformatted` (`web.c:36-51`);
  no recovery when HTTPS fails to start (HTTP root still redirects to the dead `https://`), key never validated at
  load (`tlscert.c:51`); certificate has no SAN and the "fingerprint" log prints the serial (`:154`); list rows are
  click-only divs, messages not announced (`index.html:630`); `var(--accent)` undefined, title untranslated, French
  percent spacing (`:19`); page starts in English until `/api/config` answers (`:466`); privacy of Nominatim/
  Open-Meteo/unpkg not stated (`:653`); Wi-Fi credential lengths not validated end to end (`web.c:434`).
- **ota-release:** Beta chosen by publish date, not version (`firmware.yml:126-130`: a hotfix stable removes a
  newer rc); only `-rc.N` orders among pre-releases (`ota.c:77-81`), and HISTORY `:67`'s rule "a test label above an
  rc hides that rc" is only true when X.Y.Z is higher; first check skipped if Wi-Fi is not up at 60 s, next in 6 h
  (`:315`); downloaded image version not compared with the offered one (`:256`); failed manifest fetch reported as
  "No app image in the manifest" (`:216`); bare English "Unexpected channels.json" (`:201`); esp-web-tools loaded as
  a floating major from unpkg; actions pinned by major tag, esptool by range; v1.11.1 ships new behaviour as a
  patch release against the project's own rule; README `:410` tagging recipe (annotated tags, same commit) does not
  match practice (lightweight tags, CHANGELOG commit); two rc tags have no changelog section; `OTA_SITE` baked in
  with no redirect following; flash budget 2.05 MB of 3 MB, ~23 KB per release, 318 KB of it the two TTFs.
- **data-core:** `http_evt` silently truncates bodies past `rx->cap` (a busy alert day over 64 KB freezes the old
  alerts, `alerts.c:25`); alert bookkeeping (`seen`, `map_id`, `shown`) shared between main, LVGL and httpd tasks
  without a lock (`main.c:117`); 32-bit `long` in the time path (`config.c:149`, `alerts.c:42`, `radar.c:353`:
  2038); one UTC offset for the whole 7-day response (days past a DST change labelled an hour off); a region-map
  404 flaps the EC health row and is retried every cycle (`main.c:271`); alert text cut at 899 bytes possibly
  mid-UTF-8 (`alerts.h:18`); deleting a place writes `n`, `act`, `p1..p3` as separate entries (a power cut between
  them keeps the deleted place and drops the last); a place switch commits NVS from the LVGL task at the end of a
  drag (`config.c:427`); main task stack never re-measured since `alerts_map()` was added.
- **peripherals:** `lvgl_mem.c:16` falls back to internal RAM when PSRAM is exhausted, silently, and if that fails
  `LV_ASSERT_MALLOC`'s `while(1)` freezes the LVGL task holding the lock (task WDT set to warn, not panic);
  settings loaded from NVS skip the clamps the setter applies (`presence.c:67`); brightness preview silently
  lowers the saved dim level (`:105`); quiet hours skipped when the clock is not synced (`sound.c:194`); red siren
  needs ~200 KB contiguous PSRAM per play, dropped silently if fragmented (`:112`); `sound_ok()` means "mics opened
  I2S", not "speaker present" (`:76`); "equal quiet times = off" undocumented to the user; console `press`/`drag`
  ms unbounded (negative wraps to ~71 min, `testcon.c:171`); `diag.c:79-83` keys task CPU deltas on recyclable
  handles; `:57` prints the first app partition, not the running one; no harness test for dim/off/wake or quiet
  hours; DIAGNOSTICS `:31` bench "retried every 20 s" no longer true; chimes are heard by the microphones and count
  as presence (a red siren wakes a dim screen).
- **i18n:** 12-hour clock rendered with English AM/PM, `a`/`p` in every language (`config.c:239`); a few French
  strings are anglicisms or inconsistent between display and page (`i18n_strings.h:206`); no plural forms, several
  Inuktitut rows shortened to a different meaning (`:158`); Inuktitut "Can't reach %s" overflows the 96-byte note
  for SSIDs over 28 bytes (`main.c:166`); syllabics glyph cache is 48 per font size while Settings alone uses 57
  distinct code points, and `CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT=256` is dead (`ui.c:89`, `sdkconfig.defaults:23`);
  French percent spacing differs on the volume row (`ui.c:2089`); missing glyphs draw a placeholder box; generator
  leftovers in `tools/i18n_iu.py:192`; ARCHITECTURE `:238` two-column X() example and "en|fr" comment stale.
- **tooling-tests:** crash decoder always uses `build/v55`'s ELF even for `--ota`; `boot_s.forecast_shown` takes
  the first boot in a reused window (`suites.py:173-184`); `radar_timing` fails if the radar was left at closest
  zoom (`:243`); `lvgl_profile_patch.py` silently does nothing when the LVGL source differs; Windows/English-only
  `netsh` parsing, hard-coded adapter, Node path and board IP (`board.py:266-288`, `harness.py:90`); setup-network
  profile written inside the repo tree and not git-ignored (`board.py:291`); mock cannot reach the update states and
  `units` accepts anything (`mock-server.js:76`); Playwright never touches the Wi-Fi, update, calibrate cards; no
  host unit tests for `parse_ver`/`cmp_ver`, quiet hours, `hhmm_or`, `png_rows`; snapshots saved but never
  asserted; TESTING `:144` run time, `picture` screen and bootloader paragraph out of date.
- **docs-hygiene:** `ui_preview.png` orphan at the root; four flashing scripts at the root, mostly absent from the
  README layout; personal paths, COM5, `192.168.1.156`, MAC tail in tracked files; CLAUDE.md contradicts itself on
  the ESP-IDF install (v5.4 vs v5.5.4) and has a garbled LVGL sentence (`:18`, `:53`); README `:452` building from
  source assumes the author's environment (no clone step, `firmware\` folder missing from a fresh clone); THIRD_PARTY:
  lodepng is zlib not MIT, Leaflet (BSD-2) missing, `CONFIG_LV_USE_LODEPNG=y` may be dead since `png_rows.c`; OSM
  User-Agent says "personal use" for a publicly released firmware (OSMF policy requires permission to distribute
  an app using osm.org tiles); the old shared private key remains in git history (first commit, never shipped);
  HISTORY `:10` counts 12 stable / 29 tags (now 13 / 30); ARCHITECTURE `:315` still says `gesture_cb` handles every
  move.
- **build-config:** no core dump partition while 5.9 MB of flash is unused (crash info only on UART); heap
  poisoning, end-of-stack watchpoint and stack checks off in the only configuration that exists; `LWIP_MAX_SOCKETS=16`
  exactly allotted when both servers and DNS run; `CONFIG_LV_BUILD_DEMOS=n` names a non-existent symbol;
  `CONFIG_MBEDTLS_HARDWARE_AES=n` may be pure cost now that LVGL's heap is in PSRAM (open); `EXT_RAM_BSS_ATTR` data
  is unreachable while the flash cache is off and the rule is unstated; vendored `dns_server` keeps the upstream
  socket leak instead of a 3-line fix; OTA images could be signed without secure boot
  (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`); `testcon.c:272` installs the usb_serial_jtag driver while the
  secondary console uses the non-driver VFS path on the same port (open question: interleaving?).
- **security:** README Security notes accurate but omit the trust model; `tlscert_log_fingerprint` logs no
  fingerprint; `from_setup_ap()` by subnet breaks on 192.168.4.0/24 home networks.
- **product-ux:** top risks to a second user: first-run dead end, certificate warning, no gesture hints; the RTC
  (PCF85063) and charger (AXP2101) are unused, the clock is blank until SNTP and a power cut loses the time;
  developer wording on the status page ("Forecast API v1", "never OK", "%d in a row", HTTP codes) and a "Testing:
  dim 10 s" preset on the consumer page (`index.html:116`); Beta channel on the page has no warning text (`:223`);
  the sound is called a "chime" while the owner asked for warning beeps (`i18n_strings.h:262`); smallest fonts 15 and
  19 px with no large-text option (`ui.c:2353`).
- **gap rounds (extra):** `tlscert_get()` blocks forever if the key-generation task cannot be created
  (`tlscert.c:134`); no boot counter or backoff for a crash loop in a validated firmware; brownout and reset reason
  never surfaced to the user or API; `net.c:104-107` erases the whole NVS (Wi-Fi, places, TLS key) on
  `NO_FREE_PAGES`/`NEW_VERSION_FOUND` silently; docs never describe what a wiped NVS looks like or that "Erase
  device" regenerates the certificate; inconsistent rounding across values and a "Rain" header over a probability
  that includes snow (`config.c:223`); freezing drizzle/rain use the plain rain icon; daily probability parsed but
  never shown; testcon task at 664 B free (docs say 3 KB, code 3.5 KB); the two restart-ending paths (OTA install,
  Easy Connect success) cannot be measured by the 60 s diag period.

## The critic's assessment

The picture is right and well grounded; the one high finding is a real shipped bug and the medium set sits where
the code is genuinely weakest. The skew: every review was a static read, and the runtime evidence in the repo
answered open questions outright (the display lock *is* a recursive mutex, `display.c:207/321`, so the
`ui_units_changed` re-lock is safe; `serial_log.txt:400-403` holds the task high-water marks five reviewers asked
for; the 22:25 harness run confirms the documented fps on the QIO-flashed board). `svc.c` is named in a scope but
appears in no finding beyond the shared `esp_http_client_init` pattern. The wifi_setup FAIL in the 22:25 report was
fixed by cc7181c and passed at 22:39.

## Open questions for the user

1. Is "anyone on the LAN can reconfigure the display" the intended model? If yes, M1 shrinks to "HTTPS-only API on
   the LAN + Host check" plus a README paragraph. If guests or IoT share the Wi-Fi, a token in the on-screen QR is
   the next step.
2. Should an Environment Canada *update* of an existing warning chime again (M10)? The code does; the doc says once.
3. On an LVGL out-of-memory, is a reboot acceptable? It is a one-line sdkconfig change (task WDT panic or an
   `abort()` assert handler).
4. Which bootloader is on the test board right now? The harness cannot tell and the render baselines assume QIO.
5. Should the unused test key in the first commit be purged from history, or is a README sentence enough?
6. Is the 12-hour clock meant to be offered in French at all, and in which written form?
7. Should the automatic offline setup AP time out, or is "always recoverable without touching the board" the
   priority?
