"""tools/flash_helper.py (the helper for macOS / Linux) against pyserial's loopback port: the file protocol the harness
relies on (serial_live.txt line by line, serial.send echoed as "> line", stop.request, serial_log.txt at the end).
No board. Skipped where pyserial is missing (CI's plain Python); ESP-IDF's Python has it.

    python -m unittest discover -s tools/harness -p "test_*.py"
"""
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
try:
    import serial  # noqa: F401
    import flash_helper as fh
except ImportError:
    fh = None


@unittest.skipIf(fh is None, 'pyserial missing (run from an ESP-IDF shell)')
class Monitor(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.old_root, fh.ROOT = fh.ROOT, self.root

    def tearDown(self):
        fh.ROOT = self.old_root
        shutil.rmtree(self.root, ignore_errors=True)

    def path(self, name):
        return os.path.join(self.root, name)

    def run_monitor(self, seconds, during):
        res = {}
        t = threading.Thread(target=lambda: res.update(r=fh.monitor('loop://', seconds, url=True, echo=False)))
        t.start()
        during()
        t.join(seconds + 5)
        return res['r']

    def live(self):
        with open(self.path('serial_live.txt'), encoding='utf-8') as f:
            return f.read().splitlines()

    def wait_for(self, line, timeout=3):
        end = time.time() + timeout
        while time.time() < end:
            if os.path.exists(self.path('serial_live.txt')) and line in self.live():
                return True
            time.sleep(0.05)
        return False

    def test_command_is_sent_and_logged(self):
        def during():
            time.sleep(0.3)
            with open(self.path('serial.send'), 'w') as f:
                f.write('ping\n\nscreen\n')
            self.assertTrue(self.wait_for('> ping'), 'the command was not echoed in serial_live.txt')
            self.assertTrue(self.wait_for('ping'), 'the loopback did not return the bytes sent')
        lines, early, err = self.run_monitor(1.5, during)
        self.assertIsNone(err)
        self.assertFalse(early)
        self.assertIn('> ping', lines)
        self.assertIn('> screen', lines)
        self.assertNotIn('> ', lines)                 # the empty line is skipped
        self.assertFalse(os.path.exists(self.path('serial.send')), 'serial.send was not consumed')
        with open(self.path('serial_log.txt'), encoding='utf-8') as f:
            self.assertEqual(f.read().splitlines(), lines)

    def test_stop_request_ends_the_window_early(self):
        def during():
            time.sleep(0.3)
            open(self.path('stop.request'), 'w').close()
        t0 = time.time()
        _, early, err = self.run_monitor(20, during)
        self.assertIsNone(err)
        self.assertTrue(early)
        self.assertLess(time.time() - t0, 5)
        self.assertFalse(os.path.exists(self.path('stop.request')))

    def test_stale_serial_send_is_dropped(self):
        with open(self.path('serial.send'), 'w') as f:
            f.write('reboot\n')
        lines, _, _ = self.run_monitor(0.5, lambda: None)
        self.assertNotIn('> reboot', lines)


@unittest.skipIf(fh is None, 'pyserial missing (run from an ESP-IDF shell)')
class Reopen(unittest.TestCase):
    """After a restart the board's USB may come back under another name (macOS numbers usbmodem ports by location)."""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.old = fh.ROOT, fh.board_ports, serial.Serial
        fh.ROOT = self.root

        class FakeSerial:
            def open(s):
                if s.port != '/dev/cu.usbmodem1201':
                    raise serial.SerialException('no such port')
        serial.Serial = FakeSerial

    def tearDown(self):
        fh.ROOT, fh.board_ports, serial.Serial = self.old
        shutil.rmtree(self.root, ignore_errors=True)

    def test_board_found_again_under_a_new_name(self):
        fh.board_ports = lambda: [('/dev/cu.usbmodem1201', 'USB JTAG/serial debug unit')]
        s = fh.open_port('/dev/cu.usbmodem1101', wait=3)
        self.assertIsNotNone(s, 'the board was not found under its new name')
        self.assertEqual(s.port, '/dev/cu.usbmodem1201')
        self.assertFalse(s.dtr or s.rts, 'DTR / RTS must be low (they reset the chip)')

    def test_gives_up_when_the_board_stays_away(self):
        fh.board_ports = lambda: []
        t0 = time.time()
        self.assertIsNone(fh.open_port('/dev/cu.usbmodem1101', wait=1))
        self.assertLess(time.time() - t0, 3)


class FakeBoard:
    """A port whose board answers the console's `reboot` with a boot log (answers=True), or ignores it."""
    BOOT = (b'I (227945) test: ok restarting\r\nESP-ROM:esp32s3-20210327\r\n'
            b'rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)\r\n'
            b'I (39) boot.esp32s3: SPI Mode       : QIO\r\nI (2969) test: console ready on USB\r\n')

    def __init__(self, answers):
        self.answers, self.pending, self.written = answers, b'', b''

    def write(self, data):
        self.written += data
        if self.answers and data == b'reboot\n':
            self.pending += self.BOOT

    def read(self, n):
        out, self.pending = self.pending[:n], self.pending[n:]
        if not out:
            time.sleep(0.05)
        return out

    def close(self):
        pass


@unittest.skipIf(fh is None, 'pyserial missing (run from an ESP-IDF shell)')
class ConsoleReboot(unittest.TestCase):
    """reboot.request restarts through the test console, so the boot's first lines are kept (the flash mode, the reset
    reason: the harness printed "flash ?" after esptool's reset), and falls back to esptool without a console."""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.old = fh.ROOT, fh.open_port, fh.esptool
        fh.ROOT = self.root
        self.esptool_calls = []

        def fake_esptool(args):
            self.esptool_calls.append(args)
            return 1, ['A fatal error occurred: no board (test)'], None
        fh.esptool = fake_esptool

    def tearDown(self):
        fh.ROOT, fh.open_port, fh.esptool = self.old
        shutil.rmtree(self.root, ignore_errors=True)

    def read(self, name):
        with open(os.path.join(self.root, name), encoding='utf-8') as f:
            return f.read()

    def test_boot_log_kept_from_its_first_line(self):
        board = FakeBoard(answers=True)
        fh.open_port = lambda *a, **k: board
        rc = fh.run_request('reboot', 1, port_opt='COM-test', echo=False)
        self.assertEqual(rc, 0)
        self.assertEqual(self.esptool_calls, [], 'esptool reset the board although the console did')
        self.assertEqual(board.written, b'reboot\n')
        log = self.read('serial_log.txt')
        for line in ('ESP-ROM:esp32s3', 'rst:0xc', 'SPI Mode       : QIO', 'console ready'):
            self.assertIn(line, log)
        self.assertIn('SPI Mode       : QIO', self.read('serial_live.txt'))
        self.assertIn(' resets=0 ', self.read('flash.done'), 'the restart asked for counted as an unexpected reset')

    def test_falls_back_to_esptool_without_a_console(self):
        fh.open_port = lambda *a, **k: FakeBoard(answers=False)
        t0 = time.time()
        rc = fh.run_request('reboot', 1, port_opt='COM-test', echo=False)
        self.assertEqual(len(self.esptool_calls), 1, 'no esptool reset after the console did not restart the board')
        self.assertIn('chip_id', self.esptool_calls[0])
        self.assertNotEqual(rc, 0)                     # (the fake esptool fails)
        self.assertLess(time.time() - t0, 8)


@unittest.skipIf(fh is None, 'pyserial missing (run from an ESP-IDF shell)')
class Requests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.old_root, fh.ROOT = fh.ROOT, self.root

    def tearDown(self):
        fh.ROOT = self.old_root
        shutil.rmtree(self.root, ignore_errors=True)

    def test_seconds_in_request(self):
        for text, want in (('300', 300), (' 45\n', 45), ('', 60), ('abc', 60), ('99999', 60)):
            with open(os.path.join(self.root, 'flash.request'), 'w') as f:
                f.write(text)
            self.assertEqual(fh.seconds_in('flash.request'), want, repr(text))

    def test_flash_without_staged_firmware_fails_clearly(self):
        rc = fh.run_request('flash', 1, port_opt='/dev/null-no-board', echo=False)
        self.assertNotEqual(rc, 0)
        with open(os.path.join(self.root, 'flash.status')) as f:
            self.assertEqual(f.read().strip(), 'flash_failed')
        with open(os.path.join(self.root, 'flash.done')) as f:
            self.assertTrue(f.read().startswith('exit=1 stage=flash'))
        with open(os.path.join(self.root, 'flash_log.txt')) as f:
            self.assertIn('stage a build first', f.read())

    def test_stage_copies_and_checks(self):
        b = os.path.join(self.root, 'b')
        for _, _, src in fh.IMAGES:
            os.makedirs(os.path.dirname(os.path.join(b, src)) or b, exist_ok=True)
            with open(os.path.join(b, src), 'wb') as f:
                f.write(os.urandom(64))
        fh.stage(b)
        for _, dst, src in fh.IMAGES:
            self.assertEqual(fh.md5(os.path.join(b, src)), fh.md5(os.path.join(self.root, 'firmware', dst)))


if __name__ == '__main__':
    unittest.main()
