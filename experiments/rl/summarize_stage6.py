#!/usr/bin/env python3
"""Summarize a run_stage6.py output directory: per-run metrics, mean and spread per configuration, change vs LRU.

Usage: summarize_stage6.py OUTDIR [--csv FILE]

Runs: with several manifest lines for one run, the last is used. A run is used only if its status is "ok", every
applicable check passed (check_sharers and check_rl_events, plus check_rl_policy for the RL policies), and its
sim.stats has the core statistics. The check verdicts come from the manifest ("checks", recorded by run_stage6.py
per run); for manifests written before that field existed, from the per-run RESULT lines in OUTDIR/checkers.txt.
A run without a recorded passing verdict for every check is not used. Unused runs are listed with the reason.

Consistency (over the used runs): one commit and one simulator binary, no run from a tree with uncommitted
simulator changes, and within each workload and L3 size one workload command, binary and input, and a sim.cfg
equal to the first LRU run's apart from the replacement-policy keys (replacement_policy and the sf/, rl/ and
rl_diag/ sections of perf_model/l2_cache) and the run-specific output_dir and trace_prefix.

Exit status: 0 if every run is used and the runs are consistent; otherwise the sum of 1 (some run unusable: failed,
check failed or not recorded, or statistics unusable) and 2 (inconsistent runs), with an ERROR line for each problem
and a final "NOT A VALID COMPARISON" line. The tables are still printed, for diagnosis.

Metrics (all cores; the whole run, or the region of interest for --roi workloads, as sim.stats covers):
  ipc            sum(core.instructions) / performance_model.cycle_count (aggregate over the 4 cores)
  time_us        simulated time (max performance_model.elapsed_time)
  l1d_mpki       L1-D load + store misses per 1000 instructions;  l2_mpki likewise for the L2
  l2_miss        L2 load + store misses;  l2_evict: L2 evictions (evict-M/E/S/O)
  l2_inval       L2 coherency-invalidates;  l1d_inval: the same at the L1-D
  l2_downgrade, l2_upgrade   L2 coherency downgrades (M/E -> S on a remote read) and upgrades (S -> M)
  l2_dirty_evict L2 evictions in M (write-backs to the L3);  l2_coh_wb: coherency-writebacks
  l3_acc, l3_miss, dram_rd, dram_wr
Victims (every policy, from the LRU victim statistics all policies keep): MESI state mix of L2 victims and the share
whose DELIVERED sharer hint is heavy (> cores / 2).
RL columns: decisions, the share that differed from LRU (rl: victim not the LRU way; sf: candidate #1 kept and
#2 evicted), max passes (rl) and the share of pass runs >= 5 (repeated protection, I-1), mean pinned ways, the
miss classes of the diagnostics (history hits = misses on blocks the policy evicted and that were still in its
history), rewards by kind (rl), protect-wasted rewards later contradicted (I-6), decisions tied between different
features (rl), and the no-eligible-victim counter.
"""
import csv
import json
import os
import re
import statistics
import sys

CHECKS = ('check_sharers.py', 'check_rl_events.py', 'check_rl_policy.py')
POLICY_KEYS = ('perf_model/l2_cache/sf', 'perf_model/l2_cache/rl', 'perf_model/l2_cache/rl_diag')
RUN_SPECIFIC = ('output_dir', 'trace_prefix')


def load(run):
    s = {}
    for line in open(os.path.join(run, 'simulation', 'sim.stats')):
        m = re.match(r'^(\S+)\s*=\s*(.*)$', line.strip())
        if m:
            try:
                s[m.group(1)] = [float(x) for x in m.group(2).split(',')]
            except ValueError:
                pass
    return s


def load_cfg(run):
    """sim.cfg as {(section, key): value}, without the policy keys and the run-specific paths."""
    cfg, sec = {}, ''
    for line in open(os.path.join(run, 'simulation', 'sim.cfg')):
        line = line.strip()
        if line.startswith('['):
            sec = line.strip('[]')
        elif '=' in line:
            k, v = (x.strip() for x in line.split('=', 1))
            if sec in POLICY_KEYS or (sec == 'perf_model/l2_cache' and k == 'replacement_policy') or k in RUN_SPECIFIC:
                continue
            cfg[(sec, k)] = v
    return cfg


def legacy_checks(outdir):
    """{(script, run_dir): passed} from the RESULT lines of OUTDIR/checkers.txt (runs before per-run verdicts)."""
    found, script, run = {}, None, None
    path = os.path.join(outdir, 'checkers.txt')
    if not os.path.exists(path):
        return found
    for line in open(path):
        m = re.match(r'^### (\S+)', line)
        if m:
            script, run = m.group(1), None
        elif line.startswith('== ') and script:
            run = re.sub(r'(/simulation)?(/sim\.stats)?(:.*)?$', '', line[3:].strip())
        elif line.strip().startswith('RESULT') and script and run:
            found[(script, os.path.abspath(run))] = found.get((script, os.path.abspath(run)), True) and line.split()[1] == 'PASS'
            run = None
    return found


