# Stage 5: functional and coherence validation of `sf_lru` and `rl_lru`

Stage 5 of [`ALGORITHM_AUDIT.md`](ALGORITHM_AUDIT.md) (Section 9). The question: does putting either RL
policy on Sniper's caches introduce incorrect cache behavior, a coherence violation, or an invalid learning
event? These are correctness checks only, not performance results.

## Verdict

* **No coherence violation found.** All 144 `coherence_regress` write/read pairs pass a new per-line MESI
  invariant check on every one of the 256 lines. The pairs cover five Gainestown hierarchies × {LRU,
  `sf_lru` text, `sf_lru` example, `rl_lru`} × 3 repetitions, plus 4 silvermont variants × 3.
* **Every other check passes on every completed run:**
  * 0 stale write hits (`SNIPER_COHERENCE_CHECK=1`);
  * every event check and every RL policy check, including the new learning-event and feature
    cross-checks;
  * the sharer-count invariants on all runs.
* **Aggregate regression check:** it failed in 8 of the 144 pairs, and each failure is fully explained by the
  per-line data. In 2 Gainestown pairs (`rl_lru`, 2 and 8 cores), unrelated L1-D misses were 33 and 29 lower
  in the write run while all 256 lines were refetched. In 6 silvermont pairs with `rl_lru` on the L1-D, the
  policy had evicted regression lines, so they needed no invalidation.
