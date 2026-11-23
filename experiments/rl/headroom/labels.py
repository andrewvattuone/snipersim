"""Window labels of the recorded decisions (docs/rl/HEADROOM.md, concept A). Diagnostic: uses future information.

For every recorded replacement decision (in the ROI) the candidates are the N oldest eligible ways, from the ranks
and eligibility the decision record carries; their features are the recorded way contents just before it. Each
candidate's effective next use is its next use by the deciding core (a counted access, or a recorded L1 access), or
"never" if another core writes the line first (the copy would have been invalidated anyway) or there is no further
use in the trace. The window oracle evicts the candidate used farthest in the future; the decision agrees if the
recorded victim is one of the farthest (ties allowed).

These labels are evaluated on the recorded reference stream, which depends on the recorded policy: they say which
candidate the future of this run favoured, not how many misses another choice would have saved. Agreement rates are
not miss reductions (see the shadow model for the latter).
"""

import bisect
from collections import Counter, defaultdict

import rltrace as R
import shadow as S

DIST_BUCKETS = ((0, 7), (8, 15), (16, 31), (32, 63), (64, 255), (256, None))


def bucket(d):
    if d is None:
        return 'never'
    for lo, hi in DIST_BUCKETS:
        if hi is None or d <= hi:
            return f'{lo}+' if hi is None else f'{lo}-{hi}'
    return 'never'


BUCKET_NAMES = [f'{lo}+' if hi is None else f'{lo}-{hi}' for lo, hi in DIST_BUCKETS] + ['never']


class Labeller:
    def __init__(self, tr, future=None, window=4):
        self.tr = tr
        self.window = window
        self.future = future or S.Future(tr)
        self.mask = tr.sets - 1
        # counted accesses of each (core, set), for distances in set accesses
        self.acc = defaultdict(list)
        for i, (t, c, a, b, aux, tag) in enumerate(tr.rec):
            if t == R.T_ACCESS and not a & R.ACC_PREFETCH:
                self.acc[(c, aux)].append(i)

    def distance(self, c, s, i, use):
        """Counted accesses of core c to set s strictly between record i and record `use` (None = never used)."""
        if use == S.INF:
            return None
        a = self.acc[(c, s)]
        return bisect.bisect_left(a, use) - bisect.bisect_right(a, i)

    def run(self):
        tr, W, P = self.tr, self.tr.ways, self.tr.cores
        b0, e0 = tr.roi()
        ways = {}  # (c, s) -> [(tag, state, hint, l1)] from way records
        n = Counter()
        by_victim = defaultdict(Counter)   # victim class -> counters
        by_cand = defaultdict(Counter)     # candidate class -> counters
        dist_victim, dist_best, dist_protected = Counter(), Counter(), Counter()
        for i, (t, c, a, b, aux, tag) in enumerate(tr.rec):
            if t == R.T_WAY:
                ws = ways.setdefault((c, aux >> 16), [None] * W)
                ws[a] = (tag, b & 3, aux & 0xFF, (aux >> 8) & 0xFF) if b & 4 else None
                continue
            if t != R.T_DECISION or not (b0 <= i < e0):
                continue
            s = tag & self.mask
            ws = ways.get((c, s))
            ranks = [(aux >> (3 * w)) & 7 for w in range(W)]
            elig = [w for w in range(W) if b >> w & 1 and ws and ws[w] is not None]
            if not ws or ws[a] is None or ws[a][0] != tag:
                n['victim_not_in_mirror'] += 1  # expect 0
                continue
            n['decisions'] += 1
            order = sorted(elig, key=lambda w: -ranks[w])  # oldest first
            cands = order[:self.window]
            if a not in cands:
                n['victim_outside_window'] += 1
            info = {}
            for w in order:
                ctag, st, hint, l1 = ws[w]
                eff = self.future.effective_next_use(c, ctag, i)
                info[w] = (eff, S.class_of(st, hint, P), l1 != 0, self.distance(c, s, i, eff))
            veff, vcls, vl1, vdist = info[a]
            best = max(info[w][0] for w in cands)
            best_set = max(info[w][0] for w in order)
            agree = veff == best
            n['agree_window'] += agree
            n['agree_set'] += veff == best_set
            n['window_has_dead'] += best == S.INF
            n['victim_dead'] += veff == S.INF
            n['unique_best'] += sum(1 for w in cands if info[w][0] == best) == 1
            n['victim_l1'] += vl1
            bv = by_victim[S.CLASSES[vcls]]
            bv['n'] += 1
            bv['agree'] += agree
            bv['dead'] += veff == S.INF
            bv['l1'] += vl1
            dist_victim[bucket(vdist)] += 1
            dist_best[bucket(info[[w for w in cands if info[w][0] == best][0]][3])] += 1
            for w in cands:
                eff, cls, l1, dist = info[w]
                bc = by_cand[S.CLASSES[cls]]
                bc['in_window'] += 1
                bc['chosen'] += w == a
                bc['oracle'] += eff == best
                bc['dead'] += eff == S.INF
                if w == a:
                    continue
                # pairwise: was evicting the victim rather than this kept candidate right?
                if veff > eff:
                    n['pair_right'] += 1
                elif veff < eff:
                    n['pair_wrong'] += 1
                    bc['kept_wrongly'] += 1
                else:
                    n['pair_tie'] += 1
                # protection: a candidate older than the victim was passed over
                if ranks[w] > ranks[a]:
                    n['protected'] += 1
                    bc['protected'] += 1
                    if eff < veff:
                        n['protected_used_before_victim'] += 1
                        bc['protected_used_before_victim'] += 1
                    if eff == S.INF:
                        n['protected_dead'] += 1
                        bc['protected_dead'] += 1
                    dist_protected[bucket(dist)] += 1
        return dict(counts=n, by_victim=by_victim, by_candidate=by_cand, dist_victim=dist_victim, dist_best=dist_best,
                    dist_protected=dist_protected)