def check_verdicts(j, legacy):
    """(all applicable checks passed, description) for a manifest entry."""
    need = CHECKS if j['policy'] != 'lru' else CHECKS[:2]
    if 'checks' in j:
        got = {k: v for k, v in j['checks'].items()}
        source = 'manifest'
    else:
        got = {c: legacy[(c, os.path.abspath(j['dir']))] for c in need if (c, os.path.abspath(j['dir'])) in legacy}
        source = 'checkers.txt'
    missing = [c for c in need if c not in got]
    failed = [c for c in need if c in got and not got[c]]
    if failed or missing:
        return False, (f'check failed: {", ".join(failed)}' if failed else '') + ('; ' if failed and missing else '') \
            + (f'no recorded verdict ({source}): {", ".join(missing)}' if missing else '')
    return True, ''


def metrics(s):
    t = lambda k: sum(s.get(k, [0]))
    for k in ('core.instructions', 'performance_model.cycle_count', 'performance_model.elapsed_time'):
        if k not in s:
            raise ValueError(f'sim.stats lacks {k}')
    instr = t('core.instructions')
    cyc = s['performance_model.cycle_count'][0]
    if not instr or not cyc:
        raise ValueError('no instructions or cycles in sim.stats')
    vic = sum(t(f'L2.lru-victim-{c}') for c in ('S', 'E', 'M', 'other'))
    cmp_ = t('L2.hint-victim-compared')
    m = dict(victim_S_E_M='/'.join(f'{100 * t(f"L2.lru-victim-{c}") / vic:.0f}' if vic else '-' for c in 'SEM'),
             victim_heavy_hint_pct=100 * (t('L2.hint-victim-heavy-both') + t('L2.hint-victim-heavy-delivered-only')) / cmp_ if cmp_ else 0)
    m.update(instr=instr, ipc=instr / cyc, time_us=max(s['performance_model.elapsed_time']) / 1e9,
             l1d_mpki=1000 * (t('L1-D.load-misses') + t('L1-D.store-misses')) / instr,
             l2_mpki=1000 * (t('L2.load-misses') + t('L2.store-misses')) / instr,
             l2_acc=t('L2.loads') + t('L2.stores'),
             l2_miss=t('L2.load-misses') + t('L2.store-misses'),
             l2_evict=sum(t(f'L2.evict-{c}') for c in 'MESO'),
             l1d_inval=t('L1-D.coherency-invalidates'), l2_inval=t('L2.coherency-invalidates'),
             l2_downgrade=t('L2.coherency-downgrades'), l2_upgrade=t('L2.coherency-upgrades'),
             l2_dirty_evict=t('L2.evict-M'), l2_coh_wb=t('L2.coherency-writebacks'),
             l1d_backinval=t('L1-D.rl-event-inv-back-invalidation'), l3_backinval=sum(t(f'L3.backinval-{c}') for c in 'MESO'),
             l3_acc=t('L3.loads') + t('L3.stores'), l3_miss=t('L3.load-misses') + t('L3.store-misses'),
             dram_rd=t('dram.reads'), dram_wr=t('dram.writes'),
             stale_writes=sum(t(f'{c}.coherence-check-stale-write-hits') for c in ('L1-D', 'L2', 'L3')))
    runs = lambda p, ends, buckets: sum(t(f'L2.{p}-diag-run-{e}-{b}') for e in ends for b in buckets)
    for p in ('rl', 'sf'):
        if f'L2.{p}-decisions' in s:
            dec = t(f'L2.{p}-decisions')
            m['decisions'] = dec
            m['differ_pct'] = 100 * (t('L2.rl-chose-other') if p == 'rl' else t('L2.sf-second-chances')) / dec if dec else 0
            m['passes'] = t('L2.rl-passes') if p == 'rl' else t('L2.sf-second-chances')
            m['max_passes'] = max(s['L2.rl-max-passes']) if p == 'rl' and 'L2.rl-max-passes' in s else None
            ev = t(f'L2.{p}-diag-evicted-BT') + t(f'L2.{p}-diag-evicted-AT')
            m['evicted_AT_pct'] = 100 * t(f'L2.{p}-diag-evicted-AT') / ev if ev else 0
            m['evicted_S_E_M'] = '/'.join(f'{100 * t(f"L2.{p}-diag-evicted-{c}") / ev:.0f}' if ev else '-' for c in 'SEM')
            m['tied_distinct_pct'] = 100 * t('L2.rl-diag-tied-distinct') / dec if (p == 'rl' and dec) else None
            mi = t(f'L2.{p}-diag-misses')
            for c in ('cold', 'replaced-in-history', 'coherence', 'back-invalidation'):
                m['miss_' + c] = 100 * t(f'L2.{p}-diag-miss-{c}') / mi if mi else 0
            m['no_eligible'] = t(f'L2.{p}-no-eligible-victim')
            allb = ['1', '2', '3-4', '5-8', '9-16', '17-32', '33+']
            nr = runs(p, ['hit', 'evicted', 'invalidated'], allb)
            m['runs_ge5_pct'] = 100 * runs(p, ['hit', 'evicted', 'invalidated'], allb[3:]) / nr if nr else 0
            m['runs_end_invalidated_pct'] = 100 * runs(p, ['invalidated'], allb) / nr if nr else 0
            m['pinned_long_per_dec'] = t(f'L2.{p}-diag-pinned-long-sum') / dec if dec else 0
            if p == 'rl':
                m['rewards'] = '/'.join('%.0f' % t(f'L2.rl-reward-{k}') for k in ('evict-bad', 'evict-good', 'protect-good', 'protect-wasted'))
                pw = t('L2.rl-reward-protect-wasted')
                m['protect_contradicted_pct'] = 100 * t('L2.rl-diag-double-contradict') / pw if pw else 0
    return m


