"""Shadow replay of the traced L2 caches under another replacement policy (docs/rl/HEADROOM.md, "Shadow model").

The shadow keeps, for every traced (core, set), its own contents and LRU order, and replays the recorded reference
stream (counted L2 accesses, plus the L1 accesses the trace kept for inclusion and coherence). It is a model, not the
simulator: the reference stream, the interleaving and the timing are those of the recorded run, and the coherence and
L1-inclusion effects of contents that differ from the recorded run are modelled with explicit, counted rules:

  * features: a shadow line also held by the recorded cache takes the recorded state and hint (exact); a line only
    the shadow holds keeps its last known features, updated by the modelled coherence rules below;
  * invalidation: a write or exclusive read by another core (recorded access or L1 store) invalidates a copy only the
    shadow holds at once; a copy both hold is invalidated by the recorded invalidation (exact timing), or when the
    recorded cache drops it after such a write;
  * downgrade: a shadow read miss by another core downgrades a shadow-only M/E copy to S;
  * fills: a shadow miss that the recorded run also missed is filled at the recorded fill (same timing); one the
    recorded run hit is filled at the access; the fill state is M for a write or exclusive read, else E without other
    shadow holders and S with them; the hint is 1 + other shadow holders;
  * L1 inclusion: evicting a line that the (counterfactual) L1 holds removes it from that L1; the next recorded L1 use
    of it then becomes a "phantom" L2 access. A recorded L2 access caused by the recorded run's own inclusion victim,
    of a line the shadow kept, is "suppressed" (the counterfactual L1 still holds it);
  * L3 back-invalidations are applied as recorded (exogenous).

Every rule application is counted, so a replay reports how much of its result rests on modelled behaviour.
"""

import bisect
from collections import Counter

import rltrace as R

INF = float('inf')

# Class of a block (state, sharing): S/E/M, BT (hint <= cores / 2) or AT (hint > cores / 2), as rl_lru (rl_features.h)
STATE_CODE = {R.S_S: 0, R.S_E: 1, R.S_M: 2, R.S_I: 1}  # I (no information) ranks like E, as priority_rank
CLASSES = ('S-BT', 'S-AT', 'E-BT', 'E-AT', 'M-BT', 'M-AT')


