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

With model_permissions (docs/rl/HEADROOM.md 7.7; off by default, which is the model of the 2026-11-23 calibration),
the MESI state of a counterfactual copy follows Sniper's grants instead of the recorded copy whenever the two
histories differ:
  * a read fill is S when another core holds the line, else the L3's own state: M once any core has written the line
    since it entered the L3, E otherwise (cache_cntlr.cc copyDataFromNextLevel). A shadow fill at the recorded fill
    takes the recorded state unless the counterfactual and the recorded run differ in whether another core held the
    line; the L3's state is then inferred (any write makes it M, a back-invalidation clean, a recorded read fill
    without sharers shows it);
  * a shadow copy whose residency differs from the recorded copy's (the recorded cache refetched it, the shadow
    filled it at a recorded hit or a phantom, or its fill state differed) keeps its own state; a write by the core
    in both runs makes both copies M with hint 1 again;
  * only M is writable in an L1, and an L1 copy takes the L2's state when it is filled (cache_state.h,
    copyDataFromNextLevel): a recorded L1 write hit on a counterfactual copy that is not M is an L2 upgrade, and a
    recorded upgrade of a counterfactual copy that is M is a hit.

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
    # model_permissions only (docs/rl/HEADROOM.md 7.7)
    PERM_COUNTERS = ('fill_state_differs', 'recorded_refetch_kept', 'l3_state_inferred', 'l3_state_inferred_unknown',
                     'own_state_store_hits', 'l1_write_upgrades', 'resyncs', 'l1_write_hit_implied_m', 'l3_observed_agree',
                     'l3_observed_dirty_was_clean', 'l3_observed_clean_was_dirty', 'l3_observed_unknown_m', 'l3_observed_unknown_e')
    # must be 0 in a self-replay with model_permissions (the recorded run is its own counterfactual)
    PERM_SELF_ZERO = ('fill_state_differs', 'recorded_refetch_kept', 'l3_state_inferred', 'own_state_store_hits',
                      'l1_write_upgrades')

    def __init__(self, tr, policy, window=4, future=None, epoch_accesses=1 << 16, model_inclusion=True, model_coherence=True,
                 model_permissions=False, detail=False, l3_mode='history'):
        """model_inclusion / model_coherence = False switch off the phantom/suppressed accesses and the modelled
        invalidations and downgrades (sensitivity of a prediction to those rules; the recorded invalidations of copies
        both caches hold are always applied).
        model_permissions = True models the MESI state of a counterfactual copy from its own history (its fill state
        from the counterfactual sharers and the L3's state, as Sniper grants it) and the L1 write permission that
        state implies (docs/rl/HEADROOM.md 7.7). False is the model of the four-run calibration (section 7.6).
        l3_mode: how the L3 state of a counterfactual read fill without sharers is inferred: 'history' (default: dirty
        after any write, clean after a back-invalidation, as shown by the last recorded fill without sharers; clean
        without evidence), or 'dirty' / 'clean' for every such fill (sensitivity bounds; L3 evictions of lines no L2
        holds are not traced, so the history can be stale).
        detail = True tallies every store upgrade and own-state hit by core, origin and the copy's divergence
        (self.detail, self.ages)."""
        self.tr = tr
        self.model_inclusion = model_inclusion
        self.model_coherence = model_coherence
        self.perm = model_permissions
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
        # model_permissions state
        self.diverged = [set() for _ in range(P)]   # shadow copies the recorded cache also holds, with their own state
        self.check = [dict() for _ in range(P)]     # tag -> counterfactual sharers at the fill: state awaiting the recorded one
        self.l3_dirty = {}                          # tag -> the L3 copy is M (written since it entered the L3)
        if l3_mode not in ('history', 'dirty', 'clean'):
            raise ValueError(f'l3_mode {l3_mode}')
        self.l3_mode = l3_mode
        self.last_acc = [None] * P                  # tag of the core's last counted L2 access since its last L1 record
        self.rec_fill = [dict() for _ in range(P)]  # tag -> the recorded miss that the next fill serves was a write
        self.rec_fresh = [set() for _ in range(P)]  # recorded read fills whose state the next way record gives
        self.cause = [dict() for _ in range(P)]     # tag -> [why, record index] of a copy's divergence (detail)
        self.detail = Counter() if detail else None
        self.ages = Counter() if detail else None
        self.i = 0
        self.n = Counter()
        self.in_roi = False
        self.epoch_accesses = epoch_accesses
        self.epoch_misses = []  # shadow misses per epoch of counted recorded accesses (ROI only)
        self.epoch_recorded_misses = []
        self._acc_in_roi = 0

    # ------------------------------------------------------------------------------------------------ features
    def synced(self, c, tag):
        """Does the shadow copy share the recorded copy's history (so that the recorded state is its state)?"""
        return tag in self.mir[c] and not (self.perm and (tag in self.diverged[c] or tag in self.check[c]))

    def features(self, c, tag):
        if self.synced(c, tag):
            m = self.mir[c][tag]
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

    def note(self, key, c, tag):
        """detail: tally an event by the copy's divergence, and its age in records since the divergence."""
        if self.detail is None or not self.in_roi:
            return
        if self.synced(c, tag) or not self.holds(c, tag):
            copy = 'synced' if self.synced(c, tag) else 'not held'
            age = None
        else:
            why, at = self.cause[c].get(tag, ('unknown', self.i))
            copy = ('diverged: ' if tag in self.mir[c] else 'shadow only: ') + why
            age = self.i - at
        self.detail[key + (copy,)] += 1
        if age is not None:
            self.ages[(key[0], copy, len(str(age)))] += 1

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
        if self.perm:
            self.diverged[c].discard(tag)
            self.check[c].pop(tag, None)
            self.cause[c].pop(tag, None)

    def holds(self, c, tag):
        h = self.holders.get(tag)
        return bool(h) and c in h

    def diverge(self, c, tag, why):
        if tag not in self.diverged[c]:
            self.diverged[c].add(tag)
            self.cause[c][tag] = (why, self.i)

    def l3_state(self, tag):
        """The inferred state of the L3 copy (a read fill without sharers gets it)."""
        if self.l3_mode != 'history':
            return R.S_M if self.l3_mode == 'dirty' else R.S_E
        d = self.l3_dirty.get(tag)
        if d is None:
            self.count('l3_state_inferred_unknown')
        return R.S_M if d else R.S_E

    def resync(self, c, tag):
        """A write by this core, performed by both caches, leaves both copies M with hint 1 (cache_cntlr.cc: a write
        hit resets the hint to 1; an upgrade refill counts no other sharer), whatever their histories."""
        if tag in self.diverged[c] or tag in self.check[c]:
            self.count('resyncs')
        self.diverged[c].discard(tag)
        self.check[c].pop(tag, None)
        self.cause[c].pop(tag, None)

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

    def fill(self, c, tag, exclusive, index, at_access, why='shadow fill at a recorded hit'):
        if self.holds(c, tag):
            self.count('fill_of_held_line')  # cannot happen in the recorded order (expect 0)
            return
        s = tag & self.mask
        lines = self.sets.setdefault((c, s), [])
        if len(lines) >= self.W:
            window = lines if self.policy.whole_set else lines[:self.window]
            for t in window:
                self.count('candidates_observed' if self.synced(c, t) else 'candidates_modelled')
            victim = self.policy.choose(self, c, window, index)
            self.count('decisions')
            rv = self.rec_victim.pop((c, s), None)
            if rv is not None:
                self.count('victim_same' if rv == victim else 'victim_differs')
            self.evict(c, victim)
        others = self.holders.get(tag, set()) - {c}
        if not self.perm:
            state, hint = (R.S_M if exclusive else (R.S_S if others else R.S_E)), 1 + len(others)
        elif exclusive:
            state, hint = R.S_M, 1  # the next level invalidated every other copy before granting ownership
        elif others:
            state, hint = R.S_S, 1 + len(others)
        else:
            # no other holder: the L3's own state, M when its copy is dirty (copyDataFromNextLevel, MESI fill fix)
            state, hint = None, 1  # set below
        if state is None:
            if at_access:
                self.count('l3_state_inferred')
                state = self.l3_state(tag)
            else:
                # provisional until the recorded state resolves it; not counted as an inference here
                state = R.S_M if (self.l3_dirty.get(tag) if self.l3_mode == 'history' else self.l3_mode == 'dirty') else R.S_E
        lines.append(tag)
        self.feat[c][tag] = [state, hint]
        self.holders.setdefault(tag, set()).add(c)
        self.l1_lost[c].discard(tag)
        self.l1_kept[c].discard(tag)
        self.count('fills_at_access' if at_access else 'fills_at_recorded_fill')
        if self.perm:
            if at_access:
                self.diverge(c, tag, why)
            elif not exclusive:
                # resolved against the recorded state (resolve): whether the counterfactual and the recorded run had
                # another holder at this fill
                rec_others = any(o != c and tag in self.mir[o] for o in range(self.P))
                self.check[c][tag] = (bool(others), rec_others)

    def resolve(self, c, tag, rs):
        """The recorded state rs of a read fill the shadow made at the same time. Sniper grants S when another core
        holds the line, else the L3's own state (copyDataFromNextLevel). The shadow copy takes the recorded state
        unless the counterfactual differs from the recorded run in whether another core held the line."""
        cf_others, rec_others = self.check[c].pop(tag)
        f = self.feat[c][tag]
        if cf_others == rec_others:
            cf = rs     # same sharing condition: the recorded grant (and any change since) applies
        elif cf_others:
            cf = R.S_S  # a counterfactual sharer the recorded run did not have
        elif f[0] == R.S_S:
            cf = R.S_S  # downgraded since the fill by a modelled read miss of another core
        else:
            self.count('l3_state_inferred')  # recorded sharer, none in the counterfactual: the L3's state, inferred
            cf = self.l3_state(tag)
        f[0] = cf
        if cf != rs:
            self.count('fill_state_differs')
            self.diverge(c, tag, 'fill: counterfactual sharer' if cf == R.S_S else 'fill: no counterfactual sharer')

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
            if o != c and not self.synced(o, tag):
                f = self.feat[o][tag]
                if f[0] in (R.S_M, R.S_E):
                    f[0] = R.S_S
                    # a fill awaiting its recorded state is not counted: resolve decides whether the downgrade matters
                    if not (self.perm and tag in self.check[o] and tag in self.mir[o]):
                        self.count('downgrades_modelled')

    def lookup(self, c, tag, store, readex, rec_hit, index, phantom=False, l1_write=False, rec_pre=None):
        """phantom: an L2 access the counterfactual L1 makes because it lost the line to inclusion; l1_write: a store
        the recorded L1 performed (write hit) but the counterfactual L1 cannot (no write permission)."""
        exclusive = store or readex
        self.count('stores' if store else 'loads')
        if phantom:
            self.count('phantom_accesses')
        if l1_write:
            self.count('l1_write_upgrades')
        miss = False
        if self.holds(c, tag):
            st, _ = self.features(c, tag)
            if exclusive and st != R.S_M:
                miss = True  # upgrade: S/E (or I) to M
                self.count('store_misses' if store else 'load_misses')
                self.count('store_misses_upgrade' if store else 'load_misses_upgrade')
                if self.detail is not None:
                    origin = 'L1 write hit' if l1_write else ('phantom' if phantom else 'L2 store pre ' + R.STATE_NAMES[rec_pre])
                    self.note(('upgrade', c, origin, 'from ' + R.STATE_NAMES[st]), c, tag)
                self.foreign_exclusive(c, tag)
                self.feat[c][tag][0] = R.S_M  # used once the recorded cache no longer holds the line
                if self.perm:
                    self.feat[c][tag][1] = 1
            elif exclusive and (self.perm or tag not in self.mir[c]):
                if (self.perm and not phantom and not l1_write and rec_pre is not None and rec_pre != R.S_M
                        and tag in self.mir[c] and not self.synced(c, tag)):
                    # the recorded copy needed an upgrade, the counterfactual one is M: an L1 write hit or an L2 M hit
                    self.count('own_state_store_hits')
                    if self.detail is not None:
                        self.note(('own-state hit', c, 'L2 store pre ' + R.STATE_NAMES[rec_pre], ''), c, tag)
                self.feat[c][tag][0] = R.S_M
                if self.perm:
                    self.feat[c][tag][1] = 1
            self.touch(c, tag)
            if self.perm and exclusive and not phantom and not l1_write and tag in self.mir[c]:
                self.resync(c, tag)
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
                self.fill(c, tag, exclusive, index, at_access=True,
                          why='phantom fill (inclusion)' if phantom else 'shadow fill at a recorded hit')
                if self.perm and exclusive and not phantom and not l1_write and tag in self.mir[c]:
                    self.resync(c, tag)
            else:
                self.pending[c][tag] = exclusive
        if miss and self.in_roi and self.epoch_misses:
            self.epoch_misses[-1] += 1

    def mirror_loss(self, c, tag, evicted):
        was_synced = self.synced(c, tag)
        self.mir_pos[c].pop(tag, None)
        m = self.mir[c].pop(tag, None)
        if m is None or not self.holds(c, tag):
            return
        if tag in self.remote_pending[c]:
            self.invalidate(c, tag, 'inv_deferred')
        elif evicted and m[2] != 0 and self.model_inclusion:
            self.l1_kept[c].add(tag)  # the recorded L1 lost it to the recorded eviction; the counterfactual L1 did not
        if self.holds(c, tag):
            if not self.perm:
                self.feat[c][tag] = [m[0], m[1]]  # last known features, now modelled
            else:
                if was_synced:
                    self.feat[c][tag] = [m[0], m[1]]
                self.check[c].pop(tag, None)  # an unresolved fill keeps its modelled state
                self.diverge(c, tag, 'recorded copy dropped, shadow kept')

    # ------------------------------------------------------------------------------------------------ replay
    def run(self, start=0, stop=None):
        rec = self.tr.rec
        stop = len(rec) if stop is None else stop
        perm = self.perm
        evicted = None  # recorded decision waiting for its fill record
        for i, (t, c, a, b, aux, tag) in enumerate(rec.iter(start, stop), start):
            self.i = i
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
                if perm:
                    self.last_acc[c] = tag
                    if not a & R.ACC_HIT:
                        self.rec_fill[c][tag] = store or readex
                    if store or readex:
                        self.l3_dirty[tag] = True  # any write leaves the L3 copy M
                self.l1_lost[c].discard(tag)
                if tag in self.l1_kept[c] and self.holds(c, tag):
                    st, _ = self.features(c, tag)
                    if not (store or readex) or st == R.S_M:
                        self.count('suppressed_accesses')
                        self.l1_kept[c].discard(tag)
                        if store or readex:
                            self.foreign_exclusive(c, tag)
                            if perm and tag in self.mir[c]:
                                m = self.mir[c][tag]
                                self.mir[c][tag] = (R.S_M, m[1], m[2])
                        continue
                self.l1_kept[c].discard(tag)
                self.lookup(c, tag, store, readex, bool(a & R.ACC_HIT), i, rec_pre=(a >> R.ACC_PRE_SHIFT) & 3)
                if perm and (store or readex) and tag in self.mir[c]:
                    m = self.mir[c][tag]
                    self.mir[c][tag] = (R.S_M, m[1], m[2])  # a write leaves the recorded copy M (the next way record confirms)
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
                    if perm:
                        if tag in self.rec_fresh[c]:
                            # a recorded read fill without a recorded sharer reveals the L3's state
                            self.rec_fresh[c].discard(tag)
                            st = b & 3
                            if st in (R.S_M, R.S_E):
                                d = self.l3_dirty.get(tag)
                                if d is None:
                                    self.count('l3_observed_unknown_m' if st == R.S_M else 'l3_observed_unknown_e')
                                else:
                                    self.count('l3_observed_agree' if d == (st == R.S_M) else
                                               ('l3_observed_dirty_was_clean' if st == R.S_M else 'l3_observed_clean_was_dirty'))
                                self.l3_dirty[tag] = st == R.S_M
                        if tag in self.check[c] and self.holds(c, tag):
                            self.resolve(c, tag, b & 3)
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
                rx = self.rec_fill[c].pop(tag, None) if perm else None
                if p is not None:
                    self.fill(c, tag, p, i, at_access=False)
                elif perm and self.holds(c, tag) and not b & 1 and not rx:
                    # the recorded cache refetches (a read) a line the shadow kept: the shadow copy keeps its own state
                    self.count('recorded_refetch_kept')
                    self.diverge(c, tag, 'recorded refetch, shadow kept')
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
                    self.mir[c][tag] = (R.S_M if rx else R.S_E, 0, 0)
                    if perm and not rx and not b & 1:
                        self.rec_fresh[c].add(tag)
                self.mir_pos[c][tag] = a
                self.mir_way.setdefault((c, aux), [None] * self.W)[a] = tag
            elif t == R.T_INVAL:
                if b == R.INV_UPGRADE:
                    self.upgrading[c].add(tag)  # this core's own upgrade: its access already did it; the refill follows
                    continue
                if perm and b == R.INV_BACK:
                    self.l3_dirty[tag] = False  # the L3 evicted the line: a later fetch comes clean from memory
                if self.holds(c, tag):
                    self.invalidate(c, tag, 'inv_recorded_back' if b == R.INV_BACK else 'inv_recorded_coherence')
                self.mirror_loss(c, tag, evicted=False)
                mw = self.mir_way.get((c, aux))
                if mw is not None and mw[a] == tag:
                    mw[a] = None
            elif t == R.T_L1:
                store = bool(b & R.L1_STORE)
                if perm:
                    completion = self.last_acc[c] == tag
                    self.last_acc[c] = None
                    if store:
                        self.l3_dirty[tag] = True
                        if a and not completion:
                            # a recorded L1-D write hit: the recorded L1 copy, hence the recorded L2 copy, is M
                            m = self.mir[c].get(tag)
                            if m is not None and m[0] != R.S_M:
                                self.count('l1_write_hit_implied_m')
                                self.mir[c][tag] = (R.S_M, m[1], m[2])
                            if tag in self.check[c] and self.holds(c, tag):
                                self.resolve(c, tag, R.S_M)
                            if tag not in self.l1_lost[c] and self.holds(c, tag) and self.features(c, tag)[0] != R.S_M:
                                # the counterfactual L1 copy came from a non-M L2 copy: no write permission, so the
                                # store goes to the L2 as an upgrade
                                self.lookup(c, tag, True, False, True, i, l1_write=True)
                if store:
                    self.foreign_exclusive(c, tag)
                    if self.holds(c, tag) and not self.synced(c, tag):
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
        counters = self.COUNTERS + (self.PERM_COUNTERS if self.perm else ())
        return dict(policy=self.policy.name, window=self.window, misses=self.misses(),
                    model_inclusion=self.model_inclusion, model_coherence=self.model_coherence,
                    model_permissions=self.perm,
                    **{k: n[k] for k in counters},
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
