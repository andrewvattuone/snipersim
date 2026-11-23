# Stage 7: why online learning underperforms, and one opt-in variant

**Status:**
* **Investigation:** done, from existing Stage 6 statistics and telemetry only. No new benchmark simulation was run.
* **Variant:** one experimental, opt-in variant (`rl/estimator=discounted_mean`), merged into `feature/rl-port`
  (d9d0ef43) after review. It is verified with unit tests, a differential test, deterministic replays and smoke tests.
* **Ocean pilot (12 runs, exploratory):** the variant made the weights stable and learned the predicted order, but
  **raised L2 misses by 10.93%**, with excessive repeated protection. See "Ocean `small` pilot" below.
* **Reward correction (review branch `review/stage7-protect-invalidation`, not evaluated):** an opt-in protection
  outcome when a protected line is invalidated before a hit. See "Protection feedback on invalidation" below.
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
| Snapshots in which the most common order of wS, wE, wM holds (exact ties counted as ties) | 17-19% (null 53-55%) | 42-51% (null 82-84%) |
| Order flips of the state weights | 25-27 per 1,000 decisions | 25-28 per 1,000 decisions |

**Correction (after the Stage 7 pilot):** this row first gave 23-25% for Ocean.
* **The cause:** the first count ranked exact ties in the fixed order S, E, M, so a tie such as E = M > S was counted
  as E > M > S.
* **How often ties occur:** on Ocean, rl-n4's E and M weights are often exactly equal, both saturated at the same
  value. Exact ties occur in 7-9% of second-half snapshots on Ocean and ≤ 1% on LU.
* **The corrected figures:** the most common strict order holds in 17-19% of snapshots on Ocean, and 42-51% on LU
  (unchanged).
* **The null model** draws continuous, independent weights, which practically never tie, so its figures are unaffected.

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
* **H1, stability:** the variant's state order holds in ≥ 90% of second-half snapshots on LU and Ocean (rl-n4: 17-51%),
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
* **Not done in this verification:** no LU or Ocean comparison. The Ocean pilot below came later.

## Ocean `small` pilot (`results/stage7-ocean-pilot`, archived): EXPLORATORY

Two repetitions per policy: ranges, not significance tests.

**Setup:**
* **Runs:** six policies × 2 repetitions on `ocean-small` (12 runs), 4 at a time.
* **Command:** `run_stage6.py results/stage7-ocean-pilot --workloads ocean-small --policies
  lru,rl-n4,rl-n4-ema001,rl-n4-dm65536,rl-n4-frozen,rl-n4-prio --reps 2 --jobs 4`.
* **Configuration:** as in the Stage 6 Ocean comparison:
  * `OCEAN -n258 -p4`, complete ROI;
  * 4 Gainestown cores, the policy on the private L2 only, 8 MB shared L3, prefetchers off;
  * timeout 3,000 s.
  * The 12 `sim.cfg` files are identical apart from the policy keys.
* **Policies** (`run_stage6.py` at d9d0ef43):

  | Policy | Configuration | Initial weights |
  |---|---|---|
  | `lru` | `replacement_policy=lru` | – |
  | `rl-n4` | `rl_lru`, mode both, window 4, EMA α 0.1, protection feedback, global weights | 0 |
  | `rl-n4-ema001` | rl-n4 with α 0.01 (`estimator=ema`) | 0 |
  | `rl-n4-dm65536` | rl-n4 + `estimator=discounted_mean`, `horizon=65536` | 0 (no LU input) |
  | `rl-n4-frozen` | rl-n4 + `learning=false` | S −0.09, E −0.38, M −0.76, BT −0.64, AT 0 (LU, unchanged) |
  | `rl-n4-prio` | rl-n4 + `learning=false`, `selection=priority` | – |

  Each run's manifest line records the configured options (`policy_options`) and the settings every L2 printed at
  start-up (`rl_settings`). All runs of a policy report the same settings.
* **Build and provenance:**
  * `feature/rl-port` d9d0ef43, clean tree;
  * one simulator binary for all 12 runs, md5 `ae285766…` (the merged build);
  * the Stage 6 OCEAN executable, md5 `4ce36666…`.
