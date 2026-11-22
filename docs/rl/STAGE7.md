# Stage 7: why online learning underperforms, and one opt-in variant

**Status:**
* **Investigation:** done, from existing Stage 6 statistics and telemetry only. No new benchmark simulation was run.
* **Variant:** one experimental, opt-in variant, on the review branch `review/stage7-discounted-mean`. It is
  verified with unit tests, a differential test, deterministic replays and short smoke tests, but **not evaluated**
  on any workload.
* **Unchanged:** the rl-n4 algorithm and every Stage 6 control stay as they are, and the default is the algorithm.

**Background:** on LU `tiny` and Ocean `small`, the frozen LU-derived weights and the fixed priority reduced L2 misses
more than online rl-n4 in every repetition ([`CAPBENCH.md`](CAPBENCH.md)).

## Sources

* **ROI statistics of the archived Stage 6 runs:**
  * `stage6-ocean-ablation`, all five policies × 2;
  * `stage6-capbench-lu-ablation`, all five policies × 2;
  * the Ocean `rl-n4` runs of `stage6-ocean-barnes-pilot` (2);
  * the `rl-n4` runs of `stage6-capbench-lu-pilot` (3).
* **Weight snapshots** (`telemetry/rl-snapshots-L2-<core>.csv`): one row per 1,000 decisions per core, with the
  five weights and the cumulative reward counters.
* **Controls:** they count every reward but apply none, so their reward streams describe what the workload and the
  reward definition produce without any learning feedback.
* **Scripts and outputs:** `results/stage7-investigation/` (git-ignored, read-only):
  * `scripts/stage7_investigation.py` writes `investigation.txt` and `investigation.csv`;
  * `scripts/stage7_bursts.py` writes `bursts.txt`.
* **Code reviewed:** `rl_reward_policy.h` (`WeightTable`, `RewardPolicy`), `rl_eviction_history.h`,
  `rl_diagnostics.h`, and the miss classification in `cache_set_rl.cc`.

**Reward recap** (`docs/rl/POLICY_CORES.md`):
* **History outcomes:** an evicted block that misses while still in its set's 16-entry history gets −1 (evict-bad);
  an entry pushed out of the history gets +1 (evict-good).
* **Protection outcomes:** a passed-over (protected) block that is hit gets −1 (protect-good); one evicted without a
  hit gets +1 (protect-wasted).
* **Update:** every reward moves the state weight and the sharing weight of the rewarded snapshot:
  `w ← w + α (r − w)`, α = 0.1.

## Findings

### A. Good-eviction rewards from history age-out

**Do +1 blocks come back?** The +1 is given when an entry ages out. `rl-diag-miss-replaced-not-in-history` counts the
misses on replaced blocks whose entry had already aged out.

| | Ocean `small` (all 10 + 2 pilot runs) | LU `tiny` (all 10 + 3 pilot runs) |
|---|---|---|
| +1 (aged-out) blocks that missed again later in the ROI | 92.9-93.0% | 37.5-38.3% |
| History outcomes whose block never returned in the ROI | 3.6-3.7% | 4.8-5.5% |
| Returns after age-out, share of demand misses | 46.1-46.9% | 2.1-2.2% |
| Another core wrote the block before its late return | 0.9-1.1% | 53.6-54.3% |

**Findings:**
* **On Ocean, a +1 almost never means "not used again".** It means "not reused within 16 further evictions of its
  set" (twice the associativity): Ocean's sweeps revisit nearly every block, mostly beyond the history's reach.
* **On LU, more than half of the late returns follow another core's write.** Those blocks would have been
  invalidated anyway.
* **The figures are the same for every policy** (rl-n4, frozen, priority, random), so they describe the workload,
  not the policy.

**Assessment: not shown to be misleading in direction.**
* **What the +1 measures:** whether the victim's next use lies beyond 16 more evictions in its set. Compared with a
  block reused within the history, that makes it the better victim, because keeping it would have required it to
  survive at least 16 further evictions in an 8-way set.
