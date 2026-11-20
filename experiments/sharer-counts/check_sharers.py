#!/usr/bin/env python3
"""Check the sharer-count diagnostics in one or more Sniper sim.stats files.

Usage: check_sharers.py [-q] RESULTS_DIR_OR_SIM_STATS...

For every cache component and core, with v = sum(lru-victim-{M,E,S,other}):
  oracle      v == sum(oracle-victim-dir-sharers-*) + dir-missing + dir-unavailable
              v == sum(oracle-victim-upper-sharers-*) + upper-unavailable
  cachedlocs  v == compared + unavailable, compared == match + mismatch,
              sum(cachedlocs-victim-sharers-*) == count-match + count-mismatch
                                               == sum(oracle-victim-upper-sharers-*)
  hint        v == compared + missing + unavailable,
              compared == exact + over + under == sum(delivered-*) == sum(oracle-*)
                       == heavy-both + heavy-neither + heavy-delivered-only + heavy-oracle-only

Exit status 1 if an invariant fails, the presence bitmap ever disagrees with the
probe (cachedlocs mismatch / count-mismatch), or a write fill delivered a hint
while other sharers were still listed (sharer-hint-write-with-sharers). An
inexact hint is expected (it is allowed to be stale) and is only reported.
Sections whose stats are absent (older builds) are skipped.
"""
import os
import re
import sys

B = '0 1 2 3 4plus'.split()


def load(path):
    if os.path.isdir(path):
        for cand in ('simulation/sim.stats', 'sim.stats'):
            if os.path.exists(os.path.join(path, cand)):
                path = os.path.join(path, cand)
                break
    stats = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'^(\S+)\s*=\s*(.*)$', line.strip())
            if m:
                try:
                    stats[m.group(1)] = [int(float(x)) for x in m.group(2).split(',')]
                except ValueError:
                    pass
    return path, stats


