"""Test suites. Each test is a function(ctx) that raises board.Fail on a failed check; ctx.metric() records numbers
for the performance report and ctx.note() adds a line to the report. See docs/TESTING.md, "Harness".
"""
import os
import re
import subprocess
import time

from board import Fail, OLD_SETUP_PASS, READY, SETUP_IP, dns_query, http_get, ROOT

SUITES = {}


def test(suite):
    def deco(fn):
        SUITES.setdefault(suite, []).append(fn)
        return fn
    return deco


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def go_weather(ctx):
    """Back to the weather screen from wherever the display is."""
    b = ctx.board
    for _ in range(6):
        s = b.screen()
        if s == 'weather':
            return
        if s == 'settings':
            b.cmd('tap 233 45')                      # Done
        elif s in ('extras', 'status'):
            b.cmd('swipe left')
        elif s in ('radar', 'update', 'alert'):
            b.cmd('swipe right')
        elif s in ('phone', 'hourly'):                # a tap closes them
            b.cmd('tap 233 233')
        elif s == 'message':                         # no forecast yet (a service outage): wait for it, up to 3 min
            end = time.time() + 180
            while b.screen() == 'message' and time.time() < end:
                time.sleep(5)
            if b.screen() == 'message':
                raise Fail('still no forecast after 3 min (the message screen: is Open-Meteo answering?)')
            ctx.note('waited on the message screen for the first forecast (a slow forecast service)')
            continue
        else:
            b.cmd('swipe right')
        time.sleep(1.2)
    raise Fail(f'could not get back to the weather screen (stuck on "{b.screen()}")')


# ---------------------------------------------------------------- smoke

@test('smoke')
def console_and_network(ctx):
    b = ctx.board
    v = b.cmd('ping', r'test: pong (\S+)').group(1)
    ctx.note(f'firmware {v}')
    w = b.wifi()
    check(w['connected'] == '1', f'not connected to Wi-Fi: {w}')
    cfg = b.api('/api/config')
    check('units' in cfg and 'languages' in cfg, 'GET /api/config lacks units/languages')
    before = getattr(ctx, 'places_before', None)
    if before is not None:                             # (places 2-4 moved from blobs to typed keys in v1.12.0)
        check(len(cfg['places']) == before, f'{before} places before the restart, {len(cfg["places"])} after')
        ctx.note(f'{before} places kept across the restart')
    ctx.note(f'Wi-Fi "{w["sta_ssid"]}" channel {w["channel"]}, settings API answers')


# ---------------------------------------------------------------- navigation

@test('navigation')
def every_screen(ctx):
    """Swipe through the screens like a person, check where it lands, snapshot each."""
    b = ctx.board
    b.cmd('wake')
    go_weather(ctx)
    route = [('swipe right', 'extras'), ('swipe right', 'status'),
             ('swipe right', 'status'),                  # the left end: bounces back
             ('swipe left', 'extras'),
             ('drag 233 233 300 233 600', 'extras'),     # short and slow: snaps back
             ('swipe left', 'weather'), ('swipe left', 'radar'),
             ('swipe left', 'radar'),                    # the right end: bounces back
             ('swipe right', 'weather'),
             ('press 233 233', 'settings'), ('tap 233 45', 'weather'),
             ('tap 125 350', 'hourly'), ('swipe left', 'hourly'), ('tap 233 233', 'weather')]
    for cmd, want in route:
        b.cmd(cmd)
        b.wait_screen(want, 6)
        if want not in ctx.snapped:
            ms = b.snap({'hourly': 'current'}.get(want, want), ctx.out(f'screen_{want}.png'))
            ctx.snapped.add(want)
            ctx.metric(f'snapshot_ms.{want}', round(ms))
    # Places (drag up / down on the weather screen) and days (sideways in the hourly view): pictures that follow the
    # finger (slide.c); the first page bounces
    pages = lambda: dict(kv.split('=') for kv in b.cmd('page', r'test: page (.*)').group(1).split())
    pg = pages()
    place = int(pg['place'])
    while place > 0:                                       # from the first place (the board keeps the one shown:
        b.cmd('drag 233 100 233 380 300')                  # from the last, "drag up" bounced and still passed)
        time.sleep(1.2)
        now = int(pages()['place'])
        check(now == place - 1, f'drag down from place {place}: place {now}, expected {place - 1}')
        place = now
    if int(pg['places']) > 1:
        b.cmd('drag 233 380 233 100 300')
        time.sleep(1.2)
        check(pages()['place'] == '1', f'drag up: place {pages()["place"]}, expected 1')
        b.cmd('drag 233 100 233 380 300')
        time.sleep(1.2)
        check(pages()['place'] == '0', f'drag down: place {pages()["place"]}, expected 0')
    b.cmd('drag 233 100 233 380 300')                      # above the first place: bounces
    time.sleep(1.2)
    check(pages()['place'] == '0', 'drag down on the first place did not bounce back')
    pictest(ctx, 'weather screen after place drags')
    b.wait_screen('weather', 3)
    b.cmd('tap 125 350')
    b.wait_screen('hourly', 6)
    time.sleep(1)
    d0 = int(pages()['day'])
    b.cmd('swipe left')
    time.sleep(1.2)
    check(int(pages()['day']) == d0 + 1, f'hourly: swipe left went to day {pages()["day"]}, expected {d0 + 1}')
    b.cmd('swipe right')
    time.sleep(1.2)
    check(int(pages()['day']) == d0, f'hourly: swipe right went to day {pages()["day"]}, expected {d0}')
    pictest(ctx, 'hourly view after day drags')
    b.cmd('tap 233 233')
    b.wait_screen('weather', 6)
    for extra in ('settings1', 'settings2', 'settings3', 'phone', 'setup0', 'setup1', 'update'):
        b.snap(extra, ctx.out(f'screen_{extra}.png'))
    ctx.note('screens: ' + ', '.join(sorted(ctx.snapped)) + ' (+ settings1..3, phone, setup0/1, update)')


def text_rows(bmp, y0, y1, level=128):
    """Bands of rows y0..y1 of a snapshot (24-bit BMP) holding bright pixels (text), as (top, bottom), and the number
    of bright pixels outside the round panel (text the circle cuts off)."""
    import struct
    off, = struct.unpack_from('<I', bmp, 10)
    w, h = struct.unpack_from('<ii', bmp, 18)
    top_down, h = h < 0, abs(h)
    row = (w * 3 + 3) & ~3
    bands, outside, r2 = [], 0, (w / 2) ** 2
    for y in range(y0, y1 + 1):
        src = off + (y if top_down else h - 1 - y) * row
        line = bmp[src:src + w * 3]
        cy = y + 0.5 - h / 2
        lit = False
        for x in range(w):
            if max(line[3 * x:3 * x + 3]) < level:
                continue
            if (x + 0.5 - w / 2) ** 2 + cy * cy > r2:
                outside += 1
            lit = True
        if lit and bands and bands[-1][1] == y - 1:
            bands[-1][1] = y
        elif lit:
            bands.append([y, y])
    return [tuple(b) for b in bands], outside


@test('navigation')
def easy_connect_fail_text(ctx):
    """After a failed Easy Connect attempt the page says to swipe right and join the setup network (a Pixel 8 Pro on
    5 GHz drops the display's answer, October 5: scanning again doesn't help). In every language: two lines, inside
    the round panel (snapshot "setup1fail", the page rendered off-display; the language is put back)."""
    b = ctx.board
    was = b.api('/api/config')['units']['lang']
    try:
        for lang in ('en', 'fr', 'iu'):
            b.api('/api/units', {'lang': lang})
            time.sleep(1.5)
            b.snap('setup1fail', ctx.out(f'setup1fail_{lang}.png'))
            bands, outside = text_rows(b.last_bmp, 290, 430)
            check(len(bands) == 2, f'Easy Connect failure text in {lang}: {len(bands)} lines {bands}, expected 2')
            check(outside == 0, f'Easy Connect failure text in {lang}: {outside} pixels outside the round panel')
        ctx.note('Easy Connect failure text: 2 lines inside the panel in en, fr, iu (setup1fail_*.png)')
    finally:
        b.api('/api/units', {'lang': was})
        time.sleep(1.5)


