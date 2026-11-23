#!/usr/bin/env python3
"""Tests of the offline headroom tools on small synthetic traces whose answers are worked out by hand.

    python3 -I experiments/rl/headroom/test_headroom.py

TraceBuilder writes records in the order the simulator's recorder does (rl_trace.cc): way records of a set before
each access, insert and invalidation of it; a decision record immediately followed by its fill; an upgrade as
access (miss) + upgrade invalidation + upgrade refill; another core's write invalidating the recorded copies.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import labels as L  # noqa: E402
import rltrace as R  # noqa: E402
import shadow as S  # noqa: E402


class TraceBuilder:
    """A recorded run: per (core, set) an LRU cache (or a chosen victim), emitting trace records."""

    def __init__(self, cores=1, sets=1, ways=2, modulus=1):
        self.cores, self.sets, self.ways = cores, sets, ways
        self.rec = [(R.T_HEADER, 0, cores, ways, R.SCHEMA_VERSION, R.MAGIC),
                    (R.T_HEADER, 1, 6, 0, sets, (modulus << 32) | 0)]
        self.lines = {}    # (c, s) -> [None | dict(tag, state, hint, l1)] per way
        self.order = {}    # (c, s) -> ways, MRU first
        self.emitted = {}  # (c, s, way) -> content tuple last written

    def _set(self, c, s):
        if (c, s) not in self.lines:
            self.lines[(c, s)] = [None] * self.ways
            self.order[(c, s)] = []
        return self.lines[(c, s)], self.order[(c, s)]

    def _content(self, x):
        return (x['tag'], True, x['state'], x['hint'], x['l1']) if x else (R.NO_TAG, False, R.S_I, 0, 0)

    def sync(self, c, s):
        lines, _ = self._set(c, s)
        for w in range(self.ways):
            cur = self._content(lines[w])
            if self.emitted.get((c, s, w), (R.NO_TAG, False, R.S_I, 0, 0)) != cur:
                self.emitted[(c, s, w)] = cur
                tag, valid, st, hint, l1 = cur
                self.rec.append((R.T_WAY, c, w, st | (4 if valid else 0), hint | (l1 << 8) | (s << 16), tag))

    def find(self, c, tag):
        lines, _ = self._set(c, tag % self.sets)
        for w, x in enumerate(lines):
            if x and x['tag'] == tag:
                return w
        return None

    def line(self, c, tag):
        w = self.find(c, tag)
        return None if w is None else self.lines[(c, tag % self.sets)][w]

    def touch(self, c, s, w):
        _, order = self._set(c, s)
        if w in order:
            order.remove(w)
        order.insert(0, w)

    def ranks(self, c, s):
        _, order = self._set(c, s)
        r = [0] * self.ways
        for pos, w in enumerate(order):
            r[w] = pos
        return r

    def roi(self, begin=True):
        self.rec.append((R.T_ROI, 0, 1 if begin else 0, 0, 0, 0))

    def invalidate(self, c, tag, reason):
        s = tag % self.sets
        w = self.find(c, tag)
        self.sync(c, s)
        self.rec.append((R.T_INVAL, c, w, reason, s, tag))
        self.lines[(c, s)][w] = None
        self.order[(c, s)].remove(w)

    def access(self, c, tag, op='r', state=None, hint=0, l1=0, victim=None, l1i=False):
        """op: 'r' read, 'w' write, 'x' exclusive read. state/hint/l1: the recorded line after a fill.
        victim: the recorded victim tag at a full set (default: LRU)."""
        s = tag % self.sets
        lines, order = self._set(c, s)
        self.sync(c, s)
        w = self.find(c, tag)
        x = lines[w] if w is not None else None
        exclusive = op in 'wx'
        hit = x is not None and (not exclusive or x['state'] == R.S_M)
        pre = x['state'] if x else R.S_I
        flags = ((R.ACC_HIT if hit else 0) | (R.ACC_STORE if op == 'w' else 0) | (R.ACC_READEX if op == 'x' else 0)
                 | (pre << R.ACC_PRE_SHIFT) | (R.ACC_FROM_L1I if l1i else 0))
        self.rec.append((R.T_ACCESS, c, flags, w if hit else R.NO_WAY, s, tag))
        if hit:
            self.touch(c, s, w)
            if l1:
                x['l1'] = l1
            return
        # the protocol: a write takes ownership (other copies invalidated), a read miss downgrades M/E elsewhere
        for o in range(self.cores):
            if o == c or self.find(o, tag) is None:
                continue
            if exclusive:
                self.invalidate(o, tag, R.INV_COHERENCE)
            elif x is None:
                y = self.line(o, tag)
                if y['state'] in (R.S_M, R.S_E):
                    y['state'] = R.S_S
        if x is not None:  # upgrade: S/E -> M through invalidate + refill
            self.invalidate(c, tag, R.INV_UPGRADE)
            kind = R.FILL_UPGRADE
        else:
            kind = R.FILL_DEMAND
        self.sync(c, s)  # the insert's view of the set
        free = [i for i in range(self.ways) if lines[i] is None]
        if free:
            way, evicted = free[0], False
        else:
            if victim is None:
                way = order[-1]
            else:
                way = self.find(c, victim)
            r = self.ranks(c, s)
            packed = sum((r[i] & 7) << (3 * i) for i in range(self.ways))
            self.rec.append((R.T_DECISION, c, way, (1 << self.ways) - 1, packed | (kind << 24), lines[way]['tag']))
            order.remove(way)
            evicted = True
        self.rec.append((R.T_FILL, c, way, kind | (2 if evicted else 0), s, tag))
        others = sum(1 for o in range(self.cores) if o != c and self.find(o, tag) is not None)
        st = R.S_M if exclusive else (state if state is not None else (R.S_S if others else R.S_E))
        lines[way] = dict(tag=tag, state=st, hint=hint, l1=l1)
        self.touch(c, s, way)

    def set_l1(self, c, tag, mask):
        self.line(c, tag)['l1'] = mask

    def l1(self, c, tag, store=False, l1d=True):
        s = tag % self.sets
        self.rec.append((R.T_L1, c, 1 if l1d else 0, (R.L1_STORE if store else 0) | R.L1_EPOCH_FIRST, s, tag))

    def trace(self):
        self.roi(False)
        self.rec.append((R.T_END, 0, 0, 0, 0, len(self.rec)))
        return R.Trace(data=R.encode(self.rec))


A, B, C, D = 0x10, 0x20, 0x30, 0x40  # with sets = 1 every line is in set 0


def lru_run(seq, **kw):
    b = TraceBuilder(**kw)
    b.roi()
    for t in seq:
        b.access(0, t)
    return b.trace()


class ReaderTests(unittest.TestCase):
    def test_header_geometry_and_completeness(self):
        tr = lru_run([A, B])
        self.assertEqual((tr.cores, tr.sets, tr.ways, tr.modulus, tr.residue, tr.log_block), (1, 1, 2, 1, 0, 6))
        self.assertTrue(tr.complete)
        self.assertEqual(tr.roi(), (2, len(tr) - 2))

    def test_bad_magic_and_truncation(self):
        with self.assertRaises(R.TraceError):
            R.Trace(data=R.encode([(R.T_HEADER, 0, 1, 2, 1, 1234), (R.T_HEADER, 1, 6, 0, 1, 1 << 32), (R.T_END, 0, 0, 0, 0, 2)]))
        with self.assertRaises(R.TraceError):
            R.Trace(data=R.encode([(R.T_HEADER, 0, 1, 2, 1, R.MAGIC)])[:20])
        b = TraceBuilder()
        b.access(0, A)
        tr = R.Trace(data=R.encode(b.rec))  # no end record
        self.assertFalse(tr.complete)

    def test_tallies_and_statistics_check(self):
        b = TraceBuilder()
        b.roi()
        b.access(0, A)            # read miss
        b.access(0, A, 'w')       # write to E: upgrade miss (store-misses-E)
        b.access(0, A)            # hit
        b.access(0, B, 'w')       # write miss
        tr = b.trace()
        t = R.trace_tallies(tr)[0]
        self.assertEqual((t['loads'], t['stores'], t['load_misses'], t['store_misses'], t['store_misses_upgrade']), (2, 2, 1, 2, 1))
        self.assertEqual((t['fill_demand'], t['fill_upgrade'], t['inv_upgrade'], t['decisions']), (2, 1, 1, 0))
        stats = {'L2.loads': [2], 'L2.stores': [2], 'L2.load-misses': [1], 'L2.store-misses': [2], 'L2.store-misses-S': [0],
                 'L2.store-misses-E': [1], 'L2.rl-event-fill-demand': [2], 'L2.rl-event-fill-upgrade-refill': [1],
                 'L2.rl-event-eviction': [0], 'L2.rl-event-inv-upgrade': [1], 'L2.rl-event-inv-coherence': [0]}
        rows, ok = R.check_against_stats(tr, stats)
        self.assertTrue(ok, rows)
        stats['L2.load-misses'] = [2]
        self.assertFalse(R.check_against_stats(tr, stats)[1])

    def test_a_store_hit_overlapping_a_fill_is_a_statistics_miss_not_a_contents_miss(self):
        b = TraceBuilder()
        b.roi()
        b.access(0, A, 'w')
        b.sync(0, 0)  # the recorder writes the set's changed ways before every access
        b.rec.append((R.T_ACCESS, 0, R.ACC_HIT | R.ACC_STORE | R.ACC_OVERLAP | (R.S_M << R.ACC_PRE_SHIFT), 0, 0, A))
        tr = b.trace()
        t = R.trace_tallies(tr)[0]
        self.assertEqual((t['stores'], t['store_misses'], t['store_overlap_misses'], t['contents_misses']), (2, 2, 1, 1))
        self.assertEqual(S.replay(tr, 'lru', window=2).misses(), 1)


class ShadowLRUTests(unittest.TestCase):
    def test_cyclic_pattern_larger_than_the_set_always_misses(self):
        # 3 lines cycling through a 2-way LRU set: every access misses; decisions from the third access on
        tr = lru_run([A, B, C] * 3)
        sh = S.replay(tr, 'lru', window=2)
        self.assertEqual(sh.misses(), 9)
        self.assertEqual(sh.n['decisions'], 7)
        self.assertEqual(sh.n['victim_same'], 7)
        self.assertEqual(sh.n['victim_differs'], 0)

    def test_reuse_within_the_set_hits(self):
        tr = lru_run([A, B, A, B, C, A])  # misses A B C; at C, B is LRU... A B A B -> order MRU B, A -> C evicts A; A misses
        sh = S.replay(tr, 'lru', window=2)
        self.assertEqual(sh.misses(), 4)
        self.assertEqual(sh.n['loads'], 6)

    def test_self_replay_equals_recorded_counts(self):
        seq = [A, B, C, A, D, B, A, C, C, D, A, B]
        tr = lru_run(seq, ways=3)
        sh = S.replay(tr, 'lru', window=3)
        t = R.trace_tallies(tr)[0]
        self.assertEqual(sh.misses(), t['contents_misses'])
        self.assertEqual(sh.n['decisions'], t['decisions'])
        self.assertEqual(sh.n['victim_differs'], 0)
        self.assertEqual(sh.n['candidates_modelled'], 0)  # every candidate's features are the recorded ones

    def test_upgrade_is_a_store_miss_without_a_decision(self):
        b = TraceBuilder()
        b.roi()
        b.access(0, A)
        b.access(0, B)
        b.access(0, A, 'w')  # A in E: upgrade (invalidate + refill), no replacement decision
        tr = b.trace()
        sh = S.replay(tr, 'lru', window=2)
        self.assertEqual((sh.misses(), sh.n['store_misses_upgrade'], sh.n['decisions']), (3, 1, 0))
        sh = S.replay(tr, 'lru', window=2)
        self.assertEqual(sh.n['pending_unresolved'], 0)


class ShadowPolicyTests(unittest.TestCase):
    def test_priority_keeps_the_modified_line_lru_evicts(self):
        # B written (M), A read as S, C read (E) into 2 ways: LRU evicts B, priority evicts the S line A.
        # Then B again: a recorded miss, a shadow hit. Recorded 4 misses, priority 3.
        b = TraceBuilder()
        b.roi()
        b.access(0, B, 'w')
        b.access(0, A, state=R.S_S)
        b.access(0, C)
        b.access(0, B)
        tr = b.trace()
        self.assertEqual(S.replay(tr, 'lru', window=2).misses(), 4)
        sh = S.replay(tr, 'priority', window=2)
        self.assertEqual(sh.misses(), 3)
        self.assertEqual(sh.n['decisions'], 1)
        self.assertEqual(sh.n['victim_differs'], 1)
        # the same order written out is the same policy
        sh2 = S.replay(tr, S.FixedOrder('S-AT,S-BT,E-AT,E-BT,M-AT,M-BT'), window=2)
        self.assertEqual(sh2.misses(), 3)

    def test_belady_on_a_cyclic_pattern(self):
        # A B C x3 in 2 ways: Belady misses A B C, B, A, C = 6 (worked out by hand); LRU 9
        tr = lru_run([A, B, C] * 3)
        fut = S.Future(tr)
        self.assertEqual(S.replay(tr, 'opt-window', window=2, future=fut).misses(), 6)
        self.assertEqual(S.replay(tr, 'opt-set', window=1, future=fut).misses(), 6)  # whole set, whatever the window
        self.assertEqual(S.replay(tr, 'opt-window', window=1, future=fut).misses(), 9)  # a 1-line window is LRU

    def test_additive_orders(self):
        orders = S.additive_orders()
        self.assertEqual(len(orders), 60)
        self.assertIn(('S-AT', 'S-BT', 'E-AT', 'E-BT', 'M-AT', 'M-BT'), orders)
        self.assertNotIn(('S-AT', 'E-BT', 'E-AT', 'S-BT', 'M-AT', 'M-BT'), orders)  # sharing order flips between states

    def test_priority_rank_matches_rl_lru(self):
        self.assertEqual([S.priority_rank(c) for c in range(6)], [4, 5, 2, 3, 0, 1])
        self.assertEqual(S.class_of(R.S_S, 3, 4), 1)  # hint 3 > 4 / 2: above the threshold
        self.assertEqual(S.class_of(R.S_S, 2, 4), 0)


class ShadowCoherenceTests(unittest.TestCase):
    def build(self):
        # core 0: A read as S, B written (M), C read: LRU evicts A; the shadow order "evict M first" evicts B and keeps
        # A, which the recorded cache no longer holds. Core 1 then writes A; core 0 reads A again.
        b = TraceBuilder(cores=2)
        b.roi()
        b.access(0, A, state=R.S_S)
        b.access(0, B, 'w')
        b.access(0, C)
        b.access(1, A, 'w')
        b.access(0, A)
        return b.trace()

    def test_another_cores_write_invalidates_a_shadow_only_copy(self):
        tr = self.build()
        order = S.FixedOrder('M-AT,M-BT,E-AT,E-BT,S-AT,S-BT')
        sh = S.replay(tr, order, window=2)
        self.assertEqual(sh.n['inv_modelled'], 1)
        self.assertEqual(sh.misses(), 5)  # core 0: A B C A, core 1: A
        off = S.replay(tr, order, window=2, model_coherence=False)
        self.assertEqual(off.n['inv_modelled'], 0)
        self.assertEqual(off.misses(), 4)  # core 0 would (wrongly) hit its stale copy of A

    def test_recorded_invalidation_of_a_copy_both_hold_is_applied_at_its_time(self):
        b = TraceBuilder(cores=2)
        b.roi()
        b.access(0, A)
        b.access(1, A, 'w')  # the recorded protocol invalidates core 0's copy
        b.access(0, A)
        tr = b.trace()
        self.assertEqual(R.trace_tallies(tr)[0]['inv_coherence'], 1)
        sh = S.replay(tr, 'lru', window=2)
        self.assertEqual((sh.n['inv_recorded_coherence'], sh.n['inv_modelled'], sh.misses()), (1, 0, 3))

    def test_read_miss_elsewhere_downgrades_a_shadow_only_copy(self):
        b = TraceBuilder(cores=2)
        b.roi()
        b.access(0, A, 'w')  # M
        b.access(0, B)
        b.access(0, C)       # LRU evicts A; "evict E first" keeps A (M) and evicts B
        b.access(1, A)       # core 1 read miss: core 0's shadow-only copy goes M -> S
        tr = b.trace()
        sh = S.replay(tr, S.FixedOrder('E-AT,E-BT,S-AT,S-BT,M-AT,M-BT'), window=2)
        self.assertEqual(sh.n['downgrades_modelled'], 1)
        self.assertEqual(sh.feat[0][A][0], R.S_S)


class ShadowInclusionTests(unittest.TestCase):
    def test_phantom_and_suppressed_accesses(self):
        # A (S, in the L1), B (E, in the L1), C: LRU evicts A, an inclusion victim of the recorded run; "evict E first"
        # evicts B instead, which the counterfactual L1 then loses. The next recorded L1 use of B becomes a phantom L2
        # access (a miss); the recorded re-access of A (an L1 miss caused by the recorded eviction) is suppressed,
        # since the counterfactual L1 still holds A.
        b = TraceBuilder()
        b.roi()
        b.access(0, A, state=R.S_S, l1=1)
        b.access(0, B, l1=1)
        b.access(0, C)
        b.l1(0, B)
        b.access(0, A)
        tr = b.trace()
        order = S.FixedOrder('E-AT,E-BT,M-AT,M-BT,S-AT,S-BT')
        sh = S.replay(tr, order, window=2)
        self.assertEqual(sh.n['inclusion_victims'], 1)  # B at C's fill (the phantom fill of B then evicts C, not in an L1)
        self.assertEqual(sh.n['phantom_accesses'], 1)
        self.assertEqual(sh.n['phantom_misses'], 1)
        self.assertEqual(sh.n['suppressed_accesses'], 1)
        self.assertEqual(sh.misses(), 4)  # A B C, phantom B; A suppressed
        lru = S.replay(tr, 'lru', window=2)
        # LRU (the recorded policy) evicts A at C and B at A's return, both held by the L1: the recorded inclusion victims
        self.assertEqual((lru.n['inclusion_victims'], lru.n['phantom_accesses'], lru.n['suppressed_accesses'], lru.misses()), (2, 0, 0, 4))
        off = S.replay(tr, order, window=2, model_inclusion=False)
        self.assertEqual((off.n['phantom_accesses'], off.n['suppressed_accesses']), (0, 0))


class LabelTests(unittest.TestCase):
    def test_lru_evicting_the_next_used_line_disagrees(self):
        # A B C A in 2 ways: at C, LRU evicts A, used next; B is never used again: the window oracle evicts B
        res = L.Labeller(lru_run([A, B, C, A]), window=2).run()
        n = res['counts']
        self.assertEqual(n['decisions'], 2)  # C evicts A; A evicts B
        # decision 1 disagrees (A is used next), decision 2 (A's return, B and C never used again) is a tie: agrees
        self.assertEqual(n['agree_window'], 1)
        self.assertEqual(n['pair_wrong'], 1)
        self.assertEqual(n['pair_tie'], 1)
        self.assertEqual(n['victim_dead'], 1)

    def test_lru_evicting_a_dead_line_agrees(self):
        res = L.Labeller(lru_run([A, B, C, B]), window=2).run()
        n = res['counts']
        self.assertEqual((n['decisions'], n['agree_window'], n['pair_right'], n['victim_dead']), (1, 1, 1, 1))

    def test_a_line_written_by_another_core_first_counts_as_never_used(self):
        b = TraceBuilder(cores=2)
        b.roi()
        b.access(0, A)
        b.access(0, B)
        b.access(0, C)       # LRU evicts A
        b.access(1, B, 'w')  # B's copy at core 0 is invalidated before core 0 uses it again
        b.access(0, B)
        b.access(0, A)
        tr = b.trace()
        fut = S.Future(tr)
        i = next(k for k, r in enumerate(tr.rec) if r[0] == R.T_DECISION)
        self.assertEqual(fut.effective_next_use(0, B, i), S.INF)
        self.assertLess(fut.next_use(0, B, i), S.INF)
        self.assertLess(fut.effective_next_use(0, A, i), S.INF)
        n = L.Labeller(tr, window=2, future=fut).run()['counts']
        # decision 1 (C evicts A) disagrees: the oracle would have evicted B, invalidated before its next use; decision 2
        # (A's return evicts C; B and C never used again) agrees
        self.assertEqual((n['decisions'], n['agree_window'], n['pair_wrong'], n['pair_tie']), (2, 1, 1, 1))

    def test_distances_are_counted_in_accesses_to_the_set(self):
        tr = lru_run([A, B, C, B, B, A])
        lab = L.Labeller(tr, window=2)
        i = next(k for k, r in enumerate(tr.rec) if r[0] == R.T_DECISION)
        use = lab.future.next_use(0, A, i)
        self.assertEqual(lab.distance(0, 0, i, use), 2)  # B, B between the decision and A's return
        self.assertIsNone(lab.distance(0, 0, i, S.INF))


if __name__ == '__main__':
    unittest.main(verbosity=2)
