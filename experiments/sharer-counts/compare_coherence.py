#!/usr/bin/env python3
"""Before/after comparison of coherence-related results for pairs of Sniper runs.

Usage: compare_coherence.py BEFORE_RUN AFTER_RUN [BEFORE_RUN AFTER_RUN ...]
       (results directories; e.g. results/hint-l3-1024k-u-K4 results/fixed-l3-1024k-u-K4)

Prints, per pair, simulated time, aggregate IPC (sum of instructions / cycles),
L1-D/L2/L3 misses, L2 coherence events, L2 victim MESI states and the
diagnostic counters, all summed over cores, with the relative change. Sniper
runs are not deterministic: judge changes against two runs of the same build.
"""
import os
import re
import sys

ROWS = [
    ('sim time (ns)', lambda s: max(s['performance_model.elapsed_time']) / 1e6),
    ('IPC (aggregate)', lambda s: sum(s['performance_model.instruction_count']) / max(s['performance_model.cycle_count'])),
    ('instructions', lambda s: sum(s['performance_model.instruction_count'])),
    ('L1-D load misses', 'L1-D.load-misses'),
    ('L1-D store misses', 'L1-D.store-misses'),
    ('L2 load misses', 'L2.load-misses'),
    ('L2 store misses', 'L2.store-misses'),
    ('L3 load misses', 'L3.load-misses'),
    ('L1-D coherency invalidates', 'L1-D.coherency-invalidates'),
    ('L2 coherency invalidates', 'L2.coherency-invalidates'),
    ('L2 coherency writebacks', 'L2.coherency-writebacks'),
    ('L2 coherency upgrades', 'L2.coherency-upgrades'),
    ('L2 coherency downgrades', 'L2.coherency-downgrades'),
    ('L2 victims M', 'L2.lru-victim-M'),
    ('L2 victims E', 'L2.lru-victim-E'),
    ('L2 victims S', 'L2.lru-victim-S'),
    ('L2 read fills E/M with sharers', 'L2.sharer-hint-exclusive-with-sharers'),
    ('stale write hits L1-D (check)', 'L1-D.coherence-check-stale-write-hits'),
    ('stale write hits L2 (check)', 'L2.coherence-check-stale-write-hits'),
]


def load(run):
    path = os.path.join(run, 'simulation', 'sim.stats')
    if not os.path.exists(path):
        path = os.path.join(run, 'sim.stats')
    stats = {}
    for line in open(path):
        m = re.match(r'^(\S+)\s*=\s*(.*)$', line.strip())
        if m:
            try:
                stats[m.group(1)] = [float(x) for x in m.group(2).split(',')]
            except ValueError:
                pass
    return stats


def value(stats, row):
    getter = row[1]
    try:
        return getter(stats) if callable(getter) else sum(stats[getter])
    except (KeyError, ZeroDivisionError):
        return None


def fmt(v):
    if v is None:
        return 'n/a'
    return f'{v:.3f}' if abs(v) < 100 and v != int(v) else f'{int(v)}'


def main(args):
    if len(args) < 2 or len(args) % 2:
        sys.exit(__doc__)
    for before, after in zip(args[::2], args[1::2]):
        b, a = load(before), load(after)
        print(f'== {before} -> {after}')
        for row in ROWS:
            vb, va = value(b, row), value(a, row)
            if vb is None and va is None:
                continue
            change = f'{100.0 * (va - vb) / vb:+.1f}%' if vb and va is not None else ''
            print(f'  {row[0]:32s} {fmt(vb):>12s} -> {fmt(va):>12s} {change:>8s}')


if __name__ == '__main__':
    main(sys.argv[1:])