* **Not fully explained:** 4 runs crashed (SIGSEGV), out of 99 suite runs and 132 repetitions.
  * Three crashed at simulator teardown, after the region of interest and the statistics database were
    complete; their recovered statistics pass every check.
  * One LRU run crashed at start-up.
  * The teardown crash has so far been seen only in `rl_lru` + diagnostics runs (3 of ~93, against 0 of ~137
    other runs). Its mechanism is a Sniper thread that outlives the `Simulator` object, not replacement code.
    It is open; see [Crashes](#crashes).

**Run outcomes.** The validation suite is not crash-free. "Crashed after statistics" means the simulator
completed the region of interest and wrote `sim.stats.sqlite3`, then crashed in teardown, so `run-sniper` did
not export `sim.stats`. Those runs are reported separately from clean runs everywhere below.

| Group | Simulations | Clean exit | Crashed after statistics | Crashed without statistics |
|---|---|---|---|---|
| Phase 1, `run_regress.sh` | 432 | 432 | 0 | 0 |
| Phase 2, sharer-count suite | 99 | 96 | 2 (`rl_lru`) | 1 (LRU) |
| Phase 3, FFT | 18 | 18 | 0 | 0 |
| Phase 4, single-core replay | 19 | 19 | 0 | 0 |
| Crash repetitions | 132 | 131 | 1 (`rl_lru`) | 0 |
| **Total** | **700** | **696** | **3** | **1** |

## What changed for Stage 5 (test instrumentation only)

The Stage 1 cores, the learning rules, `CacheSetLRU`, `CacheSetSRRIP`, the MESI protocol and the
delivered-sharer hints are unchanged.

| Change | Why |
|---|---|
| `parametric_dram_directory_msi/line_watch.{h,cc}` and four guarded calls (`cache_cntlr.cc` insert / invalidate / downgrade, `memory_manager.cc` mapping and hook) | Per-line record of the regression lines' coherence events, to check `coherence_regress` line by line. Off unless `SNIPER_WATCH_VADDR` is set; no state, message or timing change |
| `check_regress.py`: per-line invariant check; the result names which check failed | Separates coherence errors from run-to-run variation (the aggregate thresholds are unchanged) |
| `test_check_regress.py` | Self-test: 4 correct and 6 corrupted histories |
| `run_regress.sh`: sets the line watch for the write/read runs | Feeds the per-line check |
| `run_validation.sh`: `EXTRA_ARGS` | Runs the sharer-count suite with a replacement policy |
| `check_rl_policy.py`: history-occupancy bound, upgrade carry-over = upgrades = upgrade refills, eviction features vs. the LRU victim statistics | Learning-event checks of Stage 5 Section 3 |
| `cache_set_rl.{h,cc}`, diagnostics only: `rl/sf-diag-evicted-{S,E,M,BT,AT}`, `-eviction-features-changed` | The features recorded at each eviction, cross-checked independently |

## Configurations tested

Every run used `SNIPER_RL_EVENT_CHECK=1` and `SNIPER_COHERENCE_CHECK=1`, apart from the single-core replays
(see Phase 4). The RL policies ran with the approved default parameters and `rl_diag/enabled=true`. Stage 4
showed diagnostics never change a decision; Phase 4 confirms it again.

| Hierarchy | Arguments | Policies on the private L2 |
|---|---|---|
| Gainestown 2 cores, shared L3 | `-n 2 -c gainestown` | LRU, `sf_lru`, `sf_lru` example, `rl_lru` |
| Gainestown 4 cores | `-n 4 -c gainestown` | same |
| Gainestown 8 cores (two L3 domains) | `-n 8 -c gainestown` | same |
| Gainestown, private L3 per core | `... -g perf_model/l3_cache/shared_cores=1` | same |
| Gainestown, 2 levels (private L2 is the last level) | `... -g perf_model/cache/levels=2` | same |
| silvermont, 2 levels, L2 shared by 2 cores | `-n 2 -c silvermont` | LRU; `rl_lru` on the private L1-D; exploratory `rl_lru` / `sf_lru` on the shared L2 |

**Phase 1:** `run_regress.sh` (`coherence_regress`, 256 lines). Each run covers clean and dirty, with step 3
as write and as read, plus a traced one-line copy. That is 24 configurations × 3 repetitions = 72 runs: 144
write/read pairs and 432 simulations.

**Phase 2:** `run_validation.sh` for LRU, `sf_lru` and `rl_lru`: 33 runs each.
* FFT `-p 4 -m 10`.
* `sharetest` r / w / p / u × K = 1..4 × L3 1 MB / 512 KB (all 4-core Gainestown).

**Phase 3:** FFT `-m 12` with 2 threads on 2 cores, 4 on 4 and 8 on 8; LRU / `sf_lru` / `rl_lru`; 2
repetitions (18 runs).

**Phase 4:** deterministic single-core FFT trace replay (8 MB and 512 KB L3) against the Stage 3 results.
* LRU, `rl_lru`, `rl_lru` mode off, `sf_lru` text and example, each with diagnostics off and on.
* SRRIP against Stage 3's SRRIP run.
* `SNIPER_COHERENCE_CHECK` was unset here, as in Stage 3.

**Crash repetitions:** 132 further 4-core `sharetest` runs (see [Crashes](#crashes)).

**Coverage of the requested scenarios:**

| Scenario | Where | Evidence |
|---|---|---|
| Read sharing, E→S downgrades | `coherence_regress` steps 1-2; `sharetest r` | per-line downgrades; 0 violations |
| Writes needing an upgrade | step 3; `sharetest u`, `w` | rl suite: 4,225,514 L2 upgrade invalidations = upgrade refills = carry-overs |
| Invalidating other cores' S copies | step 3 (2-8 cores); `sharetest u` K ≤ 4 | 189,042 L2 coherence invalidations (rl suite); per-line check |
| Ownership migration | `sharetest p` K = 2..4 (4 rounds) | sharer invariants, events, policy checks pass |
| Back invalidations from inclusive L3 evictions | `sharetest` with 512 KB L3 | 3,726,397 L2 back invalidations (rl suite) = the controller's `backinval-*` |
| Repeated invalidation and refill of one address | `sharetest p`, `u`; `coherence_regress` × 3 reps × 6 hierarchies | as above |
| M granted on a read | single-core replay; multicore suite | M victims never written locally: 214 (rl suite), 46 (sf suite), 609 (silvermont L1-D), ~16% of M victims in single-core replay; features M, no error |

## Results

### Passed

* **Unit tests:** 71 cases, 0 failed, 844 assertions. `test_check_regress.py`: 10 of 10.
* **Phase 1, per-line check:** 144 of 144 pairs, 256 of 256 lines each.
  * Event checker: all 432 simulations.
  * `check_rl_events.py --regress` classification: all pairs except the 3 silvermont `rl_lru`-L1-D clean
    pairs (below).
  * RL policy checker: every RL run. 0 stale write hits.
* **Phase 1, aggregate check:** 136 of 144 pairs.
* **Phase 2:** every cleanly completed run passes `check_sharers.py`, `check_rl_events.py` and (RL) `check_rl_policy.py`,
  with 0 stale write hits. Clean exits: LRU 32 of 33, `sf_lru` 33 of 33, `rl_lru` 31 of 33.
  * The two `rl_lru` runs that crashed after writing their statistics pass every check on statistics recovered
    with `tools/dumpstats.py`. They are counted as crashed, not as clean passes.
  * The LRU run that crashed at start-up produced no results.
* **Phase 3:** 18 of 18 runs pass every check, 2 to 8 cores.
* **Phase 4:** all 18 replays give Stage 3's victim sequence and identical statistics. SRRIP is identical to
  Stage 3. The pre-existing jittery `L3.store-overlapping-misses` is excluded.

### Learning-event correctness (both policies, every RL run)

| Requirement | Evidence (checked by `check_rl_policy.py` / `check_rl_events.py` on every run) |
|---|---|
| Every replacement eviction reported exactly once | `history-inserts` = the Cache's own eviction count (`rl-event-eviction`); `eviction-without-decision` = 0 |
| Coherence invalidations create no history entry | the equality above, although coherence and back invalidations occur; `miss-inconsistent` = 0 (no line both in the history and lost to an invalidation) |
| Local upgrades give no false −1 | upgrade refills are never misses: diagnostic misses = demand fills; `history-refill-removals` = 0 |
| Metadata survives upgrade invalidation and refill | `rl-upgrade-carryovers` = `rl-inv-upgrade` = `rl-event-fill-upgrade-refill`; `upgrade-refills-unmatched` = `upgrade-stash-overwritten` = 0 |
| History accounting closes | \|inserts − aged out − returned\| ≤ capacity; diagnostic reward and class totals equal the cores' counters |
| No `SHARED_UPGRADING` victim | `rl-event-victim-upgrading` = 0; `no-eligible-victim` = 0 |
| All unexpected-event counters 0 | yes, every run |
| Oracle never influences selection | features come only from `getCState()` and `getSharerHint()`; the oracle is queried only after the choice. Evictions recorded as AT: 23,261 = victims with a heavy DELIVERED hint (23,261), not the oracle's 30,110 (they differ in 94 of 384 runs). Diagnostics on/off: identical victims |
| Eviction features reflect the block | evicted S / E / M = 67,923 / 204,794 / 619,115 = LRU's victim-state counts; `eviction-features-changed` = 0 |

### Failed (explained)

* **Aggregate `core0 L1-D load misses`, 2 Gainestown pairs:** `s5-g2-rl-r2` clean (223) and `s5-g8-rl-r3`
  dirty (227), against 230.4.
  * In both, the line watch shows exactly 256 more refills of the regression lines in the write run; the
    unrelated misses were −33 and −29.
  * Over all 120 Gainestown pairs the regression-line refill delta is exactly 256 every time. The unrelated
    component has a standard deviation of 11–14 (range −33 to +32), so with a 25.6-line margin a few percent
    of pairs fail by chance.
* **Aggregate rows, all 6 silvermont pairs with `rl_lru` on the 24 KB L1-D:** core 0's L1-D coherence removals
  and load misses fall short, and the `--regress` classification of the clean case misses too.
  * In each clean write run, the lines invalidated by coherence (175–179) and the lines `rl_lru` had already
    evicted before core 1's write (77–81) add up to exactly 256.
  * The aggregate check assumes the lines stay resident, which this policy, on this cache, does not provide.
  * LRU on the same hierarchy passes.

The aggregate thresholds were not changed. Details:
[`COHERENCE_VALIDATION.md`](../../experiments/sharer-counts/COHERENCE_VALIDATION.md) ("Per-line check").

### Could not run or not exercised

* **Exploratory `rl_lru` / `sf_lru` on silvermont's shared L2:** the runs pass every check, but the 1 MB shared
  L2 never filled a set, so the policies made no decision there. Shared last-level caches remain outside the
  target and untested; see the no-eligible-victim issue in `POLICY_CORES.md`.
* **4 crashed simulations** (below): 3 with recovered statistics that pass, 1 start-up crash without results.

### Crashes

| Run | When | Backtrace (top) | Results |
|---|---|---|---|
| `s5val-rl-l3-1024k-r-K4` | teardown, after the ROI | `Simulator::getClockSkewMinimizationServer` ← `PthreadThread::spawnedThreadFunc`, address 0x120 | statistics database complete; recovered `sim.stats` passes all checks |
| `s5val-rl-l3-1024k-u-K1` | same | same | same |
| `crash-s5bin-rl-512k-u-K3-b7` | same | same | completed ROI |
| `s5val-lru-l3-512k-u-K3` (LRU) | start-up, first memory access | `TraceThread::getThread` ← `MemoryManager::coreInitiateMemoryAccess` | none |

* **Repetitions:** 66 runs of the same `sharetest` configurations on the pre-RL baseline binary (`27e0d326`,
  LRU) did not crash. Of 66 runs on the Stage 5 binary (60 `rl_lru` + diagnostics, 6 LRU), one `rl_lru` run
  crashed at teardown.
* **Teardown crash.** A simulator thread runs after the `Simulator` object is gone: `Sim()` is null.
  `PthreadThread` does not join its threads (the join in its destructor is commented out), so this is a
  shutdown race in Sniper's thread management. No replacement code is on that stack.
  * It has been seen only with `rl_lru` + diagnostics: 3 of ~93 such runs, against 0 of ~137 LRU / `sf_lru`
    runs. That is suggestive but not significant (p ≈ 0.07).
  * Teardown takes about twice as long with diagnostics (median 0.09–0.11 s against 0.05 s for LRU), which
    could widen the window. This is not confirmed: gdb is not installed and core dumps are disabled here.
* **Start-up crash.** It happened once. In an LRU run without the line watch, the Stage 5 binary executes the
  same code as the baseline; the crash did not recur in 72 later LRU runs.
* **Status:** open. It does not affect any simulated result, but a crashed run loses its `sim.stats` export
  (recoverable with `tools/dumpstats.py -d RUN`).

## Known limitations and untested configurations

* **RL decisions in `coherence_regress`** are few (about 280 per run, core 0 only). The decision coverage
  comes from the sharer suite and FFT: about 820,000 decisions over 2–8 cores.
* **Not tested:**
  * SMT, a shared non-last-level L2, and a shared last-level cache with an RL policy making decisions (see
    above);
  * the auxiliary tag directory (refused by design);
  * prefetchers;
  * L1-I;
  * more than 8 cores;
  * `rl_lru` options other than the defaults (`window`, `alpha`, `protect_feedback`, `weight_scope`); the
    Stage 3 replay matrix covers them single-core.
* **No sharer hints** in the private-L3 and two-level hierarchies (there is no shared cache below the L2), so
  the sharing feature is constant there.
* **Line-watch limits:** see `COHERENCE_VALIDATION.md` (in-place upgrades of a last-level private L2 have no
  event of their own; 64-byte lines; states, not data).
* **Multicore runs are not reproducible run to run;** repetitions were used, and every failure was explained.
  Only the crashes are not fully explained.

## Recommendations

1. **Teardown crash:** install gdb (or enable core dumps) and capture a full backtrace. Then compare the crash
   rate of `rl_lru` with diagnostics on and off over about 100 runs each. Fixing the thread join belongs to
   Sniper's infrastructure, not to the policies. Until then, recover crashed runs with `tools/dumpstats.py`.
2. **Use the per-line verdict** to judge coherence in `run_regress.sh` results. An aggregate-only failure with
   the per-line check passed is the documented noise or eviction case. Consider demoting the aggregate rows to
   warnings in a later, reviewed change.
3. **Before a shared last-level cache or SMT:** resolve the no-eligible-victim fallback and the single upgrade
   slot (`POLICY_CORES.md`), then add a configuration where the shared cache actually evicts.
4. Stage 6 can start from this matrix. Keep `SNIPER_COHERENCE_CHECK=1` and the checkers in the experiment
   scripts, since they cost little.

## Reproducing

```
export SNIPER_RL_EVENT_CHECK=1 SNIPER_COHERENCE_CHECK=1
E=experiments/sharer-counts; L2=perf_model/l2_cache; RL="-g $L2/replacement_policy=rl_lru -g $L2/rl_diag/enabled=true"
$E/run_regress.sh s5-g2-rl -n 2 -c gainestown $RL        # likewise -n 4 / -n 8, shared_cores=1, levels=2, silvermont
JOBS=12 EXTRA_ARGS="$RL" $E/run_validation.sh s5val-rl
./run-sniper -n 8 -c gainestown $RL -d results/fft-p8 -- test/fft/fft -p 8 -m 12
python3 $E/check_rl_policy.py RUN...; python3 $E/check_rl_events.py RUN...; python3 $E/check_sharers.py RUN...
python3 $E/test_check_regress.py
```
