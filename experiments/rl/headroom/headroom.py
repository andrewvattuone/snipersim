#!/usr/bin/env python3
"""Headroom diagnostic: offline analyses of rl-trace.bin (docs/rl/HEADROOM.md).

  headroom.py check RUN...                     the trace against the run's sim.stats (exact with set_modulus = 1)
  headroom.py replay RUN --policy P [...]      shadow replays (lru, priority, order:<6 classes>, opt-window, opt-set)
  headroom.py labels RUN [--window N]          window-oracle labels of the recorded decisions (concept A)
  headroom.py calibrate --lru RUN --prio RUN   the calibration of the shadow model (docs/rl/HEADROOM.md, Phase 5)
  headroom.py orders RUN [--all]               best fixed class order in the shadow (concept B); gated on calibration
  headroom.py phase RUN --policy A --policy B  per-epoch comparison of independent replays (concept C, indicative)

RUN is a simulation output directory holding rl-trace.bin, rl-trace.json and simulation/sim.stats. Every output
starts with provenance lines (command, tool checksums, tool tree commit, input checksums). Future information is used
only by the opt-* policies and the labels, which are diagnostic and never reach a simulated policy.
"""

import argparse
import itertools
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import labels as L  # noqa: E402
import rltrace as R  # noqa: E402
import shadow as S  # noqa: E402


def trace_path(run):
    return os.path.join(run, 'rl-trace.bin') if os.path.isdir(run) else run


def load(run):
    tr = R.Trace(trace_path(run))
    if not tr.complete:
        raise R.TraceError(f'{run}: trace has no end record (run did not finish?)')
    return tr


def header(argv, runs):
    ins = []
    for r in runs:
        p = trace_path(r)
        ins.append(p)
        js = os.path.splitext(p)[0] + '.json'
        if os.path.exists(js):
            ins.append(js)
    for line in R.provenance(argv, ins):
        print(line)


def recorded_misses(tr):
    """Contents misses of the sampled sets in the ROI (what a shadow reproduces; Sniper's statistics add the store
    hits that overlap an in-flight fill, reported separately as store_overlap_misses)."""
    tal = R.trace_tallies(tr)
    return sum(t['contents_misses'] for t in tal.values()), tal


def stats_misses(run):
    st = R.load_stats(run)
    return sum(st.get('L2.load-misses', [0])) + sum(st.get('L2.store-misses', [0]))


def pct(a, b):
    return 100.0 * (a - b) / b if b else float('nan')


# ------------------------------------------------------------------------------------------------------- commands

def cmd_check(args):
    header(sys.argv, args.runs)
    ok_all = True
    for run in args.runs:
        tr = load(run)
        print(f'== {run}')
        print(f'   records {len(tr)} {tr.counts()}')
        print(f'   cores {tr.cores} sets {tr.sets} ways {tr.ways} line {1 << tr.log_block} B; sampled sets: s % {tr.modulus} == {tr.residue} '
              f'({len(tr.sampled_sets())} per cache); ROI records {tr.roi()}; complete {tr.complete}')
        b, _ = tr.roi()
        pre = sum(1 for x in tr.rec[2:b] if x[0] in (R.T_ACCESS, R.T_DECISION, R.T_FILL, R.T_INVAL))
        print(f'   cache records before the ROI {pre}' + ('' if not pre else ' (warm-up reached the traced cache: a shadow replays it from the '
                                                         'trace start, so check that the self-replay is exact)'))
        if tr.sidecar:
            print(f"   sidecar checks {tr.sidecar.get('checks')} l1 filter {tr.sidecar.get('l1_filter')}")
            ok_all &= all(v == 0 for v in tr.sidecar.get('checks', {}).values())
        st = R.load_stats(run)
        rows, ok = R.check_against_stats(tr, st)
        exact = tr.modulus == 1
        for core, k, got, want, eq in rows:
            if exact:
                print(f'   core {core} {k:22s} trace {got:10d}  sim.stats {want:10d}  {"equal" if eq else "DIFFERS"}')
            else:
                print(f'   core {core} {k:22s} trace (sampled sets) {got:10d}  sim.stats (all sets) {want:10d}  ratio {got / want if want else 0:.4f}')
        if exact:
            print(f'   RESULT {"EQUAL" if ok else "DIFFERS"}')
            ok_all &= ok
    print(f'RESULT {"PASS" if ok_all else "FAIL"}')
    return 0 if ok_all else 1