def check(path, verbose):
    path, s = load(path)
    comps = sorted({k.split('.')[0] for k in s if k.endswith('.lru-victim-M')})
    ok, problems = True, []
    tot = dict(victims=0, cv_compared=0, cv_mismatch=0, cv_count_mismatch=0,
               hint_compared=0, hint_missing=0, hint_exact=0, hint_over=0, hint_under=0, hint_abs_error=0,
               heavy_both=0, heavy_neither=0, heavy_delivered_only=0, heavy_oracle_only=0)
    lines = []
    for c in comps:
        has = lambda n: f'{c}.{n}' in s
        for core in range(len(s[f'{c}.lru-victim-M'])):
            G = lambda n: s[f'{c}.{n}'][core]
            v = sum(G(f'lru-victim-{x}') for x in 'M E S other'.split())
            tot['victims'] += v
            good = True
            if has('oracle-victim-dir-missing'):
                good &= v == sum(G(f'oracle-victim-dir-sharers-{b}') for b in B) + G('oracle-victim-dir-missing') + G('oracle-victim-dir-unavailable')
                good &= v == sum(G(f'oracle-victim-upper-sharers-{b}') for b in B) + G('oracle-victim-upper-unavailable')
            if has('cachedlocs-victim-compared'):
                cmp_, mis, cmm = G('cachedlocs-victim-compared'), G('cachedlocs-victim-mismatch'), G('cachedlocs-victim-count-mismatch')
                good &= v == cmp_ + G('cachedlocs-victim-unavailable') and cmp_ == G('cachedlocs-victim-match') + mis
                good &= sum(G(f'cachedlocs-victim-sharers-{b}') for b in B) == G('cachedlocs-victim-count-match') + cmm \
                    == sum(G(f'oracle-victim-upper-sharers-{b}') for b in B)
                tot['cv_compared'] += cmp_; tot['cv_mismatch'] += mis; tot['cv_count_mismatch'] += cmm
            if has('hint-victim-compared'):
                h = {k: G(f'hint-victim-{k}') for k in ['compared', 'missing', 'unavailable', 'exact', 'over', 'under', 'abs-error',
                                                       'heavy-both', 'heavy-neither', 'heavy-delivered-only', 'heavy-oracle-only']}
                dl = [G(f'hint-victim-delivered-{b}') for b in B]
                orc = [G(f'hint-victim-oracle-{b}') for b in B]
                good &= v == h['compared'] + h['missing'] + h['unavailable']
                good &= h['compared'] == h['exact'] + h['over'] + h['under'] == sum(dl) == sum(orc) \
                    == h['heavy-both'] + h['heavy-neither'] + h['heavy-delivered-only'] + h['heavy-oracle-only']
                for k in ['compared', 'missing', 'exact', 'over', 'under']:
                    tot[f'hint_{k}'] += h[k]
                tot['hint_abs_error'] += h['abs-error']
                for k in ['both', 'neither', 'delivered-only', 'oracle-only']:
                    tot['heavy_' + k.replace('-', '_')] += h[f'heavy-{k}']
                if h['compared'] or h['missing']:
                    lines.append(f'  {c:6s} core{core} victims={v:7d} hint: compared={h["compared"]} missing={h["missing"]} '
                                 f'exact={h["exact"]} over={h["over"]} under={h["under"]} abs-err={h["abs-error"]} '
                                 f'delivered={dl} oracle={orc} heavy[both,neither,deliv-only,oracle-only]='
                                 f'[{h["heavy-both"]},{h["heavy-neither"]},{h["heavy-delivered-only"]},{h["heavy-oracle-only"]}]')
            if not good:
                ok = False
                problems.append(f'{c} core{core}: invariant failed')
    fills = {k: sum(s.get(f'L2.sharer-hint-{k}', [0])) for k in
             ['fills', 'fills-write', 'fill-1', 'fill-2', 'fill-3', 'fill-4plus',
              'write-with-sharers', 'exclusive-with-sharers', 'write-hit-resets']}
    if tot['cv_mismatch'] or tot['cv_count_mismatch']:
        problems.append(f'presence bitmap disagrees with probe ({tot["cv_mismatch"]} bitmap, {tot["cv_count_mismatch"]} count)')
    if not comps:
        problems.append('no cache statistics found')
    if fills['write-with-sharers']:
        problems.append(f'{fills["write-with-sharers"]} write fills delivered a hint with other sharers listed')
    hc = tot['hint_compared']
    print(f'== {path}')
    if verbose:
        for l in lines:
            print(l)
    summary = f'  victims={tot["victims"]} cachedlocs: compared={tot["cv_compared"]} mismatch={tot["cv_mismatch"]}'
    if hc or tot['hint_missing']:
        summary += (f'\n  L2 hint: compared={hc} missing={tot["hint_missing"]} exact={tot["hint_exact"]} ({100.0 * tot["hint_exact"] / max(hc, 1):.1f}%) '
                    f'over={tot["hint_over"]} under={tot["hint_under"]} MAE={tot["hint_abs_error"] / max(hc, 1):.3f} '
                    f'heavy[both,neither,deliv-only,oracle-only]=[{tot["heavy_both"]},{tot["heavy_neither"]},'
                    f'{tot["heavy_delivered_only"]},{tot["heavy_oracle_only"]}]')
    if fills['fills']:
        summary += (f'\n  L2 fills: {fills["fills"]} (write {fills["fills-write"]}) delivered[1,2,3,4+]=[{fills["fill-1"]},{fills["fill-2"]},'
                    f'{fills["fill-3"]},{fills["fill-4plus"]}] write-with-sharers={fills["write-with-sharers"]} '
                    f'exclusive-with-sharers={fills["exclusive-with-sharers"]} write-hit-resets={fills["write-hit-resets"]}')
    print(summary)
    print('  RESULT', 'PASS' if ok and not problems else 'FAIL: ' + '; '.join(problems))
    return ok and not problems


def main(argv):
    verbose = '-q' not in argv
    paths = [a for a in argv if a != '-q']
    if not paths:
        print(__doc__)
        return 2
    results = [check(p, verbose) for p in paths]
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
