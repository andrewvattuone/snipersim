# Executive summary: coherence-aware RL cache replacement in Sniper (Stages 1–7)

*As of 2026-11-23. The details and sources are in [`HANDOFF_REPORT.md`](HANDOFF_REPORT.md).*

## Goal

Find out whether a multicore's private L2 cache can choose better eviction victims by using each block's **MESI
coherence state** (Modified, Exclusive, Shared) and its **degree of sharing** (how many cores held it when it was
filled). The policy learns online which kinds of block are good victims, following Souza and Freitas (IEEE Access
2024, and the thesis behind it).

Success would mean, in order:
1. no coherence errors;
2. fewer L2 misses than LRU;
3. lower execution time;
4. a learned policy that beats simple fixed rules using the same information.

## What was built

All of it is in a fork of the Sniper multicore simulator, branch `feature/rl-port`:
* **A fix to Sniper's MESI protocol** found before the RL work. On a read, a private cache could receive E or M
  while other cores still held the line, so a later write missed invalidating them.
* **A realistic sharing signal:** the "delivered sharer hint", reported by the shared L3 with each fill.
* **Two replacement policies** as simulator-independent cores with unit tests:
  * a faithful reproduction of the paper's algorithm (`sf_lru`);
  * our modified reinforcement learner (`rl_lru`).
* **The learner,** in brief:
  * a block's score is a learned weight for its MESI state plus one for its sharing class;
  * the policy evicts the highest-scoring of the 4 least-recently-used blocks;
  * the weights are updated with a moving average of ±1 rewards: did an evicted block come back while it was still
    in a 16-entry history, and was a block that was spared ("protected") used again?
* **Event hooks, diagnostics and automatic checkers** that verify the protocol and every learning event in every run.
* **No-learning controls:** frozen weights, a fixed priority (evict Shared first, Modified last) and random
  departures from LRU. Without them, a learning claim could not be tested.

## What was established

* **Correctness (validated).** Stage 5 ran 700 simulations: 0 coherence violations, every learning-event check
  passing, and 4 crashes, which were traced to pre-existing races in Sniper's trace front end and fixed.
* **Cache-level effects** (measured, two to three repetitions, one 4-core configuration):

  | Policy (L2 misses vs LRU) | Ocean `small` | LU `tiny` (exploratory) |
  |---|---|---|
  | Paper reproduction (`sf_lru`) | +11% to +18% | +46% to +73% |
  | Online learner (rl-n4) | −1.1% | −3.2% |
  | Frozen weights (from LU) | −2.4% | −4.7% |
  | **Fixed priority (S > E > M)** | **−2.2%** | **−8.1%** |
  | Random departures within the window | −0.4% | −4.8% |

* **Fixed rules beat the learner.** The fixed priority and the frozen weights beat the online learner in every
  repetition on both workloads.
* **The RL algorithm does not currently outperform the fixed priority.**
* **No execution-time improvement.** Every policy's run time stays within the simulator's run-to-run spread (about
  0.3–0.5%). On Ocean, the L3 and DRAM traffic that makes it memory-bound is unchanged by any L2 policy (within
  ±0.5%), so a saved L2 miss only becomes an L3 hit. An upper-bound estimate of the possible gain is about 1%. On LU
  the L3 holds the whole working set and the kernel is compute-bound.

## Why the learner underperforms (Stage 7, from existing data)

1. **The rewards come in long phases.** With a learning rate of 0.1 per event, the weights follow those phases and
   flip order about 26 times per 1,000 decisions.
2. **Stabilising the weights made things worse.** A time-discounted mean made the weights stable, but it learned
   to *keep* Shared blocks and raised L2 misses by 11%, with heavy "pinning" of protected blocks.
3. **A targeted fix to the protection reward helped only partly:** misses fell to +5.7% over LRU.
4. **Root cause (code fact plus measurements): the reward judges each block's own future, not the choice between
   candidates.**
   * "Good eviction" only means "not reused within 16 later evictions": on Ocean, 93% of such blocks are reused
     later anyway.
   * Under the best policy, its own outcomes say "keep Shared", the opposite of what makes it best.

## The offline model (Stage 7)

* **The tools:** an observation-only trace of every L2 event, and a "shadow cache" model that replays a recorded
  run under another policy. Its purpose is to measure how much headroom the MESI and sharing features leave, without
  hundreds of simulations.
* **First calibration: failed.** One of 8 cross-policy predictions missed the ±0.5-point tolerance.
* **Investigation.** It found two rule mistakes and a missing MESI write-permission rule. The correction is derived
  from Sniper's code.
* **Held-out calibration: passed** on four new runs, with the model and criteria frozen beforehand: largest error
  0.15 points, noise floor 0.17. The scope is narrow: one workload and one configuration. It allows only a limited
  screening of fixed rankings, each to be confirmed in Sniper. No screening has been run.

## Status and next step

* **Branches:** `feature/rl-port` holds everything validated. The invalidation-feedback correction and the
  trace/shadow tools are on unmerged review branches. Every result is archived with checksums.
* **Proposed Stage 8** (not started): learn from **pairwise comparisons** between the evicted block and the blocks
  kept, so the learner targets which choice was better rather than each block's absolute outcome. Keep a
  **set-dueling selector between LRU and the fixed priority** as the low-risk practical design. Success would
  require beating or matching the fixed priority with a learned, workload-dependent order.

**Main limitations of everything above:**
* two repetitions per policy and no significance tests;
* one 4-core configuration (the paper used 16 cores and a perfect L3);
* two workloads carry the policy conclusions, one of them exploratory;
* the sharing feature is barely exercised by the main workload.
