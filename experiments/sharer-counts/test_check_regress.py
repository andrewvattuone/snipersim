#!/usr/bin/env python3
"""Self-test of check_regress.py's per-line invariant (no simulation): synthetic line-watch histories of one
coherence_regress line, correct and with injected coherence violations. Run: python3 test_check_regress.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_regress import replay  # noqa: E402

PRIVATE = {'l1d', 'l2'}


def ev(*items):
    """(component, core, event, state) tuples -> line-watch events with sequence numbers"""
    return [(i, c, k, e, s) for i, (c, k, e, s) in enumerate(items)]


# Clean write run as Sniper performs it (L1-D and private L2 of two cores, shared L3 not private)
GOOD_WRITE = ev(
    ('l2', 0, 'fill_demand', 'E'), ('l1d', 0, 'fill_demand', 'E'),          # step 1: core 0 reads, E
    ('l1d', 0, 'downgrade', 'S'), ('l2', 0, 'downgrade', 'S'),               # step 2: core 1 reads
    ('l2', 1, 'fill_demand', 'S'), ('l1d', 1, 'fill_demand', 'S'),
    ('l1d', 1, 'inv_upgrade', 'S'), ('l2', 1, 'inv_upgrade', 'S'),           # step 3: core 1 writes
    ('l1d', 0, 'inv_coherence', 'S'), ('l2', 0, 'inv_coherence', 'S'),
    ('l2', 1, 'fill_upgrade_refill', 'M'), ('l1d', 1, 'fill_upgrade_refill', 'M'),
    ('l1d', 1, 'downgrade', 'S'), ('l2', 1, 'downgrade', 'S'),               # step 4: core 0 reads again
    ('l2', 0, 'fill_demand', 'S'), ('l1d', 0, 'fill_demand', 'S'))
# Read control
GOOD_READ = ev(
    ('l2', 0, 'fill_demand', 'E'), ('l1d', 0, 'fill_demand', 'E'),
    ('l1d', 0, 'downgrade', 'S'), ('l2', 0, 'downgrade', 'S'),
    ('l2', 1, 'fill_demand', 'S'), ('l1d', 1, 'fill_demand', 'S'))
# Core 1's step-2 read reaches the memory system before core 0's step-1 read (legal; seen in Sniper)
RACE_READ = ev(
    ('l2', 1, 'fill_demand', 'E'), ('l1d', 1, 'fill_demand', 'E'),
    ('l1d', 1, 'downgrade', 'S'), ('l2', 1, 'downgrade', 'S'),
    ('l2', 0, 'fill_demand', 'S'), ('l1d', 0, 'fill_demand', 'S'))
# Core 0's L1-D evicted the line before core 1's write: no coherence invalidation is needed there (legal)
EVICTED_WRITE = ev(
    ('l2', 0, 'fill_demand', 'E'), ('l1d', 0, 'fill_demand', 'E'),
    ('l1d', 0, 'downgrade', 'S'), ('l2', 0, 'downgrade', 'S'),
    ('l2', 1, 'fill_demand', 'S'), ('l1d', 1, 'fill_demand', 'S'),
    ('l1d', 0, 'eviction', 'S'),
    ('l1d', 1, 'inv_upgrade', 'S'), ('l2', 1, 'inv_upgrade', 'S'),
    ('l2', 0, 'inv_coherence', 'S'),
    ('l2', 1, 'fill_upgrade_refill', 'M'), ('l1d', 1, 'fill_upgrade_refill', 'M'),
    ('l1d', 1, 'downgrade', 'S'), ('l2', 1, 'downgrade', 'S'),
    ('l2', 0, 'fill_demand', 'S'), ('l1d', 0, 'fill_demand', 'S'))


def without(events, drop):
    """events minus those matching (component, core, event) of `drop` (renumbered)"""
    return ev(*[(c, k, e, s) for _, c, k, e, s in events if (c, k, e) not in drop])


CASES = [
    ('correct write run', GOOD_WRITE, True, True),
    ('correct read control', GOOD_READ, False, True),
    ('reordered first reads', RACE_READ, False, True),
    ('line evicted before the write', EVICTED_WRITE, True, True),
    # Injected violations
    ("core 0's L1-D copy not invalidated by the write", without(GOOD_WRITE, {('l1d', 0, 'inv_coherence')}), True, False),
    ("core 0's L2 copy not invalidated by the write", without(GOOD_WRITE, {('l2', 0, 'inv_coherence')}), True, False),
    ('step-4 read hits a stale copy (no refetch)', ev(*[(c, k, e, s) for _, c, k, e, s in GOOD_WRITE][:12]
                                                      + [('l1d', 1, 'downgrade', 'S'), ('l2', 1, 'downgrade', 'S')]), True, False),
    ("core 0 fills S beside core 1's M (no downgrade)", without(GOOD_WRITE, {('l1d', 1, 'downgrade'), ('l2', 1, 'downgrade')}), True, False),
    ('E granted beside another copy', ev(('l1d', 0, 'fill_demand', 'S'), ('l1d', 1, 'fill_demand', 'E')), False, False),
    ('read control checked as a write run', GOOD_READ, True, False),
]

failed = 0
for name, events, write_run, expect_ok in CASES:
    problems, _ = replay(events, PRIVATE, write_run)
    ok = not problems
    good = ok == expect_ok
    failed += not good
    print(f'{"PASS" if good else "FAIL"}  {name}: {"consistent" if ok else problems[0]}')
print(f'{len(CASES) - failed} of {len(CASES)} cases as expected')
sys.exit(1 if failed else 0)
