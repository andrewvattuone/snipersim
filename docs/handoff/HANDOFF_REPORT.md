# Coherence-aware RL cache replacement in Sniper: Stage 1–7 handoff report

**As of 2026-11-23.** This report covers the whole implementation, from the pre-RL coherence fixes to the end of
Stage 7. It is a study guide, so it summarises and points to the detailed documents rather than replacing them. It
adds no new measurement: every number comes from an archived run or a committed document, and the source is named
next to it.

**How to read the claims.** Each claim is labelled with one of the following:
* **Measured:** read from the archived statistics of a run.
* **Code fact:** follows from the source.
* **Estimate:** a first-order calculation, shown with its inputs.
* **Hypothesis:** a plausible explanation that has not been tested.

Unless stated otherwise:
* every performance comparison uses **two repetitions** per policy (three in the LU `tiny` pilot);
* the configuration is one 4-core machine with one 8 MB L3;
* the result is a range, not a significance test.

**Bottom line, stated up front:**
* **The fixed MESI priority is the best policy measured so far.** It and the frozen LU-derived weights reduced L2
  misses more than the online learner (rl-n4) in every repetition on both workloads where this was tested.
* **No policy changed execution time measurably.**
* **The RL algorithm does not currently outperform the fixed priority.**

---

## Contents