@test('navigation')
def scroll_other_languages(ctx):
    """Settings scrolled in French and Inuktitut: the picture still equals the screen. Inuktitut's syllabics (a
    fallback font drawn 5/4 larger) reach above their label's box, and a strip of rows ending just above a label lost
    their tips (pictest 1 row off, October 5): slide.c's render_rows draws a margin of rows around each strip."""
    b = ctx.board
    was = b.api('/api/config')['units']['lang']
    try:
        for lang in ('fr', 'iu'):
            b.api('/api/units', {'lang': lang})
            time.sleep(2)
            b.cmd('wake')
            go_weather(ctx)
            b.cmd('press 233 233')
            b.wait_screen('settings', 6)
            time.sleep(1)
            b.cmd('drag 233 330 233 130 300')
            time.sleep(2.5)
            pictest(ctx, f'Settings scrolled in {lang}')
            b.cmd('drag 233 130 233 330 300')       # and back to the top
            time.sleep(2.5)
            pictest(ctx, f'Settings scrolled back in {lang}')
            b.cmd('tap 233 45')
            b.wait_screen('weather', 6)
    finally:
        b.api('/api/units', {'lang': was})
        time.sleep(2)


@test('navigation')
def alert_layout(ctx):
    """The alert screen with long titles (console "alert sample"): a two-line title pushes the lines under it down.
    Real alerts are rare at the board's place, so the console lays the screen out with names Environment Canada used
    ("Wreckhouse wind warning" ran into "Until ..." and hid behind the map up to v1.12.1); the pill isn't touched."""
    b = ctx.board
    if 'alert sample' not in b.cmd('help', r'test: commands: (.*)').group(1):
        ctx.note('alert layout not checked: firmware without the console command "alert sample" (v1.12.2 and older)')
        return
    try:
        for which in ('en', 'fr', 'max'):
            m = b.cmd(f'alert sample {which}',
                      r'test: alert sample \S+ title_y=(\d+) title_h=(\d+) lines=(\d+) box_y=(\d+)')
            ty, th, lines, by = (int(g) for g in m.groups())
            b.snap('alert', ctx.out(f'alert_{which}.png'))
            check(by >= ty + th, f'alert sample {which}: the column starts at y={by}, inside the title ({ty}+{th})')
            check(lines <= 3, f'alert sample {which}: title on {lines} lines')
            check(by <= 200, f'alert sample {which}: the column starts at y={by}, too low to read the text')
            ctx.note(f'alert title "{which}": {lines} line(s), column at y={by}')
    finally:
        b.cmd('alert sample off', r'test: alert sample off')


# ---------------------------------------------------------------- web

@test('navigation')
def hourly_touches(ctx):
    """The hours list takes quick short flicks, and a day swipe made while it still coasts changes the day.
    Before v1.12.1 a 30 px flick in 60 ms was read at 10 px, LVGL's own scroll took it (17-28 fps, the touch read only
    between its frames) and nothing moved; and a touch during the coast only ever followed the finger vertically."""
    b = ctx.board
    b.cmd('wake')
    go_weather(ctx)
    b.cmd('tap 125 350')                                 # a day: the hourly view
    b.wait_screen('hourly', 6)
    time.sleep(1.5)
    # Tomorrow: its 24 hours. Today's list shrinks through the day: at 20:30 it scrolled 14 px in all ("now at 14 of
    # 0..14"), and every flick "barely moved" (October 4, v1.13.0-align.1, the code unchanged)
    b.cmd('drag 400 300 80 300 250')
    time.sleep(1.5)
    check(b.cmd('page', r'test: page .*day=(\d+)').group(1) == '1', 'the day swipe to tomorrow did not happen')
    scroll = r'slide: scroll: \d+ frames in \d+ ms, moved (\d+) px'
    for i in range(3):                                   # a quick short flick, three times
        at = len(ctx.log.lines())
        b.cmd('drag 233 330 233 300 60')
        m = ctx.log.wait(scroll, 4, f'quick flick {i + 1} taken by the list (slide.c)', start=at)
        check(int(m.group(1)) > 30, f'quick flick {i + 1} barely moved the list ({m.group(1)} px)')
        b.cmd('drag 233 160 233 400 90')                 # back to the top
        time.sleep(1.5)
    day0 = b.cmd('page', r'test: page .*day=(\d+)').group(1)
    at = len(ctx.log.lines())
    b.cmd('drag 233 330 233 300 60')                     # a flick, and while the list coasts, a day swipe
    time.sleep(0.2)
    b.cmd('drag 400 300 80 300 250')
    ctx.log.wait(r'slide: drag: first frame', 4, 'the day swipe made during the coast', start=at)
    time.sleep(1)
    day1 = b.cmd('page', r'test: page .*day=(\d+)').group(1)
    check(day1 != day0, f'day swipe during the coast missed (day {day0} -> {day1})')
    handed = any('then a sideways touch' in l for l in ctx.log.lines()[at:])
    ctx.note(f'3 quick flicks scrolled; day swipe during the coast: day {day0} -> {day1}'
             f'{" (handed over by the coasting list)" if handed else " (the list had stopped)"}')
    b.cmd('drag 80 300 400 300 250')
    time.sleep(1)
    # Tomorrow's list back to the top: perf's render bench draws that page as it was left, and scrolled down (the graph
    # gone, more rows) it took 46 ms instead of 38 (October 5): render_ms.hourly failed by where a flick had coasted
    for _ in range(2):
        b.cmd('drag 233 160 233 420 300')
        time.sleep(1.2)
    b.cmd('tap 233 233')
    b.wait_screen('weather', 6)


@test('web')
def settings_page_tests(ctx):
    """Playwright suite against the mock display (tools/webtest)."""
    wt = os.path.join(ROOT, 'tools', 'webtest')
    env = dict(os.environ)
    if os.name == 'nt':                               # (on macOS a "C:\…;" entry broke the first real PATH entry)
        env['PATH'] = r'C:\Program Files\nodejs' + os.pathsep + env.get('PATH', '')
    # Microsoft Store Python virtualises AppData\Local for its child processes, so Playwright can't see the browsers
    # in AppData\Local\ms-playwright. Use a copy next to the tests (git-ignored), see docs/TESTING.md.
    browsers = os.path.join(wt, '.browsers')
    if os.path.isdir(browsers):
        env['PLAYWRIGHT_BROWSERS_PATH'] = browsers
    import shutil
    npm = shutil.which('npm', path=env['PATH']) or r'C:\Program Files\nodejs\npm.cmd'
    if not os.path.exists(npm):
        raise Fail('Node.js (npm) not found (macOS: brew install node, then cd tools/webtest && npm ci)')
    r = subprocess.run([npm, 'test'], cwd=wt, env=env, capture_output=True, text=True, errors='replace', timeout=600)
    out = r.stdout + r.stderr
    m = re.search(r'(\d+) passed', out)
    failed = re.search(r'(\d+) failed', out)
    open(ctx.out('webtest.txt'), 'w', encoding='utf-8').write(out)
    check(r.returncode == 0 and m and not failed, f'Playwright: {failed.group(0) if failed else "error"} (webtest.txt)')
    ctx.note(f'Playwright: {m.group(0)}')


