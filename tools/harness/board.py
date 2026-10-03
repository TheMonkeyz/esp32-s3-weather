"""The harness's view of the world: the board (through the flash helper's files and its test console), its HTTPS
API and snapshots, and the PC's Wi-Fi card (netsh) acting as a phone on the setup network.

Standard library only. Paths are relative to the repository root (the flash helper's folder).
"""
import json
import os
import re
import socket
import ssl
import struct
import subprocess
import sys
import time
import http.client
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import snapshot  # noqa: E402  (tools/snapshot.py)

SETUP_SSID, SETUP_PASS, SETUP_IP = 'Weather-Setup', 'meteo1234', '192.168.4.1'   # main/net.h


class Fail(Exception):
    """A check failed: the message says what was expected and what happened."""


def p(path):
    return os.path.join(ROOT, path)


class Log:
    """serial_live.txt, written line by line by monitor.ps1 while the helper is logging."""

    def __init__(self):
        self.pos = 0          # lines already looked at by wait()
        self.mark_at = 0      # start of the current test (for lines_since_mark)

    def lines(self):
        try:
            with open(p('serial_live.txt'), encoding='utf-8', errors='replace') as f:
                return f.read().splitlines()
        except OSError:
            return []

    def mark(self):
        self.mark_at = self.pos = len(self.lines())

    def since_mark(self):
        return self.lines()[self.mark_at:]

    def wait(self, pattern, timeout, what=None, start=None):
        """First line matching pattern (regex) after the read position (or after line `start`, which leaves the
        read position alone: console replies use that, so an event logged just before a reply is still found by
        the next wait). Returns the re.Match."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            lines = self.lines()
            for i in range(self.pos if start is None else start, len(lines)):
                m = rx.search(lines[i])
                if m:
                    if start is None:
                        self.pos = i + 1
                    return m
            if time.time() > end:
                raise Fail(f'no log line /{pattern}/ within {timeout} s' + (f' ({what})' if what else ''))
            time.sleep(0.2)

    def count(self, pattern):
        rx = re.compile(pattern)
        return sum(1 for l in self.since_mark() if rx.search(l))


class Board:
    def __init__(self, ip, log):
        self.ip, self.log = ip, log
        self.ctx = ssl.create_default_context()
        self.ctx.check_hostname = False
        self.ctx.verify_mode = ssl.CERT_NONE          # the board's certificate is self-signed

    # ---- flash helper ----
    def helper_status(self):
        try:
            return open(p('flash.status')).read().strip()
        except OSError:
            return ''

    def version_before_restart(self):
        """Version running now, after waiting for a fresh update to be confirmed. A restart in the first 60 s of an
        update rolls it back to the previous firmware (seen: the harness tested the old build instead of rc.2)."""
        try:
            u = self.api('/api/update')
        except Exception:
            return None                                # offline or no API: can't tell, carry on
        if 'pending_verify' not in u:                  # firmware before v1.10.0-rc.3 can't say: assume just updated
            print(f'  {u["current"]} does not report whether it is confirmed: waiting 75 s before restarting',
                  flush=True)
            time.sleep(75)
            return u.get('current')
        if u.get('pending_verify'):
            wait = max(0, 75 - int(u.get('uptime_s', 0)))
            print(f'  {u["current"]} was just installed and is not confirmed yet: waiting {wait} s before restarting',
                  flush=True)
            time.sleep(wait)
            u = self.api('/api/update')
            if u.get('pending_verify'):
                raise Fail(f'{u["current"]} still not confirmed after {u.get("uptime_s")} s: not restarting it')
        return u.get('current')

    def start_log(self, seconds, flash_bin=None):
        """Restart the board (or flash firmware first) and log for `seconds`. Waits until logging."""
        if self.helper_status() not in ('idle', ''):
            raise Fail(f'flash helper busy ({self.helper_status()})')
        self.before = None if flash_bin else self.version_before_restart()
        # serial_live.txt goes too: "logging" shows ~2 s before monitor.ps1 recreates it, and the old one would be
        # read as the new boot
        for f in ('flash.done', 'serial_live.txt'):
            if os.path.exists(p(f)):
                os.remove(p(f))
        if flash_bin:
            import hashlib
            import shutil
            name = 'weather_amoled_harness_%d.bin' % int(time.time())
            os.makedirs(p('firmware/in'), exist_ok=True)
            shutil.copy(flash_bin, p('firmware/in/' + name))
            shutil.copy(p('firmware/in/' + name), p('firmware/weather_amoled.bin'))
            md5 = lambda f: hashlib.md5(open(f, 'rb').read()).hexdigest()
            if md5(flash_bin) != md5(p('firmware/weather_amoled.bin')):
                raise Fail('staged firmware differs from the build (md5)')
            req = 'flash.request'
        else:
            req = 'reboot.request'
        with open(p(req), 'w') as f:
            f.write(str(seconds))
        end = time.time() + 240
        while self.helper_status() != 'logging':
            if time.time() > end:
                raise Fail('the flash helper did not start logging (is start_flash_helper.bat running?)')
            time.sleep(1)
        self.log.pos = 0
        self.log.wait(r'test: console ready', 30, 'firmware with the test console')

    def stop_log(self):
        if self.helper_status() == 'logging':
            open(p('stop.request'), 'w').close()
            end = time.time() + 30
            while not os.path.exists(p('flash.done')) and time.time() < end:
                time.sleep(0.5)

    # ---- test console ----
    def cmd(self, line, expect=r'test: (ok|pong|screen|heap|wifi|presence|bench)', timeout=15):
        at = len(self.log.lines())
        with open(p('serial.send.tmp'), 'w') as f:
            f.write(line + '\n')
        os.replace(p('serial.send.tmp'), p('serial.send'))
        self.log.wait(re.escape('> ' + line), 5, 'helper passed the command on', start=at)
        try:
            m = self.log.wait(r'test: error .*|' + expect, timeout, line, start=at)
        except Fail as e:
            if line == 'where':
                raise
            # no answer: the console task is blocked (usually on the display lock). "where" takes no lock.
            try:
                w = self.cmd('where', r'test: where (.*)', timeout=5).group(1)
            except Fail:
                w = 'no answer either'
            raise Fail(f'{e}; where: {w}')
        if m.group(0).startswith('test: error'):
            if 'busy' in m.group(0) and line != 'where':
                w = self.cmd('where', r'test: where (.*)', timeout=5).group(1)
                raise Fail(f'{line}: {m.group(0)}; where: {w}')
            raise Fail(f'{line}: {m.group(0)}')
        return re.search(expect, m.string)            # the groups of the expected answer

    def screen(self):
        return self.cmd('screen', r'test: screen (\S+)').group(1)

    def wifi(self):
        line = self.cmd('wifi status', r'test: wifi (.*)').group(1)
        return dict(kv.split('=', 1) for kv in line.split())

    def heap(self):
        line = self.cmd('heap', r'test: heap (.*)').group(1)
        return {k: int(v) for k, v in (kv.split('=') for kv in line.split())}

    def wait_screen(self, name, timeout, step=1.0):
        end = time.time() + timeout
        while True:
            s = self.screen()
            if s == name:
                return
            if time.time() > end:
                raise Fail(f'screen is "{s}", expected "{name}" within {timeout} s')
            time.sleep(step)

    # ---- network API (home network, HTTPS) ----
    def api(self, path, data=None, timeout=10):
        req = urllib.request.Request(f'https://{self.ip}{path}', method='POST' if data is not None else 'GET',
                                     data=json.dumps(data).encode() if data is not None else None,
                                     headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, context=self.ctx, timeout=timeout) as r:
            return json.loads(r.read().decode())

    def install(self, want, wait_min=20):
        """Install `want` with the display's own updater, as a person would (Settings, Check for updates): ask it to
        check its channel until it offers `want` (after a tag, CI and the Pages site take ~5 min), install, then wait
        until the new firmware runs and is confirmed (a restart in its first 60 s would roll it back). Needs a log
        window. The display's channel (Beta / Stable) is left as it is: an rc needs Beta."""
        end = time.time() + wait_min * 60
        while True:
            u = self.api('/api/update', {'action': 'check'})
            for _ in range(40):                        # the check runs in the OTA task: a few seconds
                if u.get('state') != 'checking':
                    break
                time.sleep(1)
                u = self.api('/api/update')
            if u.get('current') == want:
                return
            if u.get('latest') == want and u.get('state') == 'available':
                break
            offer = f'the {u.get("channel")} channel offers {u.get("latest") or "nothing newer"}'
            if time.time() > end:
                raise Fail(f'{offer}, not {want}, after {wait_min} min (CI failed, or {want} needs the Beta channel?)')
            print(f'  {offer} (running {u.get("current")}): waiting for {want}', flush=True)
            time.sleep(60)
        print(f'  offered: installing {want}', flush=True)
        at = len(self.log.lines())
        self.api('/api/update', {'action': 'install'})
        self.log.wait(r'ota: Update installed, restarting', 300, 'download and install', start=at)
        self.log.wait(r'ota: Running ' + re.escape(want) + ' from', 90, f'{want} starting', start=at)
        self.log.wait(r'ota: New firmware ran \d+ s: marked valid', 120, 'the update confirmed (no rollback)', start=at)

    def snap(self, screen, out):
        t0 = time.time()
        with urllib.request.urlopen(f'https://{self.ip}/api/snapshot?screen={screen}', context=self.ctx,
                                    timeout=30) as r:
            bmp = r.read()
        ms = (time.time() - t0) * 1000
        open(out, 'wb').write(snapshot.bmp_to_png(bmp))
        return ms