* **Regression checks before the pilot** (`results/stage7-ocean-pilot-prep`):
  * 97 unit tests pass, and the differential test is identical over 608 configurations;
  * deterministic replays of all 9 existing policies are identical to the validated replays;
  * `rl-n4-dm65536` is identical to the review build's replays;
  * 64 of 64 checker runs pass.
* **Outcome:**
  * all runs exit 0, with no timeouts and 0 stale write hits;
  * **34 of 34 checker runs pass** (`check_sharers` and `check_rl_events` on every run, `check_rl_policy` on the 10
    `rl_lru` runs);
  * wall time 1,337-1,436 s per run;
  * the frozen and priority weights stayed constant.

**Every repetition** (`analysis.txt`, `metrics-per-run.csv`):

| Policy | Rep | Instructions (M) | IPC | Simulated time (µs) | L2 misses | L2 MPKI | L2 demand misses | L2 dirty evictions | L2 invalidations | L2 downgrades | L3 misses | DRAM reads / writes | Victims S / E / M % | Departure from LRU |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| lru | 1 | 272.64 | 2.3593 | 43,444 | 5,962,823 | 21.871 | 5,705,868 | 4,753,769 | 133,123 | 410,467 | 1,878,550 | 1,551,652 / 1,230,918 | 4.50 / 9.39 / 86.11 | – |
| lru | 2 | 272.64 | 2.3549 | 43,524 | 5,970,477 | 21.899 | 5,714,346 | 4,757,379 | 132,511 | 411,238 | 1,884,096 | 1,560,525 / 1,230,128 | 4.52 / 9.44 / 86.04 | – |
| rl-n4 | 1 | 272.64 | 2.3603 | 43,424 | 5,893,802 | 21.617 | 5,636,660 | 4,668,385 | 128,182 | 422,647 | 1,873,219 | 1,551,856 / 1,227,379 | 4.80 / 9.65 / 85.55 | 17.40% |
| rl-n4 | 2 | 272.64 | 2.3630 | 43,375 | 5,887,234 | 21.594 | 5,629,410 | 4,661,799 | 129,703 | 421,035 | 1,875,244 | 1,553,673 / 1,227,983 | 4.75 / 9.67 / 85.58 | 17.17% |
| rl-n4-ema001 | 1 | 272.64 | 2.3615 | 43,403 | 5,853,751 | 21.471 | 5,595,695 | 4,632,825 | 122,102 | 418,452 | 1,881,345 | 1,557,901 / 1,229,417 | 4.74 / 9.79 / 85.47 | 22.77% |
| rl-n4-ema001 | 2 | 272.64 | 2.3626 | 43,382 | 5,860,470 | 21.495 | 5,604,179 | 4,643,015 | 120,470 | 420,525 | 1,873,440 | 1,553,836 / 1,225,874 | 4.82 / 9.72 / 85.46 | 22.83% |
| rl-n4-dm65536 | 1 | 272.64 | 2.3702 | 43,244 | 6,653,144 | 24.403 | 6,393,544 | 5,516,448 | 135,745 | 314,189 | 1,877,569 | 1,554,164 / 1,227,334 | 2.19 / 8.56 / 89.26 | 70.03% |
| rl-n4-dm65536 | 2 | 272.64 | 2.3707 | 43,233 | 6,583,982 | 24.149 | 6,323,472 | 5,442,462 | 132,821 | 307,267 | 1,878,381 | 1,552,681 / 1,228,100 | 2.34 / 8.64 / 89.02 | 70.09% |
| rl-n4-frozen | 1 | 272.64 | 2.3602 | 43,426 | 5,829,116 | 21.381 | 5,583,730 | 4,550,412 | 110,818 | 459,274 | 1,880,084 | 1,556,437 / 1,229,632 | 5.86 / 10.19 / 83.95 | 15.62% |
| rl-n4-frozen | 2 | 272.64 | 2.3602 | 43,427 | 5,824,009 | 21.362 | 5,580,479 | 4,541,019 | 108,972 | 461,783 | 1,882,760 | 1,557,692 / 1,228,862 | 5.98 / 10.24 / 83.78 | 15.81% |
| rl-n4-prio | 1 | 272.64 | 2.3607 | 43,417 | 5,820,731 | 21.349 | 5,574,990 | 4,545,760 | 110,560 | 458,396 | 1,876,868 | 1,553,280 / 1,228,276 | 5.85 / 10.14 / 84.00 | 15.54% |
| rl-n4-prio | 2 | 272.64 | 2.3619 | 43,394 | 5,818,747 | 21.342 | 5,575,568 | 4,541,197 | 108,888 | 461,615 | 1,874,848 | 1,550,832 / 1,227,536 | 6.00 / 10.13 / 83.88 | 15.69% |

