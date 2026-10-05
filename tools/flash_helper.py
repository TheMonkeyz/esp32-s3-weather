#!/usr/bin/env python3
"""Flash helper for macOS and Linux (and Windows): the same file protocol as flash_helper.ps1 + monitor.ps1, so the
harness (tools/harness), snapshot.py and AI sessions work unchanged. Run it from a shell where ESP-IDF's export.sh
was sourced (its Python has esptool and pyserial). See docs/MACOS.md.

    python tools/flash_helper.py                   # helper: waits for flash.request / reboot.request (Ctrl-C ends it)
    python tools/flash_helper.py stage build/v55   # copy a build's 4 images into firmware/ (md5 checked)
    python tools/flash_helper.py flash 60          # flash firmware/*.bin once, log 60 s (flash.bat)
    python tools/flash_helper.py reboot 60         # restart once (no flash), log 60 s
    python tools/flash_helper.py monitor 60        # log only (monitor.ps1)
    python tools/flash_helper.py ports             # the serial ports, the board's marked

Files in the repository root (all git-ignored), as the PowerShell helper:
    flash.request / reboot.request   written by a client (content: seconds of log); the helper deletes it
    flash.status                     idle, flashing, logging or flash_failed
    flash.done                       exit=… port=… flash_s=… total_s=… errors=… warnings=… resets=… stopped_early=…
    serial_live.txt                  the log, line by line, while logging
    serial_log.txt                   the whole log window, written at its end
    serial.send                      lines for the board's test console (main/testcon.c), echoed as "> line"
    stop.request                     ends the log window early (or press q / Esc in the helper's terminal)
    flash_log.txt, flash_helper.log  esptool's output; the helper's history

The board's port: --port, else $WEATHER_PORT, else the ESP32-S3's own USB (VID 0x303A; macOS /dev/cu.usbmodem…),
else esptool's choice. The board's USB disappears at every restart (native USB): the monitor reopens it.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
ESPRESSIF_VID = 0x303A
FLASH_ARGS = ['--chip', 'esp32s3', '-b', '460800', '--before', 'default_reset', '--after', 'hard_reset',
              'write_flash', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB']
IMAGES = [('0x0', 'bootloader.bin', 'bootloader/bootloader.bin'),
          ('0x8000', 'partition-table.bin', 'partition_table/partition-table.bin'),
          ('0x10000', 'weather_amoled.bin', 'weather_amoled.bin'),
          ('0x610000', 'ota_data_initial.bin', 'ota_data_initial.bin')]   # optional: two-slot (OTA) boot selection


def p(name):
    return os.path.join(ROOT, name)


def stamp():
    return time.strftime('%H:%M:%S')


def say(msg, quiet=False):
    if not quiet:
        print(f'[{stamp()}] {msg}', flush=True)
    with open(p('flash_helper.log'), 'a', encoding='utf-8') as f:
        f.write(f'[{time.strftime("%Y-%m-%dT%H:%M:%S")}] {msg}\n')


def write(name, text):
    with open(p(name), 'w', encoding='utf-8') as f:
        f.write(text)


def remove(name):
    try:
        os.remove(p(name))
    except OSError:
        pass


def need_serial():
    try:
        import serial  # noqa: F401
        import serial.tools.list_ports  # noqa: F401
    except ImportError:
        sys.exit('pyserial is missing: run this from a shell where ESP-IDF is set up '
                 '(". $IDF_PATH/export.sh" on macOS / Linux), see docs/MACOS.md')


def md5(path):
    with open(path, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()


# ---------------------------------------------------------------------------------------------------- ports

def board_ports():
    """[(device, description)] of the ports with Espressif's USB vendor id, best first (macOS: cu.*, not tty.*)."""
    import serial.tools.list_ports
    found = [(x.device, x.description) for x in serial.tools.list_ports.comports() if x.vid == ESPRESSIF_VID]
    return sorted(found, key=lambda d: ('/tty.' in d[0], d[0]))


def find_port(explicit=None):
    if explicit:
        return explicit
    if os.environ.get('WEATHER_PORT'):
        return os.environ['WEATHER_PORT']
    ports = board_ports()
    return ports[0][0] if ports else None


