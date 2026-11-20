# RL replacement cores (Stage 1)

Stage 1 of the Sniper port: the two replacement algorithms as simulator-independent, header-only C++ cores with
deterministic unit tests. **Nothing is wired into Sniper yet**: no cache hooks, no registered policy names, no change
to `CacheSetLRU`, the MESI protocol or the sharer-hint code. The audit that motivates every decision here is
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
| `Makefile`, `.gitignore` | Build/run; the test binary is ignored |

## Running the tests

```
make -C testing/unit/rl test          # build with -std=c++20 -Wall -Wextra -Wcast-align -Wshadow -Wconversion -Werror, run
testing/unit/rl/rl_unit_tests paper    # only test cases whose name contains "paper"
make -C testing/unit/rl clean
```

Expected: `53 test cases, 0 failed, 593 assertions`.

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

**Rewards.** The paper's −1/+1 change nothing directly, and no update rule is invented. On a demand fill:

* if the tag is in the history: the entry is removed (which is what changes later ratios) and −1 is counted;
* otherwise +1 is counted.

Rewards are statistics only (`PaperStats`).

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
| Evicted block refilled while its entry is still in the history | `on_fill(demand)` | −1 | saved in the history entry (at eviction) |
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
3. Rewards are counted at demand fills (misses, [P24] §II-A), not on every access including hits. Only the statistics
   differ; ChampSim's +1 count included hits.
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
4. The −1 for a returning block is taken at the fill (`on_fill(demand)`) instead of at the miss's tag check. In Sniper
   every miss ends in an insert, so the event is the same; the victim for that fill is chosen first (as in ChampSim).
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
3. **Miss on C.** The victim is chosen first, with scores A 0.09, D −0.063, P 0.09, Q 0.09.
   * The tie goes to the oldest, A. It was protected and never hit: +1 to `{M, BT}` gives wM = 0.01, wBT = 0.271.
   * A enters the history and W ages out (+1 `{E, BT}`): wE = 0.28, wBT = 0.3439.
   * Then C's fill finds C in the history (−1 `{E, BT}`): wE = 0.152, wBT = 0.20951.
4. **Miss on Z.** LRU would evict D (S, score −0.04349), but P, Q and R (M) now score 0.21951. P is evicted and D is
   protected.
   * The history had a free slot (C's entry was removed in step 3), so nothing ages out and the weights do not change.
5. **Miss on block 11.** D is passed over again (`passes` = 2) and Q is evicted. Entry 902 `{M, BT}` ages out (+1):
   wM = 0.109, wBT = 0.288559.

[`ALGORITHM_AUDIT.md`](ALGORITHM_AUDIT.md) §3.2 shows the same weights as an illustration with independent events; this
scenario is the exact sequence the cores produce when the events happen in Sniper's order.

## Stage 2 integration interface

The adapter (planned `CacheSetRL` in `cache/`, a `CacheSet` subclass; `CacheSetLRU` untouched) owns one `LruOrder` per
set and one policy object per cache, and maps Sniper's events as follows.

| Sniper | Core |
|---|---|
| `CacheSet::insert` → `getReplacementIndex(cntlr)` with an invalid way | lowest invalid way, `LruOrder::touch`; no decision |
| `getReplacementIndex` with all ways valid | `candidates = LruOrder::candidates(K, isValidReplacement)`; `d = choose(set, candidates, info)`; `commit`; `on_eviction(set, victim tag, d.victim_features)`; `touch(victim)` |
| After the clone of the new block in `insert` | `on_fill(set, way, tag, demand)` (or `upgrade_refill`) |
| `updateReplacementIndex(way)` (hits with `update_replacement`) | `touch(way)`; `RewardPolicy::on_hit` |
| `CacheSet::invalidate` | `RewardPolicy::on_invalidate(set, way, tag, reason)` |
| Write-backs (`update_replacement = false`) | nothing |

`info(way)` = `make_features(map(block->getCState()), block->getSharerHint(), num_application_cores)`: local block
state and the delivered hint only. `oracle` sharer counts stay in the diagnostics (`queryOracleSharers`) and may only be
used in an explicitly labeled comparison mode.

Hooks Sniper needs, each a no-op for every existing policy so the LRU baseline is unchanged:

1. `CacheSet::insert` must call a virtual notification after the clone (the policy needs the new tag and the fill kind).
2. `CacheSet::invalidate` (via `Cache::invalidateSingleLine`) needs the reason. `CacheCntlr::updateCacheBlock` knows
   it (`Transition::COHERENCY`, `BACK_INVAL`, `UPGRADE`); the self-invalidations before forwarding a write
   (`processMemOpFromCore`, `processShmemReqFromPrevCache`) must pass `upgrade`.
3. The re-insert that follows an upgrade must be reported as `upgrade_refill`. The controller knows it is completing a
   write to a line it held, which the cache set does not.

Things the adapter must get right (audit I-3, I-21):

* Sniper's actual state is used, including M granted on a read: no M→E reinterpretation. A Stage 4 statistic will count
  read-granted M victims.
* Coherence and back invalidations never create history entries or rewards.
* An upgrade carries the block's metadata over and is not an eviction.
* `SHARED_UPGRADING` ways are never candidates.
* `CacheSetInfoRL` should extend `CacheSetInfoLRU` so `lru-victim-*`, `oracle-*` and `hint-*` statistics (and
  `experiments/sharer-counts/check_sharers.py`) keep working, and every parameter is printed.

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
* **Upgrade carry-over** keeps one pending upgrade per set, matched by tag. Sniper re-inserts immediately after the
  invalidation; an unmatched refill is counted (`upgrade_refills_unmatched`) and starts fresh.
* **No hardware cost** (latency, storage, energy) is modeled; full tags and `double` weights, as in ChampSim.
