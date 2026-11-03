"""Print decompiled functions from re/decompiled by address or (sub)name.

usage: python show.py [-n MAXLINES] [-c] <addr-or-name> [...]
  -c   list callers (functions whose body references the target name)
"""
import argparse
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'decompiled')


def load_index():
    rows = []
    with open(os.path.join(ROOT, 'index.tsv'), encoding='utf-8') as f:
        next(f)
        for line in f:
            addr, size, ns, name, file, sig = line.rstrip('\n').split('\t')
            rows.append(dict(addr=addr.lower(), size=int(size), ns=ns, name=name, file=file, sig=sig))
    return rows


_cache = {}


def body(row):
    path = os.path.join(ROOT, row['file'])
    if path not in _cache:
        with open(path, encoding='utf-8') as f:
            text = f.read()
        parts = re.split(r'^// ==== ([0-9a-f]{8}) .*$', text, flags=re.M)
        _cache[path] = {parts[i]: parts[i + 1] for i in range(1, len(parts), 2)}
    return _cache[path].get(row['addr'], '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-n', type=int, default=0)
    ap.add_argument('-c', action='store_true')
    ap.add_argument('targets', nargs='+')
    a = ap.parse_args()
    rows = load_index()
    for t in a.targets:
        t = t.lower().removeprefix('0x')
        hits = [r for r in rows if r['addr'] == t.zfill(8)] or \
               [r for r in rows if t in (r['ns'] + '::' + r['name']).lower()]
        for r in hits[:20]:
            print(f"==== {r['addr']} {r['ns']}::{r['name']} ({r['size']} bytes) [{r['file']}]")
            if a.c:
                pat = re.compile(r'\b' + re.escape(r['name']) + r'\b')
                callers = [c for c in rows if c is not r and pat.search(body(c))]
                for c in callers:
                    print(f"    caller {c['addr']} {c['ns']}::{c['name']}")
                continue
            lines = body(r).strip('\n').split('\n')
            if a.n and len(lines) > a.n:
                lines = lines[:a.n] + [f'    ... ({len(lines) - a.n} more lines)']
            print('\n'.join(lines))
            print()


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
