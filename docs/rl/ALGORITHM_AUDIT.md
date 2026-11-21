# RL cache replacement: algorithm audit before the Sniper port

Audit of the Souza–Freitas reinforcement-learning replacement policy and of our modified version, done before porting
both from ChampSim to Sniper (branch `feature/rl-port`). It records what the primary sources specify, what our two
ChampSim implementations actually do, every issue found, and the agreed plan. Nothing here has been implemented yet.

| | |
|---|---|
| Date | 2026-11-20 |
| Sniper baseline | `27e0d326` (MESI fill fix, delivered sharer hint, presence tracking; see `experiments/sharer-counts/COHERENCE_VALIDATION.md`) |
| ChampSim reference | `SCU-Senior-Design-Project`, branch `mesi-implementation`, `148b9c3` (read-only) |
| Status | Stages 1-4 implemented (cores and unit tests, Sniper event hooks, `sf_lru` / `rl_lru` policies, diagnostics): see [`POLICY_CORES.md`](POLICY_CORES.md). Stage 5 (correctness validation): [`STAGE5_VALIDATION.md`](STAGE5_VALIDATION.md). Stages 6+ not started |

Paths in this document:

* `CS/` = `SCU-Senior-Design-Project/ChampSim/replacement/` (at `148b9c3`)
* `SN/` = `snipersim/common/core/memory_subsystem/` (at `27e0d326`)

## Contents