def main(argv):
    outdir = argv[0]
    jobs = list({j['name']: j for j in (json.loads(l) for l in open(os.path.join(outdir, 'manifest.jsonl')))}.values())
    legacy = legacy_checks(outdir)
    failed, rows, used, errors = [], [], [], []
    for j in jobs:
        if j['status'] != 'ok':
            failed.append((j['name'], j['status'] + (' (timeout)' if j.get('timed_out') else '') + (' (crash)' if j.get('crashed') else '')
                           + ''.join(f', {k} FAIL' for k, v in j.get('checks', {}).items() if not v)))
            continue
        passed, why = check_verdicts(j, legacy)
        if not passed:
            failed.append((j['name'], why))
            continue
        try:
            m = metrics(load(j['dir']))
        except (OSError, ValueError, KeyError) as e:
            failed.append((j['name'], f'unusable statistics: {e}'))
            continue
        used.append(j)
        rows.append(dict(name=j['name'], workload=j['workload'], l3=j['l3'], policy=j['policy'], rep=j['rep'],
                         roi=j.get('roi', False), wall_s=j['wall_s'], **m))
    if '--csv' in argv and rows:
        with open(argv[argv.index('--csv') + 1], 'w', newline='') as f:
            keys = list(dict.fromkeys(k for r in rows for k in r))
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            w.writerows(rows)
    print(f'{len(rows)} of {len(jobs)} runs usable' + (''.join(f'\n  ERROR: not used: {n}: {why}' for n, why in failed)))
    prov = {(j.get('commit', 'not recorded'), j.get('binary_md5', 'not recorded')) for j in used}
    print('provenance (commit, simulator md5): ' + '; '.join(f'{c[:12]} {b}' for c, b in sorted(prov)))
    if len(prov) > 1:
        errors.append('runs from different simulator builds: ' + '; '.join(
            f'{c[:12]} {b}: {sum((j.get("commit", "not recorded"), j.get("binary_md5", "not recorded")) == (c, b) for j in used)} runs'
            for c, b in sorted(prov)))
    dirty = [j['name'] for j in used if not j.get('tree_clean', True)]
    if dirty:
        errors.append(f'{len(dirty)} runs made with uncommitted simulator changes (tree_clean false): {dirty[:3]}')
    groups = {}
    for r in rows:
        groups.setdefault((r['workload'], r['l3'], r['policy']), []).append(r)
    # Configuration identity within each (workload, L3)
    for (w, l3) in sorted({(k[0], k[1]) for k in groups}):
        g = sorted((j for j in used if j['workload'] == w and j['l3'] == l3), key=lambda j: (j['policy'] != 'lru', j['rep']))
        ref = load_cfg(g[0]['dir'])
        bad = []
        for j in g[1:]:
            c = load_cfg(j['dir'])
            diff = sorted(set(ref.items()) ^ set(c.items()))
            if diff:
                bad.append(f'{j["name"]}: ' + ', '.join(f'[{sec}] {k}: {ref.get((sec, k), "absent")} vs {c.get((sec, k), "absent")}'
                                                        for sec, k in dict.fromkeys(k for k, _ in diff)))
        cmds = {tuple(j['cmd'][j['cmd'].index('--'):]) for j in g}
        inputs = {(j.get('workload_md5'), j.get('stdin_md5')) for j in g}
        print(f'config check {w} L3 {l3}: {len(g)} runs vs {g[0]["name"]}: '
              + ('identical apart from the policy keys' if not bad else f'{len(bad)} runs DIFFER'))
        errors += [f'{w} L3 {l3}: sim.cfg differs beyond the policy keys: {b}' for b in bad]
        if len(cmds) > 1:
            errors.append(f'{w} L3 {l3}: {len(cmds)} different workload commands: {sorted(cmds)}')
        if len(inputs) > 1:
            errors.append(f'{w} L3 {l3}: {len(inputs)} different workload binaries or inputs (md5): {sorted(map(str, inputs))}')
    for e in errors:
        print(f'  ERROR: {e}')
    cols = ['ipc', 'time_us', 'l1d_mpki', 'l2_mpki', 'l2_miss', 'l2_inval', 'l2_dirty_evict', 'l3_miss', 'dram_rd', 'dram_wr']
    order = ['lru', 'sf-text', 'sf-example', 'rl-n2', 'rl-n4']
    for (w, l3) in sorted({(k[0], k[1]) for k in groups}):
        roi = any(r['roi'] for k, g in groups.items() if k[:2] == (w, l3) for r in g)
        print(f'\n== {w}, L3 {l3} KB, {"region of interest" if roi else "whole run"}: mean of repetitions '
              f'(spread = (max - min) / mean); change vs LRU')
        base = groups.get((w, l3, 'lru'))
        print('  ' + f'{"policy":11s}' + ''.join(f'{c:>17s}' for c in cols))
        for p in order:
            g = groups.get((w, l3, p))
            if not g:
                continue
            line, dline = f'  {p:11s}', f'  {"":11s}'
            for c in cols:
                v = [r[c] for r in g]
                mean = statistics.mean(v)
                spread = (max(v) - min(v)) / mean * 100 if mean else 0
                line += f'{mean:>10.4g} ±{spread:4.2f}%'
                if base and p != 'lru':
                    b = statistics.mean(r[c] for r in base)
                    dline += f'{(100 * (mean - b) / b if b else 0):>+16.2f}%'
                else:
                    dline += f'{"":>17s}'
            print(line)
            if p != 'lru':
                print(dline)
        if any(groups.get((w, l3, p)) for p in order[1:]):
            print('  RL behavior (mean over repetitions):')
        for p in order[1:]:
            g = groups.get((w, l3, p))
            if not g:
                continue
            avg = lambda k: statistics.mean(r[k] for r in g if r.get(k) is not None) if any(r.get(k) is not None for r in g) else None
            td, mp = avg('tied_distinct_pct'), [r['max_passes'] for r in g if r.get('max_passes') is not None]
            print(f'    {p:11s} decisions {avg("decisions"):9.0f}  differ-from-LRU {avg("differ_pct"):5.1f}%  '
                  + (f'max-passes {max(mp):3.0f}  ' if mp else '')
                  + f'runs>=5 {avg("runs_ge5_pct"):4.1f}%  runs ending invalidated {avg("runs_end_invalidated_pct"):4.1f}%  '
                  f'pinned/dec {avg("pinned_long_per_dec"):.3f}  ' + (f'tied-distinct {td:4.1f}%  ' if td is not None else '')
                  + f'misses: cold {avg("miss_cold"):4.1f}% in-history {avg("miss_replaced-in-history"):4.1f}% '
                  f'coherence {avg("miss_coherence"):4.1f}% back-inval {avg("miss_back-invalidation"):4.1f}%  '
                  f'no-eligible {sum(r["no_eligible"] for r in g):.0f}'
                  + (f'\n      rewards bad/good/protect-good/protect-wasted (rep 1) {g[0]["rewards"]}, '
                     f'protect-wasted contradicted {avg("protect_contradicted_pct"):4.1f}%' if p.startswith('rl') else ''))
        print('  L2 victims (all policies): ' + ', '.join(
            f'{p} S/E/M% {groups[(w, l3, p)][0]["victim_S_E_M"]} heavy-hint {statistics.mean(r["victim_heavy_hint_pct"] for r in groups[(w, l3, p)]):.1f}%'
            for p in order if groups.get((w, l3, p))))
        print('  stale write hits: ' + ', '.join(f'{p} {sum(r["stale_writes"] for r in groups[(w, l3, p)]):.0f}'
                                                 for p in order if groups.get((w, l3, p))))
        walls = lambda p: '/'.join('%.0f' % r['wall_s'] for r in groups.get((w, l3, p), []))
        print('  wall-clock s: ' + ', '.join(p + ' ' + walls(p) for p in order if groups.get((w, l3, p))))
    status = (1 if failed else 0) + (2 if errors else 0)
    if status:
        print(f'\nNOT A VALID COMPARISON (exit {status}): ' + ', '.join(
            x for x, c in ((f'{len(failed)} runs not used', failed), (f'{len(errors)} consistency errors', errors)) if c), file=sys.stderr)
        print(f'NOT A VALID COMPARISON (exit {status})')
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