def list_ports():
    need_serial()
    import serial.tools.list_ports
    for x in serial.tools.list_ports.comports():
        mark = '  <- ESP32-S3 (this board)' if x.vid == ESPRESSIF_VID else ''
        vid = f'{x.vid:04X}:{x.pid:04X}' if x.vid is not None else '-'
        print(f'{x.device}  [{vid}] {x.description}{mark}')


# ---------------------------------------------------------------------------------------------------- stage

def stage(build_dir):
    """Copy a build's images into firmware/ under fresh names first, and refuse if a checksum differs (a reused path
    delivered a stale file twice on the Windows PC, CLAUDE.md)."""
    build_dir = os.path.abspath(build_dir)
    os.makedirs(p('firmware/in'), exist_ok=True)
    tag = time.strftime('%Y%m%d_%H%M%S')
    for _, dst, src in IMAGES:
        s = os.path.join(build_dir, src)
        if not os.path.exists(s):
            sys.exit(f'{s} is missing: build first (docs/MACOS.md, "Build")')
        unique = p(f'firmware/in/{tag}_{dst}')
        shutil.copy(s, unique)
        shutil.copy(unique, p('firmware/' + dst))
        if md5(s) != md5(p('firmware/' + dst)):
            sys.exit(f'staged firmware/{dst} differs from {s} (md5): not staged')
        print(f'firmware/{dst:<22} {os.path.getsize(s):>9,} bytes  md5 {md5(s)}')


# ---------------------------------------------------------------------------------------------------- esptool

