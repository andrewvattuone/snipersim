# Advisor preparation: slides, glossary, likely questions

Companion to [`EXECUTIVE_SUMMARY.md`](EXECUTIVE_SUMMARY.md) and [`HANDOFF_REPORT.md`](HANDOFF_REPORT.md). The
numbers are the report's; check them against the report before presenting.

---

## 1. Suggested slide outline (8 slides)

**1. Problem and goal**
* Private-L2 replacement in a 4-core MESI multicore: can a block's coherence state and sharing degree pick better
  victims than LRU?
* The approach: the Souza–Freitas reinforcement-learning idea (IEEE Access 2024).
* The four success criteria: correct; fewer L2 misses; faster; learning beats fixed rules.

**2. System and the MESI details that matter**
* The hierarchy:
  * L1 32 KB;
  * private L2 256 KB 8-way, where the policy runs;
  * shared, inclusive 8 MB L3;
  * prefetchers off.
* Only M is writable: an upgrade is an invalidate plus refill.
* Read fills get S if another core holds the line, else the L3's E or M.
* The delivered sharer hint; heavy means ≥ 3 of 4 cores.
* The MESI bug we found and fixed in Sniper before the RL work.

**3. The algorithm (one diagram)**
* Window = the 4 oldest ways; score = w_state + w_sharing; evict the highest; ties go to LRU.
* The 16-entry history per set and the four ±1 rewards.
* EMA update, α = 0.1.
* The worked example from `ALGORITHM_AUDIT.md` §3.2.

**4. Engineering and validation**
* Header-only cores with unit tests; Sniper hooks; the event classification.
* Stage 5: 700 simulations, 0 coherence violations, every learning-event check passing.
* Two Sniper races found and fixed.
* Every run carries checkers and a manifest; every result is archived with checksums.

**5. Results (one table plus one sentence)**
* The Ocean and LU L2-miss table for LRU, sf_lru, rl-n4, frozen, priority and random (from the executive summary).
* The sentence: "**the fixed priority beat the online learner in every repetition; no policy changed run time.**"
* State the caveats on the slide: 2 repetitions, one configuration, LU exploratory.

**6. Why the learner loses (Stage 7)**
* Bursty rewards and an event-counted memory, so unstable weights.
* Stable weights (discounted mean) learned the wrong thing: +11% misses and pinning.
* Root cause: absolute per-block labels, not a comparison between candidates. On Ocean, 93% of "good" evictions are
  reused later; under the priority rule, its own rewards say "keep S".

**7. Offline model and calibration**
* Trace plus a shadow cache to measure headroom without many simulations.
* First calibration FAIL; investigation into the MESI write permissions; correction derived from Sniper's code.
* Held-out PASS (largest error 0.15 points), with its narrow scope.

**8. Proposed Stage 8 and the decisions needed**
* Pairwise (relative) rewards as the research direction; a set-dueling LRU/priority selector as the practical
  baseline.
* Questions for the advisor:
  * Is the contribution a learning result or a coherence-aware replacement result?
  * Which metric is primary?
  * Should we add a workload that exercises sharing under real L2 pressure?

*Optional backup slides:* why run time did not move (the L3/DRAM argument and the about 1% upper bound); the
branches and what is merged.

---

## 2. Glossary

**Architecture**

| Term | Meaning here |
|---|---|
| Private / shared cache | One per core (L1, L2), or one for all cores (L3) |
| Inclusive hierarchy | A line in L1 is also in L2, and in L2 also in L3. Evicting from a lower level **back-invalidates** the copies above |
| Set, way, associativity | A set holds 8 ways (lines) in the L2; replacement picks one way of the set |
| LRU | Evict the least recently used way. "LRU position 0" is the oldest |
| Victim | The block chosen for eviction |
| Demand miss vs upgrade miss | A demand miss means the block is absent. An upgrade miss is a store to a present S or E block; Sniper invalidates and refills it in M |
| MPKI | Misses per 1,000 instructions |
| Dirty eviction / write-back | Evicting an M block writes its data to the next level |
| ROI | Region of interest: the measured part of a program (here its parallel phase) |
| Memory-bound / compute-bound | Run time limited by memory traffic (Ocean) or by computation (LU) |
| Thrashing | A working set slightly larger than the cache: LRU evicts each block just before its reuse |
| Belady (OPT) | The hindsight-optimal policy: evict the block whose next use is farthest away |
| ATD / shadow tags | A small auxiliary tag directory that simulates another policy on sampled sets |
| Set dueling | Run two policies on a few sample sets each and use the better one on the rest |

