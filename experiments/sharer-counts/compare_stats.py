#!/usr/bin/env python3
"""Diff two Sniper runs, ignoring the sharer-count diagnostics.

Usage: compare_stats.py RUN_A RUN_B   (results directories or sim.stats files)

The diagnostics (oracle-*, cachedlocs-*, hint-*, sharer-hint-*) are excluded, so
the remaining stats show whether the instrumentation changed simulated
behaviour. Sniper is not deterministic between runs with several threads, so
compare against the spread of two runs of the same binary, not against zero.
"""
import os
import re
import sys

KEYS = ['performance_model.elapsed_time', 'performance_model.instruction_count',
        'L3.load-misses', 'L2.load-misses', 'L1-D.lru-victim-M', 'L2.lru-victim-E']


def load(p):
    if os.path.isdir(p):
        for cand in ('simulation/sim.stats', 'sim.stats'):
            if os.path.exists(os.path.join(p, cand)):
                p = os.path.join(p, cand)
                break
    d = {}
    with open(p) as f:
        for line in f:
            m = re.match(r'^(\S+)\s*=\s*(.*)$', line.strip())
            if m and not re.search(r'\.(oracle-|cachedlocs-|hint-|sharer-hint-)', m.group(1)):
                d[m.group(1)] = m.group(2).replace(' ', '')
    return d


def main(a_path, b_path):
    a, b = load(a_path), load(b_path)
    keys = sorted(set(a) & set(b))
    diff = [k for k in keys if a[k] != b[k]]
    print(f'{len(keys)} common stats, {len(diff)} differ; only-in-A={len(set(a) - set(b))} only-in-B={len(set(b) - set(a))}')
    for k in KEYS:
        if k in a and k in b:
            print(f'  {k}: A={a[k]}  B={b[k]}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