def esptool(args):
    """Runs esptool from this Python (ESP-IDF's has it); returns (exit code, output lines, port it used)."""
    cmd = [sys.executable, '-m', 'esptool'] + args
    lines = []
    try:
        pr = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors='replace', bufsize=1)
    except OSError as e:
        return 1, [f'could not start esptool: {e}'], None
    for line in pr.stdout:
        line = line.rstrip('\r\n')
        lines.append(line)
        if re.search(r'Serial port|Chip is|Writing at .*\(100 %\)|Wrote |Hash of data|Hard resetting|[Ee]rror|failed|'
                     r'No module named', line):
            print('    ' + line, flush=True)
    rc = pr.wait()
    with open(p('flash_log.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    port = next((m.group(1) for m in map(re.compile(r'Serial port (\S+)').search, reversed(lines)) if m), None)
    return rc, lines, port


def flash_args(port):
    missing = [f for _, f, _ in IMAGES[:3] if not os.path.exists(p('firmware/' + f))]
    if missing:
        return None, 'firmware/ has no ' + ', '.join(missing) + ': stage a build first (flash_helper.py stage build/v55)'
    args = (['--port', port] if port else []) + FLASH_ARGS
    for off, f, _ in IMAGES:
        if os.path.exists(p('firmware/' + f)):
            args += [off, os.path.join('firmware', f)]
    return args, None


# ---------------------------------------------------------------------------------------------------- monitor

class Keys:
    """q / Esc in the helper's own terminal ends the log window (as in the PowerShell window). No-op without a tty."""

    def __init__(self):
        self.old = None
        self.win = os.name == 'nt'
        if not self.win and sys.stdin.isatty():
            try:
                import termios
                import tty
                self.old = termios.tcgetattr(sys.stdin)
                tty.setcbreak(sys.stdin.fileno())
            except Exception:
                self.old = None

    def pressed_stop(self):
        try:
            if self.win:
                import msvcrt
                while msvcrt.kbhit():
                    if msvcrt.getwch() in ('q', 'Q', '\x1b'):
                        return True
                return False
            if self.old is None:
                return False
            import select
            while select.select([sys.stdin], [], [], 0)[0]:
                if sys.stdin.read(1) in ('q', 'Q', '\x1b'):
                    return True
        except Exception:
            pass
        return False

    def close(self):
        if self.old is not None:
            import termios
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, self.old)


def open_port(port, url=False, wait=10.0):
    """The port opened with DTR and RTS low (on the ESP32-S3's USB they reset the chip / select download mode).
    Retries for `wait` s: after a reset the USB device re-enumerates, and may come back under another name (macOS
    numbers usbmodem ports by USB location): then the board is found again by its vendor id. None if it never opens."""
    import serial
    end = time.time() + wait
    while True:
        try:
            if url:
                s = serial.serial_for_url(port, baudrate=115200, timeout=0.1)
            else:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = port, 115200, 0.1
                s.dtr, s.rts = False, False
                s.open()
            return s
        except (OSError, serial.SerialException):
            if time.time() > end:
                return None
            time.sleep(0.5)
            if not url and not os.path.exists(port):    # (COM names never "exist": the lookup finds the same one)
                ports = board_ports()
                if ports and ports[0][0] != port:
                    say(f'(the board came back as {ports[0][0]}, was {port})', quiet=True)
                    port = ports[0][0]


def monitor(port, seconds, url=False, echo=True):
    """Log the board for `seconds` into serial_live.txt (line by line) and serial_log.txt (at the end); pass each line
    of serial.send to the board. Returns (lines, stopped_early, error)."""
    import serial
    remove('serial.send')                           # stale commands from an earlier run
    out = []
    live = open(p('serial_live.txt'), 'w', encoding='utf-8', newline='\n')
    keys = Keys()

    def emit(line):
        out.append(line)
        live.write(line + '\n')
        live.flush()
        if echo:
            print(line, flush=True)

    if not url:
        time.sleep(2)                               # let USB re-enumerate after the reset
    s = open_port(port, url)
    if s is None:
        live.close()
        keys.close()
        write('serial_log.txt', f'Could not open {port}\n')
        return [], False, f'could not open {port}'
    end, buf, stopped, next_check, reopened = time.time() + seconds, b'', False, 0.0, 0
    try:
        while time.time() < end:
            try:
                chunk = s.read(4096)
            except (OSError, serial.SerialException):
                # the board restarted (its USB went away): reopen, keep logging
                try:
                    s.close()
                except Exception:
                    pass
                s = open_port(port, url, wait=max(0.0, min(15.0, end - time.time())))
                if s is None:
                    emit(f'[flash_helper] {port} gone and not back')
                    break
                reopened += 1
                continue
            if chunk:
                buf += chunk
                *done, buf = buf.split(b'\n')
                for raw in done:
                    emit(raw.decode('utf-8', errors='replace').rstrip('\r'))
            if time.time() >= next_check:
                next_check = time.time() + 0.2
                if os.path.exists(p('stop.request')):
                    remove('stop.request')
                    stopped = True
                    break
                if os.path.exists(p('serial.send')):
                    try:
                        with open(p('serial.send'), encoding='utf-8') as f:
                            cmds = f.read().splitlines()
                        os.remove(p('serial.send'))
                    except OSError:
                        cmds = []                   # still being written: next check
                    for c in cmds:
                        if c.strip():
                            s.write((c.strip() + '\n').encode())
                            emit('> ' + c.strip())
                if keys.pressed_stop():
                    stopped = True
                    break
    finally:
        if buf:
            emit(buf.decode('utf-8', errors='replace').rstrip('\r'))
        try:
            s.close()
        except Exception:
            pass
        live.close()
        keys.close()
        write('serial_log.txt', '\n'.join(out) + '\n')
    if reopened:
        say(f'(the port went away and came back {reopened}x: the board restarted)', quiet=not echo)
    return out, stopped, None


# ---------------------------------------------------------------------------------------------------- one request

def run_request(kind, seconds, port_opt=None, echo=True):
    """kind 'flash' or 'reboot': flash (or only reset), then log. Writes flash.status / flash.done like the PowerShell
    helper. Returns the exit code."""
    start = time.time()
    started = time.strftime('%Y-%m-%dT%H:%M:%S')
    remove('flash.done')
    remove('stop.request')
    write('flash.running', started)
    say(f'=== {"Reboot request, no flashing" if kind == "reboot" else "Flash request"} (serial log: {seconds} s) ===')
    write('flash.status', 'flashing\n')
    port = find_port(port_opt)
    if kind == 'flash':
        app = p('firmware/weather_amoled.bin')
        if os.path.exists(app):
            say(f'Firmware: {os.path.getsize(app):,} bytes, built {time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(app)))}')
        args, err = flash_args(port)
        if err:
            write('flash_log.txt', err + '\n')      # never leave the previous run's log for this failure
        rc, lines, used = (1, [err], None) if err else esptool(args)
    else:
        rc, lines, used = esptool((['--port', port] if port else []) +
                                  ['--chip', 'esp32s3', '--before', 'default_reset', '--after', 'hard_reset', 'chip_id'])
    port = used or port
    flash_s = int(time.time() - start)
    if rc != 0:
        say(f'{kind.upper()} FAILED (exit {rc}) after {flash_s} s - last lines:')
        for l in lines[-6:]:
            print('    ' + l, flush=True)
        say('Tip: no port found? Check the cable carries data, run "flash_helper.py ports"; else hold BOOT, '
            'tap RESET, release BOOT, and request again.')
        write('flash.status', 'flash_failed\n')
        write('flash.done', f'exit={rc} stage=flash started={started} finished={time.strftime("%Y-%m-%dT%H:%M:%S")}\n')
        remove('flash.running')
        print('\a', end='', flush=True)
        return rc
    say(f'{kind.upper()} OK on {port} in {flash_s} s - board is restarting')
    write('flash.status', 'logging\n')
    say(f'Logging serial output for {seconds} s (saved to serial_log.txt). Press q or Esc to stop early ...')
    log, early, err = monitor(port, seconds, echo=echo)
    if early:
        say('Serial log stopped early')
    errs = sum(1 for l in log if l.startswith('E ('))
    warns = sum(1 for l in log if l.startswith('W ('))
    resets = sum(1 for l in log if 'rst:0x' in l)
    say(f'Serial log saved: {len(log)} lines, {errs} errors, {warns} warnings, {resets} resets' +
        (f' ({err})' if err else ''))
    total = int(time.time() - start)
    write('flash.done', f'exit={1 if err else 0} port={port} flash_s={flash_s} total_s={total} errors={errs} '
                        f'warnings={warns} resets={resets} stopped_early={int(early)} started={started} '
                        f'finished={time.strftime("%Y-%m-%dT%H:%M:%S")}\n')
    remove('flash.running')
    write('flash.status', 'idle\n')
    say(f'=== Done in {total} s ===')
    print('\a', end='', flush=True)
    return 1 if err else 0


def seconds_in(name, default=60):
    try:
        with open(p(name), encoding='utf-8', errors='replace') as f:
            m = re.fullmatch(r'\s*(\d{1,4})\s*', f.read())
        return int(m.group(1)) if m else default
    except OSError:
        return default


def helper(port_opt=None, poll=2.0, echo=True):
    """Wait for requests forever (Ctrl-C ends it)."""
    say(f'Flash helper running in {ROOT}')
    port = find_port(port_opt)
    say(f'Board port: {port}' if port else 'No ESP32-S3 port found yet (plug the board in; esptool will look again)')
    # Requests left from before this start: whoever wrote them gave up waiting (espforge LESSONS L157)
    for old in ('flash.request', 'reboot.request', 'stop.request', 'serial.send'):
        if os.path.exists(p(old)):
            remove(old)
            say(f'Ignored a {old} left from before this start')
    write('flash.status', 'idle\n')
    say('Waiting for flash.request / reboot.request ...')
    try:
        while True:
            req = next((r for r in ('flash.request', 'reboot.request') if os.path.exists(p(r))), None)
            if not req:
                time.sleep(poll)
                continue
            secs = seconds_in(req)
            remove(req)
            run_request('reboot' if req == 'reboot.request' else 'flash', secs, port_opt, echo)
            say('Waiting for the next flash.request ...')
    except KeyboardInterrupt:
        write('flash.status', 'stopped\n')
        say('Flash helper stopped')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', nargs='?', default='helper', choices=['helper', 'flash', 'reboot', 'monitor', 'stage', 'ports'])
    ap.add_argument('arg', nargs='?', help='seconds of log (flash, reboot, monitor) or the build folder (stage)')
    ap.add_argument('--port', help='serial port (default: $WEATHER_PORT, else the ESP32-S3 USB port)')
    o = ap.parse_args(argv)
    if o.action == 'stage':
        stage(o.arg or os.path.join(ROOT, 'build', 'v55'))
        return 0
    need_serial()
    if o.action == 'ports':
        list_ports()
        return 0
    if o.action == 'helper':
        helper(o.port)
        return 0
    secs = int(o.arg or 60)
    if o.action == 'monitor':
        port = find_port(o.port)
        if not port:
            sys.exit('no ESP32-S3 port found (flash_helper.py ports)')
        _, early, err = monitor(port, secs)
        if err:
            print(err)
            return 1
        return 2 if early else 0
    return run_request(o.action, secs, o.port)


if __name__ == '__main__':
    sys.exit(main())
