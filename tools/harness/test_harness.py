"""Unit tests of the harness's own logic (no board):   python -m unittest discover -s tools/harness -p "test_*.py" """
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import BAD, compare, propose  # noqa: E402
from board import kv  # noqa: E402

BASE = {
    'render_ms.weather': {'max': 50, 'ref': 45, 'note': 'kept'},
    'internal_min_kb': {'min': 8, 'ref': 9},
    'snapshot_ms.weather': {'max': 5000, 'ref': 2300},
    'drag_fps.drag_place': {'min': 45, 'ref': 68},
}


def verdicts(rows):
    return {k: v for k, _, _, v in rows}


class Compare(unittest.TestCase):
    def test_within_and_outside_limits(self):
        v = verdicts(compare({'render_ms.weather': 49, 'internal_min_kb': 7, 'drag_fps.drag_place': 60},
                             BASE, ['perf']))
        self.assertEqual(v['render_ms.weather'], 'ok')
        self.assertEqual(v['internal_min_kb'], 'REGRESSION')

    def test_missing_metric_fails(self):
        # in the baseline, its suite ran, not measured: a regex drift or a reused log window, not a pass
        v = verdicts(compare({'render_ms.weather': 40}, BASE, ['perf']))
        self.assertEqual(v['internal_min_kb'], 'MISSING')
        self.assertIn('MISSING', BAD)

    def test_missing_from_a_suite_that_did_not_run(self):
        v = verdicts(compare({'render_ms.weather': 40, 'internal_min_kb': 9, 'drag_fps.drag_place': 60}, BASE, ['perf']))
        self.assertNotIn('snapshot_ms.weather', v)            # navigation didn't run
        v = verdicts(compare({}, BASE, ['navigation']))
        self.assertEqual(v, {'snapshot_ms.weather': 'MISSING'})

    def test_metric_without_a_limit_fails(self):
        v = verdicts(compare({'render_ms.weather': 40, 'internal_min_kb': 9, 'drag_fps.drag_place': 60,
                              'swipe_gap_max_ms.new': 20}, BASE, ['perf']))
        self.assertEqual(v['swipe_gap_max_ms.new'], 'NEW')
        self.assertIn('NEW', BAD)

    def test_explicit_skip(self):
        v = verdicts(compare({'render_ms.weather': 40, 'internal_min_kb': 9}, BASE, ['perf'],
                             {'*.drag_place*': 'one place'}))
        self.assertEqual(v['drag_fps.drag_place'], 'skipped: one place')
        self.assertNotIn(v['drag_fps.drag_place'], BAD)

    def test_propose_keeps_limits_and_notes(self):
        out = propose({'render_ms.weather': 47, 'page_kb': 53, 'drag_fps.x': 60}, BASE)
        self.assertEqual(out['render_ms.weather'], {'max': 50, 'ref': 47, 'note': 'kept'})
        self.assertIn('max', out['page_kb'])                  # bigger is worse, though it ends in _kb
        self.assertIn('min', out['drag_fps.x'])
        self.assertEqual(BASE['render_ms.weather']['ref'], 45)  # the input is not changed



class ConsoleReplies(unittest.TestCase):
    def test_kv_skips_words_without_a_value(self):
        # fps grew "gap_max_kind=move>lvgl" (v1.12.3); a word without '=' crashed dict() before
        self.assertEqual(kv('frames=3 gap_max_kind=move>lvgl note'), {'frames': '3', 'gap_max_kind': 'move>lvgl'})

    def test_kv_keeps_values_with_equals(self):
        self.assertEqual(kv('ap_pass=a=b'), {'ap_pass': 'a=b'})

if __name__ == '__main__':
    unittest.main()