**Policy diagnostics, per repetition** (`analysis.txt`, `pinning.txt`):

| Policy | Rep | Rewards: evict-bad / evict-good / protect-good / protect-wasted % | Protections contradicted | Max passes (one block) | Ways pinned ≥ 8 decisions, per set | All candidates tied | Victim at LRU position 0 / 1 / 2 / 3 % |
|---|---|---|---|---|---|---|---|
| rl-n4 | 1 | 41.6 / 44.3 / 2.8 / 11.3 | 42.0% | 58 | 0.034 | 70.8% | 82.6 / 7.3 / 3.8 / 6.2 |
| rl-n4 | 2 | 41.4 / 44.6 / 2.8 / 11.1 | 42.1% | 76 | 0.033 | 70.5% | 82.8 / 7.2 / 3.8 / 6.2 |
| rl-n4-ema001 | 1 | 40.7 / 44.2 / 3.6 / 11.5 | 40.7% | 98 | 0.097 | 65.6% | 77.2 / 9.1 / 5.6 / 8.1 |
| rl-n4-ema001 | 2 | 40.8 / 44.3 / 3.7 / 11.2 | 41.0% | 95 | 0.107 | 65.9% | 77.2 / 8.8 / 5.8 / 8.2 |
| rl-n4-dm65536 | 1 | 47.8 / 41.1 / 2.9 / 8.3 | 33.9% | 1,855 | 1.079 | 27.0% | 30.0 / 23.9 / 21.0 / 25.1 |
| rl-n4-dm65536 | 2 | 47.1 / 41.5 / 2.9 / 8.5 | 33.8% | 857 | 1.067 | 26.9% | 29.9 / 24.7 / 19.8 / 25.6 |
| rl-n4-frozen | 1 | 38.1 / 41.4 / 4.9 / 15.6 | 35.6% | 199 | 0.001 | 83.9% | 84.4 / 0.8 / 2.1 / 12.7 |
| rl-n4-frozen | 2 | 38.2 / 41.3 / 4.9 / 15.6 | 35.9% | 323 | 0.001 | 83.8% | 84.2 / 0.9 / 2.2 / 12.8 |
| rl-n4-prio | 1 | 38.1 / 41.6 / 5.0 / 15.2 | 36.1% | 207 | 0.001 | 84.0% | 84.5 / 0.8 / 2.0 / 12.7 |
| rl-n4-prio | 2 | 37.9 / 41.6 / 4.9 / 15.6 | 36.2% | 194 | 0.001 | 83.9% | 84.3 / 0.8 / 2.1 / 12.8 |

* **Pinned ways** is the mean, per decision, of the number of ways in the set that have been passed over at least 8
  consecutive decisions without a hit (`rl-diag-pinned-long`).
* **Max passes** is the largest number of times one block was passed over, on any core.

**Weights of the learned policies** (second half of the ROI, the four cores pooled):

| | rl-n4 | rl-n4-ema001 | rl-n4-dm65536 |
|---|---|---|---|
| Weight sd per feature | 0.63-0.74 | 0.48-0.66 | 0.13-0.27 |
| Snapshot-to-snapshot sign changes per feature | 15.8-23.8% | 3.6-13.5% | 0.2-1.5% |
| Snapshots with \|w\| > 0.9 per feature | 19.5-47.3% | 0.4-30.6% | 0-1.4% |
| Order changes of wS, wE, wM per 1,000 decisions | 25.9-26.4 | 5.0-5.1 | 0.10-0.11 |
| Most common order (ties distinguished) and its share | M>E>S 17.6%, E>S>M 18.3% | E>S>M 26.8-27.6% | E>M>S 69.3-72.2% |
| Snapshots with E>M>S | 15.9-16.2% | 24.2-25.8% | 69.3-72.2% |
| Mean weights S / E / M / BT / AT | +0.18 / +0.26 / +0.16-0.17 / +0.17 / +0.10-0.12 | +0.14-0.16 / +0.41-0.42 / +0.17-0.18 / +0.19 / +0.10-0.11 | −0.03 to −0.05 / +0.32-0.33 / +0.07-0.09 / +0.09-0.11 / −0.68 to −0.71 |