1. [Sources](#1-sources)
2. [The original algorithm (paper and thesis)](#2-the-original-algorithm-paper-and-thesis)
3. [Our modified algorithm (`rl_mesi`)](#3-our-modified-algorithm-rl_mesi)
4. [Comparison: sources vs. implementations](#4-comparison-sources-vs-implementations)
5. [Issues, ranked](#5-issues-ranked)
6. [Agreed initial decisions](#6-agreed-initial-decisions)
7. [Integration-time vs. later issues](#7-integration-time-vs-later-issues)
8. [Hardware cost notes](#8-hardware-cost-notes)
9. [Staged implementation and test plan](#9-staged-implementation-and-test-plan)

---

## 1. Sources

| Ref | Source | Access | Read |
|---|---|---|---|
| **[P24]** | M. A. Souza, H. C. Freitas, "Reinforcement Learning-Based Cache Replacement Policies for Multicore Processors," *IEEE Access* 12, 2024, doi 10.1109/ACCESS.2024.3409228 | `SCU-Senior-Design-Project/notes/Reinforcement_Learning-Based_Cache_Replacement_Policies_for_Multicore_Processors (1).pdf` | All 12 pages, Figure 2 rendered |
| **[T21]** | M. A. Souza, "Way-Replacement Algorithms for Multicore Processors Based on Coherence and Sharing States with Reinforcement Learning," PhD thesis, PUC Minas, 2021 (advisor H. C. Freitas). <https://bib.pucminas.br/teses/Informatica_MatheusAlcantaraSouza_19331_Textocompleto.pdf> | Downloaded (182 pp.) | §4.2, §4.2.1.1 (pp. 56-58); Ch. 5 and Ch. 6 strategy definitions with Figs. 5.1 and 6.1 (pp. 63-64, 85); Ch. 7 pp. 105-106 with Fig. 7.1; §7.2 (pp. 125-127); §7.3 |
| **[S19]** | M. A. Souza, H. C. Freitas, F. Pétrot, "Coherence State Awareness in Way-Replacement Algorithms for Multicore Processors," SSCAD 2019, pp. 240-251, doi 10.5753/wscad.2019.8672. <https://proceedings-sol.sbc.org.br/index.php/sscad/article/download/8672/8573> | Downloaded | §3, §4.1 |

The thesis and the 2024 paper describe the RL algorithm in nearly identical words; the paper adds the worked example in
§II-C. All three used Sniper, so their setups are directly relevant to this port:

| | [S19] | [T21] / [P24] |
|---|---|---|
| Sniper | (not stated) | 7.3 |
| Cores | 16 | 16 |
| Protocol | MSI | MESI, directory-based |
| Policy applied to | shared 2 MB L2 (L1 LRU) | private L1 and L2 together ("local replacement hazard") |
| L2 | shared by 16 | private 256 kB 8-way (main results); shared 4 MB also evaluated |
| L3 | 8 MB shared, LRU | 16 MB shared, **"perfect"** (all requests hit) |
| Prefetching | off | off |

Our earlier design notes (an additive update `w += alpha * r`) are not in either repository; the C prototype
`SCU-Senior-Design-Project/cache_sim_skel/cache.c:228-250` already uses the EMA rule with **per-set** weights.

---

## 2. The original algorithm (paper and thesis)

### 2.1 What the sources support explicitly

* **Eviction history table** ([P24] §II-B, [T21] p. 106): one per cache set, `2·K` entries (K = associativity). Entry =
  block tag + 1 threshold bit ("above (0) or below (1)") + 2 coherence-state bits. On an eviction the victim's entry is
  added; if the incoming block's tag is in the history, that entry is removed; when full, the **least recently updated**
  entry is dropped. Storage `2·K·N·(T+3)` bits (N sets, T tag bits).
* **Weights** ([P24] §II-B): `wM, wE, wS` = fraction of history entries in each state ("the sum of these weights
  always equals 1"); `wAT, wBT` = fraction above / below the threshold. Both figures label them **"Weights per set i"**;
  [P24] §IV-E offers "a single eviction history and vector of weights for all sets" only as a space-saving "last
  resort".
* **Rewards** ([P24] §II-A): −1 when a cache miss hits a block recorded in the history (bad eviction), +1 when the
  missing block is not in the history. **No update equation is given anywhere.** Both [P24] §II-B and [T21] p. 106
  say "the exploration-exploitation trade-off is naturally resolved during computation, as this information changes
  **independently of the reward**": the weights are pure history ratios. The −1 reward acts only through the removal
  of the returning block's entry; the +1 has no effect.
* **Threshold** ([P24] §II-A): above iff the sharer bit vector has more than half the cores set ("9 or more" of 16).
* **Initialization** ([P24] §II-B, [T21] p. 106): with no distribution yet, evict Exclusive blocks (least coherence
  traffic) and blocks below the threshold.
* **Next attempt** ([T21] §4.2.1.1, [P24] §II-D, Figs. 2 / 7.1): the base policy picks candidate 1; if it fails the
  check it is kept and the base policy's **second** choice (second LRU for LRU/PLRU, second MRU for MRU, second-lowest
  frequency for LFU) is evicted. The figures say "Try again once" and "Invalid block **or 2nd attempt** → Evict": the
  second candidate is **not** checked. For NRU/SRRIP the NRU bit / RRPV of the rejected block is changed and the search
  re-run.
* **Variants**: RL-State (MESI features), RL-Bitset (sharing features), RL-Both. RL-Bitset gave most of the best results
  ([T21] Fig. 7.31, Table 7.3: mean L1 miss-rate reduction 11.55%).

### 2.2 The selection-rule contradiction

| Supports **protecting** the candidate that matches the dominant class (`SF_RULE=example`) | Supports **evicting** it (`SF_RULE=text`) |
|---|---|
| [P24] Fig. 2 and [T21] Fig. 7.1 (identical flowcharts): "Equal states / Threshold OK → Try again once"; "Different states / Threshold not OK → Evict" | [P24] §II-C: "If the block information corresponds to high-probability elements in the vector, the block is evicted; otherwise, the block is given a second chance" |
| [P24] §II-C worked example: LRU picks an M block, "wM is the highest", so the block is kept and the second LRU block is evicted | [T21] p. 106: same wording as [P24] §II-C |
| Lineage: Fig. 7.1 is a redrawn Fig. 5.1, whose "Equal states → try again" means **keep** ([T21] p. 63, [S19] §3.1: "favors blocks that are not in the same state than the current MRU-State for eviction") | Both sources define the weights as the probability "of being a good eviction decision", so a high weight should mean "evict" |
| [T21] Ch. 7 intro and §7.3 frame RL as learning "which [dynamic] state to use", i.e. a learned version of the Ch. 5 keep-state | The initialization sets E as "the most likely candidate for eviction", which reads as "highest weight" only under the text rule |

Likely explanation (hypothesis): the flowchart was inherited from the keep-priority strategies of Ch. 5/6 while the
prose describes probability-of-good-eviction weights. The thesis does **not** resolve it. Both interpretations are
first-class options in the port (Section 6).

Lineage of the non-RL strategies, for reference:

* Ch. 5 / [S19]: static state (a global state kept with priority) and dynamic MRU-State (per-set state of the most
  recently used block; a candidate in that state gets the next attempt).
* Ch. 6 / [S19]: sharer-count bit. **The polarity flips between sources**: [S19] sets the bit when the count is
  *higher* than 50% and evicts when it is set (evicts heavily shared); [T21] Ch. 6 sets it when the count is *lower*
  than 50% and evicts when set (evicts lightly shared). The RL initialization in [T21]/[P24] evicts lightly shared
  blocks.

### 2.3 Our ChampSim reproduction (`CS/souza_freitas/`)

`sf_core.h` (pure, header-only) and `souza_freitas.{h,cc}` (ChampSim wrapper); run-time switches `SF_MODE`
(`off|state|bitset|both`, default `state`), `SF_BASE` (`lru|srrip`), `SF_RULE` (`text|example`, default `text`).

* Weights recomputed from the per-set history at each decision (`sf_core.h:144-167`); state weights over entries with
  a known state, AT/BT over all entries.
* "Highest" = argmax with ties counted as highest (`sf_core.h:174-182`); `apply_rule` inverts it for `example`
  (`:185`).
* Empty history: evict only E (state check), only light (bitset check), under either rule (`:188-204`).
* RL-Both: evict iff **both** checks say evict (`:216-218`).
* Next attempt: second choice evicted unconditionally (`:250-277`); with LRU the rejected way keeps its LRU position
  (`souza_freitas.cc:64-67`); with SRRIP it gets `maxRRPV-1` (`sf_core.h:357-361`).
* Rewards counted on every non-writeback access, hits included (`souza_freitas.cc:119-121`, `sf_core.h:285-293`);
  only the removal matters.
* Sharer count read from the directory's current mask (oracle) at decision time (`souza_freitas.cc:45-55`).

---

## 3. Our modified algorithm (`rl_mesi`)

Core in `CS/rl_mesi/rl_core.h`, ChampSim wrapper `CS/rl_mesi/rl_mesi.{h,cc}`, design notes `CS/rl_mesi/README.md`.

### 3.1 Mechanics

* **Features** of a block: state ∈ {S, E, M, Iprev} and sharing class (heavy iff count > threshold). In private
  caches the count is the delivered count stored at grant time (`rl_mesi.cc:95-109`).
* **Weights**: one table per cache, shared by all sets (`rl_mesi.h:152`): 4 state weights + 2 sharing weights,
  initialized to 0 (`rl_core.h:132-165`).
* **Score**: `V(b) = w_state[s(b)] + w_share[a(b)]` (one-hot features, so the paper's
  `wM·xM + wE·xE + wS·xS + wAT·xAT + wBT·xBT` reduces to two table lookups). Higher = more attractive to evict.
* **Victim selection** (`rl_core.h:294-323`): LRU order; among the `RL_WINDOW` = N oldest ways (default 4), evict the
  highest score; strict `>` while scanning oldest first sends ties to the older way. With all weights 0 this is exactly
  LRU. Ways **older** than the victim are "passed over"; their recency is not changed.
* **Commit** (`rl_core.h:326-350`): passed-over ways are marked protected and their decision-time features saved; if
  the victim was itself protected, its saved features get **+1** ("protect_wasted").
* **Fill** (`rl_core.h:356-392`): the victim's tag and decision-time features enter the history; an entry that ages out
  gives its features **+1** ("evict_good"). A fill into a way the policy believed still occupied infers an
  invalidation; the tag is remembered and the block returns as Iprev.
* **Hit** (`rl_core.h:395-407`): if the block was protected, its saved features get **−1** ("protect_good").
* **Miss** (`rl_core.h:409-414`): if the tag is in the history, its features get **−1** ("evict_bad"), entry removed.
* **Update**: `w += alpha * (r - w)` for the active state weight and sharing weight (mode `both`), alpha = 0.1, so
  every weight stays in [−1, 1]. Signs are consistent: +1 makes a class more evictable, −1 more retained.
* **Optional additions**, all **on by default** in ChampSim: Iprev (`RL_ISTATE=1`), adaptive threshold
  (`RL_THRESHOLD=duel`, `threshold.h`), BRRIP set dueling (`RL_DUEL=1`, `duel.h`); exploration `RL_EPSILON=0`
  (`rl_core.h:46-53`, `rl_mesi.cc:81`, `README.md:71`). The core algorithm needs
  `RL_MODE=both RL_THRESHOLD=fixed RL_DUEL=0 RL_ISTATE=0 RL_EPSILON=0`.

### 3.2 Worked example

4 cores (heavy iff ≥ 3 sharers), one 8-way L2 set, N = 4, alpha = 0.1, mode `both`. Current global weights:
`wS = −0.30, wE = +0.20, wM = −0.10, w_light = +0.10, w_heavy = −0.40`.

**Decision.** The four oldest ways, oldest first:

| Block | State | Delivered count | Class | Score |
|---|---|---|---|---|
| A | M | 1 | light | −0.10 + 0.10 = 0.00 |
| B | S | 3 | heavy | −0.30 − 0.40 = −0.70 |
| C | E | 1 | light | 0.20 + 0.10 = **0.30** |
| D | S | 2 | light | −0.30 + 0.10 = −0.20 |

C is evicted (LRU would have evicted A). A and B are passed over and become protected with snapshots (M, light) and
(S, heavy); D is younger than C and is not protected. `{C, E, light}` enters the full 16-entry history; the oldest
entry `{X, S, light}` ages out: +1 gives `wS = −0.30 + 0.1·(1 + 0.30) = −0.17`, `w_light = 0.10 + 0.1·(1 − 0.10) = 0.19`.

**Later events.**

1. C misses while still in the history, −1: `wE = 0.20 + 0.1·(−1 − 0.20) = 0.08`, `w_light = 0.19 − 0.119 = 0.071`.
2. B is hit while protected, −1: `wS = −0.17 − 0.083 = −0.253`, `w_heavy = −0.40 − 0.06 = −0.46`.
3. A is later evicted without having been hit, +1: `wM = −0.10 + 0.11 = 0.01`, `w_light = 0.071 + 0.093 = 0.164`.
   A also enters the history and will receive a second outcome later (issue I-6).

**Same set under the paper.** History of 16 entries: M 3, E 9, S 4; AT 2, BT 14, so `wM = 0.19, wE = 0.56,
wS = 0.25, wAT = 0.125, wBT = 0.875`. Candidate 1 is A (M, light):

| Rule | Outcome |
|---|---|
| `text`, state | M not highest → A kept, B (S, heavy) evicted unchecked |
| `example`, state | M not highest → A evicted |
| `example`, both, ChampSim AND composition | A's class (light) is highest → A kept, B evicted |
| Figure read literally (retain only if state and class both match) | States differ → A evicted |

If C then returns, its E entry is removed (`wE` becomes 8/15 = 0.53); when X ages out it is simply dropped. Entry
removal is the paper's only learning.

---

## 4. Comparison: sources vs. implementations

| Behavior | Paper / thesis | Intended modified algorithm | `souza_freitas` (ChampSim) | `rl_mesi` (ChampSim) |
|---|---|---|---|---|
| Weight meaning | Fraction of history entries per class (sums to 1) | Learned eviction value per class | Same as paper | EMA of rewards |
| Weight scope | Per set | Global per cache (agreed, Section 6) | Per set (from per-set history) | Global per cache |
| Update | None explicit; independent of reward | `w += alpha (r − w)` | Rewards counted, never applied | As intended |
| +1 trigger | Miss (§II-A) / access (§II-B) to a block not in history; no feature attached | Entry ages out of history | Counted on every access incl. hits | Ages out |
| −1 trigger | Miss to a block in history | Same | Same (entry removed) | Same, applied to saved features |
| Protection feedback | None | Yes | None | Hit −1; evicted unhit +1 |
| Selection | Candidate 1 checked, one unchecked retry | Highest score among N oldest | Paper; `SF_RULE=text\|example` | Highest among N (default 4), ties older |
| Rejected / passed-over recency | Unspecified | Not specified | Stays LRU (LRU base) | Stays LRU |
| Initialization | Evict E; evict light | Weights 0, i.e. LRU | Paper bias under both rules | Zeros, exact LRU |
| Threshold | > N/2 | Fixed > N/2 (agreed) | > N/2 | Default `duel` (adaptive) |
| Sharer source in private caches | "Directory bit vector" (mechanism unspecified) | Delivered hint | Oracle (directory's current mask) | Delivered (stale) count |
| Coherence invalidations | Not addressed | No reward for coherence misses | Not handled | Inferred lazily; Iprev |
| Optional features | – | Off for stage 1 | – | Iprev, C1, C2 on by default |

---

## 5. Issues, ranked

Status legend:

* **Confirmed**: verified from code or primary-source text.
* **Mechanism confirmed, impact unmeasured**: the behavior certainly occurs; how much it matters needs experiments.
* **Hypothesis**: plausible, needs an experiment or test to confirm.
* **Ambiguity**: the sources do not determine the behavior; preserve as an option.
* **Limitation**: a modeling or methodology constraint, not a bug.

"When" says whether the issue must be handled while integrating into Sniper (**integration**) or can wait for
experimental evaluation (**later**). Section 7 collects them.

### I-1. Passed-over / rejected blocks can be pinned indefinitely

* **Status**: mechanism confirmed, impact unmeasured. **When**: diagnostics at integration; fix later.
* **Cause**: neither selector changes the recency of a block it declines to evict. In `rl_mesi`, passed-over ways keep
  their LRU positions (`rl_core.h:319-321`, `commit` at `:326-350` does not touch `last_used`). In the reproduction with
  an LRU base, candidate 1 stays at LRU (`souza_freitas.cc:64-67`).
* **Consequence**: while its class keeps a low score, the same block is re-examined and passed over at every miss.
  It is never hit (dead) and never evicted, so it **never produces a reward** and the weights are never corrected by it.
  `rl_mesi` can pin up to N−1 of the oldest ways (3 of 8 with N = 4, 37% of the set), leaving LRU over the remaining
  ways. The reproduction can pin one way, turning the paper's "second chance" into an unlimited one. The SRRIP variant
  of the reproduction does not have this problem (rejected RRPV is lowered and ages back).
* **Validate**: per-block pass counter; histogram of passes before eviction or hit; maximum pass count; time-weighted
  number of pinned (passed ≥ k times, unhit) ways per set. A deterministic unit test with a dead M block and wM < wE.
* **Address (later, options)**: cap passes per block (e.g. eligible again after k passes), promote a passed-over block
  (like Sniper's QBS `moveToMRU`, `SN/cache/cache_set_lru.cc:60-66`), or add an age term to the score. For the
  reproduction this is ambiguity A-2 (`retain = stay | promote`).

### I-2. Coherence misses after an eviction are blamed on the eviction

* **Status**: mechanism confirmed, frequency unmeasured. **When**: oracle diagnostic at integration; mitigation later.
* **Cause**: block X is evicted from this private cache (enters the history); another core then writes X, which would
  have invalidated our copy; this core reads X again and misses. X is in the history, so the eviction gets −1
  (`rl_core.h:409-414`; the paper's entry removal is the same). The private cache is no longer a sharer after the
  eviction (its L3 presence bit was cleared), so it never sees the invalidation.
* **Consequence**: the policy is penalized for evicting S and heavily shared lines, exactly the ones most likely to be
  invalidated anyway, and learns to retain them. Biases `wS` and `w_heavy` downward on write-shared workloads.
* **Validate**: oracle-only statistic at each history hit: "another core wrote this line since our eviction" (for
  example a per-line write epoch kept at the L3 for diagnostics only). Report the fraction of `evict_bad` rewards that
  are such coherence misses, per feature class. Use `sharetest` mode `u`/`w` and FFT.
* **Address (later)**: only if the fraction is significant. Realistic options need an L3-side signal on the fill (e.g.
  a bit "line was written by another core since your last copy"), which has a hardware cost and must be modeled
  explicitly. Do not use the oracle in decisions.

### I-3. Sniper MESI semantics differ from ChampSim

* **Status**: confirmed (Sniper code). **When**: integration.
* **Cause (a), M on read**: since the MESI fill fix, a solo reader of a dirty L3 line receives **M** in its private
  cache (`SN/parametric_dram_directory_msi/cache_cntlr.cc`, `copyDataFromNextLevel`; see
  `COHERENCE_VALIDATION.md`). ChampSim never grants M on a read. In Sniper, M therefore means "dirty in this core or
  dirty below", not "written by this core".
* **Cause (b), upgrade = invalidate + refill**: Sniper has no silent E→M or S→M. A write that is not permissible
  invalidates the private copy and refetches: L1 at `cache_cntlr.cc:1006-1011`; L2 invalidates its upper levels with
  `Transition::UPGRADE` (`:1700-1701`) and itself (`:1716`) before forwarding, then re-inserts. Only M is writable
  (`SN/cache/cache_state.h`).
* **Consequence**: features: the M class mixes two populations. Metadata: an upgrade would wipe the block's RL
  metadata (protection flag, snapshot), move it to MRU, possibly to a different way, and could be mistaken for another
  core's invalidation (wrong Iprev, wrong reward suppression).
* **Validate**: statistic "M victims granted M on a read and never written locally"; unit/integration test that an
  upgrade preserves metadata and produces no history entry and no reward; `coherence_regress` trace of an upgrade with
  the policy on.
* **Address**: pass an invalidation reason to the policy (Section 9, Stage 2); treat `UPGRADE` as a carry-over, not an
  eviction or a coherence invalidation. For (a), keep the cache state unchanged and add the statistic first; a 1-bit
  "written locally" per-line flag is the realistic option if M-on-read turns out to matter (decision pending).

### I-4. Global weights with alpha = 0.1 adapt very quickly

* **Status**: hypothesis (agreed configuration; effect unmeasured). **When**: later.
* **Cause**: every reward from every set moves the same six weights (`rl_mesi.h:152`); with alpha = 0.1 the effective
  memory is roughly the last 10-20 events cache-wide.
* **Consequence**: the class ranking can flip many times per thousand misses, so decisions follow very recent,
  set-unrelated outcomes. The sources use per-set weights (but per-set weights learn slowly and cannot use sampled
  histories, Section 8).
* **Validate**: periodic weight snapshots (time series), ranking-flip counter, variance across repeated runs; sweep
  alpha ∈ {1/8, 1/16, 1/64, 1/256} and scope global vs. per set.
* **Address**: keep alpha = 0.1 and global scope for the first port (Section 6) but implement both as parameters so the
  sweep needs no code change.

### I-5. ChampSim `rl_mesi` defaults are not the core algorithm

* **Status**: confirmed. **When**: integration (choose defaults); check old results later.
* **Cause**: Iprev, the adaptive (dueling) threshold and BRRIP dueling are enabled by default (`rl_core.h:46-53`,
  `rl_mesi.cc:81`, `CS/rl_mesi/README.md:71`).
* **Consequence**: any earlier ChampSim result run with default settings evaluates the full system, not the core
  learner. Comparisons with the Sniper port must use matching switches.
* **Address**: Sniper defaults = core algorithm (Section 6); print every parameter into `sim.cfg`/stats.

### I-6. Protected blocks receive two rewards

* **Status**: mechanism confirmed, impact unmeasured. **When**: later (keep ChampSim behavior, add an option).
* **Cause**: a protected block that is later evicted unhit gets +1 at once (`rl_core.h:344-349`) and then a second,
  history-based outcome after its eviction (`:361-365` or `:409-414`). A block that returns soon gets +1 then −1.
* **Consequence**: protected classes get about twice the feedback of other evictions; the immediate +1 can contradict
  the history outcome.
* **Validate**: count blocks that receive both, and the sign agreement between the two rewards, per class.
* **Address**: option to drop the immediate +1 and let the history outcome judge the protection.

### I-7. The two feature groups are learned independently

* **Status**: confirmed design property; whether it matters is a hypothesis. **When**: later.
* **Cause**: each weight is an EMA of its own feature's rewards (`rl_core.h:151-161`); the score adds them. This is not a
  jointly fitted linear model (an LMS update `w_i += alpha (r − V) x_i` would be).
* **Consequence**: correlated features (S and heavy usually co-occur) are counted twice; interactions (e.g. "M and
  heavy") cannot be represented.
* **Address**: a joint 8-entry table (state × class) costs about the same; evaluate after the core port.
* **Note**: the EMA itself is preferred over the additive rule `w += alpha * r`, which is unbounded, scales with how
  often a class is evicted (frequency, not quality) and never forgets. The EMA converges to P(good) − P(bad) per class.

### I-8. The paper's rule reinforces or oscillates

* **Status**: property of the paper's design; dynamics are a hypothesis. **When**: later (reproduce as written).
* **Cause**: the ratio weights measure the share of each class among recent, not-yet-returned evictions, which depends
  on what the policy itself evicts (selection bias), not P(good eviction | class).
* **Consequence**: under `text`, the most-evicted class dominates the history and keeps being evicted
  (self-reinforcing); a class that is never evicted is never "highest" and is always retained. Under `example`, the
  dominant class gets protected, its share falls and the preference oscillates.
* **Validate**: per-set weight time series and dominant-class flip rate for both rules.

### I-9. Paper rule details (reproduction)

* **Status**: ambiguity. **When**: integration (expose as options).
* **RL-Both composition**: ChampSim evicts iff both checks say evict (`sf_core.h:216-218`). Under `example` this keeps a
  block if *either* class is the highest, while the figure reads "keep only if both match". Option `combine = and | or`.
* **Argmax ties**: ties count as highest (`sf_core.h:174-182`); with a three-way tie `example` retains every candidate 1
  and `text` evicts every candidate 1. Keep, document, and count tie decisions.
* **"High-probability"**: argmax is supported by the worked example ("wM is the highest") and the figure ("Equal
  states"); a "> 1/3" reading is possible but unsupported.

### I-10. Selection-rule contradiction (text vs. example)

* **Status**: ambiguity (Section 2.2). **When**: integration (both selectable).
* **Address**: `rule = text | example`, both first-class; neither is the "correct" reproduction.

### I-11. Recency of the retained candidate

* **Status**: ambiguity. **When**: integration (option).
* **Cause**: no source says what happens to candidate 1's replacement state when it is kept. ChampSim leaves it at LRU
  (LRU base) but lowers its RRPV (SRRIP base). Sniper's own QBS promotes rejected blocks to MRU; the authors used Sniper
  7.3, which has QBS, but nothing says they followed it.
* **Address**: `retain = stay | promote`; `stay` matches ChampSim. Interacts with I-1.

### I-12. Reward timing in the paper

* **Status**: ambiguity, statistics only. §II-A: rewards on misses; §II-B and [T21]: on accesses. The reproduction
  counts on every access, so its +1 count includes hits. No effect on decisions. Count misses only in Sniper and say so.

### I-13. Threshold boundary and polarity differ between sources

* **Status**: ambiguity. **When**: integration (fix the definition).
* **Cause**: [P24]: above iff count > N/2. [T21] Ch. 6: bit set when count is "lower than" 50%, so exactly N/2 is
  above (with 4 cores, 2 sharers would be heavy). [S19]: polarity inverted (evicts heavy).
* **Address**: use [P24]'s explicit `count > N/2` (4 cores: heavy iff ≥ 3). The count includes the requester, as in
  ChampSim's delivered count.

### I-14. Where the paper's private caches get their sharer vector

* **Status**: ambiguity and modeling limitation. **When**: integration (make the source explicit).
* **Cause**: [P24]/[T21] cite "the directory" bit vector but not how a private L1/L2 sees it. In stock Sniper the
  directory tracks LLC instances; our validation showed its count is always 1 with one shared L3, and with the thesis's
  "perfect" L3 it is unclear whether the directory sees anything. Only [S19]'s shared-L2 setup has a natural local
  vector (the L2's presence bits).
* **Address**: the reproduction uses the delivered hint (`m_sharer_hint`, from L3 presence bits, realistic) by
  default; `sharer_source = oracle` (current upper-level sharers) only for labeled controlled comparisons. Note this
  difference whenever reproduction results are compared with the paper's.

### I-15. "Perfect" L3 may break presence tracking

* **Status**: hypothesis (untested). **When**: before any paper-like configuration.
* **Cause**: a perfect cache may not insert lines, so `m_cached_locs` and therefore the delivered hint may be missing.
* **Validate**: run `experiments/sharer-counts/check_sharers.py` and the hint statistics with
  `perf_model/l3_cache/perfect=true`. Primary experiments use Gainestown's real L3.

### I-16. Sniper does not model replacement latency or the RL structures' cost

* **Status**: limitation. **When**: later (hardware evaluation).
* **Cause**: `getReplacementIndex` is functional; no policy (LRU included) is charged latency. McPAT has no model of
  history tables or weights. Host run time measures C++ cost, not hardware.
* **Address**: analytical estimates (CACTI for history SRAM, event counts × per-event energy); optional configurable
  extra fill latency for sensitivity studies. Section 8.

### I-17. Sniper run-to-run nondeterminism

* **Status**: limitation (measured earlier: simulated time ±0.4%, `sharetest` victim-state counts up to ±22%, FFT
  < 0.6%). **When**: integration (methodology) and later.
* **Address**: ≥ 5 repetitions per configuration, report mean ± range; in-run lock-step checks for exactness (Stage 3);
  shadow-LRU counterfactual in sampled sets (Stage 4).

### I-18. L1 caches receive no sharer hint

* **Status**: limitation (confirmed: `receivesSharerHint()` requires a shared next level). **When**: before the L1-D
  stage.
* **Address**: on an L1 fill from the private L2, copy the L2 block's hint (ChampSim forwards the stored count the same
  way). Needed for the paper's L1-D + L2 configuration.

### I-19. Inclusion hides L1 reuse from the L2

* **Status**: limitation (affects LRU equally). L1 hits do not update L2 recency, so blocks hot in L1 look dead to the
  L2; an L2 eviction back-invalidates L1 (`cache_cntlr.cc:2269-2283`). The resulting L1 miss returns to the L2 as a
  history hit, so the reward is correctly attributed. Note when interpreting L2 results.

### I-20. Delivered hints go stale

* **Status**: limitation, measured. With the MESI fix, hint exactness at L2 victims ranged from 70.7% (sharetest `u`,
  K = 4) to 99.8%, and the heavy/light class differed for up to 18% of victims (`experiments/sharer-counts/README.md`).
  Effect on policy quality: compare `delivered` vs. `oracle` (Stage 6).

### I-21. Minor reproduction / implementation notes

* Reproduction reads the oracle mask (`souza_freitas.cc:45-55`); fine for ChampSim, but the Sniper port must label it
  (I-14).
* `rl_mesi` infers invalidations lazily from fills (`rl_core.h:366-382`); Sniper can report them directly with a
  reason (I-3).
* Sniper must never pick a `SHARED_UPGRADING` way (`SN/cache/cache_set.cc`, `isValidReplacement`).
* The ChampSim unit tests (`ChampSim/test/cpp/src/446-souza-freitas.cc`, `447-rl-mesi.cc`) were read, not run, during
  the audit; none of them exercises I-1, I-2 or I-6.

---

## 6. Agreed initial decisions

Agreed for the first Sniper implementation:

| Item | Decision |
|---|---|
| Weight scope (modified algorithm) | **Global per cache**, as `rl_mesi` (implemented as a parameter so per-set can be tested later) |
| Learning rate | **alpha = 0.1** (parameter) |
| Update rule | `w += alpha (r − w)` on the active state and sharing weights |
| Threshold | **Fixed**, heavy iff delivered count > num_cores / 2 |
| Optional features | **Disabled**: Iprev, adaptive threshold (C1), BRRIP dueling (C2), exploration (epsilon = 0) |
| Paper rule | **Both interpretations preserved**: `rule = text \| example`, first-class, neither default-only |
| Initial comparisons | (1) unmodified LRU, (2) paper `text`, (3) paper `example`, (4) ours N = 2, (5) ours N = 4; then N = 1/8, optional features |
| Baseline | LRU code path untouched; new policies are new names |
| Sharer information | Delivered hint in decisions; oracle only for diagnostics and labeled comparisons |
| Coherence | MESI fix and sharer-count validation preserved; replacement must not change protocol behavior |

Proposed defaults, **not yet confirmed** (chosen for ChampSim parity; each is a parameter):

| Item | Proposed default | Alternative kept as option |
|---|---|---|
| Features (modified) | mode `both` (state + sharing) | `state`, `bitset` |
| Window | N = 4 (and N = 2 in the initial set) | 1, 8 |
| Protection feedback | on | off; drop immediate +1 (I-6) |
| Pinning guard | none | pass cap / promote (I-1) |
| Paper `retain` | `stay` | `promote` (I-11) |
| Paper RL-Both | `and` | `or` (I-9) |
| Paper mode | `both`, plus `bitset` (paper's best) | `state` |
| History | 2·K entries per set, full tags | partial tags / sampled sets (hardware study) |
| M granted on a read | treat as M, add statistic | written-locally bit (I-3a) |
| Cache levels | private L2 first | L1-D + L2 after I-18 |

---

## 7. Integration-time vs. later issues

**Must be handled during Sniper integration** (correctness of the port, faithful options, measurability):

* I-3: invalidation reasons passed to the policy; `UPGRADE` carries metadata over; M-on-read statistic.
* I-5: core-algorithm defaults; all parameters printed.
* I-9, I-10, I-11: paper options `rule`, `combine`, `retain` exposed.
* I-13: threshold definition fixed to `count > N/2`.
* I-14: sharer source explicit (`delivered` default, `oracle` labeled).
* I-21: `SHARED_UPGRADING` exclusion; labeling of oracle use.
* I-17: lock-step N = 1 / learning-off equivalence check against LRU.
* Diagnostics (measure now, decide later): I-1 pass counts, I-2 coherence-miss attribution, I-4 weight time series,
  I-6 double-reward counts.
* LRU baseline unchanged; coherence regressions pass with every policy.

**Can wait for experimental evaluation**: fixes for I-1, I-2, I-6; alpha and scope sweeps (I-4); joint feature table
(I-7); rule dynamics analysis (I-8); perfect-L3 configurations (I-15); hardware cost model (I-16); L1 hint forwarding
(I-18, before the L1-D stage); delivered vs. oracle comparison (I-20); optional features.

---

## 8. Hardware cost notes

Simulated hardware vs. simulator: Sniper charges no latency for victim selection and McPAT does not model RL state; all
costs below are analytical and need confirmation (synthesis / CACTI).

**Victim selection with N candidates.** With global weights there are only 8 (state × class) scores: precompute an
8-entry rank table when weights change; per candidate a 3-bit lookup; then an N-way maximum over the key
{rank, LRU age}:

| N | Comparator levels | Note |
|---|---|---|
| 1 | 0 | plain LRU |
| 2 | 1 | |
| 4 | 2 | |
| 8 | 3 | narrow (≈ 3 + 3 bit) comparators |

The "N oldest" mask is one compare per way (LRU rank ≥ K − N). Selection is on the miss path (≥ 30 cycles to L3), not
the hit path; likely well under a cycle. The paper's next attempt needs one class compare plus the second-LRU way. Per
set ratio weights need 5 small per-set counters (≈ 25 bits/set) maintained on history insert/remove.

**Storage.** Assumes 48-bit physical addresses and 64 B lines.

| Structure | Size | Share of data array |
|---|---|---|
| L2 (256 kB, 8-way, 512 sets) history, full tags: 16 × (33 + 3) bits/set | 36 kB | 14.1% |
| L2 history, 12-bit partial tags | 15 kB | 5.9% |
| L2 history, 64 sampled sets, partial tags (needs global weights) | 1.9 kB | 0.7% |
| L2 protection metadata: 1 bit + 3-bit snapshot per line | 2 kB | 0.8% |
| L1-D (32 kB, 8-way, 64 sets) history, full tags: 16 × (36 + 3) bits/set | 4.9 kB | 15.2% |
| Global weights: 6 × 8-bit | 48 bits | – |
| Per-set weights (L2) | 3 kB | 1.2% |

[T21] Table 7.2 reports +75% total size for its 64 kB example cache with full-tag histories. Storage is independent of
N; N adds comparators and changes behavior.

**Energy per event**: one history lookup (16 tag compares, CAM or set-read) per miss; one history write per eviction;
one weight update (subtract, shift by log2(1/alpha) if alpha = 2^-k, add) per reward. Combine Sniper event counts with
CACTI energies.

**Is it worth it**: compare at equal storage (RL vs. a larger or more associative cache with the same bits) and report
misses removed per kB of added state.

---

## 9. Staged implementation and test plan

### Proposed module structure

Pure C++ core without Sniper dependencies (unit-testable), e.g. under `SN/cache/rl/`, and a thin
`cache_set_rl.{h,cc}` adapter:

| Module | Responsibility |
|---|---|
| `BaseOrder` | Candidate generation: `candidates(n)`, `onHit`, `onFill`, `promote`. LRU only for now; SRRIP/PLRU/LFU later implement the same interface (not now) |
| `Selector` | `NextAttempt{rule, combine, retain, init bias}` (paper) and `TopScore{N, ties to oldest, epsilon}` (ours) |
| `Learner` | `RatioLearner` (paper: per-set ratios from the history) and `EmaLearner{scope = global \| set, alpha}` |
| `EvictionHistory` | Per-set FIFO of 2·K {tag, features}; reports aged-out entries |
| `FeatureExtractor` | Cache state → feature (M-on-read option), sharing class from `m_sharer_hint` (or labeled oracle), threshold |
| `CacheSetRL` / `CacheSetInfoRL` | Sniper wiring; `CacheSetInfoRL` extends `CacheSetInfoLRU` so `lru-victim-*`, `oracle-*`, `hint-*` stats and `check_sharers.py` keep working |

Sniper hooks needed (default no-op for all existing policies):

* `onFill(way, new_tag, evicted)` after the clone in `CacheSet::insert` (`SN/cache/cache_set.cc:142-180`); history
  lookup and removal happen here, since every Sniper miss ends in an insert.
* `onInvalidate(way, reason)` from `CacheSet::invalidate`, with the reason (`COHERENCY`, `BACK_INVAL`, `UPGRADE`) passed
  from `updateCacheBlock` and from the self-invalidations in `processMemOpFromCore` / `processShmemReqFromPrevCache`.
* Hits: existing `updateReplacementIndex(way)` (write-backs pass `update_replacement = false`,
  `cache_cntlr.cc:2578`, and get no reward).
* Configuration under `[perf_model/<cache>/rl]` (not environment variables) so `sim.cfg` records every run.

Event handling:

> **Corrected after Stage 1 review.** The table below does not fix the order of events on a miss, and the first Stage 1
> implementation recorded the victim before looking up the incoming block. The incoming block's history lookup
> (removal and −1) must come **before** the victim for its fill is chosen and recorded, as in ChampSim. See
> "Event lifecycle of a miss" in [`POLICY_CORES.md`](POLICY_CORES.md#event-lifecycle-of-a-miss).

| Event | History | Reward | Line metadata |
|---|---|---|---|
| Fill with eviction (`insert`) | insert victim; aged-out entry → +1 | as in Section 3.1 | reset for new block |
| Fill of a tag in the history | remove | −1 | |
| `COHERENCY` invalidation | none | none | reset |
| `BACK_INVAL` | none | none | reset |
| `UPGRADE` self-invalidation + refill | none | none | carried over |
| Write-back into the cache | none | none | no recency change |

### Stages

0. **Settle the open decisions** in Section 6.
1. **Core library + deterministic unit tests** (standalone, no Sniper): port the semantics of ChampSim tests 446/447;
   add the Section 3.2 example as an exact test; tests for history aging and FIFO order, rule `text` vs. `example`,
   `combine and/or`, `retain stay/promote`, pinning (I-1), invalidation reasons and upgrade carry-over (I-3), and
   "N = 1 or zero weights equals LRU" on a replayed random trace.
2. **Sniper hooks with no behavior change**: LRU statistics within noise of `27e0d326`; `run_regress.sh` on the five
   hierarchies and the `check_sharers.py` suite pass unchanged.
3. **Register `sf_lru` and `rl_lru`**, keeping an LRU order alongside in RL sets and asserting identical victims when N = 1
   or learning is off (exact, immune to run-to-run noise).
4. **Diagnostics**: pass-count histogram and pinned ways (I-1); rewards by type and class; periodic weight snapshots
   (I-4); miss classification (cold / replacement = history hit / coherence / back-invalidation); oracle "would have
   been invalidated anyway" counter (I-2); M-on-read victims (I-3); double-reward counts (I-6); decisions differing
   from LRU.
5. **Coherence correctness with the policies on the L2**: `run_regress.sh` (all five hierarchies), `SNIPER_COHERENCE_CHECK=1`
   (0 stale write hits), `check_sharers.py` invariants, `coherence_regress` data check.
6. **Experiments A**: Gainestown, 4 cores, prefetchers off, L2 only, 8 MB and 1 MB L3; the five initial configurations
   (paper in mode `both` and `bitset`); workloads `sharetest` (r/w/p/u), `coherence_regress`, FFT and other SPLASH-2,
   ideally the CAP Bench kernels used by the paper. Then L1-D + L2 after I-18.
7. **Experiments B**: N ∈ {1, 2, 4, 8}; alpha sweep; scope global vs. per set; delivered vs. oracle; hardware cost model
   and equal-storage comparison.
8. **Optional features**: Iprev, adaptive threshold, BRRIP dueling, exploration.

### Separating replacement effects from other effects

* Repetitions: ≥ 5 per configuration, mean ± range; differences inside run-to-run noise are not results.
* Paired counterfactual: shadow LRU tag directory in sampled sets, fed the same accesses and invalidations, counts LRU's
  would-be misses in the same run.
* Controls: random choice among the N oldest (window effect vs. learning effect); fixed class priority (does learning
  add anything); single-threaded workloads (coherence features nearly constant); oracle vs. delivered hint (staleness
  cost); N = 1 (must equal LRU).
* Metrics: L1-D / L2 MPKI and miss rate by miss class; IPC and simulated time; invalidations and write-backs (the
  paper's coherence metrics); L3 and DRAM traffic; McPAT energy for the caches only, with RL structures added
  analytically.
