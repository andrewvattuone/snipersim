#!/usr/bin/env python3
"""Check runs that use the RL replacement policies (sf_lru, rl_lru) and summarize their Stage 4 diagnostics.

Usage:
  check_rl_policy.py RUN...             checks on each run (exit status 1 on any failure)
  check_rl_policy.py --summary RUN...   also print the diagnostics summary (audit I-1, I-2, I-3, I-4, I-6)

Checks, every cache and core with an RL policy (docs/rl/POLICY_CORES.md):
  * the adapter's unexpected-event counters are 0: no-eligible-victim, eviction-without-decision, inv-unclassified,
    candidate-state-unknown, history-refill-removals, and for rl_lru upgrade-refills-unmatched and
    upgrade-stash-overwritten;
  * history-inserts == rl-event-eviction when the policy learns and the run has SNIPER_RL_EVENT_CHECK=1 stats
    (every replacement eviction is recorded exactly once; invalidations never are);
  * history accounting: |history-inserts - aged out - returned| <= history capacity (2 x the cache's lines);
  * rl_lru: upgrade-carryovers == inv-upgrade (== rl-event-fill-upgrade-refill): every own upgrade carries its
    metadata over, and an upgrade refill is never a miss (no false -1);
  * with rl_diag/enabled = true, the diagnostics close against the cores' own counters: rewards (rl) or history
    hits / aged-out entries (sf) by feature class sum to the core totals, decisions agree, the victim positions sum
    to the decisions, the miss classes sum to the misses and equal rl-event-fill-demand, history hits equal the
    misses classified as "replaced, in history", M victims equal lru-victim-M, and miss-inconsistent and
    double-dropped are 0;
  * with diagnostics, the features recorded into the history match the evicted blocks, counted independently by
    the LRU victim statistics: evicted-S/E/M == lru-victim-S/E/M, evicted-AT == the victims whose DELIVERED hint
    is heavy (hint-victim-heavy-both + -delivered-only; the oracle-heavy count differs whenever the hint is
    stale), and eviction-features-changed == 0.
Only counters that change together are compared, so the checks also hold for region-of-interest deltas.

The summary also reads the weight snapshots (rl-snapshots-<cache>-<core>.csv; run-sniper moves them to telemetry/).
"""
import csv
import os
import re
import sys

CLASSES = ['S', 'E', 'M', 'BT', 'AT']
RUN_ENDS = ['hit', 'evicted', 'invalidated']
BUCKETS = ['1', '2', '3-4', '5-8', '9-16', '17-32', '33+']
REWARDS = ['evict-bad', 'evict-good', 'protect-good', 'protect-wasted']


def sim_dir(run):
    return os.path.join(run, 'simulation') if os.path.isdir(os.path.join(run, 'simulation')) else run


def load(run):
    stats = {}
    with open(os.path.join(sim_dir(run), 'sim.stats')) as f:
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
    p = os.path.join(sim_dir(run), 'sim.cfg')
    if os.path.exists(p):
        for line in open(p):
            line = line.strip()
            if line.startswith('['):
                section = line[1:-1]
            elif '=' in line and section:
                k, v = line.split('=', 1)
                cfg[section + '/' + k.strip()] = v.split('#')[0].strip().strip('"')
    return cfg


SECTIONS = {'L1-D': 'l1_dcache', 'L1-I': 'l1_icache', 'L2': 'l2_cache', 'L3': 'l3_cache'}


