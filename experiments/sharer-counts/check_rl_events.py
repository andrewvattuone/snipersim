#!/usr/bin/env python3
"""Check the cache events Sniper reports to replacement policies (RL integration, Stage 2).

Usage:
  check_rl_events.py RUN...                       invariants on each run
  check_rl_events.py --regress WRITE READ NLINES  also check coherence_regress classification

The runs must be made with SNIPER_RL_EVENT_CHECK=1, which registers per cache and core the rl-event-* stats:
fills by kind (demand, upgrade-refill), invalidations by reason (coherence, back-invalidation, upgrade,
unclassified), evictions, and four error counters. See docs/rl/POLICY_CORES.md.

Invariants (every component and core):
  rl-event-upgrade-refill-unmatched == 0   an upgrade refill re-inserts a line dropped with INV_UPGRADE
  rl-event-demand-fill-of-upgrading == 0   no demand fill of a line awaiting its upgrade refill
  rl-event-victim-upgrading == 0           no SHARED_UPGRADING victim
  rl-event-upgrade-overlap == 0            one pending upgrade per set (one hardware thread per private cache)
  rl-event-inv-unclassified == 0           every invalidation of a coherent cache is classified
  rl-event-fill-upgrade-refill == rl-event-inv-upgrade        every own upgrade is refilled
  rl-event-inv-back-invalidation == sum(backinval-*)          agrees with the controller's own counters
  coherency-invalidates + coherency-writebacks <= rl-event-inv-coherence
      <= coherency-invalidates + coherency-writebacks + coherency-upgrades
      (Transition::UPGRADE also counts in-place upgrades that are not invalidations)

--regress compares a coherence_regress write run with its read control (see check_regress.py): step 3 makes
core 1 upgrade NLINES lines its L1-D (and, unless the L2 is the last level or shared, its L2) holds in S, and
invalidates core 0's copies through coherence. Each delta must reach 90% of NLINES.
Exit status 1 on any failure.
"""
import os
import re
import sys

STATES = 'I S E M O u'.split()


def stats_path(run):
    for cand in ('simulation/sim.stats', 'sim.stats'):
        p = os.path.join(run, cand)
        if os.path.exists(p):
            return p
    return run


def load(run):
    stats = {}
    with open(stats_path(run)) as f:
        for line in f:
            m = re.match(r'^(\S+)\s*=\s*(.*)$', line.strip())
            if m:
                try:
                    stats[m.group(1)] = [int(float(x)) for x in m.group(2).split(',')]
                except ValueError:
                    pass
    return stats


def config(run):
    cfg, section = {}, None
    for name in ('simulation/sim.cfg', 'sim.cfg'):
        p = os.path.join(run, name)
        if os.path.exists(p):
            for line in open(p):
                line = line.strip()
                if line.startswith('['):
                    section = line[1:-1]
                elif '=' in line and section:
                    k, v = line.split('=', 1)
                    cfg[section + '/' + k.strip()] = v.split('#')[0].strip().strip('"')
            break
    return cfg


def check(run):
    s = load(run)
    comps = sorted({k.split('.')[0] for k in s if k.endswith('.rl-event-fill-demand')})
    problems = []
    if not comps:
        problems.append('no rl-event stats (run with SNIPER_RL_EVENT_CHECK=1)')
    totals = {}
    for c in comps:
        G = lambda n, core: s.get(f'{c}.{n}', [0] * (core + 1))[core]
        for core in range(len(s[f'{c}.rl-event-fill-demand'])):
            e = lambda n: G('rl-event-' + n, core)
            where = f'{c} core{core}'
            for n in ('upgrade-refill-unmatched', 'demand-fill-of-upgrading', 'victim-upgrading', 'upgrade-overlap', 'inv-unclassified'):
                if e(n):
                    problems.append(f'{where}: rl-event-{n} = {e(n)}')
            if e('fill-upgrade-refill') != e('inv-upgrade'):
                problems.append(f'{where}: upgrade refills {e("fill-upgrade-refill")} != upgrade invalidations {e("inv-upgrade")}')
            backinval = sum(G(f'backinval-{st}', core) for st in STATES)
            if e('inv-back-invalidation') != backinval:
                problems.append(f'{where}: back invalidations {e("inv-back-invalidation")} != backinval-* {backinval}')
            lo = G('coherency-invalidates', core) + G('coherency-writebacks', core)
            hi = lo + G('coherency-upgrades', core)
            if not lo <= e('inv-coherence') <= hi:
                problems.append(f'{where}: coherence invalidations {e("inv-coherence")} outside [{lo}, {hi}]')
            for n in ('fill-demand', 'fill-upgrade-refill', 'eviction', 'inv-coherence', 'inv-back-invalidation', 'inv-upgrade'):
                totals.setdefault(c, {}).setdefault(n, 0)
                totals[c][n] += e(n)
    print(f'== {stats_path(run)}')
    for c in comps:
        print(f'  {c:6s} ' + ' '.join(f'{n}={v}' for n, v in totals[c].items()))
    print('  RESULT', 'PASS' if not problems else 'FAIL: ' + '; '.join(problems))
    return not problems


def check_regress(write_run, read_run, n):
    w, r = load(write_run), load(read_run)
    cfg = config(write_run)
    levels = int(cfg.get('perf_model/cache/levels', '3'))
    l2_shared = int(cfg.get('perf_model/l2_cache/shared_cores', '1').split(',')[0]) > 1
    get = lambda s, k, core: s.get(k, [0] * (core + 1))[core]
    checks = [('core1 L1-D upgrade invalidations', 'L1-D', 'rl-event-inv-upgrade', 1),
              ('core1 L1-D upgrade refills', 'L1-D', 'rl-event-fill-upgrade-refill', 1),
              ('core0 L1-D coherence invalidations', 'L1-D', 'rl-event-inv-coherence', 0)]
    if not l2_shared:
        if levels > 2:  # an L2 above an L3 drops and refills its own copy; a last-level L2 upgrades in place
            checks += [('core1 L2 upgrade invalidations', 'L2', 'rl-event-inv-upgrade', 1),
                       ('core1 L2 upgrade refills', 'L2', 'rl-event-fill-upgrade-refill', 1)]
        checks += [('core0 L2 coherence invalidations', 'L2', 'rl-event-inv-coherence', 0)]
    ok = True
    print(f'== {write_run} vs {read_run} (N={n}, levels={levels}, shared L2={l2_shared})')
    for name, comp, stat, core in checks:
        delta = get(w, f'{comp}.{stat}', core) - get(r, f'{comp}.{stat}', core)
        good = delta >= 0.9 * n
        ok &= good
        print(f'  {name:38s} delta={delta:6d} {"OK" if good else "MISSING"}')
    print('  RESULT', 'CLASSIFIED' if ok else 'NOT CLASSIFIED AS EXPECTED')
    return ok


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == '--regress':
        if len(argv) != 4:
            print(__doc__)
            return 2
        ok = check(argv[1]) & check(argv[2]) & check_regress(argv[1], argv[2], int(argv[3]))
        return 0 if ok else 1
    return 0 if all([check(run) for run in argv]) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