def class_of(state, hint, cores):
    return 2 * STATE_CODE[state] + (1 if hint > cores // 2 else 0)


def priority_rank(cls):
    """rl_lru Selection::priority (rl_reward_policy.h priority_rank): S/AT 5 > S/BT 4 > E/AT 3 > E/BT 2 > M/AT 1 > M/BT 0."""
    state, above = divmod(cls, 2)
    return 2 * (2 - state) + above


# ---------------------------------------------------------------------------------------------------------- policies

class Policy:
    """choose(model, core, window, index) -> victim; window: the shadow lines, oldest first (the N oldest)."""
    name = 'policy'
    oracle = False
    whole_set = False  # chooses among every way, not only the window

    def choose(self, model, core, window, index):
        raise NotImplementedError


class LRU(Policy):
    name = 'lru'

    def choose(self, model, core, window, index):
        return window[0]


class Ranked(Policy):
    """Highest rank among the window, ties to the older line (as rl_lru)."""

    def __init__(self, rank_of_class, name):
        self.rank = rank_of_class
        self.name = name

    def choose(self, model, core, window, index):
        best, best_r = window[0], self.rank[model.class_of(core, window[0])]
        for t in window[1:]:
            r = self.rank[model.class_of(core, t)]
            if r > best_r:
                best, best_r = t, r
        return best


def Priority():
    return Ranked([priority_rank(c) for c in range(6)], 'priority')


def FixedOrder(order):
    """order: the six class names, evicted first to last, e.g. 'S-AT,S-BT,E-AT,E-BT,M-AT,M-BT' (= priority)."""
    names = order.split(',') if isinstance(order, str) else list(order)
    if sorted(names) != sorted(CLASSES):
        raise ValueError(f'order must list each of {CLASSES} once: {order}')
    rank = [0] * 6
    for pos, n in enumerate(names):
        rank[CLASSES.index(n)] = 5 - pos
    return Ranked(rank, 'order:' + ','.join(names))


class OptWindow(Policy):
    """Diagnostic oracle (uses future information): the line of the window (whole_set: of the set) whose next use by
    this core is farthest, a line written by another core before that use counting as never used again; ties to the
    older line. The next uses are those of the recorded stream."""
    oracle = True

    def __init__(self, whole_set=False):
        self.whole_set = whole_set
        self.name = 'opt-set' if whole_set else 'opt-window'

    def choose(self, model, core, window, index):
        best, best_u = window[0], model.future.effective_next_use(core, window[0], index)
        for t in window[1:]:
            u = model.future.effective_next_use(core, t, index)
            if u > best_u:
                best, best_u = t, u
        return best


def make_policy(spec):
    if spec == 'lru':
        return LRU()
    if spec == 'priority':
        return Priority()
    if spec == 'opt-window':
        return OptWindow()
    if spec == 'opt-set':
        return OptWindow(whole_set=True)
    if spec.startswith('order:'):
        return FixedOrder(spec[len('order:'):])
    raise ValueError(f'unknown policy {spec}')


# ------------------------------------------------------------------------------------------------- future (oracle)

class Future:
    """Next-use index of a trace, for the oracle policies and the window labels (diagnostic: future information).

    use of (core, line): a counted access of that core to the line, or a recorded L1 access of that core (the trace's
    epoch rule keeps the first L1 use after every access or fill in the set). write of a line: a recorded write or
    exclusive read (access or L1 store), with its core."""

    def __init__(self, tr):
        self.uses = {}
        self.writes = {}
        for i, (t, c, a, b, aux, tag) in enumerate(tr.rec):
            if t == R.T_ACCESS:
                if a & R.ACC_PREFETCH:
                    continue
                self.uses.setdefault((c, tag), []).append(i)
                if a & (R.ACC_STORE | R.ACC_READEX):
                    self.writes.setdefault(tag, []).append((i, c))
            elif t == R.T_L1:
                self.uses.setdefault((c, tag), []).append(i)
                if b & R.L1_STORE:
                    self.writes.setdefault(tag, []).append((i, c))

    def next_use(self, core, tag, index):
        u = self.uses.get((core, tag))
        if not u:
            return INF
        j = bisect.bisect_right(u, index)
        return u[j] if j < len(u) else INF

    def next_foreign_write(self, core, tag, index, before=INF):
        w = self.writes.get(tag)
        if not w:
            return INF
        j = bisect.bisect_right(w, (index, 1 << 30))
        while j < len(w) and w[j][0] < before:
            if w[j][1] != core:
                return w[j][0]
            j += 1
        return INF

    def effective_next_use(self, core, tag, index):
        u = self.next_use(core, tag, index)
        if u == INF:
            return INF
        return INF if self.next_foreign_write(core, tag, index, u) < u else u


# ----------------------------------------------------------------------------------------------------------- model

class Shadow:
    COUNTERS = ('loads', 'stores', 'load_misses', 'store_misses', 'store_misses_upgrade', 'load_misses_upgrade',
                'decisions', 'evictions', 'dirty_evictions', 'victim_same', 'victim_differs', 'phantom_accesses',
                'phantom_misses', 'suppressed_accesses', 'inclusion_victims', 'inv_modelled', 'inv_deferred',
                'inv_recorded_coherence', 'inv_recorded_back', 'downgrades_modelled', 'candidates_observed',
                'candidates_modelled', 'fills_at_access', 'fills_at_recorded_fill', 'l1_orphans', 'pending_unresolved',
                'recorded_ineligible_decisions', 'fill_of_held_line')

    def __init__(self, tr, policy, window=4, future=None, epoch_accesses=1 << 16, model_inclusion=True, model_coherence=True):
        """model_inclusion / model_coherence = False switch off the phantom/suppressed accesses and the modelled
        invalidations and downgrades (sensitivity of a prediction to those rules; the recorded invalidations of copies
        both caches hold are always applied)."""
        self.tr = tr
        self.model_inclusion = model_inclusion
        self.model_coherence = model_coherence
        self.policy = policy
        self.window = window
        self.future = future
        if policy.oracle and future is None:
            raise ValueError(f'{policy.name} needs a Future index')
        P, W = tr.cores, tr.ways
        self.P, self.W, self.mask = P, W, tr.sets - 1
        self.sets = {}                          # (c, s) -> [tags], oldest first
        self.feat = [dict() for _ in range(P)]  # c -> tag -> [state, hint]: modelled features of shadow lines
        self.holders = {}                       # tag -> set of cores whose shadow holds it
        self.mir = [dict() for _ in range(P)]   # c -> tag -> (state, hint, l1_mask): recorded contents
        self.mir_way = {}                       # (c, s) -> [tag or None] * W
        self.mir_pos = [dict() for _ in range(P)]  # c -> tag -> way it occupies in the recorded cache
        self.pending = [dict() for _ in range(P)]
        self.l1_lost = [set() for _ in range(P)]
        self.l1_kept = [set() for _ in range(P)]
        self.remote_pending = [set() for _ in range(P)]
        self.upgrading = [set() for _ in range(P)]  # recorded upgrade in progress (invalidated, refill to come)
        self.rec_victim = {}
        self.n = Counter()
        self.in_roi = False
        self.epoch_accesses = epoch_accesses
        self.epoch_misses = []  # shadow misses per epoch of counted recorded accesses (ROI only)
        self.epoch_recorded_misses = []
        self._acc_in_roi = 0

    # ------------------------------------------------------------------------------------------------ features
    def features(self, c, tag):
        m = self.mir[c].get(tag)
        if m is not None:
            return m[0], m[1]
        f = self.feat[c][tag]
        return f[0], f[1]

    def class_of(self, c, tag):
        st, hint = self.features(c, tag)
        return class_of(st, hint, self.P)

    def l1_holds(self, c, tag):
        """Does the counterfactual L1 of core c hold the line?"""
        if tag in self.l1_lost[c]:
            return False
        m = self.mir[c].get(tag)
        if m is not None:
            return m[2] != 0
        return tag in self.l1_kept[c]

    def count(self, k, v=1):
        if self.in_roi:
            self.n[k] += v

    # ------------------------------------------------------------------------------------------------ shadow ops
    def _remove(self, c, tag):
        s = tag & self.mask
        self.sets[(c, s)].remove(tag)
        h = self.holders.get(tag)
        if h:
            h.discard(c)
        self.feat[c].pop(tag, None)
        self.l1_kept[c].discard(tag)
        self.remote_pending[c].discard(tag)

    def holds(self, c, tag):
        h = self.holders.get(tag)
        return bool(h) and c in h

    def evict(self, c, tag):
        st, _ = self.features(c, tag)
        self.count('evictions')
        if st == R.S_M:
            self.count('dirty_evictions')
        if self.l1_holds(c, tag):
            self.count('inclusion_victims')
            if self.model_inclusion:
                self.l1_lost[c].add(tag)
        self._remove(c, tag)

    def invalidate(self, c, tag, kind):
        self.count(kind)
        self._remove(c, tag)
        self.l1_lost[c].discard(tag)  # the counterfactual L1 copy is invalidated too: its next use is a real miss

    def fill(self, c, tag, exclusive, index, at_access):
        if self.holds(c, tag):
            self.count('fill_of_held_line')  # cannot happen in the recorded order (expect 0)
            return
        s = tag & self.mask
        lines = self.sets.setdefault((c, s), [])
        if len(lines) >= self.W:
            window = lines if self.policy.whole_set else lines[:self.window]
            for t in window:
                self.count('candidates_observed' if t in self.mir[c] else 'candidates_modelled')
            victim = self.policy.choose(self, c, window, index)
            self.count('decisions')
            rv = self.rec_victim.pop((c, s), None)
            if rv is not None:
                self.count('victim_same' if rv == victim else 'victim_differs')
            self.evict(c, victim)
        others = self.holders.get(tag, set()) - {c}
        state = R.S_M if exclusive else (R.S_S if others else R.S_E)
        lines.append(tag)
        self.feat[c][tag] = [state, 1 + len(others)]
        self.holders.setdefault(tag, set()).add(c)
        self.l1_lost[c].discard(tag)
        self.l1_kept[c].discard(tag)
        self.count('fills_at_access' if at_access else 'fills_at_recorded_fill')

    def touch(self, c, tag):
        lines = self.sets[(c, tag & self.mask)]
        lines.remove(tag)
        lines.append(tag)

    def foreign_exclusive(self, c, tag):
        """Core c takes exclusive ownership of the line: other cores' copies cannot survive it."""
        h = self.holders.get(tag)
        if not h or not self.model_coherence:
            return
        for o in list(h):
            if o == c:
                continue
            if tag in self.mir[o]:
                self.remote_pending[o].add(tag)  # the recorded invalidation will come (exact timing)
            else:
                self.invalidate(o, tag, 'inv_modelled')

    def foreign_read_miss(self, c, tag):
        h = self.holders.get(tag)
        if not h or not self.model_coherence:
            return
        for o in h:
            if o != c and tag not in self.mir[o]:
                f = self.feat[o][tag]
                if f[0] in (R.S_M, R.S_E):
                    f[0] = R.S_S
                    self.count('downgrades_modelled')

    def lookup(self, c, tag, store, readex, rec_hit, index, phantom=False):
        exclusive = store or readex
        self.count('stores' if store else 'loads')
        if phantom:
            self.count('phantom_accesses')
        miss = False
        if self.holds(c, tag):
            st, _ = self.features(c, tag)
            if exclusive and st != R.S_M:
                miss = True  # upgrade: S/E (or I) to M
                self.count('store_misses' if store else 'load_misses')
                self.count('store_misses_upgrade' if store else 'load_misses_upgrade')
                self.foreign_exclusive(c, tag)
                self.feat[c][tag][0] = R.S_M  # used once the recorded cache no longer holds the line
            elif exclusive and tag not in self.mir[c]:
                self.feat[c][tag][0] = R.S_M
            self.touch(c, tag)
        else:
            miss = True
            self.count('store_misses' if store else 'load_misses')
            if phantom:
                self.count('phantom_misses')
            if exclusive:
                self.foreign_exclusive(c, tag)
            else:
                self.foreign_read_miss(c, tag)
            if rec_hit or phantom:
                self.fill(c, tag, exclusive, index, at_access=True)
            else:
                self.pending[c][tag] = exclusive
        if miss and self.in_roi and self.epoch_misses:
            self.epoch_misses[-1] += 1

    def mirror_loss(self, c, tag, evicted):
        self.mir_pos[c].pop(tag, None)
        m = self.mir[c].pop(tag, None)
        if m is None or not self.holds(c, tag):
            return
        if tag in self.remote_pending[c]:
            self.invalidate(c, tag, 'inv_deferred')
        elif evicted and m[2] != 0 and self.model_inclusion:
            self.l1_kept[c].add(tag)  # the recorded L1 lost it to the recorded eviction; the counterfactual L1 did not
        if self.holds(c, tag):
            self.feat[c][tag] = [m[0], m[1]]  # last known features, now modelled

    # ------------------------------------------------------------------------------------------------ replay
    def run(self, start=0, stop=None):
        rec = self.tr.rec
        stop = len(rec) if stop is None else stop
        evicted = None  # recorded decision waiting for its fill record
        for i in range(start, stop):
            t, c, a, b, aux, tag = rec[i]
            if t == R.T_ACCESS:
                if a & R.ACC_PREFETCH:
                    continue
                if self.in_roi:
                    if self._acc_in_roi % self.epoch_accesses == 0:
                        self.epoch_misses.append(0)
                        self.epoch_recorded_misses.append(0)
                    self._acc_in_roi += 1
                    if not a & R.ACC_HIT:
                        self.epoch_recorded_misses[-1] += 1
                store, readex = bool(a & R.ACC_STORE), bool(a & R.ACC_READEX)
                self.l1_lost[c].discard(tag)
                if tag in self.l1_kept[c] and self.holds(c, tag):
                    st, _ = self.features(c, tag)
                    if not (store or readex) or st == R.S_M:
                        self.count('suppressed_accesses')
                        self.l1_kept[c].discard(tag)
                        if store or readex:
                            self.foreign_exclusive(c, tag)
                        continue
                self.l1_kept[c].discard(tag)
                self.lookup(c, tag, store, readex, bool(a & R.ACC_HIT), i)
            elif t == R.T_WAY:
                s = aux >> 16
                mw = self.mir_way.setdefault((c, s), [None] * self.W)
                old = mw[a]
                valid = bool(b & 4)
                # the way's previous line left without a decision or invalidation record of it only if it is not
                # somewhere else now (an upgrade refill can put a line back into another way)
                if old is not None and (not valid or old != tag) and self.mir_pos[c].get(old) == a:
                    if old in self.upgrading[c]:
                        self.mir_pos[c].pop(old, None)  # dropped for its own upgrade: comes back with the refill
                    else:
                        self.mirror_loss(c, old, evicted=False)
                if valid:
                    self.mir[c][tag] = (b & 3, aux & 0xFF, (aux >> 8) & 0xFF)
                    self.mir_pos[c][tag] = a
                    mw[a] = tag
                else:
                    mw[a] = None
            elif t == R.T_DECISION:
                # The recorded eviction takes effect after the fill record that follows it (the simulator writes the
                # two together), so that a shadow decision at that fill sees the same recorded contents
                if b != (1 << self.W) - 1:
                    self.count('recorded_ineligible_decisions')
                self.rec_victim[(c, tag & self.mask)] = tag
                evicted = (c, tag, a)
            elif t == R.T_FILL:
                p = self.pending[c].pop(tag, None)
                if p is not None:
                    self.fill(c, tag, p, i, at_access=False)
                self.rec_victim.pop((c, aux), None)
                if evicted is not None:
                    ec, etag, eway = evicted
                    evicted = None
                    self.mirror_loss(ec, etag, evicted=True)
                    mw = self.mir_way.get((ec, etag & self.mask))
                    if mw is not None and mw[eway] == etag:
                        mw[eway] = None
                # The recorded cache holds the line from now on; its state is set after the fill and arrives with the
                # next way record of the set, before any later decision there (presence matters at once, for other
                # cores' writes)
                self.upgrading[c].discard(tag)
                if tag not in self.mir[c]:
                    self.mir[c][tag] = (R.S_E, 0, 0)
                self.mir_pos[c][tag] = a
                self.mir_way.setdefault((c, aux), [None] * self.W)[a] = tag
            elif t == R.T_INVAL:
                if b == R.INV_UPGRADE:
                    self.upgrading[c].add(tag)  # this core's own upgrade: its access already did it; the refill follows
                    continue
                if self.holds(c, tag):
                    self.invalidate(c, tag, 'inv_recorded_back' if b == R.INV_BACK else 'inv_recorded_coherence')
                self.mirror_loss(c, tag, evicted=False)
                mw = self.mir_way.get((c, aux))
                if mw is not None and mw[a] == tag:
                    mw[a] = None
            elif t == R.T_L1:
                store = bool(b & R.L1_STORE)
                if store:
                    self.foreign_exclusive(c, tag)
                    if self.holds(c, tag) and tag not in self.mir[c]:
                        self.feat[c][tag][0] = R.S_M
                if tag in self.l1_lost[c]:
                    self.l1_lost[c].discard(tag)
                    self.lookup(c, tag, store, False, True, i, phantom=True)
                elif not self.holds(c, tag) and tag not in self.mir[c]:
                    self.count('l1_orphans')
            elif t == R.T_ROI:
                self.in_roi = bool(a)
        self.n['pending_unresolved'] = sum(len(p) for p in self.pending)
        return self

    # ------------------------------------------------------------------------------------------------ results
    def misses(self):
        return self.n['load_misses'] + self.n['store_misses']

    def summary(self):
        n = self.n
        dec = n['decisions'] or 1
        cand = (n['candidates_observed'] + n['candidates_modelled']) or 1
        return dict(policy=self.policy.name, window=self.window, misses=self.misses(),
                    model_inclusion=self.model_inclusion, model_coherence=self.model_coherence,
                    **{k: n[k] for k in self.COUNTERS},
                    victim_same_pct=100.0 * n['victim_same'] / ((n['victim_same'] + n['victim_differs']) or 1),
                    modelled_candidate_pct=100.0 * n['candidates_modelled'] / cand,
                    decisions_pct=100.0 * n['decisions'] / dec)


def replay(tr, policy, window=4, future=None, **kw):
    if isinstance(policy, str):
        policy = make_policy(policy)
    return Shadow(tr, policy, window=window, future=future, **kw).run()


def additive_orders(cores=4):
    """The class orders an additive score w_state + w_sharing can produce without ties (the orders rl_lru's weights
    can express), found by enumerating small integer weights."""
    found = set()
    for ws in range(6):
        for we in range(6):
            for wm in range(6):
                for wat in range(-6, 7):
                    score = {'S-BT': ws, 'S-AT': ws + wat, 'E-BT': we, 'E-AT': we + wat, 'M-BT': wm, 'M-AT': wm + wat}
                    if len(set(score.values())) == 6:
                        found.add(tuple(sorted(CLASSES, key=lambda k: -score[k])))
    return sorted(found)