* **What it cannot measure:** among blocks that all return late, which returns sooner. Under Ocean's sweeps that is
  most of the information.
* **What the data cannot show:** whether a longer history would give better decisions. No run varies the history
  length.
* **Not the cause of the instability below:** the age-out reward is not where the instability comes from, though
  the alternation between −1-dominated and +1-dominated stretches is part of the burst structure (C).

### B. Protection rewards

| | Ocean rl-n4 (4 runs) | Ocean frozen / prio | Ocean rand | LU rl-n4 (5 runs) | LU frozen / prio | LU rand |
|---|---|---|---|---|---|---|
| Protection rewards, share of all rewards | 13.9% | 20.5% | 13.1% | 12.7% | 15-16% | 13-14% |
| protect-good (−1 at the hit) / protect-wasted (+1 at the eviction) | 171-175 k / 701-727 k | 330-343 k / 1,044-1,065 k | 106-107 k / 714-715 k | 45-47 k / 31-40 k | 58-60 k / 42-44 k | 53-54 k / 32-33 k |
| Wasted, then the block returned within the history (−1): contradicted | 41.1-41.6% | 34.8-36.1% | 43.1-43.2% | 80.7-82.1% | 91.5-91.8% | 70.9-71.4% |
| Wasted, then aged out (+1 again) | 57-58% | 63-64% | 56% | 17-18% | 7-8% | 18% |
| The two rewards concern different features | 0.1-0.2% | 0.1% | 0-0.1% | 0.5-0.7% | 1.2-1.6% | 1.8% |

**The timing of protect-wasted is premature.**
* **When it is decided:** at the protected block's eviction, before that block's own history outcome is known.
* **Two rewards per block:** every protected-then-evicted block therefore gets two rewards on (almost always) the
  same features:
  * +1, then −1 when it returns: contradicted, net zero;
  * or +1, then +1 when it ages out: the same evidence counted twice.
* **How often it is reversed:** on LU, 81-92% of protect-wasted rewards are cancelled by the block's own −1 shortly
  after; on Ocean, 35-43% are.
* **protect-good is fine:** it is resolved at the hit, when its outcome is known.

**Assessment:**
* **A real attribution defect,** but protection is only 13-21% of all rewards.
* **The contradicted pairs cancel in the mean.** They add variance but do not change the long-run per-feature
  means much.
* **Not the primary cause of the instability.**

**Repeated protection:**
* **It is longer under the fixed policies than under rl-n4.** Maximum passes on one block: Ocean rl-n4 55-111,
  frozen 320-328, priority 198-382; LU rl-n4 107-137, frozen and priority 166-168.
* **It is not a learning defect,** and it did not prevent the fixed policies from doing best.
* **Expect more of it with any stabilised learner** (see the design below).

### C. Weight stability

rl-n4 runs, second half of the ROI, the four cores pooled. Measured against a **null model**: the same EMA (α = 0.1)
fed i.i.d. ±1 rewards with each feature's long-run mean reward μ in that run (theory: sd = √(α/(2−α)·(1−μ²))).

| | Ocean (2 ablation + 2 pilot runs) | LU (2 ablation + 3 pilot runs) |
|---|---|---|
| Weight sd: observed / i.i.d. null | 0.64-0.74 / 0.21-0.23 | 0.38-0.73 / 0.12-0.23 |
| Snapshots with \|w\| > 0.9 | 21-47% per feature | 4-61% per feature |
| Lag-1 autocorrelation of w_M (1,000 decisions apart); null ≈ 0 | +0.71 to +0.74 | +0.29 to +0.38 |
| Snapshots in which the most common order of wS, wE, wM holds | 23-25% (null 53-55%) | 42-51% (null 82-84%) |
| Order flips of the state weights | 25-27 per 1,000 decisions | 25-28 per 1,000 decisions |