def print_summary(sh, rec_misses=None):
    x = sh.summary()
    keys = ('misses', 'loads', 'stores', 'load_misses', 'store_misses', 'store_misses_upgrade', 'decisions', 'evictions', 'dirty_evictions',
            'victim_same_pct', 'inclusion_victims', 'phantom_accesses', 'phantom_misses', 'suppressed_accesses', 'inv_modelled',
            'inv_deferred', 'inv_recorded_coherence', 'inv_recorded_back', 'downgrades_modelled', 'modelled_candidate_pct',
            'fills_at_access', 'l1_orphans', 'pending_unresolved', 'recorded_ineligible_decisions', 'fill_of_held_line')
    rel = f' ({pct(x["misses"], rec_misses):+.3f}% vs recorded)' if rec_misses else ''
    print(f'   {x["policy"]} window {x["window"]} inclusion-model {int(x["model_inclusion"])} coherence-model {int(x["model_coherence"])}{rel}')
    print('     ' + '  '.join(f'{k} {x[k]:.2f}' if isinstance(x[k], float) else f'{k} {x[k]}' for k in keys))


def cmd_replay(args):
    header(sys.argv, [args.run])
    tr = load(args.run)
    rec, _ = recorded_misses(tr)
    print(f'== {args.run}: recorded misses in the sampled sets (ROI) {rec}')
    fut = S.Future(tr) if any(p.startswith('opt') for p in args.policy) else None
    for p in args.policy:
        sh = S.replay(tr, p, window=args.window, future=fut, model_inclusion=not args.no_inclusion_model,
                      model_coherence=not args.no_coherence_model)
        print_summary(sh, rec)
    return 0


def cmd_labels(args):
    header(sys.argv, args.runs)
    for run in args.runs:
        tr = load(run)
        print(f'== {run} (window {args.window})')
        L.report(L.Labeller(tr, window=args.window).run(), lambda s: print('   ' + s))
    return 0