**Coherence**

| Term | Meaning here |
|---|---|
| MESI | Line states: **M**odified (dirty, only copy, writable), **E**xclusive (clean, only copy), **S**hared (clean, other copies may exist), **I**nvalid |
| Coherence invalidation | Another core's write removes our copy |
| Downgrade | Another core's read turns our M or E copy into S, with a write-back if M |
| Directory / presence bits | Which cores hold a line. Here the shared L3 tracks which L2s hold each line |
| Delivered sharer hint | The number of cores (including us) that held the line when we filled it, reported by the L3. Realistic but stale |
| Heavily shared (AT) / lightly shared (BT) | Above / below the threshold: hint > cores/2, so ≥ 3 of 4 |
| MESI fill fix | Our Sniper fix: a read fill gets S when another private cache holds the line |
| M-on-read | In Sniper, a solo reader of a dirty L3 line gets M, so "M" does not always mean "written by this core" |
| SHARED_UPGRADING | A transient L3 state during an in-place upgrade; never a valid victim |

**Reinforcement learning, as used here**

| Term | Meaning here |
|---|---|
| Feature / class | A block's MESI state (S, E, M) and sharing class (BT, AT): 6 classes |
| Weight | One number per feature value in [−1, 1]; positive = more evictable |
| Score | `V = w_state + w_sharing` |
| Window (N) | The N oldest ways that compete; N = 4 in `rl-n4` |
| Passed over / protected | A window candidate older than the victim; it keeps its LRU position and is flagged |
| Eviction history | Per set, the last 16 victims with their features |
| evict-bad (−1) / evict-good (+1) | A victim missed while still in the history / aged out of it |
| protect-good (−1) / protect-wasted (+1) | A protected block hit / evicted without a hit |
| EMA | `w ← w + α (r − w)`, α = 0.1 per reward event |
| Discounted mean | The Stage 7 estimator: the mean of rewards discounted by elapsed *decisions* (horizon 65,536) |
| Global vs per-set weights | One table per L2 (shared by its sets) vs one table per set |
| Pinning / repeated protection | A low-scoring block passed over at every miss, holding a way without reward |
| Frozen weights | Learning off, with fixed weights (from LU's rl-n4 average) |
| Fixed priority | Learning off; rank S > E > M, then AT > BT, within the same window |
| Departure from LRU | The share of decisions whose victim is not the oldest candidate |
| Absolute vs relative (pairwise) reward | A label on one block's own outcome vs a comparison between the victim and a kept candidate |
| Shadow model / counterfactual replay | Replaying a recorded access stream under another policy offline |
| Calibration, null pair, held-out | Testing the shadow against real runs. A null pair is two runs of one policy (the noise floor). Held-out means runs not used to develop the model |

---

## 3. Ten likely advisor questions, with answers

**Q1. Does your RL policy beat LRU?**
* **L2 misses: slightly.** Online rl-n4 has 1.1% fewer than LRU on Ocean `small` and 3.2–5.1% fewer on LU `tiny`,
  in every repetition (2–3 per policy).
* **Run time: no.**
* **And it does not beat the fixed priority,** which uses the same features and window without learning: −2.2%
  (Ocean) and −8.1% (LU).
* **Caveats:** these are ranges from two repetitions, not significance tests. LU `tiny` is exploratory because of an
  upstream defect in that benchmark.

**Q2. Why didn't fewer L2 misses make the program faster?**
* **Ocean:** the L2 policy does not change L3 misses or DRAM traffic (within ±0.5%), which is what makes Ocean
  memory-bound. A saved L2 miss becomes an L3 hit, about 35 cycles. Even if every saved miss were fully exposed
  latency, the gain would be about 1% (estimate), comparable to the 0.3–0.5% run-to-run spread.
* **LU:** the L3 holds the whole working set and the kernel is compute-bound.
* **Not explained:** a policy with 18% *more* L2 misses on Ocean also showed no IPC change.

**Q3. How do you know the policy doesn't break coherence?**
* Stage 5 ran 700 simulations with a per-line MESI invariant check on every line of the coherence regression (144 of
  144 pairs pass), a stale-write-hit checker (0 hits), and event and learning checkers.
* The same checkers ran on every experiment since.
* The only failures were aggregate-threshold misses, each explained by the per-line data, and 4 crashes. The crashes
  were pre-existing Sniper races, which we fixed.

**Q4. Where does the sharing information come from? Is it realistic?**
* The shared L3 already keeps presence bits. On each fill it reports how many other L2s hold the line. We store that
  count + 1 in the block: the "delivered hint".
* It is realistic (a few bits on the response) but stale, because it is not updated afterwards.
* The oracle count is used only in diagnostics, never in decisions, and Stage 5 verified this.

**Q5. Why did the frozen weights and the fixed priority beat online learning?**
* **The weights are unstable.** Rewards arrive in long phases, and α = 0.1 per event makes each weight follow them.
  Rare classes (S, AT) update rarely, so they are stale.
* **The reward measures the wrong thing.** It labels each block's own future ("returned within 16 evictions?"), not
  whether evicting it was better than evicting the alternative. On Ocean, 93% of "good" evictions are reused later.
  Under the priority rule, its own outcomes say "keep S", the opposite of its winning strategy.

**Q6. You stabilised the weights. Why did that make things worse?**
* With the discounted mean, the weights became stable and learned E > M > S, the predicted order.
* But that order *keeps* S and heavily shared blocks. Because passed-over blocks are not promoted, a rarely
  occurring "keep" class gets pinned in the window: about 1.07 of 8 ways per set, with one block passed over 1,855
  times.
* L2 misses rose 11%.
* The noisy rl-n4 had been breaking those pins by chance.

**Q7. Is the fixed priority itself a result?**
* **At the cache level, yes:** a coherence-aware fixed rule cut L2 misses by 2.2% on Ocean and 8.1% on LU, and
  traded about 16% fewer invalidations for about 13% more downgrades on Ocean.
* **But it has caveats:**
  * its rule was motivated by LU findings;
  * run time did not change;
  * there are two repetitions;
  * part of the benefit is generic resistance to LRU thrashing: random departures also helped on LU.
* A non-coherence thrash-resistant baseline (for example set dueling or an RRIP-style policy) is the missing
  comparison.

**Q8. What is the shadow model, and can we trust it?**
* **What it is:** an offline replay of a recorded L2 access stream under another policy. It models L1 inclusion,
  coherence invalidations and downgrades, and MESI write permissions.
* **Calibration:** it was calibrated by predicting priority's miss reduction from LRU's trace and the reverse. After
  a failed first calibration and a code-derived correction, it passed a pre-registered **held-out** calibration on
  four new runs (largest error 0.15 points, against a ±0.5 tolerance and a noise floor of 0.17).
* **Scope:** only Ocean `small`, this configuration and LRU vs priority. It can screen fixed rankings coarsely; every
  candidate must be confirmed in Sniper.

**Q9. How does this compare with the paper's results?**
* **The setups differ.** We used 4 cores, not 16; a real 8 MB L3, not a perfect 16 MB one; the policy on the L2
  only, not L1 + L2; and a newer Sniper.
* **The workloads differ.** The paper's CAP Bench kernels could not be reproduced as published: the ROI, the
  configuration and the code are unavailable, and two kernels have upstream defects.
* **The faithful reproduction of the paper's algorithm (`sf_lru`) increased L2 misses in every test** (+11–18%
  Ocean, +46–73% LU). The paper's description is also internally contradictory on whether the dominant class is kept
  or evicted; both readings were implemented.
* A list of what to request from the authors is in `CAPBENCH.md`.

**Q10. What would you do next, and how would you know it worked?**
* **Pairwise rewards:** learn from comparisons between the victim and the blocks kept (a Bradley–Terry update),
  which targets "which choice was better" directly.
* **Success:** it beats LRU, matches or beats the fixed priority, and learns *different* orders on workloads where
  the best order differs. If it just converges to the priority order, report that learning reproduces the fixed
  rule.
* **Alongside:** a set-dueling LRU/priority selector as the practical baseline, and at least one new workload that
  exercises sharing under real L2 pressure.
* **Before any claim:** more repetitions.

---

## 4. Points to state carefully

* **Wording:**
  * Say "fewer L2 misses in every repetition", not "statistically significant".
  * Say "the online learner did not outperform the fixed rules on these two workloads", not "RL does not work".
  * Say "the shadow model passed a held-out calibration on one workload", not "the model is validated".
* **The frozen weights came from LU itself:** in-sample on LU, out of sample on Ocean.
* **The random control is "random departure from LRU within the window",** not random replacement. Its rate was not
  matched to rl-n4's on Ocean.
