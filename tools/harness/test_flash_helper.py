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