**Means, change vs LRU** (two repetitions each):

| | rl-n4 | rl-n4-ema001 | rl-n4-dm65536 | rl-n4-frozen | rl-n4-prio |
|---|---|---|---|---|---|
| L2 misses | −1.28% | −1.84% | **+10.93%** | −2.35% | −2.46% |
| L2 demand misses | −1.35% | −1.93% | +11.36% | −2.24% | −2.36% |
| L2 dirty evictions | −1.90% | −2.47% | +15.22% | −4.41% | −4.46% |
| L2 invalidations | −2.92% | −8.68% | +1.10% | −17.26% | −17.39% |
| L2 downgrades | +2.67% | +2.10% | −24.37% | +12.09% | +11.96% |
| L3 misses | −0.38% | −0.21% | −0.18% | +0.01% | −0.29% |
| Simulated time | −0.19% | −0.21% | −0.56% | −0.13% | −0.18% |
| Departure from LRU | 17.3% | 22.8% | 70.1% | 15.7% | 15.6% |

**Consistency with earlier Ocean batches** (Stage 6 ablation and Pilot 3; other binaries, same configuration):
* **LRU:** all six runs span 5,945,424-5,970,477 L2 misses (0.42%) and 43,296-43,524 µs (0.53%).
* **rl-n4:** all six runs span 5,887,234-5,906,796 L2 misses.
* **Frozen and priority:** −2.35% and −2.46% here, against −2.37% and −2.24% in the Stage 6 comparison.

**Findings, kept separate:**
1. **Weight stability improved substantially with the discounted mean** (observed):
   * the weights' sd fell from 0.63-0.74 to 0.13-0.27;
   * state-order changes fell from 26 to 0.1 per 1,000 decisions;
   * the predicted order E > M > S was learned and held in 69-72% of second-half snapshots. The target of ≥ 90% in
     the test plan above was not met.

   A smaller α (0.01) gave a partial improvement: 5 order changes per 1,000 decisions.
2. **L2 replacement performance became worse with the discounted mean** (observed):
   * **+10.93% L2 misses** against LRU's mean, above every LRU run in this pilot and in the earlier batches;
   * +15.2% dirty evictions;
   * the oldest candidate was the victim in only 30% of decisions (departure from LRU 70.1%, against 15.6-22.8% for
     the other policies).
3. **The discounted mean produced excessive repeated protection** (observed):
   * about 1.07 of every 8 ways per set were pinned (passed over ≥ 8 consecutive decisions), against 0.03 for rl-n4,
     0.10 for α 0.01 and 0.001 for frozen and priority;
   * one block was passed over up to 1,855 times;
   * 1.41 passes per decision, against 0.33-0.45.
   * Its victims shift away from S (2.2-2.3% against 4.5%) and heavily shared blocks (0.3-0.4% against 1.2%); the
     learned weights keep those strongly (AT −0.68 to −0.71).