def report(res, out):
    n = res['counts']
    d = n['decisions'] or 1
    pct = lambda k, base=d: 100.0 * n[k] / (base or 1)
    out(f"decisions {n['decisions']} (victim outside the window {n['victim_outside_window']}, not in mirror {n['victim_not_in_mirror']})")
    out(f"agreement with the window oracle {pct('agree_window'):.2f}%  (unique farthest candidate in {pct('unique_best'):.2f}% of decisions; "
        f"window has a never-used-again line in {pct('window_has_dead'):.2f}%)")
    out(f"agreement with the whole-set oracle {pct('agree_set'):.2f}%")
    out(f"victim never used again {pct('victim_dead'):.2f}%   victim held by an L1 {pct('victim_l1'):.2f}%")
    pairs = n['pair_right'] + n['pair_wrong'] + n['pair_tie']
    out(f"pairs victim vs kept candidate {pairs}: right {pct('pair_right', pairs):.2f}%  wrong {pct('pair_wrong', pairs):.2f}%  tie {pct('pair_tie', pairs):.2f}%")
    pr = n['protected']
    out(f"passed-over older candidates {pr}: used before the victim {pct('protected_used_before_victim', pr):.2f}%  never used again "
        f"{pct('protected_dead', pr):.2f}%")
    out('victim class      n   agree%   never-used%   L1%')
    for k in S.CLASSES:
        v = res['by_victim'].get(k)
        if v:
            out(f"  {k:5s} {v['n']:9d} {100 * v['agree'] / v['n']:7.2f} {100 * v['dead'] / v['n']:12.2f} {100 * v['l1'] / v['n']:6.2f}")
    out('candidate class  in-window  chosen%  oracle%  never-used%  kept-wrongly  passed-over  ...used-before-victim%  ...never-used%')
    for k in S.CLASSES:
        v = res['by_candidate'].get(k)
        if v:
            w = v['in_window']
            p = v['protected'] or 1
            out(f"  {k:5s} {w:12d} {100 * v['chosen'] / w:8.2f} {100 * v['oracle'] / w:8.2f} {100 * v['dead'] / w:12.2f} {v['kept_wrongly']:13d} "
                f"{v['protected']:12d} {100 * v['protected_used_before_victim'] / p:21.2f} {100 * v['protected_dead'] / p:14.2f}")
    for name, key in (('victim', 'dist_victim'), ('oracle choice', 'dist_best'), ('passed-over', 'dist_protected')):
        h = res[key]
        tot = sum(h.values()) or 1
        out(f"next use of the {name} (set accesses): " + '  '.join(f"{b} {100 * h[b] / tot:.1f}%" for b in BUCKET_NAMES))