def cmd_calibrate(args):
    """Phase 5 calibration (docs/rl/HEADROOM.md): the shadow must reproduce each traced policy on its own trace exactly,
    and predict the other policy's L2 misses from each trace within the threshold."""
    header(sys.argv, [args.lru, args.prio])
    trL, trP = load(args.lru), load(args.prio)
    for a, b in (('cores', 'cores'), ('sets', 'sets'), ('ways', 'ways'), ('modulus', 'modulus'), ('residue', 'residue')):
        if getattr(trL, a) != getattr(trP, b):
            print(f'traces differ in {a}: cannot calibrate')
            return 2
    mL, talL = recorded_misses(trL)
    mP, talP = recorded_misses(trP)
    selfL = S.replay(trL, 'lru', window=args.window)
    selfP = S.replay(trP, 'priority', window=args.window)
    variants = (('full model', {}), ('no inclusion model', dict(model_inclusion=False)),
                ('no coherence model', dict(model_coherence=False)),
                ('neither', dict(model_inclusion=False, model_coherence=False)))
    crossPL = [(n, S.replay(trL, 'priority', window=args.window, **kw)) for n, kw in variants]
    crossLP = [(n, S.replay(trP, 'lru', window=args.window, **kw)) for n, kw in variants]

    print('== recorded runs (sampled sets, ROI)')
    print(f'   LRU      misses {mL}   all sets (sim.stats) {stats_misses(args.lru):.0f}')
    print(f'   priority misses {mP}   all sets (sim.stats) {stats_misses(args.prio):.0f}')
    meas_s = pct(mP, mL)
    meas_all = pct(stats_misses(args.prio), stats_misses(args.lru))
    refs = []
    for ref in args.reference or []:
        pol, run = ref.split('=', 1)
        refs.append((pol, run, stats_misses(run)))
    print(f'   measured priority vs LRU: sampled sets {meas_s:+.3f}%   all sets {meas_all:+.3f}%   (sampling difference {meas_s - meas_all:+.3f} points)')
    for pol, run, m in refs:
        print(f'   reference run {pol} {run}: all-set misses {m:.0f}')

    print('== 1. self-replays (must be exact: same misses and victims as recorded)')
    print_summary(selfL, mL)
    print_summary(selfP, mP)
    exact = (selfL.misses() == mL and selfP.misses() == mP and selfL.n['victim_differs'] == 0 and selfP.n['victim_differs'] == 0)
    print(f'   self-replays {"EXACT" if exact else "NOT EXACT"}')

    print('== 2. cross-replays (predicted priority vs LRU, in points of the LRU misses)')
    errs = {}
    for (n1, pl), (n2, lp) in zip(crossPL, crossLP):
        pred_from_L = pct(pl.misses(), mL)
        pred_from_P = pct(mP, lp.misses())
        errs[n1] = (pred_from_L - meas_s, pred_from_P - meas_s)
        print(f'   {n1:20s} from the LRU trace {pred_from_L:+.3f}% (error {pred_from_L - meas_s:+.3f})   from the priority trace '
              f'{pred_from_P:+.3f}% (error {pred_from_P - meas_s:+.3f})')
    print_summary(crossPL[0][1], mL)
    print_summary(crossLP[0][1], mP)

    print('== 3. mechanism check: predicted against recorded counts (sampled sets)')

    def recorded_counts(tr, self_sh):
        tal = R.trace_tallies(tr)
        return dict(accesses=sum(t['loads'] + t['stores'] for t in tal.values()),
                    upgrade_misses=sum(t['store_misses_upgrade'] for t in tal.values()),
                    decisions=sum(t['decisions'] for t in tal.values()),
                    coherence_invalidations=sum(t['inv_coherence'] for t in tal.values()),
                    inclusion_victims=self_sh.n['inclusion_victims'], dirty_evictions=self_sh.n['dirty_evictions'])

    def predicted_counts(sh):
        n = sh.n
        return dict(accesses=n['loads'] + n['stores'], upgrade_misses=n['store_misses_upgrade'], decisions=n['decisions'],
                    coherence_invalidations=n['inv_recorded_coherence'] + n['inv_modelled'] + n['inv_deferred'],
                    inclusion_victims=n['inclusion_victims'], dirty_evictions=n['dirty_evictions'])

    rL, rP = recorded_counts(trL, selfL), recorded_counts(trP, selfP)
    pPL, pLP = predicted_counts(crossPL[0][1]), predicted_counts(crossLP[0][1])
    print(f'   {"count":24s} {"LRU rec":>10s} {"prio pred":>10s} {"prio rec":>10s} {"error%":>8s} | {"LRU pred":>10s} {"error%":>8s}')
    for k in rL:
        print(f'   {k:24s} {rL[k]:10d} {pPL[k]:10d} {rP[k]:10d} {pct(pPL[k], rP[k]):+8.2f} | {pLP[k]:10d} {pct(pLP[k], rL[k]):+8.2f}')

    # Noise floor: two runs of one policy (multi-threaded runs differ); the measured difference between them is what a
    # perfect model could not predict either. Each null pair must be two traced runs of the same policy.
    noise = []
    print('== 4. noise floor (null pairs: two runs of one policy)')
    for spec in args.null or []:
        a, b = spec.split(':', 1)
        ta, tb = load(a), load(b)
        ma, mb = recorded_misses(ta)[0], recorded_misses(tb)[0]
        ns, na = pct(mb, ma), pct(stats_misses(b), stats_misses(a))
        noise.append(abs(ns))
        print(f'   {a} vs {b}: sampled sets {ns:+.3f}%  all sets {na:+.3f}%')
    thr = args.threshold
    eL, eP = errs['full model']
    represent = abs(meas_s - meas_all) <= thr
    if not noise:
        verdict = 'INCONCLUSIVE (no null pair: the noise floor is unknown)'
    elif max(noise) > thr / 2:
        verdict = f'INCONCLUSIVE (noise floor {max(noise):.3f} points > half the threshold: more sets or repetitions needed)'
    elif not exact or not represent or abs(eL) > thr or abs(eP) > thr:
        verdict = 'FAIL'
    else:
        verdict = 'PASS'
    print(f'== RESULT: self-replays {"exact" if exact else "NOT exact"}; prediction errors {eL:+.3f} / {eP:+.3f} points (threshold ±{thr}); '
          f'sampling difference {meas_s - meas_all:+.3f} points; noise floor {max(noise) if noise else float("nan"):.3f} points '
          f'-> CALIBRATION {verdict}')
    if verdict != 'PASS':
        print('   The shadow model must not be used to rank replacement preferences (docs/rl/HEADROOM.md, "If calibration fails").')
    return 0 if verdict == 'PASS' else 1


