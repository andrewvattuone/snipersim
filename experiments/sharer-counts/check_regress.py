#!/usr/bin/env python3
"""Compare a coherence_regress "write" run with its "read" control and check MESI expectations.

Usage: check_regress.py WRITE_RUN READ_RUN NLINES   (results directories)

Both runs execute the same steps except step 3 (thread 1 writes vs. reads the
NLINES lines), so the difference of core 0's and core 1's counters isolates
what the write did. Under MESI the write must remove core 0's copies (L1-D and
L2: coherency invalidates + upgrades + writebacks), core 0's step-4 reads must
then miss (L1-D and L2 load misses), and those misses must downgrade core 1's
copies (L2 coherency downgrades). When the L2 is shared (its sim.cfg has
shared_cores > 1), the per-core L2 rows do not apply and are skipped; the
L1-D rows then check the private level. Each delta must reach 90% of NLINES; the
other activity in the two runs is the same up to Sniper's run-to-run noise.
Also requires "data check: OK" from the program. Exit status 1 on a failure.
"""
import os
import re
import sys


def load(run):
    path = os.path.join(run, 'simulation', 'sim.stats')
    if not os.path.exists(path):
        path = os.path.join(run, 'sim.stats')
    stats = {}
    for line in open(path):
        m = re.match(r'^(\S+)\s*=\s*(.*)$', line.strip())
        if m:
            try:
                stats[m.group(1)] = [int(float(x)) for x in m.group(2).split(',')]
            except ValueError:
                pass
    return stats


def l2_shared(run):
    section = None
    for name in ('simulation/sim.cfg', 'sim.cfg'):
        p = os.path.join(run, name)
        if os.path.exists(p):
            for line in open(p):
                line = line.strip()
                if line.startswith('['):
                    section = line
                elif section == '[perf_model/l2_cache]' and line.startswith('shared_cores'):
                    return int(line.split('=')[1].split()[0]) > 1
    return False


def data_ok(run):
    for name in ('simulation/sim.stdout', 'sim.stdout'):
        p = os.path.join(run, name)
        if os.path.exists(p):
            return 'data check: OK' in open(p).read()
    return False


def main(write_run, read_run, n):
    w, r = load(write_run), load(read_run)
    get = lambda s, k, core: s.get(k, [0] * (core + 1))[core]
    removed = lambda s, comp, core: sum(get(s, f'{comp}.coherency-{k}', core) for k in ('invalidates', 'upgrades', 'writebacks'))
    checks = [
        ('core0 L1-D copies removed by coherence', lambda s: removed(s, 'L1-D', 0)),
        ('core0 L2 copies removed by coherence', lambda s: removed(s, 'L2', 0)),
        ('core0 L1-D load misses', lambda s: get(s, 'L1-D.load-misses', 0)),
        ('core0 L2 load misses', lambda s: get(s, 'L2.load-misses', 0)),
        ('core1 L2 coherency downgrades', lambda s: get(s, 'L2.coherency-downgrades', 1)),
    ]
    ok = True
    print(f'== {write_run} vs {read_run} (N={n})')
    if l2_shared(write_run):
        checks = [c for c in checks if 'L2' not in c[0]]
        print('  (shared L2: per-core L2 rows skipped)')
    for name, f in checks:
        delta = f(w) - f(r)
        good = delta >= 0.9 * n
        ok &= good
        print(f'  {name:42s} write={f(w):8d} read={f(r):8d} delta={delta:6d} {"OK" if good else "MISSING (MESI expects ~N)"}')
    for run in (write_run, read_run):
        if not data_ok(run):
            ok = False
            print(f'  data check failed or missing in {run}')
    print('  RESULT', 'MESI-CONSISTENT' if ok else 'NOT MESI-CONSISTENT')
    return ok


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(0 if main(sys.argv[1], sys.argv[2], int(sys.argv[3])) else 1)