class PCWifi:
    """The PC's Wi-Fi card as a phone on the setup network (Ethernet stays up, so the PC stays online)."""

    PROFILE = '''<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>{ssid}</name>
  <SSIDConfig><SSID><name>{ssid}</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType><connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{key}</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>'''

    def __init__(self, iface='Wi-Fi'):
        self.iface = iface

    def netsh(self, *args):
        r = subprocess.run(['netsh', 'wlan', *args], capture_output=True, text=True, errors='replace')
        return r.stdout + r.stderr

    def available(self):
        return 'Name' in self.netsh('show', 'interfaces')

    def scan(self):
        """{ssid: 2.4 GHz channel} of the networks the PC sees. The display is 2.4 GHz only, and a dual-band router
        lists the same name on 5 GHz too (seen: "338" on 153 and on 1)."""
        out = self.netsh('show', 'networks', 'mode=bssid')
        nets, ssid = {}, None
        for line in out.splitlines():
            m = re.match(r'\s*SSID \d+ : (.*)', line)
            if m:
                ssid = m.group(1).strip()
                continue
            m = re.match(r'\s*Channel\s*:\s*(\d+)', line)
            if m and ssid is not None and int(m.group(1)) <= 14 and ssid not in nets:
                nets[ssid] = int(m.group(1))
        return nets

    def state(self):
        out = self.netsh('show', 'interfaces')
        st = re.search(r'^\s*State\s*:\s*(\S+)', out, re.M)
        ss = re.search(r'^\s*SSID\s*:\s*(.+)$', out, re.M)
        return (st.group(1) if st else '?', ss.group(1).strip() if ss else '')

    def join_setup(self, timeout=40):
        path = p('tools/harness/.setup_profile.xml')
        open(path, 'w').write(self.PROFILE.format(ssid=SETUP_SSID, key=SETUP_PASS))
        self.netsh('add', 'profile', f'filename={path}', f'interface={self.iface}', 'user=current')
        os.remove(path)
        end = time.time() + timeout
        while time.time() < end:
            self.netsh('connect', f'name={SETUP_SSID}', f'interface={self.iface}')
            for _ in range(10):
                time.sleep(1)
                st, ssid = self.state()
                if st == 'connected' and ssid == SETUP_SSID and self.has_setup_ip():
                    return
        raise Fail(f'the PC could not join {SETUP_SSID} within {timeout} s (state {self.state()})')

    def has_setup_ip(self):
        out = subprocess.run(['ipconfig'], capture_output=True, text=True, errors='replace').stdout
        return '192.168.4.' in out

    def leave(self):
        self.netsh('disconnect', f'interface={self.iface}')
        self.netsh('delete', 'profile', f'name={SETUP_SSID}', f'interface={self.iface}')


def dns_query(server, name, timeout=3):
    """A record for name from server (one UDP query); None if no answer."""
    q = struct.pack('>HHHHHH', 0x1234, 0x0100, 1, 0, 0, 0)
    q += b''.join(bytes([len(x)]) + x.encode() for x in name.split('.')) + b'\0' + struct.pack('>HH', 1, 1)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(q, (server, 53))
        r, _ = s.recvfrom(512)
    except OSError:
        return None
    finally:
        s.close()
    if struct.unpack('>H', r[6:8])[0] < 1:
        return None
    return '.'.join(str(b) for b in r[-4:])          # the server answers with one A record at the end


def http_get(host_ip, path, host_header, timeout=5):
    c = http.client.HTTPConnection(host_ip, 80, timeout=timeout)
    try:
        c.request('GET', path, headers={'Host': host_header})
        r = c.getresponse()
        return r.status, dict(r.getheaders()), r.read()
    finally:
        c.close()
