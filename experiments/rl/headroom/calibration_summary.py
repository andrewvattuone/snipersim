#!/usr/bin/env python3
"""Calibration summary of four headroom.py calibrate outputs (docs/rl/HEADROOM.md 7.9).
Usage: python3 -I calibration_summary.py DIR   (DIR holds calibrate-lru-r{1,2}-prio-r{1,2}.txt)

This is the held-out summary script that was frozen before the held-out runs (results/stage7-headroom-heldout/scripts/
summarize_heldout.py, sha256 915b404b...), with its parsing defect corrected: the frozen script read some signed
values with (-?[\d.]+), which rejects the leading '+' that headroom.py prints for positive values ('{:+.3f}'), so a
positive null-pair difference was silently dropped and the noise floor became nan (HEADROOM.md 7.9, erratum). Every
signed value is now read with SIGNED; parse_lines is tested in test_headroom.py. The criteria are unchanged.

Verdict (pre-registered, FREEZE.txt):
  - a self-replay not exact (any calibrate output)            -> STOP: critical tracing failure, no verdict
  - noise floor (largest null-pair |difference|) > 0.25 points -> INCONCLUSIVE
  - wrong setup (no --model-permissions, or --development-data) -> INCONCLUSIVE (not this evaluation)
  - a permission-counter consistency check fails             -> INCONCLUSIVE (measurement problem)
  - any of the 8 cross-prediction errors outside +-0.5 points, or a sampling difference (ROI trace vs sim.stats) above
    0.5 points (headroom.py's existing representativeness criterion; set_modulus=1)   -> FAIL
  - otherwise                                                 -> PASS
"""
import os
import re
import sys

THR, NOISE_MAX = 0.5, 0.25
PAIRS = [(i, j) for i in (1, 2) for j in (1, 2)]
# development traces (docs/rl/HEADROOM.md 7.7): every error positive, i.e. the model under-predicts priority's benefit
DEV_ERR = {(1, 1): (0.096, 0.187), (1, 2): (0.191, 0.321), (2, 1): (0.026, 0.112), (2, 2): (0.121, 0.247)}
# a signed decimal as headroom.py prints it: '{:+.3f}' gives a leading '+' or '-', '{:.3f}' none or '-'
SIGNED = r'[-+]?\d+(?:\.\d+)?'
NUM = re.compile(r'([a-z_0-9]+) (' + SIGNED + ')')


def parse(path):
    return parse_lines(open(path).read().splitlines(), path)


def parse_lines(lines, path=None):
    """One headroom.py calibrate output, as a list of lines."""
    S = SIGNED
    out = dict(summaries=[], variants={}, mech={}, null=[], path=path)
    for k, ln in enumerate(lines):
        m = re.match(r'   (lru|priority) window \d+ inclusion-model (\d) coherence-model (\d) permission-model (\d)', ln)
        if m:
            vals = {a: (float(b) if '.' in b else int(b)) for a, b in NUM.findall(lines[k + 1])}
            out['summaries'].append(dict(policy=m.group(1), perm=int(m.group(4)), **vals))
        m = re.match(r'   (LRU|priority) +misses (\d+)   all sets \(sim.stats\) (\d+)', ln)
        if m:
            out['rec_' + m.group(1).lower()] = (int(m.group(2)), int(m.group(3)))
        m = re.match(r'   measured priority vs LRU: sampled sets (' + S + r')%   all sets (' + S + r')%', ln)
        if m:
            out['measured'], out['measured_all'] = float(m.group(1)), float(m.group(2))
        m = re.match(r'   (full model|no inclusion model|no coherence model|neither) +from the LRU trace (' + S + r')% \(error (' + S + r')\)'
                     r'   from the priority trace (' + S + r')% \(error (' + S + r')\)', ln)
        if m:
            out['variants'][m.group(1)] = tuple(float(m.group(x)) for x in (2, 3, 4, 5))
        m = re.match(r'   ([a-z_]+) +(\d+) +(\d+) +(\d+) +(' + S + r') \| +(\d+) +(' + S + r')$', ln)
        if m:
            out['mech'][m.group(1)] = tuple(int(m.group(x)) for x in (2, 3, 4, 6)) + (float(m.group(5)), float(m.group(7)))
        m = re.match(r'   \S+ vs \S+: sampled sets (' + S + r')%  all sets (' + S + r')%', ln)
        if m:
            out['null'].append((float(m.group(1)), float(m.group(2))))
        if ln.startswith('   self-replays '):
            out['self_exact'] = ln.strip() == 'self-replays EXACT'
        if ln.startswith('== RESULT:'):
            out['result'] = ln
        if ln.startswith('== model: permission model on'):
            out['perm_on'] = True
        if '--development-data' in ln and ln.startswith('# command'):
            out['devflag'] = True
    return out


