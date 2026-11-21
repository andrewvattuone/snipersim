# RL replacement cores

Stage 1 of the Sniper port: the two replacement algorithms as simulator-independent, header-only C++ cores with
deterministic unit tests. Stage 2: Sniper reports classified cache events to its cache sets through no-op hooks
(see [Sniper integration](#sniper-integration)). **No RL policy is registered yet** (Stage 3); `CacheSetLRU`, the MESI
protocol and the sharer-hint code are unchanged. The audit that motivates every decision here is
[`ALGORITHM_AUDIT.md`](ALGORITHM_AUDIT.md) (issue numbers I-n refer to it).

| Algorithm | Core | Reference implementation (ChampSim, `mesi-implementation` `148b9c3`) |
|---|---|---|
| Souza–Freitas reproduction | `rl_paper_policy.h` (`rlrepl::PaperPolicy`) | `replacement/souza_freitas/sf_core.h` |
| Our modified reward-learning algorithm | `rl_reward_policy.h` (`rlrepl::RewardPolicy`) | `replacement/rl_mesi/rl_core.h` |

## Files

Cores, in `common/core/memory_subsystem/cache/rl/` (header-only, so the CMake glob over `common/*.cc` compiles nothing
new into Sniper; names are prefixed because every directory under `common/` is on the include path):

| File | Contents |
|---|---|
| `rl_features.h` | `Mesi`, `Features {state, above}`, fixed threshold, `make_features`, `Mode` (off/state/bitset/both) |
| `rl_events.h` | `InvalidationReason` (coherence, back_invalidation, upgrade), `FillKind` (demand, upgrade_refill), and the event classification the Sniper adapter must follow |
| `rl_eviction_history.h` | `EvictionHistory`: per-set history shared by both cores |
| `rl_lru_order.h` | `LruOrder`: candidate generation with Sniper `CacheSetLRU`'s exact rank semantics |
| `rl_paper_policy.h` | `PaperPolicy`: ratio weights, rule text/example, AND/OR, next attempt |
| `rl_reward_policy.h` | `RewardPolicy`: `WeightTable`, EMA rewards, highest-score-among-N, protection feedback |

Tests, in `testing/unit/rl/` (outside `common/`, never part of the Sniper build):

| File | Contents |
|---|---|
| `rl_test.h` | Minimal test harness with Catch2-style macros (Sniper has no test framework) |
| `rl_set_model.h` | Tiny cache-set model that drives a core in the order the Stage 2 adapter will |
| `test_common.cc` | Threshold, features, history, LRU order |
| `test_paper_policy.cc` | Paper core (ports ChampSim test 446) |
| `test_reward_policy.cc` | Reward core (ports ChampSim test 447) |
| `test_scenarios.cc` | End-to-end deterministic scenarios |
| `test_event_order.cc` | Regression tests for the order of events on a miss (history lookup before the eviction) |
| `Makefile`, `.gitignore` | Build/run; the test binary is ignored |

## Running the tests

```
make -C testing/unit/rl test          # build with -std=c++20 -Wall -Wextra -Wcast-align -Wshadow -Wconversion -Werror, run
testing/unit/rl/rl_unit_tests paper    # only test cases whose name contains "paper"
make -C testing/unit/rl clean
```

Expected: `63 test cases, 0 failed, 643 assertions`.

## Architecture

```
          base policy                  selection                       learning
   LruOrder::candidates(n)  ──►  PaperPolicy::choose (next attempt) ◄── ratio weights from EvictionHistory (per set)
   (oldest first, eligible       RewardPolicy::choose (best of N)   ◄── WeightTable (global or per set), EMA rewards
    ways only)                          │
                                        ▼
   features: make_features(state, delivered sharer count, cores)  ─►  Features {Mesi, above}
```

* **Candidate generation** is a list of ways in the base policy's preference order. Both selectors consume only that
  list, so another base policy just has to produce one. `LruOrder` reproduces `CacheSetLRU` (ranks initialised to the
  way index, `moveToMRU`), and takes an eligibility predicate for `SHARED_UPGRADING` ways.
* **Selection** and **learning** are separate per algorithm: the paper and the modified algorithm share only the
  history, features and LRU order. Their weights mean different things and are deliberately not unified.
* **Every core call is deterministic.** There is no random number generator (exploration is not ported).
* **choose / commit split.** `choose()` is `const` and changes nothing; `commit()` applies the decision. In Sniper both
  happen inside `getReplacementIndex`, but the split keeps decisions testable and matches ChampSim.

### Event lifecycle of a miss

```
on_miss(set, tag)                                   history lookup for the incoming block: remove + (-1)
[ choose -> commit -> on_eviction(victim) ]         only if no invalid way; victim enters the history (+1 on age-out)
on_fill(...)                                        metadata of the installed block
```

The incoming block's history lookup must come **before** the victim is chosen and recorded. This is ChampSim's order:
`CACHE::try_hit` calls `update_replacement_state` on every tag check, misses included (`src/cache.cc:398`), and the
victim is chosen later in `handle_fill` (`find_victim` at `:241`, `replacement_cache_fill` at `:291`).
`rl_mesi::on_access` applies the −1 there; `souza_freitas::update_replacement_state` removes the entry there and again
at the fill, before inserting the victim.

Recording the victim first is wrong when the history is full:

* With history `[A, B, C, D]` and a miss on B that evicts X, the victim's insertion ages out A (a spurious +1) before
  B's entry is removed. The result is `[C, D, X]` instead of `[A, C, D, X]`.
* If the returning block is itself the oldest entry, it is aged out with a +1 and its −1 is lost.
* Both cores would also decide with the returning entry still present: stale ratio weights in the paper core, and the
  −1 not yet applied in the modified core.

The Stage 1 version had this defect; `test_event_order.cc` reproduces it (6 of its 7 ordering tests fail against
that version) and guards the fix.

## Paper reproduction core (`PaperPolicy`)

Parameters (`PaperConfig`) and defaults:

| Parameter | Values | Default | Note |
|---|---|---|---|
| `mode` | off, state, bitset, both | both | RL-State / RL-Bitset / RL-Both; off = base policy. ChampSim's `SF_MODE` default was `state` |
| `rule` | text, example | text | Both first-class (audit §2.2, I-10) |
| `combine` | And, Or | And | RL-Both only. And = ChampSim; Or under `example` = Figure 2 read literally (I-9) |

Fixed: history of 2 × associativity entries per set; threshold `count > cores / 2`; LRU base; a rejected candidate
keeps its LRU position (no promotion, I-11).

**Weights** (`compute_ratio_weights`), recomputed from the set's own history at every decision, so they are **per set**:
`wM, wE, wS` = fraction of history entries in each state (over entries with a known state), `wAT, wBT` = fraction above
/ below the threshold. When there is no distribution yet the initial bias applies under either rule: the state check
evicts only E, the sharing check only below-threshold blocks.

**Decision** (`choose`): candidate #1 is checked once.

* State check: the candidate's state is "highest" if its weight equals the maximum (ties count as highest).
* Sharing check: its class is "highest" if its weight is ≥ the other class's.
* `text` evicts a highest element and keeps the others; `example` does the opposite.
* RL-Both combines the two verdicts with And or Or.
* An I state (no information) accepts.
* A rejected candidate gets its second chance and candidate #2 is evicted **without any check**.
* With fewer than two candidates, or mode off, candidate #1 is evicted.

**Rewards.** The paper's −1/+1 change nothing directly, and no update rule is invented. On a miss (`on_miss`, before
the victim for that fill is chosen):

* if the tag is in the history: the entry is removed (which is what changes later ratios, including the ones used for
  this miss's own decision) and −1 is counted;
* otherwise +1 is counted.

Rewards are statistics only (`PaperStats`). `on_fill` normally finds nothing; an entry found there (a fill not preceded
by `on_miss`) is removed silently and counted in `refill_removals`, which should stay 0 in Sniper.

## Modified reward-learning core (`RewardPolicy`)

Parameters (`RewardConfig`) and defaults (approved Stage 1 settings, audit §6):

| Parameter | Values | Default |
|---|---|---|
| `mode` | off, state, bitset, both | both |
| `window` (N) | ≥ 1 (clamped to the candidate count) | 4 |
| `alpha` | [0, 1] | 0.1 |
| `protect_feedback` | true, false | true |
| `scope` | global, per_set | global |

Not ported, therefore off: Iprev, adaptive threshold, BRRIP set dueling, ε-exploration. No pinning guard.

**Weights.** Five principal weights: `wS, wE, wM` (state group) and `wBT, wAT` (sharing group). They start at 0. With
`scope = global` (default, as ChampSim) one table is shared by every set of the cache; `per_set` gives each set its
own table (the paper's scope; implemented because it is one index, not yet evaluated). A weight estimates "how good is
it to evict a block with this feature": positive = evict more, negative = keep.

**Score.** `V(b) = wM*xM + wE*xE + wS*xS + wAT*xAT + wBT*xBT`. The features are one-hot per group, so this is the
state weight plus the sharing weight. In mode `state` / `bitset` only one group counts, and a block in state I
contributes no state weight.

**Selection** (`choose`). Among the first N candidates (LRU: the N oldest), the highest score is evicted. Scanning
oldest first with a strict `>` sends ties to the older candidate, so with all weights equal (initially zero) the
policy is exactly LRU. Candidates older than the victim are "passed over"; their recency is **not** changed (I-1).

**Rewards and attribution.** Every reward updates only the features in the snapshot saved when the decision was
taken, never the block's current features:

| Event | Core call | Reward | Features updated |
|---|---|---|---|
| Evicted block misses again while its entry is still in the history | `on_miss` (before the victim is chosen) | −1 | saved in the history entry (at eviction) |
| History entry pushed out by a later eviction | `on_eviction` | +1 | the aged-out entry's |
| Protected (passed-over) block hit | `on_hit` | −1 | protection snapshot |
| Protected block evicted without a hit | `commit` | +1 | protection snapshot |
| Coherence / back invalidation, upgrade | `on_invalidate` | none | – |

Update: `w_i += alpha * (r - w_i)` for the active state weight and the active sharing weight of that snapshot (only the
groups the mode uses). Each weight stays in [−1, 1] and tracks the recent mean reward of its feature.

**Protection.** At `commit`, each passed-over way is marked protected with its decision-time features (a later pass
overwrites the snapshot, as in ChampSim) and its `passes` counter grows. `max_passes` and `passes` are diagnostics for
repeated protection (I-1); they do not affect decisions. A protected block that is later evicted also gets a history
outcome (two rewards per lifetime, I-6). Both behaviors are intentional ports and are covered by tests.

## How the two cores differ

| | Paper | Modified |
|---|---|---|
| Weight meaning | Share of recent, not-returned evictions per class | Learned mean reward per feature value |
| Weight scope | Per set (from the set's history) | Global (option per_set) |
| Effect of a reward | −1 removes the entry; +1 nothing | EMA update of the snapshot's weights |
| Candidates examined | #1 checked, #2 evicted unchecked | Best of the N oldest |
| Initial behavior | Bias: evict E, evict below | All weights 0: LRU |
| Per-line state | None | Protection flag, snapshot, pass count |
| Invalidations | Ignored (no per-line state) | Clear the line's metadata; upgrade carries it over |

## Differences from the ChampSim implementations

Paper core vs. `souza_freitas`:

1. Base policy abstracted as a candidate list. `sf::select_victim` took `first()`/`second()` callbacks. SRRIP is not
   ported (its next attempt also rewrites an RRPV, which a plain list cannot express; it will need a base-policy hook).
2. `Combine::Or` added; ChampSim only had And.
3. Rewards are counted at misses (`on_miss`, [P24] §II-A), not on every access including hits. Only the statistics
   differ; ChampSim's +1 count included hits. As in ChampSim, the returning block's entry is removed before the victim
   is chosen and inserted. For a fill with no preceding miss (ChampSim write-backs, which do not occur in Sniper's
   inclusive hierarchy), ChampSim removes the incoming entry before inserting the victim; here `on_eviction` comes first
   and `on_fill` removes it afterwards, counted in `refill_removals`.
4. Default `mode` is `both` (approved setting); ChampSim's `SF_MODE` default was `state`.
5. Sharer class comes from a count supplied by the caller (Sniper: the delivered hint) instead of reading the directory's
   current mask (oracle) inside the policy.

Reward core vs. `rl_mesi`:

1. Iprev, adaptive threshold, BRRIP dueling and ε-exploration not ported. The weight table has no Iprev entry.
2. Invalidations are reported explicitly (`on_invalidate` with a reason) instead of being inferred when a fill lands in a
   way the policy believed occupied. Consequently there is no `invalidated` tag list. An upgrade keeps the block's
   metadata, which ChampSim had no notion of.
3. Recency comes from the base policy (`LruOrder`, Sniper's rank permutation) instead of `rl_mesi`'s per-line
   `last_used` stamps. The orders differ only before every way has been filled once (ChampSim: all-zero stamps, ties
   to the lowest way; Sniper: way K−1 starts as LRU), and Sniper fills invalid ways before asking for a victim.
4. The −1 for a returning block is applied in `on_miss`, before the victim for its fill is chosen, as `rl_mesi`'s
   `on_access` (miss) precedes `find_victim`. In ChampSim other events may happen between the tag check and the fill;
   in Sniper the adapter reports `on_miss` at the start of the insert (see Stage 2), when no other replacement event of
   this set can intervene. A fill not preceded by `on_miss` removes a stale entry silently (`refill_removals`), as
   `rl_mesi::on_fill` does for write-back fills.
5. `scope = per_set` added (default global, as ChampSim).
6. The pass counter and `max_passes` diagnostics added; no behavior change.
7. Mode `off`, N = 1 and frozen zero weights reproduce the base policy's victims exactly (test "N=1 and zero weights
   reproduce plain LRU victims").

## Worked scenario (test "scenario: rewards from decisions change later victim selections")

Settings: 4 cores (heavy iff ≥ 3 sharers), one 8-way set, N = 4, alpha = 0.1.

Initial weights:

| wS | wE | wM | wBT | wAT |
|---|---|---|---|---|
| −0.30 | 0.20 | −0.10 | 0.10 | −0.40 |

The history is full (16 entries), oldest first:

* X `{S, BT}`
* W `{E, BT}`
* 14 × `{M, BT}`

Ways, oldest first:

| Block | State | Sharers | Class |
|---|---|---|---|
| A | M | 1 | BT |
| B | S | 3 | AT |
| C | E | 1 | BT |
| D | S | 2 | BT |
| P, Q, R, T | M | 1 | BT |

1. **Miss on Y.** Window scores: A 0.00, B −0.70, C 0.30, D −0.20.
   * C is evicted; LRU would evict A.
   * A and B are protected, with snapshots `{M, BT}` and `{S, BT/AT}`; D is younger than C, so it is not protected.
   * C enters the history and X ages out (+1): wS = −0.17, wBT = 0.19.
2. **Hit on B** (protected, −1 to `{S, AT}`): wS = −0.253, wAT = −0.46.
3. **Miss on C.** The miss is seen first.
   * C is in the history (−1 `{E, BT}`): wE = 0.08, wBT = 0.071. This also frees C's history slot.
   * Victim scores with the updated weights: A −0.029, D −0.182, P −0.029, Q −0.029. The tie goes to the oldest, A.
   * A was protected and never hit: +1 to `{M, BT}` gives wM = 0.01, wBT = 0.1639.
   * A takes C's free slot in the history, so nothing ages out.
4. **Miss on Z.** LRU would evict D (S, score −0.0891), but P, Q and R (M) now score 0.1739. P is evicted and D is
   protected.
   * P enters the full history and W ages out (+1 `{E, BT}`): wE = 0.172, wBT = 0.24751.
5. **Miss on block 11.** D (−0.00549) is passed over again (`passes` = 2) and Q (0.25751) is evicted. Entry 902
   `{M, BT}` ages out (+1): wM = 0.109, wBT = 0.322759.

After step 3 the weights equal the end of the illustration in [`ALGORITHM_AUDIT.md`](ALGORITHM_AUDIT.md) §3.2
(wE 0.08, wM 0.01, wBT 0.164, wS −0.253, wAT −0.46). This scenario is the exact sequence the cores produce when the
events happen in Sniper's order.

## Sniper integration

**Stage 2 (implemented): event hooks.** Sniper reports every cache event a learning policy needs to its `CacheSet`, with
the controller's classification. All hooks are no-ops in the base class, so every existing policy, including
`CacheSetLRU`, behaves exactly as before. **Stage 3 (not started)** adds the `CacheSetRL` adapter that implements the
hooks with the cores and registers the policy names.

### Hooks (`cache/cache_set.h`, `cache/cache_set_events.h`)

```
CacheSet::insert(block, ..., cntlr, fill)              fill = DEMAND_FILL (default) | UPGRADE_REFILL
   onMiss(tag)                     demand fill only, BEFORE getReplacementIndex      cache_set.cc:148
   getReplacementIndex(cntlr)      victim selection, unchanged
   onEviction(way, victim)         a valid block is replaced; victim still holds     cache_set.cc:164
                                   its state, tag and sharer hint
   [clone]
   onFill(way, tag, fill)          new block installed (the controller sets its       cache_set.cc:180
                                   coherence state and sharer hint afterwards)
CacheSet::invalidate(tag, reason)
   onInvalidate(way, tag, reason)  before the block is invalidated (tag reset to ~0)  cache_set.cc:132
updateReplacementIndex(way)        hits that update recency (unchanged)
```

`Cache::insertSingleLine(..., fill)` and `Cache::invalidateSingleLine(addr, reason)` pass the classification through.
Both arguments default to `DEMAND_FILL` / `INV_UNCLASSIFIED`, so the TLB, page-walk cache, NUCA and DRAM-cache callers
are unchanged.

### Classification (`parametric_dram_directory_msi/cache_cntlr.cc`)

Checked against the source (line numbers at the time of Stage 2):

| Site | Reason | Why |
|---|---|---|
| `processMemOpFromCore`, L1 copy present but access not permitted (`:1012`) | `INV_UPGRADE` | Own write to an S/E line; the L2 response refills it |
| `processShmemReqFromPrevCache`, L2 copy present but not permitted (`:1719`) | `INV_UPGRADE` | Own write to S/E; `copyDataFromNextLevel` refills it |
| `processMemOpFromCore`, accelerator port write below the L1 (`:752`) | `INV_COHERENCE` | Another agent writes; this L1 is not refilled |
| `updateCacheBlock(INVALID, Transition::COHERENCY)` (`:2541`) | `INV_COHERENCE` | Directory INV/FLUSH, or a shared level invalidating its other upper caches |
| `updateCacheBlock(INVALID, Transition::UPGRADE)` | `INV_COHERENCE` | **Another** cache's write: a shared level invalidating the other cores' copies (`:1701`, `:1753`) or an L2 invalidating the sibling L1 |
| `updateCacheBlock(INVALID, Transition::BACK_INVAL / EVICT)` | `INV_BACK_INVALIDATION` | An inclusive lower level evicted the line (`EVICT` is passed up as `BACK_INVAL`, `:2404`) |

`updateCacheBlock` is always driven by another cache, which passes its own reason up the hierarchy. A cache's own
upgrade never goes through it, so `Transition::UPGRADE` must **not** be mapped to `INV_UPGRADE`. The plan written
before Stage 2 said otherwise; that was wrong.

**Fills.** `CacheCntlr::invalidateCacheBlock(addr, INV_UPGRADE)` remembers `addr` in the controller
(`m_upgrade_refill_address`). The controller's next `insertCacheBlock` is an `UPGRADE_REFILL` if it is that line,
otherwise a `DEMAND_FILL`, and any insert clears the mark (`:2239`). This is sound because only the cache's own user
thread starts an upgrade, and it re-inserts the line before that controller inserts anything else:

* L3 hit: `copyDataFromNextLevel` runs synchronously.
* L3 miss: the thread blocks in `waitForNetworkThread` and refills on wake-up (`:752-765`).

The simulator thread only invalidates lines of a private cache; its fills go to the last level, whose own upgrades are
in place (`SHARED_UPGRADING`), never invalidate-and-refill. The line tracer prints the classification
(`INVALIDATE (was S, upgrade)`, `INSERT as M (upgrade-refill)`).

**Hits.** Recency-updating hits arrive through `updateReplacementIndex`: in an L1, `accessCache(..., hit_where == this
level && count)` (`:1162`); in an L2, `retrieveCacheBlock(..., first_hit && count)` (`:1820`). Fills and write-backs pass
`update_replacement = false`. A learning policy therefore sees exactly the hits LRU sees.

### Stage 3 adapter mapping

| Hook | Core call |
|---|---|
| `onMiss(tag)` | `on_miss(set, tag)` |
| `getReplacementIndex` with an invalid way | lowest invalid way, `LruOrder::touch` |
| `getReplacementIndex`, set full | `candidates = LruOrder::candidates(K, isValidReplacement)`, `d = choose(...)`, `commit`, `touch(d.victim)` |
| `onEviction(way, victim)` | `on_eviction(set, victim->getTag(), d.victim_features)` |
| `onFill(way, tag, fill)` | `on_fill(set, way, tag, demand / upgrade_refill)` |
| `updateReplacementIndex(way)` | `touch(way)`, `RewardPolicy::on_hit` |
| `onInvalidate(way, tag, reason)` | `RewardPolicy::on_invalidate` with `coherence`, `back_invalidation` or `upgrade` |

`info(way)` = `make_features(map(block->getCState()), block->getSharerHint(), num application cores)`: the block's own
state and its delivered hint, never the oracle. `queryOracleSharers` stays a diagnostic (`lru-victim`/`hint-victim`
statistics) and may only be used in an explicitly labeled comparison mode.

Things the adapter must get right (audit I-3, I-21):

* Sniper's actual state is used, including M granted on a read: no M→E reinterpretation. A Stage 4 statistic will count
  read-granted M victims.
* Coherence and back invalidations never create history entries or rewards.
* An upgrade carries the block's metadata over and is not an eviction.
* `SHARED_UPGRADING` ways are never candidates (`isValidReplacement`).
* `INV_UNCLASSIFIED` should not occur in an L1/L2; the adapter treats it like a coherence invalidation (no feedback).
* `CacheSetInfoRL` should extend `CacheSetInfoLRU` so `lru-victim-*`, `oracle-*` and `hint-*` statistics (and
  `experiments/sharer-counts/check_sharers.py`) keep working, and every parameter is printed.

### Event checker

`SNIPER_RL_EVENT_CHECK=1` makes every non-TLB `Cache` count the classified events and check them; its `rl-event-*`
statistics are registered only then, so default runs are unchanged. `experiments/sharer-counts/check_rl_events.py`
checks a run:

* An upgrade refill always matches a pending own upgrade of that line, and no demand fill hits a pending one.
* No victim is `SHARED_UPGRADING`, no invalidation is unclassified, and no two upgrades in a set overlap.
* Upgrade invalidations equal upgrade refills.
* Back invalidations equal the controller's `backinval-*` counters, and coherence invalidations fall within its
  `coherency-*` counters.

With `--regress WRITE READ N` it also checks that a `coherence_regress` write makes the writing core upgrade about N
lines in its L1-D (and in its L2 when that is not the last level) while the other core's copies are invalidated by
coherence.

## Known limitations and open questions

* **Repeated protection (I-1).** A low-scoring dead block can be passed over at every miss without ever producing a
  reward (test "a low-scoring dead block is passed over…"). The paper core likewise re-rejects a block left at the LRU
  position. Measured, not fixed, until Stage 4 data exist.
* **Double reward for protected blocks (I-6).** Ported as is (test "protected blocks receive a protection reward and
  later a history reward").
* **Coherence misses after eviction (I-2)** still count as bad evictions. Nothing in the core can see them; a Stage 4
  oracle diagnostic will measure how often.
* **Global weights with alpha = 0.1 (I-4)** adapt very quickly; per-set scope and alpha are parameters for the later sweep.
* **Ties.** The paper core counts ties as highest, so a three-way state tie keeps (example) or evicts (text) every
  candidate #1.
* **Paper ambiguities** kept as options: rule, combine, retain (only `stay` implemented, as approved). The
  boundary/polarity of the threshold follows [P24] (`> cores/2`).
* **Delivered hint 0** (no hint) classifies as below the threshold, as ChampSim's 0. L1 caches get no hint yet (I-18).
* **Upgrade carry-over** keeps one pending upgrade per set, matched by tag (see "Overlapping upgrades" below). The
  Stage 2 event checker counts overlaps (`rl-event-upgrade-overlap`).
* **No hardware cost** (latency, storage, energy) is modeled; full tags and `double` weights, as in ChampSim.
* **Invalidations that bypass the hooks.** A few controller paths call `CacheBlockInfo::invalidate()` directly instead of
  `CacheSet::invalidate`, so no `onInvalidate` is reported:
  * passthrough caches (`perf_model/*/passthrough`, off by default), `cache_cntlr.cc:826` and `:1493`;
  * in an L1, page-table metadata blocks that hit while metadata passthrough applies (`:833`).

  The private L2 target is not affected. For an L1 adapter, such a block simply disappears; the next `onFill` of its way
  resets the metadata, so the only cost is a protection outcome that is never rewarded.

### Overlapping upgrades

**Can an upgrade overlap another in the same set?** Sniper's upgrade of a private copy is: invalidate the own copy,
forward the write to the next level, refill. At the L2 the steps are:

* invalidate the upper levels and the L2's own copy (`cache_cntlr.cc:1700-1716`);
* call `m_next_cache_cntlr->processShmemReqFromPrevCache`;
* on an L3 hit, `copyDataFromNextLevel` refills synchronously;
* on an L3 miss, the requester releases the stack lock, blocks in `waitForNetworkThread`, and refills when it wakes
  (`cache_cntlr.cc:752-765`).

Only the cache's own user threads start upgrades. While the lock is released, the simulator thread can only invalidate
lines (directory `INV`/`FLUSH`, L3 back invalidations), which never creates a pending upgrade.

* **No overlap** when one hardware thread uses the cache: a private L1/L2 with `logical_cpus = 1`, which includes the
  Stage 2 target (Gainestown private L2). That thread is blocked until its refill completes, so at most one upgrade is
  pending per cache.
* **Overlap is possible** when several user threads share the cache:
  * SMT: `memory_manager.cc:170` multiplies `shared_cores` by `logical_cpus`, so a "private" L2 is shared by the SMT
    threads;
  * or a cache with `shared_cores > 1` that is not the last level.

  Two threads can then each be waiting between their invalidation and refill, with blocks in the same set. The last
  level does not invalidate and refill: it upgrades in place through `SHARED_UPGRADING`, and those ways are excluded
  from replacement.

**Consequence.** With one slot per set, the second upgrade invalidation replaces the first (`upgrade_stash_overwritten`).
The first block's refill does not match, so it starts with fresh metadata (`upgrade_refills_unmatched`): its protection
and pass count are lost. Nothing is misattributed (the match is by tag), no reward is given, and the second block is
restored normally. A refill that does not match leaves another block's pending upgrade in place. This was corrected
alongside the ordering fix; previously it cleared the slot.

**Recommendation.** Keep the single slot for the Stage 2 target, and check `upgrade_stash_overwritten == 0` in the
integration tests. Before using the policy in SMT or shared non-last-level configurations, replace the slot with a small
per-set list of pending upgrades keyed by tag, at most one per hardware thread sharing the cache.