def history_capacity(cfg, comp):
    """2 x (lines of the cache + one set of slack for the L2 metadata set), or None if unknown"""
    sec = 'perf_model/' + SECTIONS.get(comp, '')
    try:
        size = int(cfg[sec + '/cache_size'].split(',')[0])
        assoc = int(cfg[sec + '/associativity'].split(',')[0])
        block = int(cfg.get(sec + '/cache_block_size', cfg.get('perf_model/l1_dcache/cache_block_size', '64')).split(',')[0])
    except (KeyError, ValueError):
        return None
    return 2 * (size * 1024 // block + assoc)


def policies(s):
    """(component, prefix) for every cache with an RL policy"""
    out = []
    for k in s:
        comp, _, name = k.partition('.')
        if name in ('rl-decisions', 'sf-decisions'):
            out.append((comp, name[:2]))
    return sorted(out)


def check(run, s):
    problems = []
    cfg = config(run)
    found = policies(s)
    if not found:
        problems.append('no sf_lru / rl_lru statistics')
    for comp, p in found:
        cores = len(s[f'{comp}.{p}-decisions'])
        for core in range(cores):
            g = lambda n: s.get(f'{comp}.{n}', [0] * (core + 1))[core]
            has = lambda n: f'{comp}.{n}' in s
            where = f'{comp} core{core}'
            zero = ['no-eligible-victim', 'eviction-without-decision', 'inv-unclassified', 'candidate-state-unknown', 'history-refill-removals']
            if p == 'rl':
                zero += ['upgrade-refills-unmatched', 'upgrade-stash-overwritten']
            for n in zero:
                if g(f'{p}-{n}'):
                    problems.append(f'{where}: {p}-{n} = {g(p + "-" + n)}')
            learns = g(f'{p}-history-inserts') > 0
            if has('rl-event-eviction') and learns and g(f'{p}-history-inserts') != g('rl-event-eviction'):
                problems.append(f'{where}: history-inserts {g(p + "-history-inserts")} != evictions {g("rl-event-eviction")}')
            cap = history_capacity(cfg, comp)
            removed = g('rl-reward-evict-good') + g('rl-reward-evict-bad') if p == 'rl' else g('sf-history-aged-out') + g('sf-reward-negative')
            occupancy = g(f'{p}-history-inserts') - removed
            if cap is not None and abs(occupancy) > cap:
                problems.append(f'{where}: history occupancy change {occupancy} exceeds capacity {cap}')
            if p == 'rl':
                if g('rl-upgrade-carryovers') != g('rl-inv-upgrade'):
                    problems.append(f'{where}: upgrade-carryovers {g("rl-upgrade-carryovers")} != inv-upgrade {g("rl-inv-upgrade")}')
                if has('rl-event-fill-upgrade-refill') and g('rl-event-fill-upgrade-refill') != g('rl-inv-upgrade'):
                    problems.append(f'{where}: upgrade refills {g("rl-event-fill-upgrade-refill")} != inv-upgrade {g("rl-inv-upgrade")}')

            d = lambda n: g(f'{p}-diag-{n}')
            if not has(f'{p}-diag-misses'):
                continue
            eq = []
            if p == 'rl':
                for r in REWARDS:
                    eq.append((f'reward-{r} by sharing', d(f'reward-{r}-BT') + d(f'reward-{r}-AT'), g(f'rl-reward-{r}')))
                    eq.append((f'reward-{r} by state', sum(d(f'reward-{r}-{c}') for c in 'SEM'), g(f'rl-reward-{r}')))
                eq.append(('decisions', d('decisions'), g('rl-decisions')))
                eq.append(('victim positions', sum(d(f'victim-position-{i}') for i in range(8)), g('rl-decisions')))
                eq.append(('victim position 0', d('victim-position-0'), g('rl-chose-oldest')))
                eq.append(('history hits', d('miss-replaced-in-history'), g('rl-reward-evict-bad')))
                eq.append(('double-dropped', d('double-dropped'), 0))
                # Stage 7 rl/protect_invalidation_feedback (statistics exist only with the option on): every protected
                # line removed by a coherence or back invalidation gets exactly one outcome, so the runs of passes that
                # ended by an invalidation must equal those rewards
                if has('rl-reward-protect-invalidated'):
                    pi = g('rl-reward-protect-invalidated')
                    eq.append(('reward-protect-invalidated by sharing', d('reward-protect-invalidated-BT') + d('reward-protect-invalidated-AT'), pi))
                    eq.append(('reward-protect-invalidated by state', sum(d(f'reward-protect-invalidated-{c}') for c in 'SEM'), pi))
                    eq.append(('runs ended by invalidation vs protect-invalidated',
                               sum(d(f'run-invalidated-{b}') for b in ('1', '2', '3-4', '5-8', '9-16', '17-32', '33+')), pi))
            else:
                eq.append(('history-hit by sharing', d('history-hit-BT') + d('history-hit-AT'), g('sf-reward-negative')))
                eq.append(('history-hit by state', sum(d(f'history-hit-{c}') for c in 'SEM'), g('sf-reward-negative')))
                eq.append(('aged-out by sharing', d('aged-out-BT') + d('aged-out-AT'), g('sf-history-aged-out')))
                eq.append(('decisions', d('decisions'), g('sf-decisions')))
                eq.append(('history hits', d('miss-replaced-in-history'), g('sf-reward-negative')))
            classes = ['cold', 'replaced-in-history', 'replaced-not-in-history', 'coherence', 'back-invalidation']
            eq.append(('miss classes', sum(d(f'miss-{c}') for c in classes), d('misses')))
            if has('rl-event-fill-demand'):
                eq.append(('misses vs demand fills', d('misses'), g('rl-event-fill-demand')))
            eq.append(('history-hit-other-write by sharing', d('history-hit-other-write-BT') + d('history-hit-other-write-AT'),
                       d('miss-replaced-in-history-other-write')))
            eq.append(('M victims', d('victims-M'), g('lru-victim-M')))
            for c in 'SEM':
                eq.append((f'evicted-{c} vs lru-victim-{c}', d(f'evicted-{c}'), g(f'lru-victim-{c}')))
            evictions = sum(g(f'lru-victim-{c}') for c in ('S', 'E', 'M', 'other'))
            eq.append(('evicted by sharing vs victims', d('evicted-BT') + d('evicted-AT'), evictions))
            if has('hint-victim-heavy-both'):
                eq.append(('evicted-AT vs delivered-heavy victims', d('evicted-AT'),
                           g('hint-victim-heavy-both') + g('hint-victim-heavy-delivered-only')))
            eq.append(('eviction-features-changed', d('eviction-features-changed'), 0))
            eq.append(('miss-inconsistent', d('miss-inconsistent'), 0))
            for name, a, b in eq:
                if a != b:
                    problems.append(f'{where}: {name} {a} != {b}')
            if d('victims-M-unwritten') > d('victims-M'):
                problems.append(f'{where}: victims-M-unwritten > victims-M')
    print(f'== {sim_dir(run)}: ' + ', '.join(f'{c} {p}_lru' for c, p in found))
    print('  RESULT', 'PASS' if not problems else 'FAIL: ' + '; '.join(problems))
    return not problems


def pct(a, b):
    return f'{100.0 * a / b:5.1f}%' if b else '    -'


def summary(run, s):
    for comp, p in policies(s):
        if f'{comp}.{p}-diag-misses' not in s:
            print(f'  {comp}: no diagnostics (rl_diag/enabled = false)')
            continue
        for core in range(len(s[f'{comp}.{p}-decisions'])):
            g = lambda n: s.get(f'{comp}.{n}', [0] * (core + 1))[core]
            d = lambda n: g(f'{p}-diag-{n}')
            dec = d('decisions')
            print(f'  -- {comp} core {core} ({p}_lru): {dec} decisions, {d("misses")} misses')
            # I-1: runs of consecutive passes / rejections
            what = 'passes' if p == 'rl' else 'rejections of candidate #1'
            for e in RUN_ENDS:
                row = [d(f'run-{e}-{b}') for b in BUCKETS]
                print(f'     I-1 runs of {what} ended by {e:11s}: ' + ' '.join(f'{b}:{v}' for b, v in zip(BUCKETS, row)))
            total_runs = sum(d(f'run-{e}-{b}') for e in RUN_ENDS for b in BUCKETS)
            long_runs = sum(d(f'run-{e}-{b}') for e in RUN_ENDS for b in BUCKETS[3:])
            print(f'     I-1 runs >= 5: {long_runs} of {total_runs} ({pct(long_runs, total_runs)}); pinned ways per decision: '
                  f'>=2 {d("pinned-short-sum") / dec if dec else 0:.3f}, >=8 {d("pinned-long-sum") / dec if dec else 0:.3f}'
                  + (f'; max passes {g("rl-max-passes")}' if p == 'rl' else ''))
            if p == 'rl':
                print(f'     I-4 decisions: all tied {pct(d("all-tied"), dec)}, tied with different features {pct(d("tied-distinct"), dec)}, '
                      f'margin < 0.01 with different features {pct(d("small-margin"), dec)}; flips per 1000 decisions: '
                      f'state {1000 * d("flips-state") / dec if dec else 0:.1f}, sharing {1000 * d("flips-sharing") / dec if dec else 0:.1f}')
                pos = [d(f'victim-position-{i}') for i in range(8)]
                print('     victim window position: ' + ' '.join(f'{i}:{v}' for i, v in enumerate(pos) if v))
                for r in REWARDS:
                    print(f'     reward {r:15s} {g("rl-reward-" + r):8d}: ' + ' '.join(f'{c}:{d("reward-" + r + "-" + c)}' for c in CLASSES))
                print(f'     I-6 protect-wasted then history outcome: agree (+1,+1) {d("double-agree")}, contradict (+1,-1) '
                      f'{d("double-contradict")} of {g("rl-reward-protect-wasted")}; snapshot differs {d("double-snapshot-differs")}')
            else:
                print('     history hits (-1) by class: ' + ' '.join(f'{c}:{d("history-hit-" + c)}' for c in CLASSES))
                print('     aged-out entries by class:  ' + ' '.join(f'{c}:{d("aged-out-" + c)}' for c in CLASSES))
            m = d('misses')
            print('     misses: ' + ', '.join(f'{c} {pct(d("miss-" + c), m)}' for c in
                                              ['cold', 'replaced-in-history', 'replaced-not-in-history', 'coherence', 'back-invalidation']))
            hits = d('miss-replaced-in-history')
            print(f'     I-2 history hits where another core wrote the line after the eviction: {d("miss-replaced-in-history-other-write")} '
                  f'of {hits} ({pct(d("miss-replaced-in-history-other-write"), hits)}); by class: '
                  + ' '.join(f'{c}:{d("history-hit-other-write-" + c)}' for c in CLASSES))
            if f'{comp}.hint-victim-heavy-both' in s:
                delivered = g('hint-victim-heavy-both') + g('hint-victim-heavy-delivered-only')
                oracle = g('hint-victim-heavy-both') + g('hint-victim-heavy-oracle-only')
                print(f'     features: AT recorded for {d("evicted-AT")} evictions = delivered-heavy victims {delivered} '
                      f'(the oracle would say {oracle})')
            print(f'     I-3 M victims never written by this core while resident: {d("victims-M-unwritten")} of {d("victims-M")} '
                  f'({pct(d("victims-M-unwritten"), d("victims-M"))})')
            snapshots(run, comp, core, p)


def snapshots(run, comp, core, p):
    paths = [os.path.join(d, f'rl-snapshots-{comp}-{core}.csv') for d in (os.path.join(run, 'telemetry'), run, sim_dir(run))]
    path = next((x for x in paths if os.path.exists(x)), None)
    if path is None:
        return
    rows = list(csv.DictReader(open(path)))
    if not rows:
        print(f'     snapshots: {path}: no rows')
        return
    if p == 'sf':
        last = rows[-1]
        print(f'     snapshots: {len(rows)} rows; history composition at the end: M {float(last["frac_M"]):.2f} E {float(last["frac_E"]):.2f} '
              f'S {float(last["frac_S"]):.2f} AT {float(last["frac_AT"]):.2f}')
        return
    names = ['wS', 'wE', 'wM', 'wBT', 'wAT']
    trained = [n for n in names if any(float(r[n]) != 0.0 for r in rows)]
    sat = lambda r: trained and all(abs(float(r[n])) >= 0.9 for n in trained)
    first = next((r['decisions'] for r in rows if sat(r)), None)
    frac = sum(1 for r in rows if sat(r)) / len(rows)
    near_m1 = sum(1 for r in rows if trained and all(float(r[n]) <= -0.9 for n in trained)) / len(rows)
    last = rows[-1]
    flips = sum(1 for a, b in zip(rows, rows[1:]) for n in trained if abs(float(b[n]) - float(a[n])) >= 1.0)
    print(f'     I-4 snapshots: {len(rows)} rows every {rows[0]["decisions"]} decisions; trained weights {",".join(trained) or "none"}; '
          f'all |w| >= 0.9 in {100 * frac:.0f}% of rows (first at decision {first}), all <= -0.9 in {100 * near_m1:.0f}%; '
          f'weight changes >= 1.0 between consecutive rows: {flips}')
    print(f'     I-4 last snapshot (decision {last["decisions"]}): ' + ' '.join(f'{n} {float(last[n]):+.3f}' for n in names)
          + '; range over the run: ' + ' '.join(f'{n} [{min(float(r[n]) for r in rows):+.2f},{max(float(r[n]) for r in rows):+.2f}]'
                                                for n in trained))


def main(argv):
    show = False
    if argv and argv[0] == '--summary':
        show, argv = True, argv[1:]
    if not argv:
        print(__doc__)
        return 2
    ok = True
    for run in argv:
        if not os.path.exists(os.path.join(sim_dir(run), 'sim.stats')):
            print(f'== {run}\n  RESULT FAIL: no sim.stats')
            ok = False
            continue
        s = load(run)
        ok &= check(run, s)
        if show:
            summary(run, s)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
