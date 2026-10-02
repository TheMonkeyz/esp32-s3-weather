"""Test suites. Each test is a function(ctx) that raises board.Fail on a failed check; ctx.metric() records numbers
for the performance report and ctx.note() adds a line to the report. See docs/TESTING.md, "Harness".
"""
import os
import re
import subprocess
import time

from board import Fail, SETUP_IP, dns_query, http_get, ROOT

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
    ctx.note(f'Wi-Fi "{w["sta_ssid"]}" channel {w["channel"]}, settings API answers')


# ---------------------------------------------------------------- navigation

@test('navigation')
def every_screen(ctx):
    """Swipe through the screens like a person, check where it lands, snapshot each."""
    b = ctx.board
    b.cmd('wake')
    go_weather(ctx)
    route = [('swipe right', 'extras'), ('swipe right', 'status'), ('swipe left', 'extras'),
             ('swipe left', 'weather'), ('swipe left', 'radar'), ('swipe right', 'weather'),
             ('press 233 233', 'settings'), ('tap 233 45', 'weather'),
             ('tap 125 350', 'hourly'), ('swipe left', 'hourly'), ('tap 233 233', 'weather')]
    for cmd, want in route:
        b.cmd(cmd)
        b.wait_screen(want, 6)
        if want not in ctx.snapped:
            ms = b.snap({'hourly': 'current'}.get(want, want), ctx.out(f'screen_{want}.png'))
            ctx.snapped.add(want)
            ctx.metric(f'snapshot_ms.{want}', round(ms))
    for extra in ('settings1', 'settings2', 'settings3', 'phone', 'setup0', 'setup1', 'update'):
        b.snap(extra, ctx.out(f'screen_{extra}.png'))
    ctx.note('screens: ' + ', '.join(sorted(ctx.snapped)) + ' (+ settings1..3, phone, setup0/1, update)')


# ---------------------------------------------------------------- web