**The reward stream itself is phase-structured** (`bursts.txt`), measured per 1,000-decision interval and per core:

| | Ocean, every policy | LU, every policy |
|---|---|---|
| sd of the interval's mean reward (i.i.d. expectation) | 0.63-0.66 (0.028-0.029), about 22× | 0.40-0.43 (0.020-0.021), about 19-21× |
| sd across intervals of the share of history outcomes that were −1 | 0.35 | 0.21-0.22 |
| Autocorrelation of the interval mean reward at 1 k / 5 k / 10 k / 50 k decisions | +0.92 / +0.62 / +0.40 / −0.05 | +0.90 / +0.47 / +0.11 / −0.04 |
| Same for frozen, priority and random (no learning feedback) | yes: 22.3-22.8× | yes: 18.9-21.4× |

**How the weights follow the stream** (rl-n4):
* **Each weight tracks the stream:** w_M correlates with the interval mean reward at +0.78 (Ocean) and +0.62-0.67
  (LU).
* **The weights move together:** w_M with w_BT at +0.94 (Ocean) and +0.81-0.86 (LU); w_M with w_S at +0.43 to +0.46 and
  +0.32 to +0.38; w_M with w_E at +0.59 to +0.61 and +0.28 to +0.45.

**Memories are counted in events.** With α = 0.1 each weight remembers about 10 of *its own* rewards, so its memory in
decisions depends on its event rate:

| rl-n4, rewards per decision (memory in decisions) | S | E | M | BT | AT |
|---|---|---|---|---|---|
| Ocean | 0.063 (≈160) | 0.13 (≈80) | 0.97 (≈10) | 1.14 (≈9) | 0.015 (≈660) |
| LU | 0.13 (≈80) | 0.015 (≈690) | 0.96 (≈10) | 1.03 (≈10) | 0.063 (≈160) |

**Rarely rewarded weights go stale.** On Ocean, the time-average of a rarely rewarded weight differs even in sign from
the mean of its rewards:
* **S:** time-average +0.17, against a mean reward of −0.10;
* **AT:** +0.11 against −0.27.

For the frequent M the two agree better (+0.15-0.17 against +0.09-0.11).

**Conclusion for C: a combination, in this order.**
1. **The reward stream is strongly phase-structured.** Its interval means vary about 20× more than i.i.d. rewards
   would, and stay correlated over about 5-50 k decisions. The same holds without learning, so this is a property
   of the workload and the reward definition, not a feedback loop.
2. **α = 0.1 per event gives a memory of about 10 rewards,** so each weight follows these phases, often to
   saturation near ±1, instead of averaging them. That is what the sign changes are.
3. **The memory is counted in events, so features with different event rates remember different stretches of
   time.** A decision then compares, for example, an M weight from the last ~10 decisions with an S weight from the
   last ~160. This reorders the weights far more often than the null model predicts.

**Ruled out as the primary explanation:**
* **i.i.d. reward noise:** at α = 0.1 it would give an sd of 0.13-0.23 and no correlation across 1,000 decisions,
  where the observed sd is 0.38-0.74 and the lag-1 correlation +0.3 to +0.7.
* **Conflicting feature updates:** each reward moves one state and one sharing weight toward the same value. For two
  below-threshold candidates, the score difference does not involve the sharing weight at all, and BT simply tracks
  the same stream as M.
* **Reward quality:** it explains neither the size nor the time structure of the fluctuations. Its open questions
  (B, and the S question in D) remain.

### D. Feature usefulness

**Feedback per feature, rl-n4:**