@test('web')
def live_api(ctx):
    b = ctx.board
    for path, key in (('/api/config', 'units'), ('/api/presence', 'state'), ('/api/sound', 'level')):
        j = b.api(path)
        check(key in j, f'GET {path}: no "{key}"')
    # This board has a speaker: the page said "No speaker found" in v1.12.0-rc.3..rc.5 (a probe that never answered)
    check(b.api('/api/sound').get('ok') is True, 'GET /api/sound says there is no speaker (the page shows "No speaker found")')
    import urllib.request
    with urllib.request.urlopen(f'https://{b.ip}/', context=b.ctx, timeout=15) as r:
        page = r.read()
    check(b'</html>' in page[-200:], 'settings page arrived truncated (hardware-AES bug class)')
    ctx.metric('page_kb', len(page) // 1024)


@test('web')
def who_may_change(ctx):
    """main/web.c "Who may change things": HTTPS only on the home network, the device's own Host, JSON, the key."""
    b = ctx.board
    if not b.key():
        ctx.note('firmware without a settings key: rules not checked')
        return
    import http.client
    import ssl as _ssl

    def req(method, path, body=None, headers=None, tls=True, host=None):
        c = (http.client.HTTPSConnection(b.ip, 443, context=b.ctx, timeout=10) if tls
             else http.client.HTTPConnection(b.ip, 80, timeout=10))
        h = {'Host': host or b.ip}
        h.update(headers or {})
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        r.read()
        c.close()
        return r.status, r.getheader('Location')

    js = {'Content-Type': 'application/json'}
    k = {'X-Key': b.key()}
    st, _ = req('POST', '/api/units', '{}', js, tls=False)
    check(st == 403, f'POST over plain HTTP on the home network: {st}, expected 403')
    st, loc = req('GET', '/api/config', tls=False)
    check(st == 302 and loc == f'https://{b.ip}/', f'GET /api over plain HTTP: {st} {loc}, expected 302 to the page')
    st, loc = req('GET', '/', tls=False, host='evil.example')
    check(loc == f'https://{b.ip}/', f'the HTTP redirect follows the Host header ({loc})')
    st, _ = req('POST', '/api/units', '{}', js)
    check(st == 401, f'POST without the key: {st}, expected 401')
    st, _ = req('POST', '/api/units', '{}', {**js, 'X-Key': '0' * 16})
    check(st == 401, f'POST with a wrong key: {st}, expected 401')
    st, _ = req('POST', '/api/units', '{}', {'Content-Type': 'text/plain', **k})
    check(st == 415, f'POST as text/plain: {st}, expected 415')
    st, _ = req('GET', '/api/config', host='rebind.example')
    check(st == 421, f'GET with another Host (DNS rebinding): {st}, expected 421')
    st, _ = req('GET', '/api/snapshot?screen=current')
    check(st == 401, f'snapshot without the key: {st}, expected 401')
    st, _ = req('POST', '/api/units', '{}', {**js, **k})
    check(st == 200, f'POST with the key: {st}, expected 200')
    ctx.note('settings API: 403 over HTTP, 302 to the device itself, 401 without/with a wrong key, 415, 421, 200')


# ---------------------------------------------------------------- performance

def ms_of(line):
    m = re.match(r'[IWE] \((\d+)\)', line)
    return int(m.group(1)) if m else None


@test('perf')
def boot_and_memory(ctx):
    lines = ctx.log.lines()
    # The last boot in the window only: a reused window can hold several (a test restarted the board), and the first
    # one's numbers were taken. "start" and "display" are logged before the helper's log always catches up: not used.
    boots = [i for i, l in enumerate(lines) if 'diag: mark start' in l or 'rst:0x' in l]
    lines = lines[boots[-1]:] if boots else lines       # (none: the window began with this boot, after its first lines)
    for l in lines:
        m = re.search(r'diag: mark (.+?)\s+internal (\d+) KB', l)
        if m and m.group(1).strip() not in ('start', 'display'):
            ctx.metric(f'boot_s.{m.group(1).strip().replace(" ", "_")}', round(ms_of(l) / 1000, 1))
            ctx.metric(f'boot_internal_kb.{m.group(1).strip().replace(" ", "_")}', int(m.group(2)))
    # "first weather" (above) is logged after the whole first round: every place's forecast, alerts and air quality,
    # ~4 s per Open-Meteo request on a slow evening. What the user waits for is the active place on screen.
    for l in lines:
        if re.search(r'weather: .+: now ', l):
            ctx.metric('boot_s.forecast_shown', round(ms_of(l) / 1000, 1))
            break
    if not any(k.startswith('boot_s.') for k in ctx.metrics):
        ctx.skip('boot_*', 'no start-up in this log window (it was reused): restart the board to measure it')
    h = ctx.board.heap()
    ctx.metric('internal_free_kb', h['internal'])
    ctx.metric('internal_min_kb', h['min'])
    ctx.metric('psram_min_kb', h['psram_min'])
    ctx.metric('failed_allocs', h['failed_allocs'])       # any allocation that failed since boot
    ctx.metric('lvgl_internal_fallbacks', h['lvgl_fallbacks'])   # LVGL blocks in internal RAM (PSRAM full)


@test('perf')
def render_bench(ctx):
    """Full-screen render times, best of 3: background work (the radar loading frames after a visit, ~1.5x slower
    once) is noise, a slower renderer shows in every run."""
    b = ctx.board
    go_weather(ctx)
    best = {}
    runs = 0
    for _ in range(8):                               # 3 runs; "postponed" (a redraw going on) doesn't count
        if runs == 3:
            break
        time.sleep(2)
        b.cmd('bench', r'test: ok bench')
        m = ctx.log.wait(r'diag: bench (render-only full screen:.*|postponed.*)', 20)
        if not m.group(1).startswith('render'):
            continue
        runs += 1
        for name, v in re.findall(r'(\w+) ([\d.]+) ms', m.group(1)):
            best[name] = min(best.get(name, 1e9), float(v))
        m = ctx.log.wait(r'diag: bench weather incl. panel transfer: ([\d.]+) ms', 5)
        best['weather_with_panel'] = min(best.get('weather_with_panel', 1e9), float(m.group(1)))
    check(runs, 'bench always postponed: the weather screen was never idle')
    for name, v in best.items():
        ctx.metric(f'render_ms.{name}', v)


@test('perf')
def radar_timing(ctx):
    """Swipe to the radar: time to the first new frame; play the animation and read the frame rate."""
    b = ctx.board
    go_weather(ctx)
    at = len(ctx.log.lines())
    t0 = time.time()
    b.cmd('swipe left')
    b.wait_screen('radar', 6)
    try:
        m = ctx.log.wait(r'radar: Frame ', 30, 'a radar frame after opening the radar', start=at)
        ctx.metric('radar_first_frame_s', round(time.time() - t0, 1))
        ctx.log.wait(r'radar: Lightning ', 30, 'lightning fetched with the frames', start=at)
    except Fail:
        ctx.skip('radar_first_frame_s', 'no new radar frame within 30 s (all frames loaded on an earlier visit)')
    time.sleep(8)                                    # history frames load while the radar is visible
    b.cmd('tap 233 233')                             # play the last 3 h (3 fps by design: no fps metric)
    at = len(ctx.log.lines())
    time.sleep(10)
    frames = ctx.log.count(r'radar: Frame ', start=at)
    lt = [int(x) for x in re.findall(r'radar: Lightning \S+: \d+ px, (\d+) marks', '\n'.join(ctx.log.lines()[at:]))]
    ctx.note(f'{frames} radar frames loaded during playback; lightning marks per frame: {lt[:15]}')
    b.cmd('tap 233 233')                             # stop
    # Zoom in, then back out: drawn by slide.c (LVGL's own zoom transformed the whole image: ~10 fps). From one level
    # out first: at the closest zoom a swipe down does nothing and the check failed
    b.cmd('swipe up')
    time.sleep(4)
    start = len(ctx.log.lines())
    b.cmd('swipe down')
    m = ctx.log.wait(r'slide: zoom \d+ -> \d+: overlays (\d+) px in (\d+) ms, (\d+) frames in (\d+) ms', 10,
                     'the zoom drawn by slide.c', start=start)
    ctx.metric('radar_zoom_fps', round(int(m.group(3)) * 1000 / int(m.group(4)), 1))
    ctx.metric('radar_zoom_start_ms', int(m.group(2)))
    ctx.note(f'radar zoom: {m.group(3)} frames in {m.group(4)} ms, overlays ({m.group(1)} px) in {m.group(2)} ms')
    time.sleep(5)
    b.cmd('swipe up')                                # back to the zoom level the user had
    time.sleep(5)
    b.cmd('swipe right')


SCROLL_LINE = re.compile(r'slide: scroll: (\d+) frames in (\d+) ms, .*per frame: move ([\d.]+), render ([\d.]+), '
                         r'send ([\d.]+) ms')


def pictest(ctx, what):
    """slide.c's picture of the screen shown must equal a fresh rendering of it: drags start from it, and list
    scrolls move it and only render the rows coming in (a bug there smeared the hourly graph's labels)."""
    b = ctx.board
    start = len(ctx.log.lines())
    b.cmd('pictest', r'test: ok pictest')
    m = ctx.log.wait(r'slide: pictest rows_differ=(-?\d+) first=(-?\d+)', 10, 'pictest result', start=start)
    bad = int(m.group(1))
    check(bad == 0, f'{what}: the picture of the screen differs from the screen in {bad} rows (from row {m.group(2)})'
          if bad > 0 else f'{what}: no picture of the screen (pictest {bad})')


DRAG_LINE = re.compile(r'slide: drag: first frame after (\d+) ms \((\d+) pictures rendered\), (\d+) frames in (\d+) ms')


def measure(ctx, name, action, settle=1.0):
    """Frame rate during `action` (a list of console commands): fps reset, act, wait for the animation to end.
    swipe_fps counts every frame less than 250 ms apart, so it includes LVGL's redraws after the move (a new place's
    clock and data: 57 fps for the drag, 30 with them). For drags, slide.c's own line gives the drag alone:
    drag_fps and drag_start_ms (finger recognised -> first frame; the user noticed 0.1 s). swipe_gap_max_ms leaves out
    the gap from an LVGL redraw to the move's first frame (display.c): the update check's redraw of Settings 247 ms
    before a scroll failed v1.12.3-rc.1's run once."""
    b = ctx.board
    if time.localtime().tm_sec > 55:                 # not across a minute change: the clock's redraw right after a
        time.sleep(62 - time.localtime().tm_sec)     # move counts in swipe_gap_max_ms (127 ms once, after a radar swipe)
    b.cmd('fps reset')
    start = len(ctx.log.lines())
    for c in action:
        b.cmd(c)
    time.sleep(settle)
    line = b.cmd('fps', r'test: fps (.*)').group(1)
    kv = dict(x.split('=', 1) for x in line.split())
    v = {k: float(x) for k, x in kv.items() if re.fullmatch(r'-?[\d.]+', x)}
    ctx.metric(f'swipe_fps.{name}', round(v['anim_fps'], 1))
    ctx.metric(f'swipe_gap_max_ms.{name}', round(v['gap_max_ms']))
    ctx.metric(f'swipe_render_avg_ms.{name}', round(v['render_avg_ms'], 1))
    drag = ''
    lines = ctx.log.lines()
    hit = next((l for l in lines[start:] if DRAG_LINE.search(l)), None)
    m = DRAG_LINE.search(hit) if hit else None
    if m:
        first, pics, frames, ms = (int(x) for x in m.groups())
        ctx.metric(f'drag_fps.{name}', round(frames * 1000 / ms, 1) if ms else 0)
        ctx.metric(f'drag_start_ms.{name}', first)   # (right after the minute too: only clock rows re-render)
        drag = (f'; the drag alone {frames * 1000 / ms if ms else 0:.0f} fps, first frame after {first} ms '
                f'({pics} pictures rendered)')
    m = next((SCROLL_LINE.search(l) for l in ctx.log.lines()[start:] if SCROLL_LINE.search(l)), None)
    if m:                                            # a list scroll (slide.c): time per frame, by part
        mv, rd, sd = (float(x) for x in m.groups()[2:])
        ctx.metric(f'scroll_frame_ms.{name}', round(mv + rd + sd, 1))
        drag = f'; scroll frames {mv + rd + sd:.1f} ms (move {mv}, render {rd}, send {sd})'
    ctx.note(f'{name}: {v["anim_fps"]:.1f} fps, {int(v["anim_frames"])} frames, render avg {v["render_avg_ms"]:.1f} ms '
             f'max {v["render_max_ms"]:.1f} ms, worst gap {v["gap_max_ms"]:.0f} ms' +
             (f' ({kv["gap_max_kind"]}, {kv["gap_max_at_ms"]} ms after the reset)' if 'gap_max_kind' in kv else '') + drag)


def radar_settled(ctx, since, quiet=5, timeout=45):
    """Wait until the radar has logged nothing for `quiet` s and no map preload is running (log lines from `since`).
    After a place change it downloads the place's maps and saves them to flash (radar.c cache_save, the background
    preload): flash writes pause both cores in bursts for seconds, and a move measured then shows the flash, not the
    drawing (a day drag at 3 fps on October 2, its first frame after 2 s)."""
    end = time.time() + timeout
    seen, calm = -1, time.time()
    while time.time() < end:
        lines = [l for l in ctx.log.lines()[since:] if 'radar: ' in l]
        preloading = False
        for l in lines:
            if 'radar: Preloading' in l:
                preloading = True
            elif 'radar: Preload finished' in l or 'radar: Preload stopped' in l:
                preloading = False
        if len(lines) != seen:
            seen, calm = len(lines), time.time()
        elif not preloading and time.time() - calm >= quiet:
            return
        time.sleep(0.5)
    ctx.note(f'the radar was still busy {timeout} s after the place change (moves measured anyway)')


@test('perf')
def swipes(ctx):
    """Frame rate of the moves people make: screen to screen, the hourly view's days, places, Settings scroll."""
    b = ctx.board
    b.cmd('wake')
    go_weather(ctx)
    time.sleep(1)
    measure(ctx, 'screen_weather_to_extras', ['swipe right'])
    measure(ctx, 'screen_extras_to_weather', ['swipe left'])
    measure(ctx, 'screen_weather_to_radar', ['swipe left'])
    b.cmd('swipe right')
    b.wait_screen('weather', 6)
    places = len(b.api('/api/config').get('places', []))
    since = len(ctx.log.lines())
    if places > 1:                                   # places: drawn by slide.c, vertical
        if time.localtime().tm_sec > 48:             # not across a minute change: it makes every picture out of
            time.sleep(64 - time.localtime().tm_sec)  # date (the clocks), and drag_start_ms would measure that
        measure(ctx, 'drag_place', ['drag 233 380 233 120 400'], settle=1.5)
        # Back ~2 s later, like a person going through their places: the pictures must still be ready. On October 2
        # every switch redrew the pages with nothing changed, and the drag back waited 0.2-0.6 s (drag_start_ms).
        measure(ctx, 'drag_place_back', ['drag 233 120 233 380 400'], settle=1.5)
        # Back on the first place, the radar fetches its maps and saves them to flash (both cores pause in bursts):
        # a tap then can go unseen and a move crawls. Wait for it, and tap twice if needed.
        radar_settled(ctx, since)
        # A place chosen on the settings page: the pictures slide (slide_page), ~65 fps; LVGL's own scroll of the
        # place pager ran at ~10 fps and the owner found it sluggish (v1.12.0-rc.5)
        for sel in (1, 0):
            at = len(ctx.log.lines())
            b.api('/api/places', {'select': sel})
            m = ctx.log.wait(r'slide: page: pictures (\d+) ms, (\d+) frames in (\d+) ms', 10,
                             'the place change from the settings page drawn by slide.c', start=at)
            pics, frames, ms = (int(x) for x in m.groups())
            ctx.metric(f'page_fps.web_place_{sel}', round(frames * 1000 / ms, 1) if ms else 0)
            ctx.metric(f'page_start_ms.web_place_{sel}', pics)
            time.sleep(2)
        radar_settled(ctx, len(ctx.log.lines()) - 1)
        # New data (console 'dirty': hidden pictures out of date and the screen redrawn, as a forecast or alerts do),
        # then a place drag 0.8 s later, nobody having touched the display: its pictures must be ready again. Before
        # v1.12.1 they were rendered only after 0.8 s of quiet, a strip per 30 ms: such a drag waited ~0.12 s.
        try:
            b.cmd('dirty', r'test: ok dirty')
        except Fail as e:
            if 'unknown command' not in str(e):
                raise
            ctx.skip('*.drag_place_after_data', "this firmware has no 'dirty' command")
        else:
            time.sleep(0.8)
            measure(ctx, 'drag_place_after_data', ['drag 233 380 233 120 400'], settle=1.5)
            b.cmd('drag 233 120 233 380 400')
            time.sleep(2)
            radar_settled(ctx, len(ctx.log.lines()) - 1)
    else:
        ctx.skip('*.drag_place*', 'one place on the display: no place drag')
        ctx.skip('page_*', 'one place on the display: no place change')
    for attempt in range(2):
        b.cmd('tap 125 350')                         # a day: the hourly view
        try:
            b.wait_screen('hourly', 6)
            break
        except Fail:
            if attempt:
                raise
            ctx.note('tap on the forecast not seen the first time (radar saving the new map?), tried again')
    time.sleep(1)
    measure(ctx, 'drag_hourly_day', ['drag 380 233 100 233 400'], settle=1.5)
    measure(ctx, 'scroll_hourly_list', ['drag 233 400 233 250 300'], settle=2.0)
    pictest(ctx, 'hourly list scrolled')
    b.cmd('tap 233 233')
    b.wait_screen('weather', 6)
    b.cmd('press 233 233')
    b.wait_screen('settings', 6)
    time.sleep(1)
    measure(ctx, 'scroll_settings', ['drag 233 330 233 130 300'], settle=2.0)
    pictest(ctx, 'Settings scrolled')
    b.cmd('tap 233 45')
    b.wait_screen('weather', 6)


EC_ALERTS = 'https://api.weather.gc.ca/collections/weather-alerts/items'


def alert_point():
    """A point under an alert in force somewhere in Canada, from Environment Canada's API (from the PC): (lat, lon,
    name, area), or None. A vertex of the region's shape: the display asks for the alerts of a 0.01-degree box around
    its point, which the server intersects with the regions, so a point on the edge finds the region."""
    import datetime
    import json
    import urllib.request
    try:
        now = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S')
        lst = json.load(urllib.request.urlopen(f'{EC_ALERTS}?f=json&lang=en&skipGeometry=true&limit=100', timeout=20))
        for f in lst.get('features', [])[:10]:
            p = f.get('properties', {})
            if p.get('status_en') in ('ended', 'cancelled') or (p.get('expiration_datetime') or '9') < now:
                continue
            raw = urllib.request.urlopen(f'{EC_ALERTS}/{f["id"]}?f=json', timeout=20).read()
            if len(raw) > 150 * 1024:                       # (the display takes shapes up to 160 KB)
                continue
            g = json.loads(raw)['geometry']
            ring = g['coordinates'][0] if g['type'] == 'Polygon' else g['coordinates'][0][0]
            lon, lat = ring[0][:2]
            return lat, lon, p.get('alert_name_en', '?'), p.get('feature_name_en', '?')
    except Exception as e:                                  # (no internet on the PC: the test is skipped)
        print(f'    Environment Canada alerts: {e}', flush=True)
    return None


def place_now(b):
    return dict(kv.split('=') for kv in b.cmd('page', r'test: page (.*)').group(1).split())


@test('perf')
def alert_active(ctx):
    """An alert on the first place (console "alert at": its alerts looked up at a point under a real alert, RAM only):
    PSRAM's low point while it arrives, its region map is drawn and the weather and alert screens are snapshot
    (`memlow`), and place drags with it. October 5, a frost advisory at home: alerts_map took ~830 KB of PSRAM at once
    and again at every return to the place (low point 11-190 KB, floor 300), and the switch marked every picture out of
    date, so the drag back waited 124-142 ms for its picture."""
    b = ctx.board
    if 'alert at' not in b.cmd('help', r'test: commands: (.*)').group(1):
        ctx.skip('*alert', 'firmware without the console commands "alert at" and "memlow" (v1.14.1 and older)')
        return
    pt = alert_point()
    if not pt:
        ctx.skip('*alert', 'no alert in force in Canada, or Environment Canada unreachable from the PC')
        return
    lat, lon, name, area = pt
    b.cmd('wake')
    go_weather(ctx)
    while int(place_now(b)['place']) > 0:                   # the first place: the one the test point stands for
        b.cmd('drag 233 100 233 380 300')
        time.sleep(1.5)
    places = int(place_now(b)['places'])
    since = len(ctx.log.lines())
    b.cmd('memlow start', r'test: ok memlow start')
    low = None
    try:
        at = len(ctx.log.lines())
        b.cmd(f'alert at {lat:.5f} {lon:.5f}', r'test: ok alert at')
        m = ctx.log.wait(r'alerts: (\d+) alert\(s\)|alerts: no alerts', 30, 'the alerts at the test point', start=at)
        if not m.group(1):
            ctx.skip('*alert', f'"{name}" ({area}) no longer in force at {lat:.4f},{lon:.4f}')
            return
        m = ctx.log.wait(r'app: alert map (drawn|failed)', 45, 'the region map of the test alert', start=at)
        check(m.group(1) == 'drawn', f'the region map of "{name}" ({area}) failed: see the "alerts:" lines')
        time.sleep(3)                                       # the pictures (slide.c) settle
        for scr in ('weather', 'alert'):
            b.snap(scr, ctx.out(f'alert_active_{scr}.png'))
        if places > 1:
            if time.localtime().tm_sec > 48:                # not across a minute change (see swipes)
                time.sleep(64 - time.localtime().tm_sec)
            measure(ctx, 'drag_place_alert', ['drag 233 380 233 120 400'], settle=1.5)
            check(place_now(b)['place'] == '1', 'the place drag from the alert\'s place did not change the place')
            back = len(ctx.log.lines())
            measure(ctx, 'drag_place_back_alert', ['drag 233 120 233 380 400'], settle=1.5)
            check(place_now(b)['place'] == '0', 'the drag back to the alert\'s place did not change the place')
            ctx.log.wait(r'alerts: \d+ alert\(s\)', 20, 'the alerts again on the way back', start=back)
            time.sleep(2)
            again = ctx.log.count(r'alerts: region shape', start=back)
            check(again == 0, 'back on the alert\'s place, its region map was downloaded and drawn again (it is kept)')
        else:
            ctx.skip('*.drag_place*_alert', 'one place on the display: no place drag')
        m = b.cmd('memlow stop', r'test: memlow psram_min=(\d+) internal_min=(\d+)')
        low = int(m.group(1))
        ctx.metric('psram_min_kb.alert', low)
        ctx.note(f'alert "{name}" ({area}) at {lat:.4f},{lon:.4f}: region map drawn, then snapshots'
                 f'{" and place drags" if places > 1 else ""}; PSRAM low point {low} KB, internal {m.group(2)} KB '
                 f'(alert_active_*.png)')
    finally:
        if low is None:
            try:
                b.cmd('memlow stop', r'test: memlow')
            except Fail:
                pass
        at = len(ctx.log.lines())
        b.cmd('alert at off', r'test: ok alert at off')
        if place_now(b)['place'] == '0':                     # the first place's own alerts again
            ctx.log.wait(r'alerts: (\d+ alert\(s\)|no alerts)', 30, 'the first place\'s own alerts back', start=at)
        radar_settled(ctx, since)


# ---------------------------------------------------------------- start-up lines

@test('boot')
def start_lines_after_reset(ctx):
    """The lines the tools rely on come after the first ~2.5 s, which a PC monitor misses after a reset on USB
    Serial/JTAG (espforge LESSONS L154): before v1.13.0 "ota: Running" was lost in about one restart out of two."""
    b = ctx.board
    ctx.reset_ok = True                              # this test restarts the board on purpose
    for i in range(3):
        at = len(ctx.log.lines())
        b.cmd('reboot', r'test: ok restarting')
        ctx.log.wait(r'ota: Running \S+ from', 40, f'"ota: Running" after restart {i + 1}', start=at)
        ctx.log.wait(r'diag: firmware ', 10, f'the boot info after restart {i + 1}', start=at)
    ctx.log.wait(r'diag: mark first weather', 120, 'the first forecast after the restarts', start=at)
    ctx.note('3 restarts: "ota: Running" and the boot info logged each time')


# ---------------------------------------------------------------- Wi-Fi: lost at run time

def back_online(ctx, wait=20):
    """After a Wi-Fi test, also one that failed half-way: setup closed, the saved network restored, connected. The next
    test must not inherit a fake network or an open setup screen (one failure once failed the two tests after it).
    Firmware before v1.13.0 can refuse 'wifi online' at every try (an attempt is nearly always running: the bug
    online_during_attempt finds): then a restart, which forgets the fake network."""
    b = ctx.board
    if b.screen().startswith('setup'):
        b.cmd('tap 233 233')                         # closes setup (offline: "Tap to try again")
        time.sleep(1)
    end = time.time() + wait
    while b.wifi()['connected'] != '1':
        if time.time() > end:
            ctx.reset_ok = True
            at = len(ctx.log.lines())
            b.cmd('reboot', r'test: ok restarting')
            ctx.log.wait(r'diag: mark first weather', 120, 'back online after a restart', start=at)
            ctx.note('(back online by a restart: "wifi online" was refused)')
            return
        b.cmd('wifi online', r'test: wifi (.*)')
        time.sleep(5)


def recovering(ctx):
    """with recovering(ctx): ... runs back_online() after the block, without hiding the block's own failure."""
    import contextlib

    @contextlib.contextmanager
    def cm():
        try:
            yield
        except Exception:
            try:
                back_online(ctx)
            except Exception as e:                   # the test's own failure is the one to report
                ctx.note(f'(and getting back online failed: {e})')
            raise
        back_online(ctx)
    return cm()


@test('wifi_runtime')
def online_during_attempt(ctx):
    """'wifi online' while the station is busy with an attempt: esp_wifi_set_config() refuses then ("sta is
    connecting, cannot set config"), and firmware before v1.13.0 set the config before disconnecting: the refusal
    went unnoticed and it kept trying the fake network forever (espforge LESSONS L155). v1.12.3 fails this test
    (ESP-IDF's own E line in its log, checked October 4); v1.13.0 disconnects first."""
    b = ctx.board
    go_weather(ctx)
    at = len(ctx.log.lines())
    b.cmd('wifi offline', r'test: wifi (.*)')
    with recovering(ctx):
        end = time.time() + 15
        while b.wifi()['connected'] == '1':
            if time.time() > end:
                raise Fail('still connected 15 s after the network was taken away')
            time.sleep(1)
        # An attempt starts 1 s after each failure and scans for the missing network for 1-2 s: land inside one
        r0 = b.wifi()['retries']
        end = time.time() + 20
        while b.wifi()['retries'] == r0:
            if time.time() > end:
                raise Fail('no reconnect attempt within 20 s')
            time.sleep(0.2)
        time.sleep(1.3)
        b.cmd('wifi online', r'test: wifi (.*)')
        end = time.time() + 30
        while b.wifi()['connected'] != '1':
            if time.time() > end:
                raise Fail('not connected 30 s after "wifi online" sent during an attempt (ESP-IDF refused the '
                           f'config {ctx.log.count(r"cannot set config", start=at)} time(s))')
            time.sleep(1)
        refused = ctx.log.count(r'cannot set config|station config not set', start=at)
        check(refused == 0, f'the station config was refused {refused} time(s) (set while connecting)')
        ctx.note('"wifi online" during an attempt: accepted (disconnect first), connected')


@test('wifi_runtime')
def setup_pages_quick(ctx):
    """The Easy Connect page appears at once (its channel scan runs in a task, not in the swipe's handler: it froze
    the screen 1.6-2.6 s before v1.13.0), and quick switches back don't crash (Easy Connect deinit before its listen
    started: an assert and a restart, espforge LESSONS L165). Offline, where setup opens on a long-press."""
    b = ctx.board
    go_weather(ctx)
    b.cmd('wifi offline', r'test: wifi (.*)')
    with recovering(ctx):
        end = time.time() + 15
        while b.wifi()['connected'] == '1':
            if time.time() > end:
                raise Fail('still connected 15 s after the network was taken away')
            time.sleep(1)
        b.cmd('press 233 233')
        b.wait_screen('setup0', 6)
        at = len(ctx.log.lines())
        b.cmd('swipe left')
        t0 = time.time()
        b.wait_screen('setup1', 5, step=0.05)
        shown = time.time() - t0
        check(shown < 0.6, f'the Easy Connect page took {shown:.2f} s after the swipe (the radio work blocks the screen)')
        ctx.log.wait(r'Easy Connect: setup AP held on channel \d+', 10, 'the setup network held on the channel', start=at)
        check(b.wifi()['ap'] == '1', 'the setup network is down on the Easy Connect page')
        m = b.cmd('setup fail', r'test: setup fail lines=(\d+) bottom=(\d+)')   # the real page after a failure
        check(m.group(1) == '2' and int(m.group(2)) < 440,
              f'Easy Connect failure text: {m.group(1)} lines, bottom at y={m.group(2)} (expected 2 lines above the dots)')
        for i in range(3):                           # back within ~0.3 s: before Easy Connect's listen started
            at = len(ctx.log.lines())
            b.cmd('swipe right')
            b.cmd('swipe left')
            b.wait_screen('setup1', 6, step=0.2)
            ctx.log.wait(r'Easy Connect: QR code ready, listening', 15, f'Easy Connect listening again (round {i + 1})',
                         start=at)
        b.cmd('swipe right')
        b.wait_screen('setup0', 6)
        b.cmd('tap 233 233')                         # close: "Tap to try again"
        ctx.log.wait(r'Setup closed: trying the saved network again', 10)

    ctx.note(f'Easy Connect page shown {shown:.2f} s after the swipe; 3 quick switches back and forth, no restart')


@test('wifi_runtime')
def lose_and_recover(ctx):
    """Saved network disappears while running: retries go on; opening setup pauses them; closing resumes them."""
    b = ctx.board
    go_weather(ctx)
    b.cmd('wifi offline', r'test: wifi (.*)')
    end = time.time() + 15
    while b.wifi()['connected'] == '1':
        if time.time() > end:
            raise Fail('still connected 15 s after the network was taken away')
        time.sleep(1)
    r0 = int(b.wifi()['retries'])
    time.sleep(8)
    r1 = int(b.wifi()['retries'])
    check(r1 > r0, f'no reconnect attempts while offline without setup (retries {r0} -> {r1})')
    b.cmd('press 233 233')                           # offline: the long-press opens Wi-Fi setup directly
    b.wait_screen('setup0', 6)
    ctx.log.wait(r'Setup open: not trying the saved network', 5)
    w0 = b.wifi()
    time.sleep(10)
    w1 = b.wifi()
    check(w1['retries'] == w0['retries'], f'reconnect attempts while setup is open (retries {w0["retries"]} -> {w1["retries"]})')
    check(w1['ap'] == '1', 'setup network not up')
    b.cmd('tap 233 233')                             # close: "Tap to try again"
    ctx.log.wait(r'Setup closed: trying the saved network again', 5)
    back = len(ctx.log.lines())
    b.cmd('wifi online', r'test: wifi (.*)')
    end = time.time() + 40
    while b.wifi()['connected'] != '1':
        if time.time() > end:
            raise Fail('did not reconnect within 40 s after the network came back')
        time.sleep(1)
    if b.key():                                      # since v1.12.0: Wi-Fi back -> an update check at once
        ctx.log.wait(r'ota: checking', 30, 'an update check when Wi-Fi came back', start=back)
    ctx.note(f'retries paused at {w0["retries"]} while setup was open; reconnected after "wifi online"')


# ---------------------------------------------------------------- Wi-Fi: unreachable at start-up (the October 1 bugs)

def phone_check(ctx, ap_seen_by_pc=True):
    """The PC joins the setup network like a phone: DNS, captive-portal redirect, setup page, stays connected."""
    w = ctx.wifi
    ctx.board.cmd('portal windows-quiet')            # no msn.com tab on this PC (see main/web.c)
    w.join_setup(ctx.board.wifi().get('ap_pass', OLD_SETUP_PASS))   # this display's own password
    try:
        ctx.log.wait(r'wifi:station: .* join', 15, 'the board sees the PC join')
        for name in ('connectivitycheck.gstatic.com', 'captive.apple.com', 'example.org'):
            a = dns_query(SETUP_IP, name)
            check(a == SETUP_IP, f'DNS for {name} gave {a}, expected {SETUP_IP} (captive portal needs the DNS server)')
        st, hdr, _ = http_get(SETUP_IP, '/generate_204', 'connectivitycheck.gstatic.com')
        check(st == 302 and hdr.get('Location', '').startswith('http://192.168.4.1'),
              f'Android connectivity check: HTTP {st} {hdr.get("Location")}, expected 302 to the portal')
        st, _, body = http_get(SETUP_IP, '/', SETUP_IP)
        check(st == 200 and b'</html>' in body[-300:], f'setup page: HTTP {st}, {len(body)} bytes')
        st, _, body = http_get(SETUP_IP, '/api/config', SETUP_IP)
        check(st == 200 and b'"units"' in body, f'GET /api/config on the setup network: HTTP {st}')
        if ctx.board.key():                           # since v1.12.0 (main/web.c, "Who may change things")
            check(b'"lat"' not in body, 'the setup network gets the coordinates of the places')
            st, _, body = http_get(SETUP_IP, '/api/sound', SETUP_IP)
            check(st == 200 and b'"level"' in body, f'GET /api/sound on the setup network (the Sound card): HTTP {st}')
            st, _, _ = http_get(SETUP_IP, '/api/snapshot', SETUP_IP)
            check(st == 403, f'snapshot on the setup network: HTTP {st}, expected 403')
        at = len(ctx.log.lines())
        time.sleep(15)                               # a reconnect attempt used to knock phones off here
        state, ssid = w.state()
        check(state == 'connected' and ssid == 'Weather-Setup', f'PC dropped off the setup network ({state} {ssid})')
        check(ctx.log.count(r'wifi:station: .* leave', start=at) == 0, 'the board saw the PC leave')
    finally:
        w.leave()


@test('wifi_setup')
def unreachable_at_startup(ctx):
    b, w = ctx.board, ctx.wifi
    if not w.available():
        raise Fail('no Wi-Fi card on the PC')
    saved = b.wifi()['sta_ssid']
    seen = w.scan()
    want_ch = seen.get(saved)
    ctx.note(f'saved network "{saved}" seen by the PC on channel {want_ch}')

    # 1. restart with the saved network unreachable: Connecting... -> offline setup after ~30 s
    ctx.reset_ok = True                              # this test restarts the board on purpose
    b.cmd('wifi offline-boot', r'test: ok restarting')
    ctx.log.wait(READY, 30, 'the restart')
    ctx.log.wait(r'TEST: this boot uses', 10, 'the fake network on this boot')
    ctx.log.wait(r'offering the setup network', 50, 'offline setup after the 30 s connect timeout')
    b.wait_screen('setup0', 5)
    ctx.log.wait(r'Setup open: not trying the saved network', 5)
    w0 = b.wifi()
    check(w0['ap'] == '1' and w0['connected'] == '0', f'unexpected state {w0}')

    # 2. no reconnect attempts while setup is open; a phone (the PC) joins and gets the captive portal
    phone_check(ctx)
    w1 = b.wifi()
    check(w1['retries'] == w0['retries'], f'reconnect attempts while setup was open ({w0["retries"]} -> {w1["retries"]})')

    # 3. Easy Connect: listens on the saved network's channel (the phone's), not 6
    at = len(ctx.log.lines())
    b.cmd('swipe left')
    b.wait_screen('setup1', 6)
    m = ctx.log.wait(r'Easy Connect: channel (\d+) \(([^,]+),', 15, start=at)
    ch, why = int(m.group(1)), m.group(2)
    # The setup network stays up, moved to that channel: it keeps the radio there for the phone's confirmation
    # (without it a Pixel 8 Pro's confirmation was never received, v1.13.0)
    held = int(ctx.log.wait(r'Easy Connect: setup AP held on channel (\d+)', 10, start=at).group(1))
    check(held == ch, f'setup network held on channel {held}, Easy Connect listens on {ch}')
    ctx.log.wait(r'Easy Connect: QR code ready, listening on channel', 10, start=at)
    check(b.wifi()['ap'] == '1', 'the setup network is down on the Easy Connect page')
    if want_ch:
        check(ch == want_ch, f'Easy Connect listens on channel {ch} ({why}); "{saved}" is on {want_ch}')
    ctx.note(f'Easy Connect channel {ch} ({why}), setup network held there')
    if ctx.opts.phone:
        ctx.ask('On your Android phone (connected to "%s"): scan the code on the display now with the camera or '
                'any QR scanner. The harness waits 3 minutes.' % saved)
        ctx.log.wait(r'Easy Connect: received "', 180, 'credentials from the phone')
        ctx.log.wait(r'net: Connected, IP', 90, 'the restart joins the network')
        ctx.note('Easy Connect: phone sent the network, board restarted and connected')
        ctx.skip('internal_min_kb.reconnect', 'Easy Connect restarted the board (--phone)')
        return                                       # the restart cleared the fake network: done

    # 4. back to the setup network: its DNS must work (stop_dns_server leaked the socket once: errno 112). Since
    # v1.13.0 it stayed up during Easy Connect, so it isn't started again.
    at = len(ctx.log.lines())
    b.cmd('swipe right')
    b.wait_screen('setup0', 6)
    ctx.log.wait(r'Easy Connect stopped', 10, start=at)
    phone_check(ctx)
    check(ctx.log.count(r'unable to bind') == 0, 'DNS server could not bind port 53 again')

    # 5. tap = try the saved network for 30 s; still unreachable -> setup comes back
    b.cmd('tap 233 233')
    ctx.log.wait(r'Setup closed: trying the saved network again', 5)
    b.wait_screen('message', 5)
    ctx.log.wait(r'Still can\'t reach', 45, 'setup comes back after the 30 s retry')
    b.wait_screen('setup0', 5)

    # 6. network back: restore it, close setup, the display carries on
    b.cmd('wifi online', r'test: wifi (.*)')
    b.cmd('tap 233 233')
    ctx.log.wait(r'Saved network is back', 45, 'reconnect after closing setup')
    # The weather screen comes with the first forecast. A failed fetch is retried 30 s later: on October 2 Open-Meteo
    # timed out once and a 30 s wait ended a second before the retry (v1.11.1's only failure). Wait for two tries.
    back = len(ctx.log.lines())
    end = time.time() + 75
    while b.screen() not in ('weather',):
        if time.time() > end:
            raise Fail(f'not back on the weather screen ({b.screen()}) 75 s after the network came back')
        time.sleep(1)
    if any('Update failed' in l for l in ctx.log.lines()[back:]):
        ctx.note('the first forecast after reconnecting failed (service slow): back on the weather screen at the retry')
    # The lowest internal RAM of the boot, now that the reconnect path ran (5 KB once: the perf suite's reading,
    # taken before these tests, never saw it)
    ctx.log.wait(r'diag: mark first weather', 60, 'the first round of fetches after reconnecting', start=back)
    ctx.metric('internal_min_kb.reconnect', b.heap()['min'])
    # The captive portal's DNS server (running since the setup network started) answers the setup network only:
    # bound to all interfaces it also answered every name with 192.168.4.1 on the home network (v1.12.0-rc.5)
    a = dns_query(b.ip, 'example.com', timeout=2)
    check(a is None or not ctx.board.dns_lan_ok_expected(), f'the display answers DNS on the home network (example.com -> {a})')
    if a is not None:
        ctx.note(f'the display answers DNS on the home network ({a}) (firmware before v1.12.0-rc.5)')
    ctx.note('start-up offline path: setup, captive portal x2, Easy Connect channel, retry, recovery')


# ---------------------------------------------------------------- presence: dim, off, wake

@test('presence')
def dim_off_wake(ctx):
    """Short delays through the API (the user's put back after): ACTIVE -> DIM -> OFF -> wake. Swipes while it fades
    up: the brightness command (presence task, core 0) and the panel transfers (LVGL, core 1) at the same time, the
    cross-core case that hung raw frames (display_brightness() waits for LVGL's last band). 'where' must stay clear."""
    b = ctx.board
    saved = b.api('/api/presence')
    keys = ('enabled', 'margin_db', 'wake_s', 'dim_s', 'off_s', 'bright_pct', 'dim_pct', 'motion_wake')
    orig = {k: saved[k] for k in keys}
    state = lambda: re.search(r'state=(\d) brightness=(\d+)', b.cmd('presence', r'test: presence (.*)').group(1))
    try:
        go_weather(ctx)
        # loud never (60 dB over the background), no pick-up: only time decides
        b.api('/api/presence', {'enabled': True, 'margin_db': 60, 'dim_s': 3, 'off_s': 4, 'motion_wake': False})
        for i in range(3):
            at = len(ctx.log.lines())
            ctx.log.wait(r'presence: ACTIVE -> DIM', 15, 'dims after 3 s of quiet', start=at)
            time.sleep(1.2)                          # faded down
            b.cmd(['swipe right', 'swipe left'][i % 2])   # the touch wakes it: fades up while the screen moves
            time.sleep(0.3)
            b.cmd('fps', r'test: fps ')             # a console command between: answers while both run
            time.sleep(1.5)
            w = b.cmd('where', r'test: where (.*)').group(1)
            check('raw_phase=0' in w, f'display busy after a fade during a move: {w}')
        go_weather(ctx)
        at = len(ctx.log.lines())
        ctx.log.wait(r'presence: DIM -> OFF', 20, 'off after 3 + 4 s of quiet', start=at)
        time.sleep(1.5)
        m = state()
        check(m.group(1) == '2' and m.group(2) == '0', f'not off: state {m.group(1)}, brightness {m.group(2)}')
        b.cmd('wake')
        time.sleep(1.5)
        m = state()
        check(m.group(1) == '0' and int(m.group(2)) > 0, f'not awake: state {m.group(1)}, brightness {m.group(2)}')
        ctx.note('dim -> off -> wake; three fades during moves, display never stuck')
    finally:
        b.api('/api/presence', orig)
        b.cmd('wake')


@test('wifi_setup')
def setup_stops_opening_by_itself(ctx):
    """Router out for long: the setup network opens by itself for 15 min only (the owner's choice), then the display
    just keeps trying, and a long-press still opens setup. 'offline-boot-short' makes that 60 s for one boot."""
    b = ctx.board
    if not b.key():
        ctx.note('firmware without the settings key (before v1.12.0-rc.3): not checked')
        return
    ctx.reset_ok = True
    b.cmd('wifi offline-boot-short', r'test: ok restarting')
    ctx.log.wait(READY, 30, 'the restart')
    at = len(ctx.log.lines())
    ctx.log.wait(r'offering the setup network', 50, 'offline setup after the 30 s connect timeout', start=at)
    b.wait_screen('setup0', 5)
    b.cmd('tap 233 233')                             # close: tries the saved network for 30 s
    ctx.log.wait(r'Setup network: no longer opened by itself', 75, 'the 60 s test window running out', start=at)
    time.sleep(2)
    check(b.screen() == 'message', f'setup reopened by itself after the window ({b.screen()})')
    check(b.wifi()['ap'] == '0', 'the setup network is still up')
    b.cmd('press 233 233')                           # a long-press still opens it
    b.wait_screen('setup0', 6)
    b.cmd('wifi online', r'test: wifi (.*)')
    b.cmd('tap 233 233')
    ctx.log.wait(r'Saved network is back', 60, 'reconnect after closing setup', start=at)
    end = time.time() + 75
    while b.screen() != 'weather':
        if time.time() > end:
            raise Fail('not back on the weather screen 75 s after the network came back')
        time.sleep(1)
    ctx.note('automatic setup network: closed after its window, long-press reopens it, recovery')


# ---------------------------------------------------------------- first run

@test('firstrun')
def location_hint(ctx):
    """A new display (the built-in place, never chosen) shows the settings QR by itself once, titled "Choose your
    location", as soon as a forecast is on screen; a tap closes it. 'hint next-boot' asks for it on the next boot
    without touching the places or the real once-only flag."""
    b = ctx.board
    try:
        b.cmd('hint next-boot', r'test: ok hint')
    except Fail as e:
        if 'unknown command' not in str(e):
            raise
        ctx.note('firmware without the first-run hint (before v1.12.0-rc.4): not checked')
        return
    ctx.reset_ok = True
    at = len(ctx.log.lines())
    b.cmd('reboot', r'test: ok')
    ctx.log.wait(READY, 40, 'the restart', start=at)
    ctx.log.wait(r'ui: first run: location hint', 90, 'the hint once the forecast is shown', start=at)
    b.wait_screen('phone', 5)
    b.snap('current', ctx.out('screen_first_run_hint.png'))
    at = len(ctx.log.lines())
    b.cmd('tap 233 233')
    try:                                             # then the gesture hint (v1.12.0-rc.6)
        ctx.log.wait(r'ui: first run: gesture hint', 3, 'the gesture hint after the location hint', start=at)
        time.sleep(1)
        b.snap('current', ctx.out('screen_first_run_gestures.png'))
        b.cmd('tap 233 233')
        gest = ', then the gesture hint'
    except Fail:
        gest = ''
    b.wait_screen('weather', 6)
    ctx.note(f'first-run hint: shown after the first forecast{gest}, closed by a tap')