@test('web')
def settings_page_tests(ctx):
    """Playwright suite against the mock display (tools/webtest)."""
    wt = os.path.join(ROOT, 'tools', 'webtest')
    env = dict(os.environ)
    env['PATH'] = r'C:\Program Files\nodejs;' + env.get('PATH', '')
    # Microsoft Store Python virtualises AppData\Local for its child processes, so Playwright can't see the browsers
    # in AppData\Local\ms-playwright. Use a copy next to the tests (git-ignored), see docs/TESTING.md.
    browsers = os.path.join(wt, '.browsers')
    if os.path.isdir(browsers):
        env['PLAYWRIGHT_BROWSERS_PATH'] = browsers
    npm = r'C:\Program Files\nodejs\npm.cmd'
    if not os.path.exists(npm):
        raise Fail('Node.js not found (C:\\Program Files\\nodejs)')
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
    import urllib.request
    with urllib.request.urlopen(f'https://{b.ip}/', context=b.ctx, timeout=15) as r:
        page = r.read()
    check(b'</html>' in page[-200:], 'settings page arrived truncated (hardware-AES bug class)')
    ctx.metric('page_kb', len(page) // 1024)


# ---------------------------------------------------------------- performance

def ms_of(line):
    m = re.match(r'[IWE] \((\d+)\)', line)
    return int(m.group(1)) if m else None


@test('perf')
def boot_and_memory(ctx):
    lines = ctx.log.lines()
    for l in lines:
        m = re.search(r'diag: mark (.+?)\s+internal (\d+) KB', l)
        if m:
            ctx.metric(f'boot_s.{m.group(1).strip().replace(" ", "_")}', round(ms_of(l) / 1000, 1))
            ctx.metric(f'boot_internal_kb.{m.group(1).strip().replace(" ", "_")}', int(m.group(2)))
    h = ctx.board.heap()
    ctx.metric('internal_free_kb', h['internal'])
    ctx.metric('internal_min_kb', h['min'])
    ctx.metric('psram_min_kb', h['psram_min'])


@test('perf')
def render_bench(ctx):
    b = ctx.board
    go_weather(ctx)
    b.cmd('bench', r'test: ok bench')
    m = ctx.log.wait(r'diag: bench (render-only full screen:.*|postponed.*)', 20)
    check(m.group(1).startswith('render'), 'bench postponed: the weather screen was not idle')
    for name, v in re.findall(r'(\w+) ([\d.]+) ms', m.group(1)):
        ctx.metric(f'render_ms.{name}', float(v))
    m = ctx.log.wait(r'diag: bench weather incl. panel transfer: ([\d.]+) ms', 5)
    ctx.metric('render_ms.weather_with_panel', float(m.group(1)))


@test('perf')
def radar_timing(ctx):
    """Swipe to the radar: time to the first new frame; play the animation and read the frame rate."""
    b = ctx.board
    go_weather(ctx)
    ctx.log.mark()
    t0 = time.time()
    b.cmd('swipe left')
    b.wait_screen('radar', 6)
    try:
        ctx.log.wait(r'radar: Frame ', 30, 'a radar frame after opening the radar')
        ctx.metric('radar_first_frame_s', round(time.time() - t0, 1))
    except Fail:
        ctx.note('no new radar frame within 30 s (frames may all be cached)')
    ctx.log.wait(r'radar: Lightning ', 30, 'lightning fetched with the frames')
    time.sleep(8)                                    # history frames load while the radar is visible
    b.cmd('tap 233 233')                             # play the last 3 h
    ctx.log.mark()
    m = ctx.log.wait(r'diag: display: .*animation ([\d.]+) fps \((\d+) frames, worst gap (\d+) ms\)', 75,
                     'the next diag period')
    ctx.metric('radar_anim_fps', float(m.group(1)))
    ctx.metric('radar_anim_worst_gap_ms', int(m.group(3)))
    frames = ctx.log.count(r'radar: Frame ')
    lt = [int(x) for x in re.findall(r'radar: Lightning \S+: \d+ px, (\d+) marks', '\n'.join(ctx.log.since_mark()))]
    ctx.note(f'{frames} radar frames loaded during playback; lightning marks per frame: {lt[:15]}')
    b.cmd('tap 233 233')                             # stop
    b.cmd('swipe right')


# ---------------------------------------------------------------- Wi-Fi: lost at run time

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
    b.cmd('wifi online', r'test: wifi (.*)')
    end = time.time() + 40
    while b.wifi()['connected'] != '1':
        if time.time() > end:
            raise Fail('did not reconnect within 40 s after the network came back')
        time.sleep(1)
    ctx.note(f'retries paused at {w0["retries"]} while setup was open; reconnected after "wifi online"')


# ---------------------------------------------------------------- Wi-Fi: unreachable at start-up (the October 1 bugs)

def phone_check(ctx, ap_seen_by_pc=True):
    """The PC joins the setup network like a phone: DNS, captive-portal redirect, setup page, stays connected."""
    w = ctx.wifi
    ctx.board.cmd('portal windows-quiet')            # no msn.com tab on this PC (see main/web.c)
    w.join_setup()
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
        ctx.log.mark()
        time.sleep(15)                               # a reconnect attempt used to knock phones off here
        state, ssid = w.state()
        check(state == 'connected' and ssid == 'Weather-Setup', f'PC dropped off the setup network ({state} {ssid})')
        check(ctx.log.count(r'wifi:station: .* leave') == 0, 'the board saw the PC leave')
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
    ctx.log.wait(r'test: console ready', 30, 'the restart')
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
    b.cmd('swipe left')
    b.wait_screen('setup1', 6)
    m = ctx.log.wait(r'Easy Connect: channel (\d+) \(([^,]+),', 15)
    ch, why = int(m.group(1)), m.group(2)
    ctx.log.wait(r'Easy Connect: QR code ready, listening on channel', 10)
    if want_ch:
        check(ch == want_ch, f'Easy Connect listens on channel {ch} ({why}); "{saved}" is on {want_ch}')
    ctx.note(f'Easy Connect channel {ch} ({why})')
    if ctx.opts.phone:
        ctx.ask('On your Android phone (connected to "%s"): Settings > Wi-Fi > the QR icon, scan the code on the '
                'display now. The harness waits 3 minutes.' % saved)
        ctx.log.wait(r'Easy Connect: received "', 180, 'credentials from the phone')
        ctx.log.wait(r'net: Connected, IP', 90, 'the restart joins the network')
        ctx.note('Easy Connect: phone sent the network, board restarted and connected')
        return                                       # the restart cleared the fake network: done

    # 4. back to the setup network: its DNS must work again (stop_dns_server leaked the socket: errno 112)
    b.cmd('swipe right')
    b.wait_screen('setup0', 6)
    ctx.log.wait(r'Setup AP started', 5)
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
    end = time.time() + 30
    while b.screen() not in ('weather',):
        if time.time() > end:
            raise Fail(f'not back on the weather screen ({b.screen()})')
        time.sleep(1)
    ctx.note('start-up offline path: setup, captive portal x2, Easy Connect channel, retry, recovery')
