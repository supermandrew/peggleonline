"""Repeat the online duel start several times and report any mismatch between the two copies.
Usage: python stress.py <runs> [shots]"""
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import duo  # noqa: E402

LOGS = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'mods', 'logs')


def log_lines(pid):
    with open(os.path.join(LOGS, f'netplay-{pid}.log')) as f:
        return f.read().splitlines()


def main(runs, shots):
    for run in range(1, runs + 1):
        duo.start()
        duo.connect()
        time.sleep(3)
        duo.duel()
        duo.autoplay(shots, seed=run)
        time.sleep(6)
        st = duo.load()
        report = []
        for k in 'AB':
            lines = log_lines(st[k]['pid'])
            bad = [l for l in lines if 'DESYNC' in l or 'EXCEPTION' in l or 'mismatch' in l]
            sel = [l for l in lines if 'selection 0->1' in l]
            report.append(f"{k}: {'%d problem lines, first: %s' % (len(bad), bad[0][13:]) if bad else 'clean'}"
                          f" | {sel[0][13:] if sel else 'no selection dialog'}")
        print(f"run {run}: " + ' || '.join(report), flush=True)


if __name__ == '__main__':
    main(int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 2)
