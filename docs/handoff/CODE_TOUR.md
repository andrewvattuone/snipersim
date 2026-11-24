# Code tour: the sections to read yourself

The most important code to understand personally, in reading order. Line numbers are at `feature/rl-port`
2363b52e unless another branch is named. Read the test or worked example beside each section. Most of the logic is
easiest to follow through a concrete trace.

## 1. The learner: `common/core/memory_subsystem/cache/rl/rl_reward_policy.h`

This one header holds the whole algorithm. It is 532 lines.

| Lines | What to understand | Check yourself with |
|---|---|---|
| 4–37 | The header comment: what was ported from ChampSim, the reward events and their order | `docs/rl/POLICY_CORES.md`, "Modified reward-learning core" |
| 102–111 `priority_rank` | The fixed priority: S = 4, E = 2, M = 0, plus 1 if above the threshold. The highest rank is evicted | `testing/unit/rl/test_controls.cc` |
| 129–150 `random_draw` | The random-departure control (SplitMix64, no generator state) | `test_controls.cc` |
| 185–280 `WeightTable` | `score` (state weight + sharing weight); `reward` at :240 (the EMA `w += α(r − w)`); `discounted_update` at :200 (the Stage 7 estimator) | `test_reward_policy.cc`, `test_estimator.cc` |
| 384–412 `choose` | The window of N oldest candidates, highest score wins, strict `>` so ties go to the older block; `passed_over` lists the candidates older than the victim | Audit §3.2 example |
| 419–443 `commit` | Passed-over blocks become protected with a snapshot and `passes++` (no promotion: audit I-1); a protected victim gets +1 (protect-wasted) | `test_scenarios.cc` |
| 447–455 `on_eviction` | The victim enters the 16-entry history; an aged-out entry gives +1 (evict-good) | – |
| 464–472 `on_miss` | A returning block in the history gives −1 (evict-bad). It must run before `choose` | `test_event_order.cc` |
| 480–498 `on_fill` | A demand fill resets the metadata; an upgrade refill restores it | – |
| 501–509 `on_hit` | A protected block hit gives −1 (protect-good) | – |
| 515–530 `on_invalidate` | No reward for coherence, back-invalidation or upgrade; an upgrade stashes the metadata | – |

**Exercise.** Run `make -C testing/unit/rl test`, then read the test "scenario: rewards from decisions change later
victim selections" in `test_scenarios.cc`. Reproduce its five steps by hand from `POLICY_CORES.md`, "Worked
scenario".

## 2. Features and history

* `cache/rl/rl_features.h:21–46`:
  * `Mesi`, `Features {state, above}`;
  * `above_threshold` (:33): count > cores/2;
  * `make_features` (:40).
* `cache/rl/rl_eviction_history.h:32–80`: a per-set FIFO of `{tag, features}`. `insert` removes a duplicate and
  returns the aged-out entry; `remove` serves the returning-block lookup.
* `cache/rl/rl_lru_order.h`: the candidate list in Sniper `CacheSetLRU`'s exact rank semantics, with the
  eligibility filter.

## 3. Where Sniper calls the policy

* `cache/cache_set.cc:127–140` `CacheSet::invalidate`: calls `onInvalidate(way, tag, reason)` before the line is
  cleared.
* `cache/cache_set.cc:144–182` `CacheSet::insert`, the miss lifecycle:
  1. `onMiss(tag)` (demand fills only);
  2. `getReplacementIndex`;
  3. `onEviction`;
  4. the clone of the new block;
  5. `onFill`.
* `cache/cache_set_rl.cc`, the adapter:
  * `getReplacementIndex` :465: invalid way first, otherwise `LruOrder::candidates` → `choose` → `commit`;
  * `updateReplacementIndex` :540: a hit → `on_hit`;
  * `onMiss` :560, `onEviction` :623, `onFill` :661, `onInvalidate` :683;
  * `featuresOf` / `features` :449–464: the MESI state and the delivered hint, never the oracle.
* `cache/cache_set_events.h`: the invalidation reasons and fill kinds.

## 4. MESI and the event classification: `parametric_dram_directory_msi/cache_cntlr.cc`

| Lines | What to understand |
|---|---|
| 1014 | The L1 upgrade: a store to a non-M L1 copy invalidates it (`INV_UPGRADE`) before forwarding |
| 1255–1283 `copyDataFromNextLevel` | The **MESI fill fix**: a read fill gets S when `otherUpperCacheHolds`, otherwise the L3's E or M |
| 1300–1331 | The **delivered sharer hint**: `countOtherSharers` + 1, stored in the block |
| 1634–1641 | A write hit resets the hint to 1 |
| 1721 | The L2 upgrade invalidation |
| 2171–2186 `invalidationReason` | How `Transition` reasons map to coherence and back-invalidation (`UPGRADE` from another cache = coherence) |
| 2188 `invalidateCacheBlock`, 2228 `insertCacheBlock` | Upgrade-refill detection (`m_upgrade_refill_address`) |
| 2760 `otherUpperCacheHolds` | The presence-bit test used by the fill fix |

Also read `cache/cache_state.h:31–40`: `readable` / `writable`. Only M is writable.

## 5. The paper reproduction (for contrast): `cache/rl/rl_paper_policy.h`

* :82 `compute_ratio_weights`: the per-set history fractions.
* :200 `PaperPolicy::choose`: candidate #1 is checked under the `text` or `example` rule (And/Or); a rejected
  candidate causes candidate #2 to be evicted unchecked.

## 6. Experiment harness: `experiments/rl/run_stage6.py`

* :67–103 `POLICIES`: the exact `-g` options of every named policy (`rl-n4`, `rl-n4-prio`, `rl-n4-frozen`,
  `rl-n4-rand`, `rl-n4-dm65536`, …).
* :115 `WORKLOADS`: the commands, ROI and timeouts.
* :173 `command`: the full `run-sniper` command.
* :263 `run_one`: the run, timeout, ROI checks, checkers and manifest line.
* `experiments/sharer-counts/check_rl_events.py`, `check_rl_policy.py`, `check_sharers.py`: what "every checker
  passes" means.

## 7. Diagnostics (skim)

* `cache/rl/rl_diagnostics.h`: observers that predict the core's counters and must match them.
* `cache/rl_write_oracle.{h,cc}`: records every store with a global sequence number (diagnostics only: "did another
  core write this line after we evicted it?").

## 8. Stage 7 tools (branch `review/stage7-headroom-trace`, at cf776026)

* `common/core/memory_subsystem/cache/rl_trace.{h,cc}`, `cache/rl/rl_trace_core.h`: the observation-only trace
  writer (`perf_model/l2_cache/rl_trace/*`).
* `experiments/rl/headroom/shadow.py`, the shadow model:
  * `Shadow` :196;
  * `fill` :371 (fill state, with the permission model);
  * `resolve` :419;
  * `lookup` :470 (hits, upgrades, the L1-write-permission upgrade);
  * `run` :545 (record dispatch: accesses, way records, decisions, fills, invalidations, L1 events);
  * the module docstring.
* `experiments/rl/headroom/headroom.py` `cmd_calibrate` :137: the calibration criteria in code.
* `experiments/rl/headroom/test_headroom.py`: hand-worked synthetic traces. Each test is a small, checkable example
  of one modelling rule.

## 9. Stage 7 correction (branch `review/stage7-protect-invalidation`, 80972542)

* `cache/rl/rl_reward_policy.h`:
  * the `protect_invalidation_feedback` option (:183);
  * the counter `protect_invalidated` (:302);
  * its use in `on_invalidate`: a protected line removed by coherence or back-invalidation gets +1 once.