def cmd_orders(args):
    header(sys.argv, [args.run])
    tr = load(args.run)
    rec, _ = recorded_misses(tr)
    orders = list(itertools.permutations(S.CLASSES)) if args.all else S.additive_orders(tr.cores)
    print(f'== {args.run}: {len(orders)} fixed class orders ({"all" if args.all else "additive (expressible by rl_lru weights)"}), window {args.window}; '
          f'recorded misses {rec}')
    base = {p: S.replay(tr, p, window=args.window).misses() for p in ('lru', 'priority')}
    res = []
    for o in orders:
        sh = S.replay(tr, S.FixedOrder(o), window=args.window)
        res.append((sh.misses(), ','.join(o), sh.summary()['modelled_candidate_pct']))
    res.sort()
    print(f'   shadow lru {base["lru"]}  shadow priority {base["priority"]} ({pct(base["priority"], base["lru"]):+.3f}% vs lru)')
    for m, o, mod in res[:args.top]:
        print(f'   {m:10d} {pct(m, base["lru"]):+8.3f}% vs lru {pct(m, base["priority"]):+8.3f}% vs priority  modelled candidates {mod:.1f}%  {o}')
    print(f'   ... worst {res[-1][0]} ({pct(res[-1][0], base["lru"]):+.3f}% vs lru) {res[-1][1]}')
    return 0


def cmd_phase(args):
    header(sys.argv, [args.run])
    tr = load(args.run)
    runs = [(p, S.replay(tr, p, window=args.window, epoch_accesses=args.epoch)) for p in args.policy]
    k = min(len(sh.epoch_misses) for _, sh in runs)
    tot = {p: sum(sh.epoch_misses[:k]) for p, sh in runs}
    best = sum(min(sh.epoch_misses[e] for _, sh in runs) for e in range(k))
    wins = {p: sum(1 for e in range(k) if sh.epoch_misses[e] == min(x.epoch_misses[e] for _, x in runs)) for p, sh in runs}
    print(f'== {args.run}: {k} epochs of {args.epoch} counted accesses')
    for p, _ in runs:
        print(f'   {p:30s} misses {tot[p]:10d}  epochs best {wins[p]}')
    ref = min(tot.values())
    print(f'   per-epoch minimum {best} ({pct(best, ref):+.3f}% vs the best single policy). INDICATIVE ONLY: independent replays, '
          f'so the cache contents a switch would inherit are ignored; this is neither an upper nor a lower bound for an adaptive policy.')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('check')
    p.add_argument('runs', nargs='+')
    p = sub.add_parser('replay')
    p.add_argument('run')
    p.add_argument('--policy', action='append', required=True)
    p.add_argument('--window', type=int, default=4)
    p.add_argument('--no-inclusion-model', action='store_true')
    p.add_argument('--no-coherence-model', action='store_true')
    p = sub.add_parser('labels')
    p.add_argument('runs', nargs='+')
    p.add_argument('--window', type=int, default=4)
    p = sub.add_parser('calibrate')
    p.add_argument('--lru', required=True)
    p.add_argument('--prio', required=True)
    p.add_argument('--window', type=int, default=4)
    p.add_argument('--threshold', type=float, default=0.5, help='points of the priority-vs-LRU miss difference (default 0.5)')
    p.add_argument('--reference', action='append', help='POLICY=RUN: an untraced run whose all-set misses are listed for context')
    p.add_argument('--null', action='append', help='RUN_A:RUN_B: two traced runs of one policy (noise floor); required for a PASS')
    p = sub.add_parser('orders')
    p.add_argument('run')
    p.add_argument('--all', action='store_true', help='all 720 orders (default: the 60 additive ones)')
    p.add_argument('--window', type=int, default=4)
    p.add_argument('--top', type=int, default=10)
    p = sub.add_parser('phase')
    p.add_argument('run')
    p.add_argument('--policy', action='append', required=True)
    p.add_argument('--window', type=int, default=4)
    p.add_argument('--epoch', type=int, default=1 << 16)
    args = ap.parse_args(argv)
    return {'check': cmd_check, 'replay': cmd_replay, 'labels': cmd_labels, 'calibrate': cmd_calibrate, 'orders': cmd_orders,
            'phase': cmd_phase}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