def main(D):
    R = {p: parse(os.path.join(D, f'calibrate-lru-r{p[0]}-prio-r{p[1]}.txt')) for p in PAIRS}
    ok_setup = all(r.get('perm_on') and not r.get('devflag') and len(r['summaries']) == 4 and all(s['perm'] == 1 for s in r['summaries'])
                   for r in R.values())
    print(f'== 0. setup: every output ran --model-permissions without --development-data, 4 permission-model summaries each: '
          f'{"yes" if ok_setup else "NO"}')

    print('== 1. self-replays (exact reproduction of the recorded policy: correctness of the replay, not predictive accuracy)')
    self_exact = all(r.get('self_exact') for r in R.values())
    for p, r in R.items():
        sL, sP = r['summaries'][0], r['summaries'][1]
        print(f'   pair lru-r{p[0]} prio-r{p[1]}: {"EXACT" if r.get("self_exact") else "NOT EXACT"} (lru misses {sL["misses"]} rec {r["rec_lru"][0]}, '
              f'victims same {sL["victim_same_pct"]:.2f}%; priority misses {sP["misses"]} rec {r["rec_priority"][0]}, victims same {sP["victim_same_pct"]:.2f}%)')

    print('== 2. noise floor (null pairs, new runs only)')
    nulls = R[(1, 1)]['null']
    noise = max(abs(a) for a, _ in nulls) if nulls else float('nan')
    same_nulls = all(r['null'] == nulls for r in R.values())
    for a, b in nulls:
        print(f'   null pair: sampled sets {a:+.3f}%  all sets {b:+.3f}%')
    print(f'   noise floor {noise:.3f} points (limit {NOISE_MAX}); identical in all four outputs: {same_nulls}')

    print('== 3. the eight cross-policy predictions, priority vs LRU (full model; points of the LRU misses; threshold +-0.5)')
    print(f'   {"pair":16s} {"measured":>9s} | {"from LRU":>9s} {"error":>7s} {"dev err":>7s} | {"from prio":>9s} {"error":>7s} {"dev err":>7s}')
    errs = []
    for p, r in R.items():
        pl, el, pp, ep = r['variants']['full model']
        errs += [el, ep]
        print(f'   lru-r{p[0]} prio-r{p[1]}    {r["measured"]:+8.3f}% | {pl:+8.3f}% {el:+7.3f} {DEV_ERR[p][0]:+7.3f} | {pp:+8.3f}% {ep:+7.3f} {DEV_ERR[p][1]:+7.3f}'
              f'   (all-set measured {r["measured_all"]:+.3f}%)')
    print(f'   errors: max |error| {max(abs(e) for e in errs):.3f}; within +-{THR}: {sum(abs(e) <= THR for e in errs)}/8; '
          f'mean {sum(errs) / 8:+.3f}; from LRU traces mean {sum(errs[0::2]) / 4:+.3f}, from priority traces mean {sum(errs[1::2]) / 4:+.3f}')
    pos = sum(e > 0 for e in errs)
    print(f'   bias direction: {pos}/8 errors positive (predicted priority benefit smaller than measured), {8 - pos}/8 negative; '
          f'development traces: 8/8 positive (mean {sum(sum(v) for v in DEV_ERR.values()) / 8:+.3f})')
    print('   sensitivity variants (errors from LRU / priority traces; reported, not criteria):')
    for v in ('no inclusion model', 'no coherence model', 'neither'):
        print(f'     {v:20s} ' + '  '.join(f'r{p[0]}r{p[1]} {R[p]["variants"][v][1]:+.3f}/{R[p]["variants"][v][3]:+.3f}' for p in PAIRS))

    print('== 4. mechanism counts: recorded against predicted (sampled = all sets)')
    KEYS = ('misses', 'demand', 'store_misses_upgrade', 'accesses', 'decisions', 'inclusion_victims', 'inv_recorded_coherence', 'inv_modelled',
            'inv_deferred', 'downgrades_modelled', 'dirty_evictions', 'phantom_accesses', 'phantom_misses', 'suppressed_accesses',
            'modelled_candidate_pct', 'victim_same_pct')
    seen = set()
    for p, r in R.items():
        sL, sP, cPL, cLP = r['summaries']
        for name, rec, pred, tag in ((f'LRU r{p[0]} trace -> priority (vs recorded priority r{p[1]})', sP, cPL, ('L', p[0], p[1])),
                                     (f'priority r{p[1]} trace -> LRU (vs recorded LRU r{p[0]})', sL, cLP, ('P', p[1], p[0]))):
            print(f'   {name}')
            for k in KEYS:
                g = lambda s: (s['misses'] - s['store_misses_upgrade']) if k == 'demand' else (s['loads'] + s['stores'] if k == 'accesses' else s.get(k))
                a, b = g(rec), g(pred)
                if isinstance(a, float) or isinstance(b, float):
                    print(f'     {k:24s} recorded-policy self-replay {a:12.2f}   predicted {b:12.2f}')
                else:
                    print(f'     {k:24s} recorded {a:12d}   predicted {b:12d}   {100.0 * (b - a) / a if a else 0.0:+7.2f}%   diff {b - a:+9d}')

    print('== 5. permission model: inferred L3 state, uncertainty and correction counters (cross-replays)')
    PC = ('fill_state_differs', 'recorded_refetch_kept', 'l3_state_inferred', 'l3_state_inferred_unknown', 'own_state_store_hits',
          'l1_write_upgrades', 'resyncs', 'l1_write_hit_implied_m', 'l3_observed_agree', 'l3_observed_dirty_was_clean',
          'l3_observed_clean_was_dirty', 'l3_observed_unknown_m', 'l3_observed_unknown_e')
    for p, r in R.items():
        for lab, s in (('self lru', r['summaries'][0]), ('self priority', r['summaries'][1]), (f'LRU r{p[0]} as priority', r['summaries'][2]),
                       (f'priority r{p[1]} as LRU', r['summaries'][3])):
            obs = s['l3_observed_agree'] + s['l3_observed_dirty_was_clean'] + s['l3_observed_clean_was_dirty']
            print(f'   r{p[0]}r{p[1]} {lab:22s} ' + ' '.join(f'{k} {s[k]}' for k in PC) +
                  f'  | observed L3 disagreement {100.0 * (s["l3_observed_dirty_was_clean"] + s["l3_observed_clean_was_dirty"]) / max(1, obs):.2f}%'
                  f'  unknown share of inferences {100.0 * s["l3_state_inferred_unknown"] / max(1, s["l3_state_inferred"]):.3f}%')

    print('== 6. permission-counter consistency (pre-registered invariants)')
    checks = []
    SELF_ZERO = ('fill_state_differs', 'recorded_refetch_kept', 'l3_state_inferred', 'l3_state_inferred_unknown', 'own_state_store_hits',
                 'l1_write_upgrades', 'resyncs')
    TRACE_ONLY = ('l1_write_hit_implied_m', 'l3_observed_agree', 'l3_observed_dirty_was_clean', 'l3_observed_clean_was_dirty',
                  'l3_observed_unknown_m', 'l3_observed_unknown_e')
    for p, r in R.items():
        sL, sP, cPL, cLP = r['summaries']
        checks.append((f'r{p[0]}r{p[1]} C1 self-replays: correction counters all 0', all(s[k] == 0 for s in (sL, sP) for k in SELF_ZERO)))
        checks.append((f'r{p[0]}r{p[1]} C2 trace-derived counters identical in self and cross replays of the same trace',
                       all(sL[k] == cPL[k] and sP[k] == cLP[k] for k in TRACE_ONLY)))
        checks.append((f'r{p[0]}r{p[1]} C3 no observed M that the history called clean, no M without a recorded write',
                       all(s['l3_observed_dirty_was_clean'] == 0 and s['l3_observed_unknown_m'] == 0 for s in r['summaries'])))
        checks.append((f'r{p[0]}r{p[1]} C4 bounds: unknown <= inferred, L1 write upgrades <= upgrade misses, own-state hits <= recorded upgrades',
                       all(s['l3_state_inferred_unknown'] <= s['l3_state_inferred'] for s in r['summaries'])
                       and cPL['l1_write_upgrades'] <= cPL['store_misses_upgrade'] and cLP['l1_write_upgrades'] <= cLP['store_misses_upgrade']
                       and cPL['own_state_store_hits'] <= sL['store_misses_upgrade'] and cLP['own_state_store_hits'] <= sP['store_misses_upgrade']))
    for name, ok in checks:
        print(f'   {name}: {"ok" if ok else "FAIL"}')
    consistent = all(ok for _, ok in checks) and same_nulls
    sampling = max(abs(r['measured'] - r['measured_all']) for r in R.values())
    print(f'   largest sampling difference (trace ROI vs sim.stats) {sampling:.3f} points (limit {THR})')

    print('== 7. calibrate RESULT lines')
    for p, r in R.items():
        print(f'   r{p[0]}r{p[1]} {r.get("result")}')
    if not self_exact:
        verdict = 'STOP: a self-replay is not exact (critical tracing failure; no calibration verdict)'
    elif not noise <= NOISE_MAX:
        verdict = f'INCONCLUSIVE (noise floor {noise:.3f} > {NOISE_MAX} points)'
    elif not ok_setup:
        verdict = 'INCONCLUSIVE (setup: not the pre-registered --model-permissions held-out evaluation)'
    elif not consistent:
        verdict = 'INCONCLUSIVE (a consistency check failed: measurement problem)'
    elif any(abs(e) > THR for e in errs) or sampling > THR:
        verdict = f'FAIL ({sum(abs(e) > THR for e in errs)}/8 cross-prediction errors outside +-{THR} points; sampling difference {sampling:.3f})'
    else:
        verdict = 'PASS (self-replays exact, noise floor within 0.25, all 8 cross-prediction errors within +-0.5 points)'
    print(f'== HELD-OUT VERDICT: {verdict}')
    return 0 if verdict.startswith('PASS') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
