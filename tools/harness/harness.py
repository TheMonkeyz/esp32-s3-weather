#!/usr/bin/env python3
"""Autonomous test harness for the weather display (see docs/TESTING.md, "Harness").

    python tools/harness/harness.py                      # all suites on the firmware that is on the board
    python tools/harness/harness.py smoke navigation     # some suites
    python tools/harness/harness.py --flash build/v55/weather_amoled.bin   # flash first
    python tools/harness/harness.py wifi_setup --phone   # also the Easy Connect phone step (asks you)
    python tools/harness/harness.py perf --update-baseline   # accept the measured numbers as the new reference

Needs the flash helper (start_flash_helper.bat) and firmware with the test console (main/testcon.c). The
wifi_setup suite joins the PC's Wi-Fi card to the display's setup network (Ethernet keeps the PC online).
Exit code 0 = all passed. Report: tools/harness/reports/<date-time>/report.md (+ screenshots, results.json).
"""
import argparse
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board import Board, Fail, Log, PCWifi, ROOT  # noqa: E402
from suites import SUITES  # noqa: E402

ORDER = ['smoke', 'navigation', 'web', 'perf', 'wifi_runtime', 'wifi_setup']
BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'baseline.json')


class Ctx:
    def __init__(self, board, log, wifi, outdir, opts):
        self.board, self.log, self.wifi, self.dir, self.opts = board, log, wifi, outdir, opts
        self.metrics, self.notes, self.snapped = {}, [], set()

    def out(self, name):
        return os.path.join(self.dir, name)

    def metric(self, name, value):
        self.metrics[name] = value

    def note(self, text):
        self.notes.append(text)
        print('    ' + text, flush=True)

    def ask(self, text):
        print('\n>>> ASK THE USER: ' + text + '\n', flush=True)
        open(os.path.join(ROOT, 'harness.ask'), 'w', encoding='utf-8').write(text)


def compare(metrics, update):
    """[(metric, value, limit, ok)] against baseline.json {"metric": {"max"|"min": n, "ref": n}}."""
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    rows = []
    for k, v in sorted(metrics.items()):
        b = base.get(k)
        if update and isinstance(v, (int, float)):
            lo_is_bad = any(s in k for s in ('_kb', 'fps'))
            base[k] = {'ref': v, ('min' if lo_is_bad else 'max'): round(v * (0.75 if lo_is_bad else 1.4), 1)}
            b = base[k]
        if not b:
            rows.append((k, v, '', None))
        elif 'max' in b:
            rows.append((k, v, f'≤ {b["max"]}', v <= b['max']))
        else:
            rows.append((k, v, f'≥ {b["min"]}', v >= b['min']))
    if update:
        json.dump(base, open(BASELINE, 'w'), indent=1, sort_keys=True)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('suites', nargs='*', help='any of: ' + ', '.join(ORDER) + ' (default: all)')
    ap.add_argument('--ip', default='192.168.1.156', help="the display's address on the home network")
    ap.add_argument('--flash', help='firmware .bin to flash first')
    ap.add_argument('--phone', action='store_true', help='wifi_setup: ask for a phone to test Easy Connect fully')
    ap.add_argument('--update-baseline', action='store_true', help='store the measured numbers as the reference')
    ap.add_argument('--minutes', type=int, default=40, help='log window to request from the flash helper')
    opts = ap.parse_args()
    suites = opts.suites or ORDER
    for s in suites:
        if s not in SUITES:
            ap.error(f'unknown suite {s}')

    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'reports', time.strftime('%Y-%m-%d_%H%M%S'))
    os.makedirs(outdir, exist_ok=True)
    log = Log()
    board = Board(opts.ip, log)
    ctx = Ctx(board, log, PCWifi(), outdir, opts)
    results = []
    started = time.time()

    reuse = board.helper_status() == 'logging' and not opts.flash
    print(('Using the running log window' if reuse else 'Restarting the board and logging') + ' ...', flush=True)
    try:
        if not reuse:
            board.start_log(opts.minutes * 60, opts.flash and os.path.join(ROOT, opts.flash))
            log.wait(r'diag: mark first weather', 90, 'start-up')   # Wi-Fi up and the first forecast shown
            time.sleep(5)
        board.cmd('ping', r'test: pong', timeout=10)
    except Fail as e:
        print('Cannot start:', e)
        return 2

    for s in [x for x in ORDER if x in suites]:
        for fn in SUITES[s]:
            name = f'{s}.{fn.__name__}'
            print(f'- {name}', flush=True)
            log.mark()
            ctx.reset_ok = False                        # tests that restart the board on purpose set it
            t0 = time.time()
            try:
                fn(ctx)
                status, detail = 'pass', ''
                resets = [l for l in log.since_mark() if 'rst:0x' in l]
                if resets and not ctx.reset_ok:          # a crash (its panic text goes to the UART, not USB)
                    status, detail = 'FAIL', f'unexpected restart: {resets[0][:60]}'
            except Fail as e:
                status, detail = 'FAIL', str(e)
            except Exception as e:                      # a bug in the harness itself, or the board vanished
                status, detail = 'ERROR', f'{type(e).__name__}: {e}'
                traceback.print_exc()
            dt = time.time() - t0
            if status != 'pass':
                with open(os.path.join(outdir, f'{name}.log.txt'), 'w', encoding='utf-8') as f:
                    f.write('\n'.join(log.since_mark()))
            print(f'  {status} ({dt:.0f} s) {detail}', flush=True)
            results.append({'test': name, 'status': status, 'seconds': round(dt), 'detail': detail})

    board.stop_log()
    rows = compare(ctx.metrics, opts.update_baseline)
    perf_bad = [r for r in rows if r[3] is False]
    failed = [r for r in results if r['status'] != 'pass']

    # report
    lines = [f'# Harness report {time.strftime("%Y-%m-%d %H:%M")}', '',
             f'{len(results) - len(failed)}/{len(results)} tests passed, {len(perf_bad)} performance regressions, '
             f'{time.time() - started:.0f} s.', '', '| test | result | time | detail |', '|---|---|---|---|']
    lines += [f'| {r["test"]} | {r["status"]} | {r["seconds"]} s | {r["detail"]} |' for r in results]
    if ctx.notes:
        lines += ['', '## Notes', ''] + [f'- {n}' for n in ctx.notes]
    if rows:
        lines += ['', '## Performance', '', '| metric | value | limit | |', '|---|---|---|---|']
        lines += [f'| {k} | {v} | {lim} | {"" if ok is None else "ok" if ok else "**REGRESSION**"} |'
                  for k, v, lim, ok in rows]
    shots = sorted(f for f in os.listdir(outdir) if f.endswith('.png'))
    if shots:
        lines += ['', '## Screens', ''] + [f'![{s}]({s})' for s in shots]
    open(os.path.join(outdir, 'report.md'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    json.dump({'results': results, 'metrics': ctx.metrics, 'notes': ctx.notes},
              open(os.path.join(outdir, 'results.json'), 'w'), indent=1)
    print(f'\n{len(results) - len(failed)}/{len(results)} passed, {len(perf_bad)} performance regressions')
    print('Report:', os.path.relpath(os.path.join(outdir, 'report.md'), ROOT))
    return 1 if failed or perf_bad else 0


if __name__ == '__main__':
    sys.exit(main())
