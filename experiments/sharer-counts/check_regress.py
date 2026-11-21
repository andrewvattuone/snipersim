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

Per-line check (when both runs have linewatch.csv, i.e. were run with SNIPER_WATCH_VADDR /
SNIPER_WATCH_LINES as run_regress.sh does). The counts above are whole-run totals and include the
run-to-run variation of all other activity. The line watch records, for the NLINES regression lines
only, every fill (with the state granted), downgrade, invalidation and eviction in every cache, in the
order the controllers performed them. For every line of both runs the event history is replayed over
the caches private to one core (L1-D, a non-shared L2, a private L3) and must satisfy:
  * single writer: a fill or refill in M or E happens only while no other core holds the line;
  * no reader beside a writer: a fill in S happens only while no other core holds it in M or E;
and in the write run, after core 1 obtains M (step 3), core 0's L1-D fills the line again (its
step-4 read missed: its copy had been removed) and core 1's copy is downgraded or removed.
These hold whatever the order in which the cores' requests reached the memory system and whatever
the replacement policy evicted; the number of lines with an eviction from a private cache is reported.
Both checks must pass; the result line says which one failed.
"""
import csv
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


def levels(run):
    section = None
    for name in ('simulation/sim.cfg', 'sim.cfg'):
        p = os.path.join(run, name)
        if os.path.exists(p):
            for line in open(p):
                line = line.strip()
                if line.startswith('['):
                    section = line
                elif section == '[perf_model/cache]' and line.startswith('levels'):
                    return int(line.split('=')[1].split()[0])
    return 3


def linewatch(run):
    """{line: [(seq, component, core, event, state)]} from linewatch.csv, ordered, or None"""
    for d in ('telemetry', '', 'simulation'):
        p = os.path.join(run, d, 'linewatch.csv')
        if os.path.exists(p):
            out = {}
            for r in csv.DictReader(l for l in open(p) if not l.startswith('#')):
                out.setdefault(int(r['line']), []).append((int(r['seq']), r['component'], int(r['core']), r['event'], r['state']))
            for events in out.values():
                events.sort()
            return out
    return None


def private_components(run):
    comps = {'l1d'}
    if not l2_shared(run):
        comps.add('l2')
    cfg = open(os.path.join(run, 'simulation', 'sim.cfg')).read() if os.path.exists(os.path.join(run, 'simulation', 'sim.cfg')) else ''
    m = re.search(r'\[perf_model/l3_cache\][^\[]*?shared_cores\s*=\s*(\d+)', cfg)
    if levels(run) > 2 and m and int(m.group(1)) == 1:
        comps.add('l3')
    return comps


def replay(events, comps, write_run):
    """MESI invariant over one line's events; returns (problems, evicted)"""
    held = {}  # (component, core) -> state
    problems, evicted = [], False
    core1_m_at = None
    for i, (seq, comp, core, ev, st) in enumerate(events):
        if comp not in comps:
            continue
        others = {k: v for k, v in held.items() if k[1] != core}
        if ev.startswith('fill'):
            if st in 'ME' and others:
                problems.append(f'#{seq} {comp} c{core} filled {st} while {others}')
            if st == 'S' and any(v in 'ME' for v in others.values()):
                problems.append(f'#{seq} {comp} c{core} filled S while {others}')
            held[(comp, core)] = st
            if write_run and core == 1 and comp == 'l1d' and st == 'M':
                core1_m_at = i
        elif ev == 'downgrade':
            if (comp, core) in held:
                held[(comp, core)] = st
        else:
            held.pop((comp, core), None)
            evicted |= ev == 'eviction'
    if write_run:
        if core1_m_at is None:
            problems.append('core 1 never obtained M in its L1-D')
        else:
            after = events[core1_m_at + 1:]
            if not any(c == 'l1d' and k == 0 and e.startswith('fill') for _, c, k, e, _ in after):
                problems.append('core 0 L1-D did not refetch the line after core 1 obtained M')
            if not any(c == 'l1d' and k == 1 and e != 'fill_demand' and e != 'fill_upgrade_refill' for _, c, k, e, _ in after):
                problems.append("core 1's M copy in its L1-D was neither downgraded nor removed")
    return problems, evicted


def perline(write_run, read_run, n):
    """None when the runs have no line watch; else (lines consistent, failures, lines with evictions)"""
    w, r = linewatch(write_run), linewatch(read_run)
    if w is None or r is None:
        return None
    comps = private_components(write_run)
    failures, evicted = [], 0
    for line in range(n):
        pw, ew = replay(w.get(line, []), comps, True)
        pr, er = replay(r.get(line, []), comps, False)
        evicted += ew or er
        if pw or pr:
            failures.append((line, ['write: ' + p for p in pw] + ['read: ' + p for p in pr]))
    return n - len(failures), failures, evicted


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
    data = True
    for run in (write_run, read_run):
        if not data_ok(run):
            data = False
            print(f'  data check failed or missing in {run}')
    pl = perline(write_run, read_run, n)
    if pl is None:
        print('  per-line check: not available (no linewatch.csv)')
        line_ok = True
    else:
        good, failures, evicted = pl
        line_ok = not failures
        print(f'  per-line check: {good} of {n} lines MESI-consistent in both runs ({evicted} lines had an eviction from a '
              f'private cache)' + ('' if line_ok else '; first failures: '
              + '; '.join(f'line {l}: ' + ', '.join(b) for l, b in failures[:3])))
    if ok and data and line_ok:
        verdict = 'MESI-CONSISTENT'
    elif data and line_ok and pl is not None:
        verdict = 'NOT MESI-CONSISTENT (aggregate totals below threshold; per-line check passed)'
    else:
        verdict = 'NOT MESI-CONSISTENT' + (' (per-line violation)' if not line_ok else '')
    print('  RESULT', verdict)
    return ok and data and line_ok


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(0 if main(sys.argv[1], sys.argv[2], int(sys.argv[3])) else 1)