| | Ocean | LU |
|---|---|---|
| Reward events per run: S / E / M / BT / AT | 0.34 M / 0.68-0.69 M / 5.28-5.30 M / 6.23 M / 0.083 M | 71-75 k / 7.1-9.5 k / 0.53-0.57 M / 0.58-0.62 M / 35-38 k |
| Long-run mean reward μ: S / E / M / BT / AT | −0.09 to −0.12 / +0.35 to +0.36 / +0.09 to +0.11 / +0.12 / −0.26 to −0.28 | −0.10 to −0.11 / −0.49 to −0.57 / −0.83 to −0.84 / −0.79 to −0.80 / −0.03 to +0.01 |
| All window candidates tied (rl-n4; frozen and priority) | 71-72% (84%) | 82-83% (87-88%) |
| Tied with different features (rl-n4) | 1.7-2.7% | 0-0.2% |

**Separating diversity from updating:**
* **Diversity limits how often a weight can matter.** In 71-83% of rl-n4's decisions every candidate has the same
  features (typically four M blocks below the threshold), so no weight can change the victim. The sharing feature
  is above threshold for 1.3% of Ocean's rewarded events and 5.8% of LU's.
* **The features do receive distinct feedback in aggregate.** The long-run means differ by much more than their
  sampling error, even allowing for the burstiness:
  * Ocean: E − M +0.27, AT − BT −0.38;
  * LU: S − M +0.72, AT − BT +0.76.
* **The update mechanism (C) is what fails to turn this into a stable order.** On LU the order of the long-run
  means, S > E > M (evict S first), is the order of the frozen vector and of the priority rule. The frozen vector is
  close to those means: S −0.09, E −0.38, M −0.76 against μ −0.10, −0.53, −0.83 in the pilot runs it came from. Yet rl-n4's weights hold that
  order in only 42-51% of snapshots.

**The open reward-quality question (Ocean):**
* **The disagreement:** the long-run means order E > M > S (keep S). Yet the two fixed orders that did best both
  evict S first (S > E > M).
* **Why the reward can disagree:** it judges the victim's own outcome (S victims return within the history more
  often), not the outcome of the candidate kept instead.
* **Why the data cannot settle it:** with S at 5-6% of victims, the existing data cannot separate three possible
  sources of the frozen/priority advantage on Ocean: evicting S first, evicting E before M, or simply holding a
  stable order.

## Candidate improvements