* [Where things are](#where-things-are)
* [A. Project objective](#a-project-objective)
* [B. Implementation](#b-implementation)
* [C. Stages 1–5: building and validating the policies](#c-stages-15-building-and-validating-the-policies)
* [D. Stage 6: experiments](#d-stage-6-experiments)
* [E. Stage 7: why online learning underperforms, and the offline model](#e-stage-7-why-online-learning-underperforms-and-the-offline-model)
* [F. Current project status](#f-current-project-status)
* [G. Proposed Stage 8 (not started)](#g-proposed-stage-8-not-started)

## Where things are

| What | Where |
|---|---|
| Repository | `snipersim` (`github.com/andrewvattuone/snipersim`). The main integration branch is `feature/rl-port` |
| Algorithm audit (the design rationale for everything) | [`docs/rl/ALGORITHM_AUDIT.md`](../rl/ALGORITHM_AUDIT.md) |
| Cores, Sniper integration, controls, diagnostics (Stages 1–4, 6–7 options) | [`docs/rl/POLICY_CORES.md`](../rl/POLICY_CORES.md) |
| Stage 5 validation and crash fix | [`docs/rl/STAGE5_VALIDATION.md`](../rl/STAGE5_VALIDATION.md), [`docs/rl/CRASH_INVESTIGATION.md`](../rl/CRASH_INVESTIGATION.md) |
| Pre-RL MESI fix | [`experiments/sharer-counts/COHERENCE_VALIDATION.md`](../../experiments/sharer-counts/COHERENCE_VALIDATION.md) |
| Stage 6 | [`docs/rl/STAGE6_PILOTS.md`](../rl/STAGE6_PILOTS.md), [`docs/rl/CAPBENCH.md`](../rl/CAPBENCH.md) |
| Stage 7 (learning investigation, estimator, invalidation feedback) | [`docs/rl/STAGE7.md`](../rl/STAGE7.md) |
| Stage 7 objective investigation | `results/stage7-objective-investigation/REPORT.md` (local results directory, git-ignored, archived) |
| Stage 7 trace, shadow model and calibration | `docs/rl/HEADROOM.md` on branch `review/stage7-headroom-trace` ([GitHub](https://github.com/andrewvattuone/snipersim/blob/review/stage7-headroom-trace/docs/rl/HEADROOM.md)) |
| Invalidation-feedback design | `docs/rl/STAGE7.md` on branch `review/stage7-protect-invalidation` ("Protection feedback on invalidation") |
| Results | `snipersim/results/` (git-ignored, read-only per experiment). Archives are in `results/archive/*.tar.gz`, each with a `.sha256` (verify from `results/`) |

Code paths below are relative to the repository root. Line numbers are at `feature/rl-port` 2363b52e unless another
branch is named.

---

## A. Project objective

### A.1 The problem

A shared-memory multicore has private caches per core (here L1 and L2) and a shared last-level cache (L3), kept
coherent by a protocol (here MESI). When a private L2 set is full and a new block arrives, the replacement policy
chooses a victim. Classical policies (LRU and its variants) use only recency.

**The question of the project:** can a replacement policy for the private L2 make better choices by also using the
coherence information each block already carries?
* **Its MESI state:** Modified, Exclusive or Shared.
* **Its degree of sharing:** how many cores held the line when it was filled.

The policy should learn online which kinds of block are good victims, as proposed by Souza and Freitas:
* [P24] M. A. Souza, H. C. Freitas, "Reinforcement Learning-Based Cache Replacement Policies for Multicore
  Processors," *IEEE Access* 12, 2024;
* [T21] the PhD thesis behind it;
* [S19] the SSCAD 2019 paper.

### A.2 Why coherence state and sharing could help (hypotheses that motivate the work)

These are reasons to expect a signal, not results:
* **Shared (S) blocks may be invalidated before reuse.** Another core may soon write the line, and that write
  invalidates our copy anyway. Keeping such a block wastes a way.
* **Modified (M) blocks cost a write-back when evicted.** They may also be the ones this core is actively working
  on.
* **Exclusive (E) blocks are clean and private.** Evicting one is cheap, but it may be reused.
* **Heavily shared blocks** may be read again by this core soon, or their eviction may cost more coherence traffic
  later.
* **Which of these dominates depends on the workload and its phase.** That is the argument for learning rather than
  a fixed rule.

The project tests whether any of this turns into fewer misses, and whether learning adds anything over a fixed
preference.

### A.3 What success would mean

The criteria were agreed in the audit (§9) and refined after Stage 6:
1. **Correctness:** the policy must never break the MESI protocol, and its learning events must be correctly
   classified. This criterion was met (Stage 5).
2. **Cache effect:** fewer L2 misses than LRU, outside the run-to-run noise. Several policies meet it on Ocean and
   LU `tiny` (Stage 6).
3. **Performance:** lower execution time. No policy met it (Stage 6).
4. **Learning adds value:** the online learner must beat the no-learning controls: frozen weights, a fixed priority
   and random departures within the same window. Not met: the fixed priority and frozen weights did better (Stage 6).

---

## B. Implementation

### B.1 What we implemented in Sniper (overview)

| Layer | What | Where |
|---|---|---|
| Pre-RL fixes and instrumentation | MESI fill-state fix; delivered sharer hint; sharer-count validation; victim-state statistics; line tracer; stale-write-hit checker | commits `aedc1882`, `6b5cbd68`, `9b80650e`, `dfc1868f`, `27e0d326`; `common/core/memory_subsystem/parametric_dram_directory_msi/cache_cntlr.cc` |
| Algorithm cores (header-only, simulator-independent) | `PaperPolicy` (Souza–Freitas reproduction), `RewardPolicy` (our modified learner), eviction history, LRU order, features | `common/core/memory_subsystem/cache/rl/` |
| Sniper adapter | `CacheSetRL` / `CacheSetInfoRL`, policy names `sf_lru` and `rl_lru` | `common/core/memory_subsystem/cache/cache_set_rl.{h,cc}` |
| Event hooks | `onMiss`, `onEviction`, `onFill`, `onInvalidate(reason)` on every `CacheSet` (no-ops by default) | `cache/cache_set.{h,cc}`, `cache/cache_set_events.h` |
| Diagnostics (observation-only) | reward and pass histograms, miss classification, weight snapshots, a write oracle | `cache/rl/rl_diagnostics.h`, `cache/rl_write_oracle.{h,cc}` |
| Checkers | event, policy, sharer and per-line coherence checks | `experiments/sharer-counts/check_*.py` |
| Experiment tooling | the runner with manifest and checks; summaries; SPLASH-2 and CAP Bench builds | `experiments/rl/` |
| Stage 6 controls (merged) | `selection=learned|priority|random`, `learning=false`, `initial_weights` | `rl_reward_policy.h` |
| Stage 7 estimator (merged, opt-in) | `estimator=discounted_mean`, `horizon` | `rl_reward_policy.h` |
| Stage 7 invalidation feedback (**unmerged**) | `protect_invalidation_feedback` | branch `review/stage7-protect-invalidation` |
| Stage 7 trace and offline model (**unmerged**) | `rl_trace/*` options; `experiments/rl/headroom/` | branch `review/stage7-headroom-trace` |

### B.2 The cache hierarchy and MESI in our configuration

**Machine** (`config/gainestown.cfg` including `config/nehalem.cfg`):
* 4 cores at 2.66 GHz;
* 64-byte lines;
* inclusive hierarchy.

| Cache | Size and organisation | Sharing | Replacement in our runs |
|---|---|---|---|
| L1-I | 32 KB, 4-way | private | LRU |
| L1-D | 32 KB, 8-way, 4-cycle data access | private | LRU |
| **L2** | **256 KB, 8-way (512 sets)**, 8-cycle data, 3-cycle tags | **private** | **the policy under test** |
| L3 | 8 MB, 16-way, 30-cycle data and 10-cycle tags (about 35 cycles total per the config comment) | shared by the 4 cores | LRU |

Every experiment also uses:
* all prefetchers off (`perf_model/{l1_icache,l1_dcache,l2_cache}/prefetcher=none`);
* `traceinput/num_runs=1`;
* the checker environment `SNIPER_RL_EVENT_CHECK=1 SNIPER_COHERENCE_CHECK=1`.

**How MESI works here.** Code facts, from `parametric_dram_directory_msi/cache_cntlr.cc`:
* **Only M is writable** (`cache/cache_state.h:36`). A store to an S or E line in a private cache is an **upgrade**:
  * the private copy is invalidated (L1 `:1014`, L2 `:1721`);
  * the write goes to the next level, which invalidates the other cores' copies;
  * the line is refilled in M.
  * There is no silent E→M in a private cache. The shared L3, as the last level, upgrades in place.
* **Read-fill state.** A private L2 reading from the L3 gets S if another core's L2 holds the line. Otherwise it gets
  the L3's own state: E, or **M if the L3 copy is dirty**. This is the MESI fill fix (`copyDataFromNextLevel`,
  `:1255–1283`; `otherUpperCacheHolds`, `:2760`).
* **Inheritance and inclusion.** An L1 copy takes its L2's state. An L2 eviction back-invalidates the L1, and an L3
  eviction back-invalidates the L2s.
* **Consequence for the features:** "M" means *dirty in this core or dirty below*, not only "written by this core"
  (audit I-3).

**The MESI fill fix** (pre-RL; `COHERENCE_VALIDATION.md`). Upstream Sniper 9.0 copied the L3's E or M state into a
reading private cache even while other cores held the line. A later write by that core then hit silently and never
invalidated the other copies: a real coherence violation (measured, with a deterministic regression test).
`27e0d326` grants S when another upper cache holds the line.

**Delivered sharer hint** (`9b80650e`; `cache_cntlr.cc:1300–1331`). On a fill from the shared L3, the L3 reports how
many *other* cores' L2s its presence bits list for the line. The private L2 stores **others + 1** in the block
(`CacheBlockInfo::m_sharer_hint`). A write hit resets it to 1 (`:1634–1641`). The hint is realistic: the bits ride on
the response. It is also stale: it is not updated when other cores later fetch or drop the line.
* **Threshold:** "above threshold" (heavily shared, AT) iff hint > cores/2, so **≥ 3 of 4** (`rl/rl_features.h:33`).
  Otherwise "below" (BT).

### B.3 The original RL algorithm (`rl_lru`, `rlrepl::RewardPolicy`)

This is "our modified algorithm". It comes from the earlier ChampSim `rl_mesi` and is ported with the defaults agreed
in the audit, §6. Its core is `common/core/memory_subsystem/cache/rl/rl_reward_policy.h`.

**Features of a block** (`rl_features.h:40`, `make_features`):
* the MESI state, S, E or M (M/O → M, S and SHARED_UPGRADING → S);
* the sharing class, BT or AT, from the delivered hint.

The oracle sharer count is never used in decisions (Stage 5 verified this).

**Weights.** Five numbers in [−1, 1]: `wS, wE, wM` (state group) and `wBT, wAT` (sharing group). They start at 0.
With `weight_scope=global` (the default) there is **one table per L2 cache**, shared by its 512 sets but not by the
cores (`WeightTable`, `:185`). `per_set` exists but was never evaluated.

**Score:** `V(b) = w_state(b) + w_sharing(b)` (`WeightTable::score`). **Higher means more attractive to evict.**

**Victim selection** (`RewardPolicy::choose`, `:384`):
1. List the eligible ways oldest first (LRU order; a `SHARED_UPGRADING` way is never eligible).
2. Take the **N = 4 oldest** (the "window").
3. Evict the highest score. Ties go to the older block (strict `>` while scanning oldest first). So with all weights
   equal (initially 0) the policy **is exactly LRU**.
4. The candidates older than the victim are **"passed over"**. Their recency is *not* changed (no promotion), so they
   stay in the window at the next miss.

**Protection** (`commit`, `:419`): each passed-over block is marked protected, its decision-time features are saved
(a "snapshot"), and its pass counter grows.

**Eviction history** (`rl_eviction_history.h`). One FIFO per set of **16 entries** (2 × associativity). Each entry
holds the victim's tag and its decision-time features.

**The four rewards**, each applied to the *saved* features, never to the block's current ones:

| Reward | When | Value | Meaning it is meant to carry | Code |
|---|---|---|---|---|
| evict-bad | a demand miss on a tag that is still in the set's history | **−1** | "this victim was needed again soon: keep this class" | `on_miss`, `:464` |
| evict-good | a history entry is pushed out by the 16th later eviction in the set | **+1** | "this victim was not needed for 16 evictions: evict this class" | `on_eviction`, `:447` |
| protect-good | a protected block is hit | **−1** | "protecting it was right: keep this class" | `on_hit`, `:501` |
| protect-wasted | a protected block is evicted without a hit | **+1** | "protecting it was wasted: evict this class" | `commit`, `:438–442` |

**Events that give no reward:**
* coherence invalidations;
* back-invalidations;
* upgrades: the metadata is carried across the invalidate-and-refill;
* write-backs.

**Order on a miss** (a bug fixed in Stage 1, `715366bc`): the incoming block's history lookup, with its −1, comes
*before* the victim is chosen and inserted (`POLICY_CORES.md`, "Event lifecycle of a miss").

**Update rule** (`WeightTable::reward`, `:240`): the rewarded snapshot's state weight and sharing weight each move
`w ← w + α (r − w)` with **α = 0.1**. That is an exponential moving average of the rewards of each feature, so every
weight stays in [−1, 1] and tracks the recent mean reward of its class.

**Defaults** (`rl-n4` in `experiments/rl/run_stage6.py:73`): `mode=both`, `window=4`, `alpha=0.1`,
`protect_feedback=true` and `weight_scope=global`. Not ported: Iprev, the adaptive threshold, BRRIP dueling and
ε-exploration.

**Worked example:** `ALGORITHM_AUDIT.md` §3.2 and the `POLICY_CORES.md` "Worked scenario" (which is also a unit
test). Study it by hand. It shows a non-LRU victim, protection, an age-out +1, a protect-good −1 and a history −1.

### B.4 The paper reproduction (`sf_lru`, `rlrepl::PaperPolicy`)

The paper gives **no update equation**: its weights are the fractions of history entries in each class, per set
(`compute_ratio_weights`, `rl_paper_policy.h:82`).
* **Selection:** LRU's candidate #1 is checked. If it fails, candidate #2 is evicted **unchecked**.
* **Ambiguity:** the paper's flowchart and its prose disagree on whether a block of the dominant class is kept or
  evicted, so both readings are options: `rule=text|example` and `combine=and|or` (audit §2.2, I-9, I-10).
* **Outcome:** it was implemented for fidelity and compared in Stage 6, where it was clearly worse than LRU.

### B.5 Key files and functions

See [`CODE_TOUR.md`](CODE_TOUR.md) for a guided reading list.

| File | Functions | Why it matters |
|---|---|---|
| `common/core/memory_subsystem/cache/rl/rl_reward_policy.h` | `priority_rank` `:102`, `random_draw` `:129`, `WeightTable` `:185` (`reward` `:240`, `discounted_update` `:200`), `RewardPolicy::choose` `:384`, `commit` `:419`, `on_eviction` `:447`, `on_miss` `:464`, `on_fill` `:480`, `on_hit` `:501`, `on_invalidate` `:515` | the whole learner and its controls |
| `cache/rl/rl_features.h` | `above_threshold` `:33`, `make_features` `:40` | features |
| `cache/rl/rl_eviction_history.h` | `remove` `:52`, `insert` `:63` | the 16-entry history |
| `cache/rl/rl_paper_policy.h` | `compute_ratio_weights` `:82`, `PaperPolicy::choose` `:200` | the paper reproduction |
| `cache/cache_set.cc` | `CacheSet::invalidate` `:127`, `CacheSet::insert` `:144` (calls `onMiss`, `getReplacementIndex`, `onEviction`, `onFill`) | where Sniper calls the policy |
| `cache/cache_set_rl.cc` | `getReplacementIndex` `:465`, `updateReplacementIndex` `:540`, `onMiss` `:560`, `onEviction` `:623`, `onFill` `:661`, `onInvalidate` `:683` | the adapter between Sniper and the cores |
| `parametric_dram_directory_msi/cache_cntlr.cc` | `copyDataFromNextLevel` `:1255` (fill fix, hint), upgrade invalidations `:1014`, `:1721`, `invalidationReason` `:2171`, `insertCacheBlock` `:2228`, `otherUpperCacheHolds` `:2760` | MESI and the event classification |
| `experiments/rl/run_stage6.py` | `POLICIES` `:67`, `WORKLOADS` `:115`, `command` `:173`, `run_one` `:263` | how every experiment was run |

---

## C. Stages 1–5: building and validating the policies

Stages 1–5 run from 2026-11-19 to 2026-11-21.

| Stage | Implemented | Important design decisions | Validation | Limitations left open |
|---|---|---|---|---|
| **Pre-RL** (before Stage 1) | MESI fill fix; delivered sharer hint; L3 presence-tracking validation; victim-state statistics; line tracer; `SNIPER_COHERENCE_CHECK` | Fix the protocol before building on it. Use a realistic delivered hint instead of an oracle | Deterministic `coherence_regress` test; sharer-count suite (`COHERENCE_VALIDATION.md`) | The hint goes stale: hint exactness at victims ranged from 70.7% to 99.8% (audit I-20) |
| **Audit** (`c1472c51`) | `ALGORITHM_AUDIT.md`: the sources, our ChampSim implementations, 21 ranked issues | Global weights, α = 0.1, fixed threshold > N/2, optional features off, both paper rules preserved, delivered hint only | Review | I-1 (pinning), I-2 (coherence misses blamed on evictions), I-4 (fast global weights), I-6 (double reward): to be measured, not fixed |
| **1** (`4b2e6db6`, fix `715366bc`) | Header-only cores with deterministic unit tests, ported from ChampSim tests 446 and 447 | choose/commit split; no random numbers; the incoming block's history lookup must precede the victim | Unit tests, including the worked scenario; `test_event_order.cc` reproduces the ordering bug | – |
| **2** (`8c44afdf`) | No-op event hooks in Sniper with a classification: invalidation reasons (coherence, back-invalidation, upgrade) and fill kinds (demand, upgrade refill) | `Transition::UPGRADE` from another cache is a *coherence* invalidation, not an own upgrade (the plan said otherwise and was wrong) | LRU statistics unchanged | – |
| **3** (`ec227e95`) | `CacheSetRL` and the policies `sf_lru` / `rl_lru` | Upgrades carry the metadata over; `SHARED_UPGRADING` is never a victim; the RL policies refuse the auxiliary tag directory | N = 1, `alpha=0`, or mode off give exactly LRU's victim sequence (equal insert hash); repeated single-core replays are identical | Weight saturation and long pass runs observed (max passes 26) |
| **4** (`12eda347`) | Observation-only diagnostics: pass-run histograms, pinned ways, tie and flip counts, rewards by class, miss classification, write oracle, weight snapshots | Diagnostics must not change any decision | Identical victims with diagnostics on and off; observer totals equal the cores' counters | First measurements of I-1, I-4 and I-6 (small runs, indications only) |
| **5** (`20ef93de`; crash fix `fa8d21b6`) | A coherence and learning-event validation suite; per-line MESI check (`line_watch`) | Separate per-line invariants from noisy aggregate thresholds | **700 simulations.** 144/144 `coherence_regress` pairs pass the per-line check on all 256 lines; 0 stale write hits; every event and policy check passes. 8 aggregate failures, each explained | 4 SIGSEGVs: pre-existing races in Sniper's trace front end, fixed by `fa8d21b6` (`CRASH_INVESTIGATION.md`) |

**The learning-event correctness evidence** (`STAGE5_VALIDATION.md`, checked on every run):
* every replacement eviction is reported once;
* invalidations create no history entry;
* upgrades give no false −1, and their metadata survives;
* no `SHARED_UPGRADING` victim;
* the oracle never influences selection: recorded AT evictions equal the delivered-hint count, not the oracle's.

**Outstanding from Stages 1–5** (`POLICY_CORES.md`, "Outstanding issues"; `STAGE5_VALIDATION.md`):
* **No-eligible-victim fallback to way 0.** It is possible only on a shared last-level or SMT cache. It was never
  observed, and it is checked as 0 in every run.
* **One pending upgrade slot per set.** This is safe only when one hardware thread uses the cache. It is checked as
  0 in every run.
* **Not tested:** SMT, a shared L2 or LLC with an RL policy that makes decisions, prefetchers, the L1-I, more than 8
  cores, and the paper's perfect L3.
* **Invalidations that bypass the hooks:** passthrough caches, which are off in every run.
* **Multicore nondeterminism.** Live 4-core runs are not repeatable, so every comparison needs repetitions.
* **Repeated application runs** (`num_runs > 1`) hang. This is pre-existing. Two alternative fixes are unmerged on
  `review/simulator-races` and `review/multirun-reset`, and every experiment uses `num_runs = 1`.

---

## D. Stage 6: experiments

### D.1 Benchmarks and configuration

**Fixed for every run:**
* 4 Gainestown cores;
* the policy on the private L2 only;
* 8 MB shared L3;
* prefetchers off;
* `--roi` for SPLASH-2 and CAP Bench;
* checkers on every run, and a manifest that records the commit, binary md5, workload md5, command and checker
  verdicts.

**Runner:** `experiments/rl/run_stage6.py`.

| Workload | Input | Instructions (ROI) | What it can and cannot test (measured, `STAGE6_PILOTS.md`) |
|---|---|---|---|
| `sharetest` r/u | microbenchmark | – | Streams 1 MB per thread; 52–55% cold misses; nearly constant features. **A correctness test, not a performance workload** |
| FFT `-m16` | 2^16 points | 233 M (whole run) | Has reuse (41% history hits), but L2 MPKI is about 2 and 89–92% of victims are M |
| Radix, LU (SPLASH-2) `small` | – | 46 M, 238 M | Low L2 MPKI (2.6, 1.4) and constant features; sanity checks only |
| **Ocean `small`** (SPLASH-2) | 258 × 258 | 273 M | **The only memory-bound workload:** L2 MPKI 21.8, 73% of L2 hits at LRU positions 4–7, overflows the L3 (1.2 M DRAM write-backs). But 86% of victims are M and only 1.2% are heavily shared |
| Barnes (SPLASH-2) | 4,096 bodies | 372 M | The only workload whose features vary (54% S victims, 10.7% heavy). But L2 pressure is low: about 42 decisions per set over the whole ROI |
| CAP Bench KM `paper` | – | 1.77 G (iteration 2) | Almost no replacement pressure |
| **CAP Bench LU `tiny`** | exploratory | 717 M | Reuse just past the L2's capacity (79% of hits at positions 4–7). **Exploratory only:** the upstream pivot defect makes the factorisation unreliable (`CAPBENCH.md`) |

**Policies:**

| Name | Meaning |
|---|---|
| `lru` | baseline |
| `sf-text`, `sf-example` | the paper reproduction, the two rule readings |
| `rl-n2`, `rl-n4` | the online learner, window 2 or 4 |
| `rl-n4-frozen` | rl-n4 with learning off and fixed weights S −0.09, E −0.38, M −0.76, BT −0.64, AT 0. These are the time-averages of rl-n4's weights on LU, so they are in-sample for LU and out of sample for Ocean |
| `rl-n4-prio` | learning off and a **fixed priority**: S-AT > S-BT > E-AT > E-BT > M-AT > M-BT (evict Shared first, Modified last), chosen within the same 4-oldest window (`priority_rank`, `:102`) |
| `rl-n4-rand` | learning off; the oldest candidate, except that with probability 0.083 one of the other 3 is chosen uniformly. **This is not fully random replacement:** it is a "random departure from LRU within the window" control, at LU's departure rate |

### D.2 Results (measured; changes against the same batch's LRU mean)

**Pilot 3, Ocean and Barnes** (`results/stage6-ocean-barnes-pilot`, `STAGE6_PILOTS.md`):

| Policy | Ocean L2 MPKI | Ocean IPC | Barnes L2 MPKI | Barnes IPC |
|---|---|---|---|---|
| sf-text | +11.4% | +0.22% | +4.0% | −0.10% |
| sf-example | +17.9% | 0.00% | +5.6% | −0.06% |
| rl-n2 | −0.70% | −0.03% | −0.13% | +0.02% |
| rl-n4 | −0.85% | +0.04% | +1.0% | −0.07% |

LRU's own IPC spread is 0.20% on Ocean.

**LU `tiny`, five-policy pilot** (3 repetitions; `CAPBENCH.md`):

| Policy | L2 misses | Simulated time |
|---|---|---|
| sf-text | +46.3% | +1.77% |
| sf-example | +73.1% | +2.22% |
| rl-n2 | −2.0% | −0.10% |
| rl-n4 | −5.1% (outside LRU's range in all 3 repetitions; not a significance claim) | −0.14% |

**No-learning ablations** (the decisive Stage 6 experiment; `CAPBENCH.md`):

| | rl-n4 | rl-n4-frozen | rl-n4-prio | rl-n4-rand |
|---|---|---|---|---|
| **LU `tiny`:** L2 misses | −3.2% | −4.7% | **−8.1%** | −4.8% |
| LU `tiny`: simulated time | +0.04% | +0.03% | −0.14% | −0.12% |
| LU `tiny`: departure from LRU | 8.3–8.5% | 8.4% | 8.7% | 8.2% |
| **Ocean `small`:** L2 misses | −1.09% | **−2.37%** | **−2.24%** | −0.40% |
| Ocean `small`: dirty evictions | −1.97% | −4.51% | −4.44% | −0.70% |
| Ocean `small`: coherence invalidations | −2.08% | −16.6% | −16.4% | +1.08% |
| Ocean `small`: downgrades | +2.41% | +12.8% | +13.0% | +0.87% |
| Ocean `small`: simulated time | +0.32% | +0.04% | +0.32% | +0.19% |
| Ocean `small`: departure from LRU | 16.9% | 15.6% | 15.6% | 8.3% |

* **Ocean LRU noise:** 4 LRU runs span 0.42% in L2 misses and 0.28% in time.
* **Order of L2 misses,** the same in both repetitions: lru > rand > rl-n4 > {prio, frozen}.
* **Size of the gap:** frozen and priority beat rl-n4 by about 54–92 k misses, several times the largest
  within-policy spread (about 15 k).

### D.3 What was demonstrated and what was not

**Demonstrated** (measured, 2–3 repetitions, one configuration):
* **The policies run correctly** with coherence on, at Ocean scale: every checker passes in every run, with 0 stale
  write hits.
* **The paper reproduction (`sf_lru`) increases L2 misses** strongly on Ocean, Barnes and LU `tiny`.
* **rl-n4 reduces L2 misses slightly** on Ocean (−0.85% to −1.09%) and LU `tiny` (−3.2% to −5.1%), in every
  repetition.
* **Fixed rules beat the learner:** a fixed MESI preference within the same window (priority) and the frozen weights
  reduce L2 misses more than the online learner, in every repetition on both workloads.
* **Coherence information carries some signal:**
  * on LU, at a matched departure rate, the priority saved about 3.3 points more than random departures;
  * on Ocean, the state-aware fixed rules traded about 16% fewer invalidations for about 13% more downgrades.

**Not demonstrated:**
* **Any execution-time improvement.** Every policy's time is within the run-to-run spread.
* **That online learning helps.** The opposite was observed on these two workloads.
* **That the sharing feature helps.** Ocean almost never exercises it (about 1.2% heavy victims). Barnes exercises it
  but has too few decisions per set to train or judge it.
* **Statistical significance** of any difference.
* **Generality:** 16 cores, the paper's perfect L3, the policy on L1 and L2 together, other workloads. The paper's
  CAP Bench results were **not** reproduced: no policy comparison ran on a non-defective CAP Bench kernel at the
  paper's inputs (`CAPBENCH.md`, "Reproduction status").

### D.4 Why fewer L2 misses did not improve execution time

* **Measured, Ocean:** L3 misses and DRAM reads and writes stay within ±0.5% of LRU under every L2 policy, while the
  L2 policy changes L2 misses by −2.4% to +18%. Ocean is memory-bound because of its DRAM traffic, and the L2 policy
  does not change it. A saved L2 miss becomes an L3 *hit* (about 35 cycles), not a saved DRAM access.
* **Measured, LU `tiny`:** the 8 MB L3 holds the whole working set (L3 misses are the 33.4 k cold misses), and the
  kernel is compute-bound (IPC 8.8 on 4 cores). A saved L2 miss again saves only an L3 hit.
* **Estimate, Ocean** (the first-order upper bound, inputs shown):
  * the priority saves about 2.2% of about 5.96 M L2 misses, about 131 k, or about 33 k per core;
  * at about 35 cycles each, fully exposed, that is about 1.15 M cycles per core;
  * the run is about 43,400 µs × 2.66 GHz ≈ 115 M cycles, so the saving is about **1.0%** at most.

  An out-of-order core overlaps part of that latency, so the real effect should be smaller still. It is of the same
  order as LRU's own time spread (0.28–0.53%), so it would be hard to see with two repetitions even if real.
* **Observed and unexplained:** on Ocean, `sf-example` had 18% *more* L2 misses with no IPC change
  (`STAGE6_PILOTS.md`). How Ocean's time depends on L2 misses versus coherence events has not been established.
* **Implication:** with this hierarchy, an L2 policy can only change performance if it changes L3 or DRAM traffic, or
  on a workload where L2→L3 latency is on the critical path. A smaller L3 would add L3 back-invalidation effects
  (`STAGE6_PILOTS.md`, "Why not 1 MB first").

---

## E. Stage 7: why online learning underperforms, and the offline model

### E.1 Weight instability and bursty rewards

Measured from Stage 6 telemetry with no new runs (`STAGE7.md` §C).
* **The weights swing far more than noise would explain.** rl-n4's weights have a standard deviation of 0.64–0.74
  on Ocean, against 0.21–0.23 for an i.i.d. null model with the same mean rewards. 21–47% of snapshots are
  saturated (|w| > 0.9).
* **The state order is unstable.** The most common order of wS, wE, wM holds in only 17–19% of snapshots (null
  53–55%), and the order changes about 26 times per 1,000 decisions.
* **The reward stream itself is phase-structured, without learning.** The mean reward per 1,000-decision interval
  varies about 20× more than i.i.d. rewards would, and stays autocorrelated over about 5–50 k decisions. The
  no-learning controls show this too.
* **The memory is counted in events, not time.** α = 0.1 remembers about 10 rewards of each weight's own class. M is
  rewarded almost every decision (memory about 10 decisions), while S is rewarded rarely (about 160 decisions on
  Ocean) and AT even more rarely (about 660). Every decision therefore compares estimates of different stretches of
  time. Rarely rewarded weights go stale: on Ocean, the time-average of wS is +0.17 against a mean S reward of −0.10.
* **Protect-wasted is premature.** It is decided at the eviction, before the block's own history outcome is known,
  and 35–43% (Ocean) and 81–92% (LU) of these rewards are cancelled by the block's later −1 (§B). This is a real
  attribution defect, but protection is only 13–21% of all rewards.

### E.2 The discounted-mean estimator and why it failed

* **The change** (`rl/estimator=discounted_mean`, merged, opt-in; `rl_reward_policy.h:200`). Each weight is the mean
  of its class's rewards, discounted by *elapsed decisions*: `w = Σ e^{−(t−t_k)/H} r_k / Σ e^{−(t−t_k)/H}`, with
  H = 65,536. Every class then has the same memory in time, and phases shorter than H are averaged.
* **Verification:** unit tests, a differential test, deterministic replays and smoke runs.
* **Ocean pilot** (`results/stage7-ocean-pilot`, 12 runs; measured):
  * **The weights became stable.** Their sd fell to 0.13–0.27, order changes fell from about 26 to 0.1 per 1,000
    decisions, and the order predicted before the run (E > M > S) was learned and held in 69–72% of snapshots.
  * **The L2 misses got worse: +10.93% against LRU**, with +15.2% dirty evictions, 70% departure from LRU and about
    1.07 of 8 ways per set pinned. One block was passed over 1,855 times.
  * For comparison: α = 0.01 gave −1.84%; rl-n4 −1.28%; frozen −2.35%; priority −2.46%.
* **Interpretation:**
  * **Measured:** the stable weights learned to *keep* S and heavily shared blocks (AT −0.68 to −0.71), the opposite
    of the priority rule that does best.
  * **Hypothesis, later supported by E.5:** the reward itself points the wrong way. rl-n4's noisy weights had only
    broken the pins by chance.

### E.3 Excessive repeated protection

* **Code fact.** A passed-over block keeps its LRU position. So if its class has the lowest score in its window, it
  is passed over at every miss until it is hit, invalidated, or no higher-scoring candidate remains.
* **Measured:** under the discounted mean, residencies of ≥ 33 passes were 6.5–8.7% of residencies but held 72–78%
  of all passes, at about 0.0007–0.0008 hits per pass. Holding a way is worth about 0.21 hits per set-decision on
  Ocean (H7/dec), so the yield is about 250× too low (objective investigation, §3).
* **Measured:** the pinning is a symptom of the ordering keeping a *rare* class (E4). The priority rule keeps M,
  about 85% of blocks, so its windows almost always hold other M blocks, and it pins 0.001 ways per set.

### E.4 Invalidation-feedback correction (partial improvement, unmerged)

* **The change** (`rl/protect_invalidation_feedback`, branch `review/stage7-protect-invalidation`). A protected line
  removed by a coherence invalidation or a back-invalidation before any hit gets +1, once. Previously such a pin
  ended with no reward.
* **Ocean comparison** (`results/stage7-ifb-ocean`, 8 runs; measured):
  * L2 misses went from +11.15% to **+5.65% against LRU**, about half the excess removed;
  * pinning went from 1.16 to 0.78 ways per set;
  * departure from LRU went from 75% to 58%;
  * priority was −2.04% in the same batch.
* **Against the pre-stated rule,** pinning stayed above 0.5 ways per set, so the missing invalidation feedback is
  *not* the main cause of the pinning.
* **Verdict:** partially helps, still worse than LRU. Not merged.

### E.5 Why the reward objective may be misaligned with replacement quality

Objective investigation, `results/stage7-objective-investigation/REPORT.md`. The code facts and measurements behind
the conclusion:
1. **Every reward judges one block's own future** (code fact). No reward refers to the candidate kept or evicted
   *instead*. A weight estimates E[r | class], an absolute outcome under the current policy, not "was evicting this
   class better than the alternative?"
2. **"Good eviction" means "not reused within 16 evictions of this set",** not "not reused". On Ocean, misses on
   aged-out blocks equal **93% of all +1 rewards**: nearly every "good" eviction is followed by a miss, just later
   (measured, E1).
3. **The best policy's own experience points the reward away from it** (measured, E2). Under the fixed priority,
   evicted S blocks get a mean reward of −0.45 ("keep S"), because 72–73% return within the history, and M gets
   +0.17. A learner trained on these labels moves *towards keeping S*: what the discounted mean learned, with +11%
   misses. Priority's gain comes from *which* blocks it keeps: M blocks at LRU positions 6–7.
4. **The protection reward ignores how long a block was held** (code fact): 1 pass and 900 passes ending in a hit
   both give −1 (E3).
5. **Coherence bias in evict-bad** (measured, E5). 11–20% (Ocean) and 30–33% (LU) of S returns follow another core's
   write. They would have missed anyway, yet they count as −1. It is secondary: removing them does not flip the sign
   of S on Ocean.
6. **Not all of the benefit is coherence-specific** (measured, E6). Random departures alone reduced misses (LU
   −4.8%, Ocean −0.35%) because strict LRU thrashes at this capacity edge. A thrash-resistant, non-coherence control
   is missing from the comparisons.

**Conclusion of that report:** the current formulation is fundamentally inadequate, not merely missing an
opportunity-cost term. It recommended measuring the right target offline before any redesign. That recommendation
led to E.6.

### E.6 The offline trace and shadow-cache model

All of this is on branch `review/stage7-headroom-trace`, documented in `docs/rl/HEADROOM.md`.

**Purpose:** answer three separate questions about how much headroom the existing features leave (HEADROOM.md §1):
* **A.** For each recorded decision, which of the 4 candidates should have been evicted in hindsight?
* **B.** Which fixed ranking of the six MESI × sharing classes gives the fewest misses?
* **C.** Could an adaptive use of the same features do better?

B and C need a *counterfactual*: what the cache would have done under another policy.

**Trace** (simulator side, observation-only, opt-in):
* options `perf_model/l2_cache/rl_trace/enabled` and `set_modulus`;
* files `cache/rl_trace.{h,cc}` and `cache/rl/rl_trace_core.h`;
* contents: every L2 access, decision (with its candidates), fill, invalidation (with its reason), way state, L1
  event and ROI marker, for the sampled sets (here every set).
* **Verified:** deterministic replays are identical with tracing on and off, and trace tallies equal `sim.stats`.

**Offline tools** (`experiments/rl/headroom/`):

| Tool | Role |
|---|---|
| `rltrace.py` | trace reader |
| `shadow.py` | the shadow cache model |
| `labels.py` | window labels (question A) |
| `headroom.py` | CLI: `check`, `replay`, `calibrate`, `orders`, `phase` |
| `calibration_summary.py` | calibration summary and verdict |
| `test_headroom.py` | unit tests: 26 pass |

**What the shadow model does.** It replays a recorded L2 stream through a model of the private L2s under another
policy: LRU, priority, any fixed class order, or the hindsight oracles. It must model what the other policy would
change:
* **L1 inclusion** (phantom and suppressed accesses);
* **coherence invalidations and downgrades** of copies only the shadow holds;
* **fill states**;
* since Stage 7's investigation, **MESI write permissions** (the permission model, below).

**Limitations by design** (HEADROOM.md §2.4):
* other cores' streams and the timing are taken as recorded;
* only the first later L1 use is known;
* the L3 is not modelled (its state is inferred);
* rl_lru, whose decisions depend on its own reward history, is not replayed;
* timing-dependent traffic (spinning) is not modelled.

**Calibration** is what makes it falsifiable. Replay priority on an LRU trace, and LRU on a priority trace, and
compare the predicted priority-vs-LRU miss difference with the measured one. The criteria, fixed in advance:
* the self-replays are exact;
* null pairs (two runs of one policy) differ by at most 0.25 points;
* all 8 predictions are within ±0.5 points.

### E.7 The calibration history

| Step | Data | Result |
|---|---|---|
| First calibration (`results/stage7-headroom-calibration`) | 4 new traced Ocean runs (LRU ×2, priority ×2), every set | **FAIL:** self-replays exact and noise 0.08–0.10 points, but all 8 predictions under-estimated priority's benefit (+0.24 to +0.51 points), one beyond 0.5 (+0.508) |
| Investigation (`results/stage7-headroom-upgrade-investigation`; HEADROOM.md §7.7) | the same 4 traces only | Priority removes about 12 k S/E→M store upgrades, about 9% of its miss reduction. It evicts S copies, so their refetch finds no sharer and the dirty L3 grants M, and the later stores hit in the L1. The shadow had two rule mistakes and no L1 write-permission rule. **Correction:** an opt-in `model_permissions` derived from Sniper's code, with exact self-replays. On these *development* traces the errors fell to +0.03 to +0.32 |
| **Held-out calibration** (`results/stage7-headroom-heldout`; HEADROOM.md §7.9) | **4 new traced runs**; model, tools and criteria frozen at `54e00fdc` before simulation | **PASS:** 8/8 errors within ±0.5 points, the largest 0.152; noise floor 0.165; self-replays exact; consistency checks pass. Independently recomputed from raw miss counts (`results/stage7-heldout-verdict-verification`) |

**Erratum in the held-out evaluation.** The frozen summary script could not parse a leading "+", so it printed
INCONCLUSIVE with a `nan` noise floor. That output is preserved. The corrected script differs by one pattern, and a
regression test covers it (HEADROOM.md §7.9).

### E.8 What the PASS means, and its limited scope

* **It is a model-validity result, not a replacement-policy result.** The shadow predicts the priority-vs-LRU miss
  difference to within about ±0.15 points on Ocean `small` (measured difference −2.1 to −2.3%).
* **Its scope:**
  * one workload, one configuration, one policy pair, two repetitions;
  * errors below the noise floor, so a residual bias of about +0.1 point (from LRU traces) cannot be separated from
    noise;
  * differences under about 0.5 points cannot be resolved.
* **Its use:** at most a limited offline *screening* of fixed MESI × sharing class rankings on this configuration.
  Every promising ranking must be verified directly in Sniper. **No screening has been run.**
* **What it does not support:**
  * a claim about adaptive policies (question C);
  * other workloads;
  * the oracle "headroom" analyses, none of which has been run.

---

## F. Current project status

### F.1 Algorithms: what exists and where

| Algorithm | Name or option | Branch | Status |
|---|---|---|---|
| LRU | `lru` | upstream | baseline, unchanged |
| Souza–Freitas reproduction | `sf_lru` (`sf/mode`, `rule`, `combine`) | `feature/rl-port` | implemented and validated; worse than LRU in every test |
| Online RL learner | `rl_lru` (`rl-n2`, `rl-n4`) | `feature/rl-port` | implemented and validated; slightly better than LRU in L2 misses on Ocean and LU `tiny`, worse than the fixed rules |
| No-learning controls | `rl/selection=priority|random`, `rl/learning=false`, `rl/initial_weights/*` | `feature/rl-port` | implemented and validated; **priority is the best measured policy** |
| Discounted-mean estimator | `rl/estimator=discounted_mean`, `rl/horizon` | `feature/rl-port` (opt-in) | stable weights, +11% L2 misses on Ocean; kept as an option |
| Invalidation feedback | `rl/protect_invalidation_feedback` | `review/stage7-protect-invalidation` (unmerged) | +5.65% L2 misses on Ocean; not merged |
| L2 trace and offline shadow model | `rl_trace/*`; `experiments/rl/headroom` | `review/stage7-headroom-trace` (unmerged) | held-out calibration PASS (limited scope) |

### F.2 Branches

| Branch | Head | State |
|---|---|---|
| `main` | `7500506e` | upstream Sniper 9.0 |
| `senior-design-rl` | `27e0d326` | pre-RL coherence work (MESI fix); merged into `feature/rl-port` |
| **`feature/rl-port`** | **`2363b52e`** | **main integration branch:** Stages 1–6, the Stage 7 estimator and the Stage 7 documentation |
| `review/capbench-tooling`, `review/stage6-pilots`, `review/stage6-ocean-barnes-doc`, `review/stage6-controls`, `review/stage7-discounted-mean` | – | reviewed and **merged** into `feature/rl-port` |
| `review/stage7-protect-invalidation` | `80972542` | **experimental, unmerged:** invalidation feedback |
| `review/stage7-headroom-trace` | `cf776026` | **experimental, unmerged:** trace, shadow model, calibrations |
| `review/simulator-races`, `review/multirun-reset` | `1c87e9b4`, `ed6e6af5` | **unmerged:** two versions of a fix for the `num_runs > 1` hang (not needed: `num_runs = 1`) |
| `docs/stage1-7-handoff` | – | this handoff material (documentation only, branched from `feature/rl-port`) |

### F.3 Validated configurations and key archives

* **Validated:**
  * 4-core Gainestown, any of the policies above on the private L2, 8 MB L3, prefetchers off, `num_runs = 1`, with
    the checkers;
  * single-core trace replays, which are deterministic (used for exact equivalence checks);
  * 2–8 cores in the Stage 5 coherence suite.
* **Binaries,** preserved read-only in `results/` with their `.md5` files:

  | md5 prefix | Build | Used for |
  |---|---|---|
  | `192619ee` | validated pilot binary | Stage 6 pilots |
  | `deafea4e` | the LU ablation's binary | LU ablation |
  | `e65141e5` | the main tree's build | Ocean ablation |
  | `ae285766` | the merged build (current main tree) | Stage 7 pilot |
  | `784fc365` | the invalidation-feedback branch | ifb comparison |
  | `25290da9` | the trace branch | headroom calibrations |
  | `10f8c646` | `2363b52e` built unmodified | trace-verification baseline |
  | `d009e143` | the discounted-mean review build | estimator review |

* **Archives** (`results/archive/`, sha256 beside each):

  | Archive | Contents |
  |---|---|
  | `stage6-pilots` | sharetest and FFT pilots |
  | `stage6-splash2-qual` | SPLASH-2 qualification runs |
  | `stage6-ocean-barnes-pilot` | Pilot 3 |
  | `stage6-capbench-qual`, `stage6-capbench-qual2` | CAP Bench LRU qualification |
  | `stage6-capbench-lu-pilot` | LU `tiny` five-policy pilot |
  | `stage6-capbench-lu-ablation` | LU `tiny` no-learning ablation |
  | `stage6-ocean-ablation` | Ocean no-learning comparison |
  | `stage6-controls-verification`, `stage6-rebuild-verification` | control and rebuild checks |
  | `stage7-investigation` | weight-stability investigation |
  | `stage7-discounted-mean-verification` | estimator verification |
  | `stage7-ocean-pilot-prep`, `stage7-ocean-pilot` | Stage 7 Ocean pilot |
  | `stage7-protect-invalidation-verification`, `stage7-ifb-ocean` | invalidation feedback |
  | `stage7-objective-investigation` | objective investigation |
  | `stage7-headroom-verification` | trace verification |
  | `stage7-headroom-calibration` | first calibration (FAIL) |
  | `stage7-headroom-upgrade-investigation` | upgrade-miss investigation |
  | `stage7-headroom-heldout` | held-out calibration (PASS) |
  | `stage7-heldout-verdict-verification` | independent verdict check |

  All 22 archives verify against their checksums (2026-11-23).

### F.4 Known correctness concerns

None is known to affect the reported results.
* **Untested paths** (§C): no-eligible-victim fallback; one upgrade slot per set; SMT and shared-cache policies;
  prefetchers; L1-I; more than 8 cores.
* **Remaining ThreadSanitizer reports.** These are pre-existing Sniper set-lock patterns and possible cache-state
  races. They would add multicore nondeterminism equally to every policy (`CRASH_INVESTIGATION.md`).
* **Known noise sources:**
  * `L3.store-overlapping-misses` jitters, even in single-core replay, so it is excluded from exact comparisons;
  * `sim.info`'s `git_revision` is a configure-time label, so provenance uses the manifest's commit and binary md5.
* **Upstream workload defects:** CAP Bench LU's pivot search and IS's sort (`CAPBENCH.md`). LU `tiny` results are
  exploratory for that reason.

### F.5 Experimental limitations

* **Sampling:** two repetitions per policy; live multicore runs are not repeatable.
* **Workloads:** two workloads carry all the policy conclusions: Ocean `small` (validated) and LU `tiny`
  (exploratory). Ocean barely exercises the sharing feature, and Barnes, which does, is too thin to train on.
* **Configuration:** one, with 4 cores, a real 8 MB L3, and the policy on the L2 only. The paper used 16 cores, a
  perfect L3, and L1 + L2.
* **Controls:** the frozen weights and the priority were both motivated by LU findings. The random control's rate
  was not matched to rl-n4's on Ocean.
* **Measurement:** no hardware cost (latency, area, energy) is modelled for any policy (audit I-16).

### F.6 Unresolved research questions

1. **Can any learning on these features beat the fixed priority?** This is the open question of the project.
2. **Is the right learning target relative (pairwise) rather than absolute** (E.5)?
3. **On Ocean, is the priority's advantage due to evicting S first, E before M, or simply a stable order?** With S
   at 5–6% of victims the Stage 6 data cannot tell (STAGE7.md §D). The calibrated shadow could screen this, within
   its ±0.5-point resolution.
4. **Does the sharing feature carry value on a workload that exercises it with enough L2 pressure?**
5. **Can an L2 policy change L3 or DRAM traffic, and so performance,** on some workload or configuration?
6. **How much of the benefit is generic thrash resistance** rather than coherence awareness (E6)?

---

## G. Proposed Stage 8 (not started)

Nothing below is implemented or scheduled. These are directions to discuss.

### G.1 The candidate directions

| Direction | Idea | What it fixes (Stage 7 evidence) | Cost and risk | How it would be judged |
|---|---|---|---|---|
| **1. Pairwise (relative) rewards** | For each decision, compare the victim with the candidates kept. The pair is "right" if a kept block is used before the victim returns. Learn a Bradley–Terry or logistic preference, `P(evict a over b) = σ(w(a) − w(b))` | The absolute-label defects E1 and E2, by design. It approximates window-Belady online and causally | Medium: link each history entry to its kept candidates, plus a pending flag per line, possibly on sampled sets only. Risks: censored (unresolved) pairs; it may simply converge to the priority order | Must beat LRU on both workloads, come within 0.5 points of priority or better, and **learn a different order where the workloads differ**. Converging to S > E > M everywhere = "learning reproduces the fixed rule" |
| **2. Opportunity-cost-aware protection** | Price holding a block. Measure the marginal value of a way, m (hits per set-decision at the last LRU position, from a sampled LRU shadow directory), and give protect-wasted at L* = 1/m passes without a hit | E3 (unpriced occupancy) and the pins | Low to medium: a pass counter plus a small sampled tag directory. Drives towards LRU; leaves E1, E2 and E5 untouched; m is an average, not the specific sacrificed block | Pinning < 0.05 ways per set and misses below LRU on both workloads. It cannot by itself show learning value |
| **3. Better features** | Add information the current 6 classes lack: recency or age inside the window, the pass count, a "written locally" bit (so M splits into really written and M-on-read, audit I-3), reuse predictors (sampled-set Belady labels as in Hawkeye-style designs, or a signature if the L2 sees one) | E6 (generic thrashing), the M-mixing of I-3, and the low feature diversity (all window candidates tie in 71–72% (Ocean) and 82–83% (LU) of rl-n4's decisions, 84–88% under the fixed rules) | Varies; each feature needs a hardware justification. Risk: the gains become generic reuse prediction, not coherence awareness | An ablation per feature, with a non-coherence thrash-resistant control (for example a set-dueling or RRIP-like baseline) |
| **4. Phase adaptation** | Choose between policies per phase or per set group: set dueling between LRU and priority (and other fixed orders), or a slow selector between a few fixed rankings | The phase-structured reward stream (E.1), without trusting the per-class labels | Low: standard set dueling, a few counters. It adapts *which fixed rule* runs, not class values | Must beat the best single fixed rule on a workload whose best rule changes over time. On Ocean it can at best equal priority |

**How they relate:**
* 1 changes *what is learned*;
* 2 changes *how holding is priced*;
* 3 changes *what is observed*;
* 4 changes *how a fixed rule is chosen*.

They can be combined later. Each should be evaluated against the Stage 6 baselines (LRU, priority, frozen, rl-n4)
with the controls, and ideally on one workload that motivated nothing so far.

### G.2 A cheap step that the held-out PASS now permits (optional)

A limited offline screening of the 720 (or the 60 additive) fixed class orders on the existing Ocean traces, with the
calibrated shadow (`headroom.py orders`):
* **What it answers:** open question 3 (S-first vs E-before-M vs stability) at ±0.5-point resolution, without new
  simulations.
* **What it requires:** any promising order must be verified in Sniper.
* **Status:** this was not run.

### G.3 Recommended direction to discuss with the advisor

**Recommendation: pairwise (relative) rewards (direction 1) as the Stage 8 research contribution.**
* **Practical baseline:** a set-dueling LRU/priority selector (direction 4) as the low-risk counterpart, plus an
  honest comparison against the fixed priority.
* **Why direction 1:**
  * The evidence points at the *objective* rather than the estimator. Stabilising the weights (E.2) made things
    worse, because the absolute labels say "keep S" while the best fixed rule evicts S first (E.5).
  * Direction 1 is the only option that removes that defect by construction. It is causal and local to the L2.
  * It has a pre-statable success test: it must learn workload-dependent orders and beat or match priority.
* **What it does not fix:** pinning (E3), so it may still need direction 2's bound, which can be added as an
  ablation.
* **Why direction 4 alongside it:** it is the practical fallback. If pairwise learning only reproduces the priority
  order, set dueling between fixed rules is the simplest design that still adapts.
* **What to settle with the advisor first:**
  * whether the contribution should be a learning result or a coherence-aware replacement result (the fixed priority
    is already a positive cache-level result);
  * whether to add one workload that exercises sharing under real L2 pressure;
  * whether execution time or L2 misses is the primary metric, given D.4.