4. **Simulated time was slightly lower, but that is not evidence of a speedup.**
   * **The measurement:** −0.56% against this pilot's LRU mean. Its two runs (43,233 and 43,244 µs) are the lowest of
     all 32 Ocean runs so far.
   * **Why it is not a speedup:**
     * the next lowest (Pilot 3's `sf-text`, 43,264 and 43,288 µs) differ by only 0.05-0.13%;
     * LRU's six runs span 0.53%;
     * the reduction coincides with 11% more L2 misses and 24% fewer downgrades, and how Ocean's run time depends on
       L2 misses versus coherence events has not been established.

   Two runs per policy do not support claiming a meaningful speedup.
5. **The smaller α did better than rl-n4, but not as well as the fixed settings:**
   * rl-n4-ema001's L2 misses (−1.84%) are below all six rl-n4 runs to date;
   * they are still 0.5-0.6 points above frozen and priority.

**Observation versus hypothesis.**
* **Observed:** the 10.93% miss increase, the 70% departure rate and the pinning are measurements.
* **Hypothesis, not established:** the explanation offered so far is that the reward gives a pinned block no
  corrective feedback. A protected block that is hit earns −1 ("keep"). One that is never evicted earns nothing
  until it leaves. One that leaves through a coherence invalidation earns nothing at all. rl-n4's noisy weights had
  broken such pins by chance.
* **Consistent with it:** of the runs of ≥ 33 passes, 27.4-27.6 k ended in an invalidation under the discounted mean,
  against 22-25 under rl-n4.
* **Not shown:** that the missing feedback causes the pinning, nor that supplying it would remove the pinning or the
  extra misses.

**Archive:**
* **Pilot:** `results/archive/stage7-ocean-pilot-20261122.tar.gz` (sha256 in its `.sha256`). It holds:
  * the run directories, manifest, checker output, summary and CSVs;
  * `analysis.txt`, `pinning.txt` and their scripts (`analysis-scripts/`);
  * `PROVENANCE.txt`, `READOUT.txt`, the command and the progress log.
* **Merge, build and regression checks:** `results/archive/stage7-ocean-pilot-prep-20261122.tar.gz`.
* **Binaries,** kept read-only in `results/`: the main-tree binary used before the merge,
  `stage6-ocean-ablation.sniper-e65141e5`, together with the earlier ones.

## Protection feedback on invalidation (`review/stage7-protect-invalidation`)

**Status:** an experimental, opt-in reward correction on a review branch. It is verified (unit, differential, replay
and smoke tests) but **not evaluated**. The default is the algorithm, unchanged.

### What happens to a protected line today (code review of `rl_reward_policy.h`)

A line is *protected* when a decision passes it over: an older candidate is kept while a younger one is evicted.

| Event | Call | Protection outcome today | Other effects |
|---|---|---|---|
| Passed over by a decision | `commit` | none | `is_protected` set; the snapshot is overwritten with the features at this pass; `passes` +1 |
| Passed over again, without a hit | `commit` | none (no reward per pass) | snapshot overwritten again; `passes` grows (diagnostic only; `max_passes` records it) |
| Hit (a recency-updating hit) | `on_hit` | **protect-good −1** on the snapshot; protection cleared | a later pass protects it again |
| Evicted by replacement | `commit` (it is the victim) | **protect-wasted +1** on the snapshot; protection cleared | `on_eviction` puts it in the history, so its return (−1) or age-out (+1) follows: two outcomes per lifetime |
| Coherence invalidation (another core's write) | `on_invalidate(coherence)` | **none: the protection is dropped** (`LineMeta{}`) | no history entry; the later miss counts as a coherence miss, not the policy's |
| Back-invalidation (the inclusive L3 evicts it) | `on_invalidate(back_invalidation)` | **none: dropped** | as for coherence |
| Upgrade (this core writes a Shared line) | `on_invalidate(upgrade)`, then `on_fill(upgrade_refill)` | none | the metadata (protection, snapshot, passes) is stashed and restored at the refill. In Sniper every upgrade refill matches its stash: `check_rl_policy` requires 0 unmatched refills and 0 overwritten stashes |

**So a protected line has three ways out, and only two of them produce an outcome.** Hits and evictions produce one;
invalidations produce none.

### The excessive-protection problem in the archived diagnostics

`results/stage7-protect-invalidation-verification/protection-exits.txt` (`scripts/protection_exits.py`), from the
archived ROI statistics. A *run* of passes (`rl-diag-run-<end>-<length>`) is a block's consecutive passes since its
fill or last hit. With protection feedback on, a run is exactly a protected residency.
* **The identity:** in every run of every batch, the runs ended by a hit equal the core's protect-good count, and those
  ended by an eviction equal its protect-wasted count.
* **What follows:** the runs ended by an invalidation are exactly the protected residencies that received no outcome.

| Batch | Policy | Protected residencies without an outcome (invalidated) | Share of all protection outcomes | Share among residencies of ≥ 9 passes | Share among residencies of ≥ 33 passes | Share of the L2's coherence + back invalidations that hit a protected line | Back-invalidations |
|---|---|---|---|---|---|---|---|
| Stage 7 Ocean pilot | rl-n4-dm65536 | 41,492-44,001 | 5.0-5.4% | 26.4-27.8% | 51.0-51.3% | 21.5-22.4% | 23,891-24,252 |
| | rl-n4 | 3,359-3,955 | 0.4% | 1.7-3.6% | 5.8-7.1% | 2.0-2.4% | 520-539 |
| | rl-n4-ema001 | 4,778-4,904 | 0.5% | 2.0-4.5% | | 3.1% | 642-881 |
| | rl-n4-frozen, rl-n4-prio | 217-425 | ≤ 0.05% | 0% | | 0.1-0.3% | 342-640 |
| Stage 6 Ocean ablation | rl-n4 | 3,448-3,449 | 0.4% | 2.4-2.9% | | 2.1% | 316-1,697 |
| | frozen, priority, random | 115-1,426 | ≤ 0.2% | 0% | | 0.1-0.8% | 283-1,685 |
| Stage 6 LU `tiny` ablation | rl-n4 | 2,123-2,177 | 2.5% | 3.3-6.1% | | 1.0-1.1% | 0 |
| | frozen, priority, random | 938-3,280 | 0.9-3.7% | 0-16.7% | | 0.5-1.5% | 0 |

**Findings:**
* **The missing feedback is negligible where the policy does not pin.** It is 0.4% of protection outcomes for rl-n4
  and ≤ 0.05% for frozen and priority on Ocean; on LU it is small in absolute terms.
* **In the discounted-mean variant it is concentrated where the pinning is:**
  * half of the residencies of ≥ 33 passes end in an invalidation with no outcome;
  * one in five of all the L2's invalidations hits a protected line.
* **Back-invalidations rise from about 0.3-1.7 k to 24 k per run.** Lines held this long are dropped by the inclusive
  L3, plausibly because the L3 sees no access to a line the L2 keeps hitting or holding. This is not verified.
* **What is not known:** the feature classes of the unrewarded residencies. The diagnostics did not classify them, and
  the new option records them.
  * **The protection outcomes the variant did receive** (per run): S 75 k good / 46 k wasted (protection mean
    −0.24) and AT 15 k / 5 k.
  * **If most of the 41-44 k unrewarded residencies were S or AT,** recording them would reverse the sign of the
    protection evidence for those features. That is a hypothesis, and it is what the correction would test.
* **Conclusion: significant enough to test,** because the missing outcomes sit exactly in the regime that failed. It is
  not shown to be sufficient: pinned lines ended by hits (−1, "keep") and late evictions are unaffected.

### Is a +1 on invalidation defensible?

**What it asserts:** "this protection produced no reuse before the line left." That is the meaning protect-wasted
already has at an eviction.
* **Not a verdict that the protection was wrong:** the alternative, evicting this line, would have kept another block
  whose value is unknown.
* **Not a claim about the coherence event:** it does not say the policy caused the invalidation.

**Why it is better than no outcome:** today the protection outcomes are censored by how the line leaves.
* **Who is missed:** lines likely to be invalidated (Shared lines that other cores write, and long-held lines that the
  L3 back-invalidates) are counted only when they happen to reach a hit or an eviction first.
* **The resulting bias:** the evidence is tilted toward "keep" for exactly those classes, a survivorship bias.
* **The fix removes it:** recording an outcome for every exit removes that bias; it does not add one.

**Bias against Shared blocks:**
* **The direct effect:** it raises wS (evict S sooner) when S lines die by invalidation before any reuse. That is the
  intended correction, and it moves toward what the two fixed settings that did best on Ocean do. It also teaches
  "evict blocks likely to be invalidated", a known coherence-aware heuristic.
* **Risk 1, an unrepresentative snapshot:** the +1 goes to the snapshot of the last pass. A line passed over as S that
  was later written locally (upgrade, still protected) and then invalidated as M rewards S. This is the same
  attribution rule as every other protection outcome.
* **Risk 2, workload dependence:** where Shared lines are read again soon after the protection decision and only then
  invalidated, the hit comes first (−1) and nothing changes. Where they are invalidated first, evicting them earlier
  costs nothing the invalidation would not have cost.

  A workload where evicting S earlier is harmful, despite those lines being invalidated before reuse, would have to show
  higher L2 misses for the corrected policy. That is one of the proposed metrics below.
* **Magnitude:** +1, the same as an eviction without a hit. Not scaled by the number of passes, and applied once per
  protected residency.
* **Back-invalidation:** treated the same way; the line sat unused in the L2 until the L3 dropped it.
* **Upgrades:** not an exit, so never rewarded; the protection continues into the refill.

**What it does not address:**
* pins that end in a hit (−1 reinforces keeping);
* the cost of holding a way across hundreds of passes.

If the correction does not reduce the pinning, the next candidate is to charge that cost (a pass limit or a per-pass
cost), which changes the protection semantics more.

### Implementation

```
-g perf_model/l2_cache/rl/protect_invalidation_feedback=false   # default; true: the correction (needs rl/protect_feedback=true)
```

* **Semantics** (`RewardPolicy::on_invalidate`): with the option on, mode not off, and reason coherence or
  back-invalidation, a line that is protected (passed over and not hit since) gets `reward(snapshot, +1)` and
  `stats.protect_invalidated` +1. This happens before its metadata is cleared, so at most once per protected residency.
  * The reward goes through the configured estimator: the EMA, or the discounted mean on the decision clock.
  * `learning=false` counts it without changing a weight.
  * Upgrades are unchanged.
* **Statistics** (registered only with the option on, so default runs keep exactly their statistics):
  * `L2.rl-reward-protect-invalidated`;
  * `L2.rl-diag-reward-protect-invalidated-{S,E,M,BT,AT}`, by snapshot class (observer).
* **Checker** (`check_rl_policy.py`, when those statistics exist):
  * the class totals equal the core counter;
  * the runs of passes ended by an invalidation equal `rl-reward-protect-invalidated`.
* **Start-up line** (option on only): `[RL] L2 core N: rl_lru STAGE7 protect_invalidation_feedback=1 (+1 to a protected
  line invalidated before a hit)`. Configuring it without `rl/protect_feedback=true` is an error.
* **Runner:** `run_stage6.py` policy `rl-n4-dm65536-ifb` = `rl-n4-dm65536` + the option, defined for the proposed
  comparison and not run. `summarize_stage6.py` lists it and reports the setting.
* **Overhead:**
  * no new per-line state: it uses the existing protection flag and snapshot;
  * one 64-bit counter per cache in `RewardStats`, and 5 in the observer;
  * one branch per invalidation, plus one weight update per rewarded invalidation;
  * `sizeof(RewardConfig)` and `sizeof(RewardStats)` grow by one field each.
* **Unchanged:** candidate selection, eligibility, both estimators, the history and every other reward event, cache
  geometry, MESI and timing.

### Verification (`results/stage7-protect-invalidation-verification`)

* **Unit tests:** 108 cases (97 existing, 11 new), 0 failed. The new cases:
  * **Off by default,** and off the old behaviour.
  * **Exits:** coherence and back-invalidation of a protected line give one +1 on its snapshot; an unprotected line, or
    a hit after the protection, gives nothing.
  * **Upgrades:** they carry the protection without a reward, and a later exit is rewarded once; duplicate events give
    nothing.
  * **Repeated passes:** 50 passes give one outcome, on the last snapshot.
  * **Other rewards:** eviction, history and protection rewards are unchanged.
  * **Mode off, learning off, and the priority control,** whose decisions are unchanged.
  * **The discounted-mean clock.**
  * **Whole traces:** off is identical to the default over a full trace with invalidations and upgrades; on, the trace
    differs.
  * **Identities:** the observer's class totals and the runs ended by invalidation equal the core counter (3 seeds × 2
    estimators).
* **Mutation checks:** each deliberate mistake makes cases fail:
  * rewarding upgrades: 2 fail;
  * rewarding unprotected lines: 4 fail;
  * ignoring back-invalidations: 3 fail.
* **Differential test** (`make diff-cores OLD=<feature/rl-port 5cb01d4e tree>`): identical, 608 configurations.
* **Deterministic replays** with the review build (md5 `784fc365…`, configured as the main build), on the LU `tiny`
  and FFT traces.
  * **With the option off:** `lru`, `lru-hist`, `sf-text`, `sf-example`, `rl-n2`, `rl-n4`, `rl-n4-frozen`,
    `rl-n4-prio`, `rl-n4-rand`, `rl-n4-ema001` and `rl-n4-dm65536` are identical to the merged-build replays (`ae285766…`).
    That is 22 of 22, with the same statistic names and values beyond host wall-clock times, the same victim-sequence
    hash and the same start-up lines.
  * **With the option on:** `rl-n4-dm65536-ifb` prints its STAGE7 line and registers the new counter. A single core
    has no coherence invalidations, so it is a smoke test only.
  * **Checkers:** 70 of 70 checker runs pass.
* **Live 4-core smoke** (`sharetest-w` and `capbx-lu-tiny-smoke`, `rl-n4-dm65536` with the option off and on):
  * all 4 runs ok, and all 18 checker runs pass (including the measured interval and the new identity);
  * the settings are recorded in the manifest.
  * **How much this exercises:** the new path fired only 3 times (LU, identity 3 = 3) and 0 times on `sharetest-w`.
    The volume test of the path is the unit tests (thousands of rewarded invalidations on random traces), and the
    proposed Ocean comparison.
* **Tooling:** with the new build, the summaries of the Stage 6 and Stage 7 batches are byte-identical, and
  `check_rl_policy.py` output is identical on archived runs. The pilot's CSV gains one empty column
  (`set_protect_invalidation_feedback`).

### Proposed next experiment (not run)

**Ocean `small`, two repetitions, 8 runs:** `lru`, `rl-n4-prio`, `rl-n4-dm65536` and `rl-n4-dm65536-ifb`.
* **Configuration and timeout:** as in the Stage 7 pilot; about 45 minutes at 4 concurrent.
* **Binary:** all four policies on one binary that has the option, the review build or the main tree after a merge.
  The existing policies are verified identical on it.
* **Optional:** `rl-n4-frozen` as a second reference (+2 runs).

| Question | Metrics (per run, vs `rl-n4-dm65536`, LRU and priority) | Pre-stated expectation if the hypothesis holds |
|---|---|---|
| Does excessive protection decrease? | ways pinned ≥ 8 decisions per set; max passes; passes per decision; residencies of ≥ 33 passes and how they end; back-invalidations; `rl-reward-protect-invalidated` and its class split | pinned well below 1.07 (e.g. < 0.2); back-invalidations back near 1 k; most rewarded invalidations S or AT |
| Is the departure from LRU less extreme? | departure rate; victim window position 0-3; all-tied share | departure well below 70% (toward the 16-23% of the other policies) |
| Do L2 misses improve? | L2 misses, demand misses, MPKI | below `rl-n4-dm65536` in both repetitions; below LRU would be needed for the correction to be useful |
| Do coherence or write-back costs worsen? | invalidations, downgrades, dirty evictions, coherence write-backs, L3 misses, DRAM traffic, simulated time | invalidations and downgrades move toward priority's (fewer invalidations, more downgrades); no DRAM change |
| Is it just forced toward the fixed priority? | learned weights and order over time (S, AT especially); victim S / E / M and heavily-shared shares; window positions, each against `rl-n4-prio` | if its order becomes S > E > M with AT first and its victim mix matches priority's, the correction makes the learner reproduce the fixed rule. That is worth knowing, but it is not evidence that learning adds value. Value would need a workload where it differs from priority and does better |

**Interpretation rules:**
* **If pinning stays high** (> 0.5 ways per set), the missing invalidation outcome is not the main cause; the next
  candidate is the cost of holding a way.
* **If pinning falls but misses stay above LRU,** the reward direction itself (the victim's own return, not a
  comparison with the kept candidate) remains the problem.

Two repetitions remain a pilot: ranges, not significance.

## Limits of this investigation

* **Sources:** the findings come from two workloads with two to five runs each, one configuration, and archived
  diagnostics.
* **What C rests on:** the snapshot series (1,000-decision resolution) and per-run aggregates. Per-feature reward
  timing inside an interval is not recorded.
* **Prediction, not evidence:** the expected benefit of the variant rests on the in-sample LU frozen vector. Its effect
  on Ocean is a stated prediction.
* **The Ocean S question in D is open.** On Ocean, stability alone did not help (the pilot below).