| | Addresses | Change | For | Against |
|---|---|---|---|---|
| 1. Smaller α (EMA) | C2 | configuration only (`rl/alpha`) | no code; lower variance | memory still counted in events: rare features keep 16-66× longer memories than M (C3); a zero start takes ~1/α rewards per feature to wear off (on Ocean, about 66 k decisions for AT at α = 0.001) |
| **2. Time-discounted mean** | **C2 and C3** | **each weight = discounted mean of its rewards, discount by elapsed decisions** | **same time span for every feature; averages phases shorter than the horizon; no per-decision work; exact, causal** | **slower to adapt (by design); one parameter (horizon)** |
| 3. Baseline-subtracted rewards | C1 (common phase) | learn E[r − b_t \| f], b_t a running mean of all rewards | removes the common-mode phase directly | keeps the per-event memory and its variance; a second estimator with its own rate; changes what a weight means |
| 4. Deferred protection outcome | B | resolve protect-wasted by the block's history outcome | fixes a real attribution defect | protection is only 13-21% of rewards, so it cannot remove the instability; needs a per-entry flag |
| 5. Counterfactual (relative) rewards | D (Ocean S question) | reward the victim relative to the candidates kept | targets the open reward-quality question | larger design (tracking the kept candidates' next use); not small or well-defined yet |

**Recommended: 2, the time-discounted mean.** It addresses both measured mechanisms of the instability, it is small
and exactly specified, and it uses only rewards already received.
* **Comparator:** 1 needs no code and should be run alongside 2 in an evaluation.
* **Follow-up:** 4 is the natural next step if B shows up as residual noise.
* **Later:** 5 belongs to a later step, once 2 has shown whether stability alone closes the gap.

## The recommended variant: `rl/estimator=discounted_mean`

**1. The weakness it addresses.** The per-event EMA (C):
* follows reward phases instead of averaging them;
* gives each feature a memory of a different length in time;
* therefore compares estimates of different stretches of time in every decision.

**2. The change.** For feature f with rewards r_k received at decisions t_k (t counts this cache's committed
decisions), and an initial weight w0 counted as one pseudo-reward at decision 0:

```
w_f(t) = ( w0·e^(−t/H) + Σ_k e^(−(t − t_k)/H)·r_k ) / ( e^(−t/H) + Σ_k e^(−(t − t_k)/H) )
```

* **Incrementally,** at a reward r at decision t: `g = exp(−(t − t_f)/H); R ← g·R + r; N ← g·N + 1; t_f ← t;
  w_f = R / N`.
* **Between rewards** the ratio does not change, so nothing happens per decision.
* **Relation to the EMA:** the EMA is the same ratio with a discount of (1 − α) per *event*, normalised by its
  steady-state count 1/α.
* **Everything else is unchanged:** scoring, window selection, ties, eligibility, protection, history and every
  reward event.
* **Default horizon:** H = 65,536 decisions, not tuned. That is several times the reward correlation time (≈ 0 beyond
  50 k decisions on Ocean and 20 k on LU), and short against Ocean's 1.36 M decisions per core. It is about 45% of
  LU's 145 k decisions per core, so on LU the estimate is close to a whole-run mean.

**3. Why it might improve victim selection.**
* **Evidence from LU (in-sample):** the frozen vector, essentially the time-average of rl-n4's own weights and close
  to its long-run reward means, did better than rl-n4 in both repetitions (−4.7% against −3.2%). A learner that
  keeps its weights near its own reward means should behave like that vector without being given it.
* **Prediction for Ocean, recorded before any run:** it will learn E > M > S.
  * **If it then matches frozen and priority,** the instability was the problem.
  * **If it stays near rl-n4 or does worse,** the reward's verdict on S (D) is the problem, and option 5 is next.

**4. Overhead.**
* **Simulator memory, measured** (g++ 11.4, x86-64, `sizeof`):
  * **Per table:** `WeightTable` is 200 bytes, against 40 before the change. Each of its five accumulators is 32
    bytes: a flag padded to 8 bytes, the sum, the count and a 64-bit time.
  * **Per cache:** +160 bytes for each L2 at global scope (the default, one table per cache). With
    `weight_scope=per_set` and the 512 sets of the 256 KB 8-way L2, about +80 KB per L2.
  * **Default runs carry the accumulators too,** unused; behaviour is unchanged.
  * **`RewardConfig`** grows from 48 to 64 bytes, once per cache.
* **Simulator time:** per reward, one `exp`, two multiply-adds and a division for each of the two rewarded features.
  This is negligible.
* **Hardware sketch:**
  * one fixed-point sum and count per feature (for example 16 bits each) and a coarse shared timestamp;
  * with H a power of two and Δt quantised, the discount is a shift or a small table;
  * the division can be done per update or replaced by a comparison of cross-products when two weights are compared.

**5. How to test the hypothesis independently** (a proposal; not run):
* **H1, stability:** the variant's state order holds in ≥ 90% of second-half snapshots on LU and Ocean (rl-n4: 23-51%),
  and its weight sd is near the i.i.d. level. This can be measured from the snapshots, the same analysis as C.
* **H2, misses:** L2 misses at or below `rl-n4-frozen` on LU and below rl-n4 on Ocean, in every repetition.
* **Runs:** `rl-n4`, the variant at H = 2^16, the horizon sensitivity H = 2^14 and 2^18, and the EMA with α = 0.01
  (candidate 1). Two repetitions on LU `tiny` and Ocean `small`, with `lru` and `rl-n4` rerun in the same batch and
  binary.
* **Independence:** add a workload that motivated nothing so far, such as Barnes or the corrected LU, before any
  claim.

**6. Distinguishing an improvement from "behaving like the fixed priority".**
* **Report the learned order** per workload, and its stability. On Ocean the predicted order (E > M > S) is not the
  priority rule's (S > E > M), so matching priority there with a different order would itself be informative.
* **No outside input:** the variant gets no LU-derived input; it starts from zero weights on every workload.
* **Own-frozen control:** freeze the variant's own second-half weights from repetition 1 and run them with
  `learning=false`.
  * **If online ≈ own-frozen,** the gain comes from a stable estimate: a learned fixed rule. That is still an
    improvement over rl-n4, but it must be reported as such, not as online adaptation.
  * **Online adaptation** must show up as a gain over its own frozen weights, or on a workload whose best order
    changes during the run.
* **Victim states and positions:** compare the variant's victim MESI states and window positions with the priority
  rule's. Identical distributions would mean it reproduces the rule.

## Implementation (`review/stage7-discounted-mean`)

**Options** (L2: `perf_model/l2_cache`):

```
-g perf_model/l2_cache/rl/estimator=ema              # ema | discounted_mean                  (default ema: the algorithm)
-g perf_model/l2_cache/rl/horizon=65536              # discounted_mean only, decisions, >= 1  (default 65536)
```

* **Validation:**
  * `rl/horizon` must be finite and ≥ 1;
  * setting it without `estimator=discounted_mean` is a configuration error, as `random_rate` is without
    `selection=random`;
  * `alpha` is not used by the variant.
* **Start-up line:** a cache using the variant prints
  `[RL] L2 core N: rl_lru STAGE7 estimator=discounted_mean horizon=65536 decisions (alpha unused)`. Default runs
  print exactly what they printed before.
* **Composition with the controls:**
  * `initial_weights` sets the prior;
  * `learning=false` keeps the weights bit-identical, as with the EMA;
  * `selection=priority` and `random` ignore the weights, so the estimator changes nothing for them;
  * `weight_scope=per_set` keeps one accumulator per set, on the cache's decision clock.
* **Code:**
  * `rl_reward_policy.h`: `Estimator`, `RewardConfig::{estimator, horizon}`, `WeightTable::reward_discounted`, and a
    dispatch in `RewardPolicy::reward`;
  * `cache_set_rl.cc`: option parsing and the start-up line;
  * tests in `testing/unit/rl/test_estimator.cc`.
* **Runner policies** (`experiments/rl/run_stage6.py`), both learning from zero initial weights:
  * `rl-n4-dm65536`: rl-n4 + `rl/estimator=discounted_mean` + `rl/horizon=65536` (`rl/alpha=0.1` stays in the list,
    unused);
  * `rl-n4-ema001`: rl-n4 with `rl/alpha=0.01` instead of 0.1, plus an explicit `rl/estimator=ema` (candidate 1).
* **Recorded per run:** each manifest line records `policy_options` (the policy's configured `-g` options) and
  `rl_settings`, the settings each L2 reported in its `[RL]` start-up lines:
  * mode, window, alpha, protection and scope;
  * the control settings;
  * the estimator (`ema` when no STAGE7 line was printed) and the horizon.
* **Summaries:** `summarize_stage6.py` lists both policies, appends any policy it does not know instead of leaving it
  out, and prints the reported settings per policy. Runs of one policy that report different settings are a
  consistency error.

## Verification

See `results/stage7-discounted-mean-verification/PROVENANCE.txt` for the binary md5s and every command.

* **Unit tests:** 97 cases (83 existing, 14 new), 0 failed. The new cases:
  * **Defaults and parsing:** the default is the EMA, and an explicit `ema` (with any horizon) gives the default's
    victims and weights bit for bit.
  * **The formula:** equals a brute-force discounted average, prior included, over 400 random event times for three
    horizons; plus a hand-computed case.
  * **Time, not event count:** a long silence discounts old evidence; an underflowing discount and a huge horizon
    stay finite.
  * **Phase-structured rewards:** under the same reward stream, features rewarded every decision and every 50th see
    the same span (largest gap < 0.1, against > 0.5 for the EMA).
  * **Bursty rewards:** the order of two true means 0.1 apart is kept in 100% of samples, against 64.8% for the EMA.
  * **Prior and learning off:** the initial weight is one pseudo-reward at decision 0, and `learning=false` keeps the
    weights bit-identical while every reward is counted.
  * **Eviction-history hard cases:**
    * the oldest entry's block returns before the victim's insertion: a −1 and no spurious age-out;
    * an aged-out block returns: no reward;
    * the reward counts are identical to the EMA's.
  * **A contradicted protection:** +1 at the eviction and −1 at the return, each at its own decision time.
  * **Protection across an upgrade:** carried over, and the later hit is rewarded at its own time.
  * **Controls:** priority and random decisions are unchanged by the estimator.
  * **Per-set scope,** and the diagnostics' totals equal the core's counters.
* **The tests catch the obvious mistakes:**
  * with the discount applied per event instead of per decision, 9 of the 14 new cases fail;
  * with the variant silently falling back to the EMA, 5 fail.
* **Differential test** (`make diff-cores OLD=<feature/rl-port 3862d867 tree>`): identical, 608 configurations
  (576 `rl_lru`, 32 `sf_lru`). It covers every mode, window, alpha, protection setting and scope, with the
  default estimator.
* **Simulator build:** the review worktree, configured as the main tree's build (Release, SDE frontend, same CMake
  options), md5 `d009e143…`. Neither the main tree's binary (`e65141e5…`) nor the preserved validated binary
  (`192619ee…`) was rebuilt or touched.
* **Deterministic replays** (single core; the LU `tiny` 200 M trace and the Stage 2 FFT trace).
  * **Existing policies and controls:** `lru`, `lru-hist`, `sf-text`, `sf-example`, `rl-n2`, `rl-n4`,
    `rl-n4-frozen`, `rl-n4-prio` and `rl-n4-rand` with the review build are identical to the `deafea4e…` replays
    in `stage6-controls-verification`, which equal the `e65141e5…` ones.
    * **What "identical" covers:** all 18 replays, 3,979-4,090 statistics each, 0 different beyond host
      wall-clock times; the same victim-sequence hash and the same start-up lines.
    * **No STAGE7 line** in any of them.
  * **The variant** (rl-n4 + `estimator=discounted_mean`, horizon 65,536):
    * **Repeatable:** two runs per trace are identical, and each core printed its STAGE7 line.
    * **Its decisions differ from rl-n4's,** as they should.
    * **Its state-weight order changed 8 times on LU and 15 times on FFT,** against 274 and 880 for rl-n4. On a single
      core with no S blocks, this is a plausibility check, not an evaluation.
  * **Checkers:** 64 of 64 checker runs pass on the 22 replays (`check_sharers`, `check_rl_events`, and
    `check_rl_policy` for every policy except `lru`).
* **Live 4-core smoke** (`capbx-lu-tiny-smoke`, LU steps 2-4, about 5 M instructions):
  * `rl-n4` and the variant, one run each, both exit 0;
  * all 12 checker runs pass, on the full region and on the measured interval;
  * each core printed the STAGE7 line once.
  * The variant was added to `run_stage6.py`'s policy table at run time by a wrapper
    (`results/stage7-discounted-mean-verification/scripts/smoke.py`); the script itself is unchanged.
* **Not done:** no LU or Ocean comparison, and no claim about misses or performance.

## Limits of this investigation

* **Sources:** the findings come from two workloads with two to five runs each, one configuration, and archived
  diagnostics.
* **What C rests on:** the snapshot series (1,000-decision resolution) and per-run aggregates. Per-feature reward
  timing inside an interval is not recorded.
* **Prediction, not evidence:** the expected benefit of the variant rests on the in-sample LU frozen vector. Its effect
  on Ocean is a stated prediction.
* **The Ocean S question in D is open.** The variant may well show that stability is not enough there.
