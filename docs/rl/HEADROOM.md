# Headroom diagnostic: L2 trace, shadow model and calibration plan (RL Stage 7)

**Status:** review branch `review/stage7-headroom-trace` (implementation `7eff9503`, binary `25290da9`). **Not merged**, and kept separate from
`review/stage7-protect-invalidation` (also unmerged).
* It adds observation-only instrumentation and offline tools, verified on unit tests, deterministic single-core
  replays and short 4-core smoke runs ([section 6](#6-verification-phase-4)).
* No learning algorithm or reward was changed.
* **The four-run Ocean calibration (section 7) was run on 2026-11-23 and FAILED** under the pre-registered criterion
  ([section 7.6](#76-calibration-result-ocean-small-four-runs-2026-11-23)): 7 of 8 predictions were within ±0.5 points and one
  missed by +0.508. All 8 underestimate priority's benefit. Until a revised model passes a held-out calibration, the
  shadow must not be used to rank replacement preferences ([section 7.4](#74-if-calibration-fails)).

**Why:** the Stage 7 objective investigation (`results/stage7-objective-investigation/REPORT.md`) found that the
rl_lru rewards are absolute per-block labels, not relative judgements. Before another learning redesign, we want to
know how much replacement headroom the existing MESI and sharing features leave on these workloads.

## 1. Three questions that must stay separate

| | Question | Evaluated on | What it can and cannot say |
|---|---|---|---|
| **A** | Which of the eligible candidates (the four oldest) should have been evicted, in hindsight? | The recorded reference stream of one run | A label per recorded decision. The future it consults was produced by the recorded policy, so it describes that run, not another policy's. |
| **B** | Which fixed ranking of the six MESI × sharing classes gives the fewest L2 misses? | A shadow replay (a model) of a recorded stream, valid only if the model passes calibration | The best *static* use of these features. It is **not** an upper bound on C. |
| **C** | How well could a policy do that adapts its use of the same features over time (phases, sets)? | Shadow replays of adaptive policies, after calibration | A dynamic policy can beat every fixed ranking. B close to priority says nothing about C. |

* **Agreement rate is not a miss reduction.** How often a policy agrees with an oracle (A) is a different quantity
  from how many misses it saves in a replay (B, C):
  * a disagreement can cost nothing, if the evicted line would have been evicted soon anyway;
  * one early mistake can cost several misses.
  * Each analysis below names which of the two it reports.
* **Future information** (next uses, other cores' writes) is used only offline, by the labels and the `opt-*` oracle
  policies. The simulator never reads the trace ([section 2.5](#25-no-future-information-in-an-online-policy)).

## 2. What a meaningful offline replay needs (Phase 1)

### 2.1 Required reference events

A shadow cache that replays another policy needs, for every traced (core, set):

1. **Every counted L2 access**, in order, with its outcome in the recorded run:
   * load, store, or exclusive read (`READ_EX`), and whether it came from the L1-I;
   * hit or miss, and the line's state before the access.
   * This is the stream that the L2's hit/miss statistics (`L2.loads`, `stores`, `load-misses`, `store-misses`) and
     recency updates are made of. In Sniper, a counted hit is exactly an LRU update
     (`retrieveCacheBlock(..., first_hit && count)`), and L1 write-backs do not update L2 recency.
   * It is also the stream Sniper's own ATDs (`cache_atd.cc`, fed in `CacheCntlr::updateCounters`) see. The ATDs
     ignore coherence state, so they cannot replay a MESI-aware policy, but they confirm the choice of stream.
2. **Each line's coherence state, sharer hint and L1 presence at every decision.** The decisions of priority, the
   fixed orders and rl_lru depend on state and hint; the L1-inclusion modelling depends on L1 presence (the L2
   block's record of which L1 holds it).
3. **Fills, with their kind and timing.**
   * A demand fill or an upgrade refill (`CacheSetEvent::fill_t`).
   * A miss is filled after a directory round trip, while other cores' events interleave. A shadow decision for the
     same miss should therefore happen at the same point in the order.
4. **Invalidations, with their reason:**
   * another core's write (coherence);
   * an L3 eviction (back invalidation);
   * the core's own upgrade (S/E → M is invalidate + refill in Sniper).
5. **The other cores' ownership-taking accesses:** writes and exclusive reads, at the L2 and at the L1. With them, a
   line the shadow keeps but the recorded run evicted can be invalidated when it would have been.
6. **Each core's L1 uses of lines resident in the L2.** An L2 eviction back-invalidates the L1 (inclusion), and L1
   hits never reach the L2. A shadow that evicts a line the L1 is using must see that line's next L1 use, which in
   the counterfactual becomes an L2 miss.
7. **The region of interest**, so that tallies match `sim.stats` (roi-begin to roi-end).

### 2.2 Why logging only the L2 replacement decisions is insufficient

A decision log contains, for every eviction, the candidates, their features and the victim. It supports concept A
on the recorded trajectory (with the next-use information of item 1), and nothing else:

* **Another policy produces different cache contents.** After its first different victim, its later decisions are
  about candidates that the log never shows, and its hits and misses differ. Replaying it needs the access stream
  itself (item 1), not the recorded policy's decisions.
* **The access stream depends on the policy**, through the L1 and coherence. Archived Ocean `small` ROI statistics
  (`results/stage7-ifb-ocean`, LRU r1/r2 against priority r1/r2, four cores):

  | Policy-dependent quantity | LRU | Priority | Mechanism |
  |---|---|---|---|
  | L2 loads | 8.851 M | 8.865-8.867 M | +14-16 k: L1-I lines re-fetched after L2 evictions |
  | L1-I back invalidations (L2 eviction of a line in the L1-I) | 108 k | 121-122 k | inclusion: L2 recency never sees L1 hits |
  | L1-D back invalidations | 4.1-4.2 k | 5.8-6.3 k | same |
  | L2 stores | 2.302 M | 2.290-2.292 M | upgrade misses 257 k vs 245-246 k |
  | L2 coherence invalidations | 168 k | 147-148 k | priority evicts S lines that LRU kept until another core wrote them |
  | L2 back invalidations from the L3 | 267-370 | 320-861 | L3 contents |

  * Priority's L2-miss saving over LRU is about 120 k. The L1-I effect alone, about 14 k extra L2 accesses, is
    about 12% of it.
  * A replay that ignores inclusion would therefore be biased in favour of policies that evict L1-held lines, as
    priority does with S code lines.
* **So the trace records the access stream and the L1 events of items 5-6**, and the shadow model treats every
  counterfactual effect as an explicit, counted rule ([section 4.2](#42-shadow-model)). Calibration
  ([section 7](#7-calibration-plan-phase-5)) then tests whether those rules are good enough.

### 2.3 How the events are captured

Every hook is observation only, and does nothing unless the traced cache configuration has `rl_trace/enabled = true`:

| Event | Hook (file) | When | Recorded |
|---|---|---|---|
| Counted L2 access | `CacheCntlr::processShmemReqFromPrevCache`, right after `updateCounters`, under the same lock (`cache_cntlr.cc`) | Before the miss is serviced | Op, requester, hit, pre-state, way |
| Way contents | `RlTrace::access`, `beforeInsert`, `beforeInvalidate` (`rl_trace.cc`) | Before each access, decision and invalidation of a sampled set | Only ways whose tag, valid, state, hint or L1 mask changed |
| Decision | `Cache::insertSingleLine`, around `CacheSet::insert` (`cache.cc`) | Full set: view before, victim after | Victim way and line, ranks (`getRecencyBits`), eligible mask (`isValidReplacement`), fill kind |
| Fill | `Cache::insertSingleLine` | After every insert into a sampled set | Way, line, kind, evicted? (the state is set by the controller afterwards: it appears in the next way record) |
| Invalidation | `Cache::invalidateSingleLine`, before `CacheSet::invalidate` (`cache.cc`) | Present lines only | Way, line, reason (`CacheSetEvent::invalidation_t`) |
| L1 access | `CacheCntlr::accessCache` at the L1-I and L1-D (`cache_cntlr.cc`), next to the existing write-oracle hook | Every L1 access to a line of a sampled set, filtered | Core, L1, store/exclusive, flags |
| ROI | `HOOK_ROI_BEGIN`, `HOOK_ROI_END` | | Marker |
| Close | `HOOK_SIM_END` | | End record, sidecar JSON |

* **Order:** records are appended under one global (leaf) lock, so the file has one total order. That is the order
  in which the simulator performed the events. Sniper performs coherence functionally in host order, so it is also
  the order the recorded run's coherence used.
* **Placement:** the decision hooks live in `Cache`, not in a replacement policy, so the same trace is produced for
  `lru`, `sf_lru` and `rl_lru`, and no policy code changed.

### 2.4 Limits of counterfactual accuracy

| Source | Effect on a shadow replay | Handled by | Residual, and how it is measured |
|---|---|---|---|
| **L1 inclusion** | Evicting a line the L1 uses turns its later L1 hits into L2 misses. A line the recorded run evicted from both caches, but the shadow kept, would still be in the L1. | Phantom accesses (the next recorded L1 use becomes an L2 access) and suppressed accesses (a recorded L1 miss caused by the recorded eviction) | Only the *first* later use is known. The counterfactual L1's own replacement is not modelled (a phantom refill assumes the L1 keeps the line). Counted, plus a no-inclusion-model sensitivity replay. |
| **Inter-core coherence** | A line the shadow keeps can be invalidated by another core's write that the recorded run never sent to it; a downgrade changes its state. | Other cores' writes and exclusive reads invalidate shadow-only copies; their read misses downgrade them | Other cores' own streams are the recorded ones: their misses and timing also depend on the policy (all cores run it). Counted (`inv_modelled`, `downgrades_modelled`), plus a no-coherence-model sensitivity replay. |
| **Directory and protocol details** | Fill states (E vs S vs M on a read), sharer hints | A line the recorded cache also holds takes its recorded state and hint (exact); otherwise the modelled state | `modelled_candidate_pct`: the share of decision candidates whose features were modelled |
| **Inclusive L3** | L3 back-invalidations depend on L3 contents, which depend on all L2 miss streams | Applied as recorded (exogenous) | 267-861 per Ocean run (LRU, priority), against 5.96 M misses: negligible there. dm65536 had 27 k, so check per run. |
| **Timing and interleaving** | A different policy changes miss latencies, so the cores' relative progress and the interleaving change | Not modelled: the recorded order is used | Ocean simulated time differs by < 0.6% across policies. Not separately measurable here: it is part of the calibration error. |
| **Policies with state** | rl_lru's weights depend on its own reward history | Not replayed in this version (only LRU, priority, fixed orders and oracles) | rl_lru decisions are evaluated with concept A on its own traces |
| **Timing-dependent traffic** (synchronization, spinning) | A policy that changes miss latencies changes how often threads poll shared flags, adding or removing L2 accesses that no trace of another run contains | Not modelled | Visible as an access-count difference between policies beyond the run-to-run spread. On the 4-core LU smoke: +5.6% under priority against +0.8% between two LRU runs. Ocean `small` differs by only +0.04% in L2 accesses (LRU vs priority, archived), the favourable case. |
| **Run-to-run variation** | Multi-threaded runs are not deterministic, so two runs of one policy differ | Null pairs in the calibration (two traced runs of one policy) | The measured difference of a null pair is a noise floor that no model can beat ([section 7](#7-calibration-plan-phase-5)) |
| **ROI boundary** (multi-threaded) | Another core's event can race the ROI hook, so the statistics snapshot and the trace's ROI marker can be a few records apart | None needed | Seen once in the smoke runs: 1 load in 26,485. |

Two facts about the recorded runs:

* **Pre-ROI warm-up.** With `--roi`, Sniper runs the pre-ROI region in `CACHE_ONLY` mode, and in this configuration
  that mode produced no L2 events. On the 4-core Ocean `-n66` probe the trace's ROI begins right after the header,
  every tally equals `sim.stats`, and the LRU self-replay is exact. So a shadow starts from the same state as the
  recorded run. `headroom.py check` prints the number of cache records before the ROI, so a configuration that does
  warm the traced cache is visible.
* **Sniper's store-miss statistic** also counts a store *hit* whose line is still being filled (an MSHR overlap; loads
  do not count it). The trace marks these accesses (`overlap`). Tallies report both Sniper's statistic and
  "contents misses", the definition a shadow can reproduce; the calibration compares contents misses. On LU the
  difference is 8-22 of 18,500 store misses.

### 2.5 No future information in an online policy

* The simulator only writes the trace: there is no read path, and no replacement policy, controller or statistic
  depends on it.
* With the option off, nothing is created and every hook is one relaxed atomic load or one flag test.
* Next uses and future writes exist only in the offline `Future` index (`shadow.py`), used by `labels.py` and the
  `opt-window` / `opt-set` policies. Those are labelled diagnostic everywhere they are printed.
* If a later redesign learns from pairwise labels online, it must resolve each label causally, at the time of the
  later reference (as OPTgen does in Hawkeye). The offline tools can emulate that by using only labels resolved
  before the decision being learned. That is not part of this branch.

## 3. The trace (Phase 2)

### 3.1 Configuration

```
-g perf_model/l2_cache/rl_trace/enabled=true    # default false
-g perf_model/l2_cache/rl_trace/set_modulus=8   # trace set s iff s mod set_modulus == set_residue (default 8)
-g perf_model/l2_cache/rl_trace/set_residue=0   # default 0
```

* Every L2 (one per core) traces the same sampled sets. The same sets are needed in all cores for the coherence
  rules, because a line maps to the same set in every L2.
* **Requirements:** address hash `mask` (checked at every access), at most 8 ways, at most 65,536 sets, one traced
  configuration.
* **Start-up line:** `[RL-TRACE] L2 (perf_model/l2_cache, <policy>): sets % M == R traced, ...`, deliberately not an
  `[RL]` line. Existing `[RL]` lines are unchanged.
* **Output** (in the run's output directory):
  * `rl-trace.bin`: the records;
  * `rl-trace.json`: geometry, sampling, record counts by type, L1-filter counters and two consistency counters
    that must be 0;
  * the run's `simulation/sim.cfg` is the configuration record.

### 3.2 Records

Fixed 16-byte little-endian records `(type u8, core u8, a u8, b u8, aux u32, tag u64)`, defined in
`common/core/memory_subsystem/cache/rl/rl_trace_core.h`:

| Type | a | b | aux | tag |
|---|---|---|---|---|
| header (core 0) | cores | ways | schema version (1) | magic `RLTRACE1` |
| header (core 1) | log2(line bytes) | 0 | sets | modulus << 32, residue |
| access | flags: hit, store, pre-state (2 bits), from L1-I, prefetch, exclusive read, MSHR overlap | way if hit, else 0xff | set | line |
| way | way | state (2 bits), valid << 2 | hint, L1 mask << 8, set << 16 | line, or ~0 |
| decision | victim way | eligible-way mask | ranks (3 bits per way, 0 = MRU), fill kind << 24 | victim line |
| fill | way | kind, evicted << 1 | set | line |
| inval | way | reason: 0 unclassified, 1 coherence, 2 back, 3 upgrade | set | line |
| l1 | 0 = L1-I, 1 = L1-D | flags: store or exclusive, epoch-first, store-filter | L2 set | line |
| roi | 1 begin, 0 end | | | |
| end | | | | records before it |

* **Units:** `line` = physical address >> 6 (Sniper's tag). State: 0 I, 1 S, 2 E, 3 M; `SHARED_UPGRADING` is
  recorded as S and is not eligible; `OWNED` is recorded as M. `hint` = the delivered sharer hint (0 = none).
* **Time** is the record index, an order, not cycles. Distances in the analyses are in counted accesses to the set,
  by the same core.
* **Sampling** is a fixed residue class of set indices, so the same sets are traced in every run. There is nothing
  random in the recorder.
* **L1 filters** (`L1Filter`), which keep the volume bounded:
  * **Epoch rule:** an epoch of (core, set) ends at each recorded access and fill of that core in that set. The
    first L1 access of the core to a line of the set in each epoch is kept. A shadow decides only at an access or a
    fill, so the first L1 use of any line it evicts is always in the trace.
  * **Store rule:** a store or exclusive read by core j to line x is kept unless j's previous kept store to x had
    no access or L1 record of x after it. A shadow copy can only appear at such a record, so the first foreign
    store after it is always kept.
  * **Memory:** the filters use one entry per (core, line) and per line of the sampled footprint, bounded by the
    footprint, not by run length. All output is streamed in 1 MB blocks.

### 3.3 Size and overhead

**Estimated before implementation** (Phase 2): per counted L2 access, about 1 access record, 1 decision and 1 fill
per miss, 1-2 way records and 0.6-1.5 L1 records. For Ocean `small` (11.15 M counted L2 accesses in the ROI) that is
about 45-65 M records, 0.7-1.0 GB with every set traced. At modulus 8, about one eighth of that **if the sets'
traffic were uniform**.

**Measured** (`results/stage7-headroom-verification`):

| Trace (every set traced) | Records per counted L2 access | way | decision | fill | inval | l1 |
|---|---|---|---|---|---|---|
| FFT, 1 core | 5.05 | 1.51 | 0.89 | 0.96 | 0.01 | 0.68 |
| LU tiny, 1 core | 5.20 | 1.70 | 0.97 | 0.98 | 0.00 | 0.53 |
| sharetest-w, 4 cores | 5.50 | 1.98 | 0.57 | 0.98 | 0.33 | 0.64 |
| Ocean `-n66`, 4 cores | 4.31 | 1.58 | 0.14 | 0.52 | 0.35 | 0.71 |

* **Ocean `small` every-set trace:** about 4.3 × 11.15 M = 48 M records, about **0.77 GB per run**.
* **Set sampling is not uniform on Ocean.** At modulus 8 the residue-0 sets received 1/4.8 of the accesses, not 1/8
  (Ocean `-n66`: 111,664 of 533,715), because the grids' alignment concentrates traffic in some set classes. Section
  7 therefore traces every set.
* **L1 filter:** of 13.0 M L1 accesses to the traced sets (Ocean `-n66`, every set), 0.38 M were recorded; the filter
  held 43 k entries.
* **Simulator overhead, CPU time** (user + system, `sim.info`), every set traced against the trace off, same binary,
  same host load:
  * median **+1.1%** over the 22 deterministic replays, range −3.3% to +4.8%;
  * the negative values show that host noise is of the same size;
  * the modulus-8 replays ran later at a lower host load, so their CPU times are not comparable and are not
    reported as an overhead.
  * With tracing off, the cost is one flag test per hook, and the defaults-unchanged comparison shows identical
    results.
* **Offline cost** (streamed, memory-mapped reader):
  * about 3.4 µs per record per shadow replay: LU every-set trace, 9.4 M records, in 32 s at 184 MB peak memory;
  * so about 3 minutes per replay of an Ocean `small` every-set trace;
  * the `Future` index used by the oracles and labels adds memory proportional to the access and L1 records.

## 4. Offline model (`experiments/rl/headroom/`)

| File | Role |
|---|---|
| `rltrace.py` | Reader, ROI, tallies in `sim.stats` units, comparison with `sim.stats`, provenance lines |
| `shadow.py` | Shadow model, policies (`lru`, `priority`, `order:<six classes>`, `opt-window`, `opt-set`), the `Future` index, additive class orders |
| `labels.py` | Concept A labels of the recorded decisions |
| `headroom.py` | CLI: `check`, `replay`, `labels`, `calibrate`, `orders`, `phase` |
| `test_headroom.py` | Synthetic traces worked by hand |

Every output starts with provenance lines:
* the command;
* the SHA-256 of each tool source;
* the tool tree commit, flagged if the tool directory is modified;
* each input file with its SHA-256 and size.

### 4.1 Mirror of the recorded cache

The way records give every traced way's content at every access, decision and invalidation of its set; the decision,
fill and invalidation records give presence changes at once. The mirror is exact at those points by construction
([section 6](#6-verification-phase-4) checks it with exact self-replays).

### 4.2 Shadow model

* **State:** per traced (core, set), the shadow keeps its own contents and LRU order, and replays the stream with
  Sniper's permission rules: a read hits in M/O/S/E, a write or exclusive read only in M, and S/E becomes M through
  an upgrade miss.
* **Decisions:**
  * The **window** is the N oldest lines (default 4, as rl_lru). Ties go to the older line.
  * A miss that the recorded run also had is filled at the recorded fill record, so at the same point in the order.
  * A miss the recorded run did not have is filled at the access.
* **Rules for contents that differ from the recorded run.** Each is counted in the replay's summary:

| Rule | Counter |
|---|---|
| A line both caches hold uses the recorded state and hint | `candidates_observed` |
| A line only the shadow holds keeps its last known features, updated by the rules below | `candidates_modelled`, `modelled_candidate_pct` |
| A foreign write or exclusive read invalidates a shadow-only copy at once | `inv_modelled` |
| A copy both hold is invalidated by the recorded invalidation; if the recorded cache drops it first, then | `inv_recorded_coherence`, `inv_deferred` |
| A foreign shadow read miss downgrades a shadow-only M/E copy to S | `downgrades_modelled` |
| Fill state M for a write or exclusive read; else E without other shadow holders, S with; hint = 1 + other holders | (modelled fill features) |
| Evicting a line the counterfactual L1 holds makes its next recorded L1 use an L2 access | `inclusion_victims`, `phantom_accesses`, `phantom_misses` |
| A recorded L2 access to a line whose L1 copy only the recorded eviction removed is not an L2 access | `suppressed_accesses` |
| L3 back invalidations as recorded | `inv_recorded_back` |
| Sensitivity switches `model_inclusion=False` and `model_coherence=False` turn off the phantom/suppressed and the modelled invalidations and downgrades | |

* **Consistency counters that must be 0** on a recorded run:
  * `l1_orphans`: an L1 record of a line neither cache holds;
  * `pending_unresolved`;
  * `fill_of_held_line`.
* **What the model cannot do:**
  * re-time anything;
  * model the counterfactual L1's own replacement, or other cores' changed miss streams beyond the coherence rules;
  * model the L3;
  * replay rl_lru's learning (it could be added with the C++ cores; it is not needed for B).
* **The narrower diagnostic that is always possible** is concept A on a recorded run (descriptive). A shadow replay
  is a counterfactual estimate only for runs that pass calibration, and only while its modelled shares stay
  comparable to the calibration runs'.

### 4.3 Window labels (concept A)

* **Candidates:** for each recorded decision in the ROI, the candidates are the N oldest eligible ways, from the
  decision record's ranks and eligibility. Their features are the recorded way contents just before it.
* **Effective next use** of a candidate: its next use by the deciding core, or **never** if another core writes or
  exclusively reads it first, or if it is not used again in the trace.
  * A use is a counted access, or a recorded L1 access; the epoch rule keeps the first L1 use after every decision.
  * The victim's later L1 uses are L2 misses after its back invalidation, so victims and kept candidates are
    measured on the same footing.
* **Labels:**
  * The decision **agrees** if the victim's effective next use is the farthest in the window (ties count as
    agreement).
  * **Pairwise:** for each kept candidate, the eviction was right if the victim's next use is later, wrong if it is
    earlier, and a tie otherwise (both never used again, for instance).
  * **Protection opportunity cost:** for each candidate older than the victim (passed over), the analysis reports
    whether it was used before the victim, whether it was never used again, and the distance to its next use.
* **Assumption, stated in every report:** the future is the recorded run's. A label says which candidate this run's
  future favoured, not what another policy would have saved.

## 5. Distinguishing reward-signal and feature limits (after calibration)

* **Feature limit:**
  * the gap between `opt-window` (all information) and the best fixed class order (B) is headroom that no static
    use of these six classes reaches;
  * the gap between the best fixed order and priority is the room left for a better static ranking.
* **Reward-signal limit:**
  * on the same recorded decisions, compare rl_lru's actual choices (traced rl_lru runs) and its reward events,
    reconstructed offline from the trace (the history rule is deterministic given the decisions), with the window
    labels;
  * a reward whose sign often contradicts clear pairwise labels points to the reward;
  * choices that disagree with the labels even where the class information separates the candidates point to the
    learner.
* **Evidence for pairwise learning:** pairwise label statistics by class pair (`labels.py`), and whether they are
  stable across runs and phases.
  * A class pair whose right-rate is far from 50%, consistently, is learnable from these features.
  * Rates near 50% mean the features do not separate the candidates.

## 6. Verification (Phase 4)

All outputs, scripts and provenance are in `results/stage7-headroom-verification/` (`PROVENANCE.txt`,
`VERIFICATION.txt`, `verify-smoke.txt`).
* Review binary `25290da9` (implementation commit `7eff9503`);
* base binary `10f8c646` (feature/rl-port `2363b52e`, built from that commit's source).

| # | Requirement | How it was checked | Result |
|---|---|---|---|
| 1 | Tracing does not change replacement decisions | 11 policies × 2 single-core traces (LU tiny 200 M instructions, FFT): trace off vs every set traced, same binary | Identical statistics (3,979-4,090 per run), identical `rl-event-insert-hash`, identical `[RL]` lines: 22/22 |
| 2 | Existing policies unchanged with tracing disabled | The same 22 replays, base binary vs review binary with the option absent | Identical: 22/22 |
| 3 | Identical deterministic replays give identical traces | 3 policies × 2 traces, replayed twice | Byte-identical `rl-trace.bin` and sidecar: 6/6 |
| 4 | Events classified correctly | Every-set traces against `sim.stats`, per core: loads, stores, load and store misses, upgrade misses, demand and upgrade fills, decisions, invalidations by reason | 12 tallies equal on all 22 single-core traces. On 4 four-core traces (sharetest-w, LU smoke) all 48 per-core tallies are equal, with one ROI-boundary race (1 load). Coherence invalidations, upgrades and their refills are present and equal to the simulator's counts. No decision had an ineligible way. Sidecar consistency counters are 0 everywhere. |
| 4 | The record format | C++ unit tests (`test_trace.cc`: field layout, flags, ranks and eligibility including `SHARED_UPGRADING`, way-change records, epoch and store filters, sampling, writer) | 106 cases pass (97 existing + 9 new); `diff-cores` identical (608 configurations) |
| 5 | Offline next-use and labels match hand calculations | `test_headroom.py`: 20 synthetic traces worked by hand (cyclic LRU, upgrade, priority vs LRU, Belady on a cycle, foreign write before reuse, distances, coherence invalidation and downgrade, phantom and suppressed accesses, overlap misses) | 20/20 |
| 6 | The shadow reproduces known behaviour | Self-replays of the recording policy on its own trace: LRU, `lru-hist` (rl_lru, α = 0), priority, and rl-n4-frozen as the fixed order its weights imply. Single core, every set and modulus 8; four cores (sharetest-w, LU smoke, Ocean `-n66`), every set and modulus 8 | All exact: same misses, same decisions, 100% identical victims, 0 modelled features, 0 phantom, suppressed or modelled invalidations. Inclusion victims equal the simulator's L1 back invalidations (e.g. LU 20,752, FFT 1,163). |
| - | Sampled traces | Modulus-8 traces against the every-set traces of the same deterministic run, restricted to sets 0 mod 8 | Equal (4/4) |

**Known non-deterministic statistic.** `L3.store-overlapping-misses` differs by 1-2 counts between runs of one binary
and configuration (repeats: FFT sf-text 3 vs 5 with tracing on; LU rl-n4-dm65536 240 vs 241 with it off). It is an
MSHR timing test at the shared L3, and it varies with tracing off. The comparisons report it separately and exclude
it; every other statistic is identical.

**Demonstrations of the calibration mechanics** (4 cores, tiny inputs; not calibrations):

| | Ocean `-n66` (modulus 8) | LU tiny smoke (modulus 8) |
|---|---|---|
| Self-replays | Exact | Exact |
| Measured priority vs LRU, sampled sets / all sets | −0.30% / −3.06% | −21.6% / −22.5% |
| Null pair (LRU vs LRU), sampled / all sets | +0.75% / +0.33% | +3.6% / +2.2% |
| Predicted from the LRU trace / priority trace | +0.06% / −0.82% | −1.7% / −7.6% |
| Mechanism check | Inclusion victims predicted 2,430 vs recorded 2,380. Without the inclusion model the prediction moves 1.5 points. | Accesses +5.6% recorded vs +0.02% predicted (timing-dependent traffic) |
| Verdict of `calibrate` | INCONCLUSIVE: noise floor 0.75 points and sampling bias 2.8 points | INCONCLUSIVE: noise floor 3.6 points (and errors of 14-20 points) |

* These runs are far too short and too noisy to say anything about the model's accuracy on Ocean `small`.
* What they show is that the gate behaves as intended: it refuses a verdict when the noise or the sampling is too
  large. They are also why section 7 traces every set and uses null pairs.
* Window labels of the single-core traces are in `VERIFICATION.txt` as examples of the output, for instance LU LRU's
  agreement of 0.9%. On LU's sweeps larger than the cache, LRU evicts the line used next; the shadow `opt-window`
  replay has 11.8% fewer misses than LRU on that trace. These are smoke traces of an exploratory workload, not
  findings.

## 7. Calibration plan (Phase 5)

### 7.1 Runs

Ocean `small`, the configuration of `stage7-ifb-ocean` (4 cores, gainestown, L3 8 MB, prefetchers off, complete ROI),
plus `rl_trace/enabled=true` and **`rl_trace/set_modulus=1`** (every set):

| Run | Policy | Repetitions |
|---|---|---|
| `lru` | LRU | 2 |
| `rl-n4-prio` | rl_lru, `selection=priority` | 2 |

* **Why every set:** on Ocean the traffic of one residue class of sets is not a fixed fraction of the whole
  ([section 3.3](#33-size-and-overhead)). On the `-n66` demonstration the modulus-8 sample saw −0.30% where the whole
  cache saw −3.06%. With every set traced, the sampling question disappears.
* **Cost:**
  * 4 runs, 2 at a time, about 2,000 s each (the archived runs took 1,543-2,091 s);
  * about 0.8 GB of trace per run, about 3.2 GB in all;
  * offline, about 3 minutes per shadow replay; one calibration pair is 2 self-replays + 8 cross-replays, about 30
    minutes.
* **Tracing perturbs host timing**, so each traced run is one more sample of the nondeterministic simulation. Its
  all-set statistics must fall within the spread of the untraced `stage7-ifb-ocean` and `stage7-ocean-pilot` runs of
  the same policy. Otherwise the traced runs are not representative.

### 7.2 What the offline model must reproduce

`headroom.py calibrate --lru <LRU run> --prio <priority run> --null <LRU r1>:<LRU r2> --null <prio r1>:<prio r2>`, for
each of the 4 LRU × priority pairs:

1. **Self-replays, exact.** Shadow LRU on each LRU trace and shadow priority on each priority trace must give the
   recorded counts exactly:
   * contents misses (ROI);
   * decisions;
   * 100% identical victims;
   * 0 modelled candidates.
   * The deterministic and four-core smoke traces already pass this ([section 6](#6-verification-phase-4)).
2. **Noise floor.** The null pairs' measured differences must be at most half the threshold (0.25 points).
   Otherwise the verdict is INCONCLUSIVE, and more repetitions are needed before any verdict.
   * The archived all-set LRU r1 vs r2 difference is 0.12 points, and priority's is 0.24, so this is expected to
     hold.
3. **Cross-replay predictions** (the calibration proper):
   * priority predicted from each LRU trace, and LRU predicted from each priority trace, as percentage differences
     in contents misses;
   * **pass if every prediction is within ±0.5 points of the measured difference** of its pair;
   * the measured all-set difference is −2.04% (archived range −1.92 to −2.16), so the threshold is about a quarter
     of the effect;
   * differences between policies smaller than about 0.5 points of L2 misses cannot be resolved by this model even
     if it passes.
4. **Mechanism check.** These are not pass/fail, but each must be explained:
   * predicted against recorded counts of accesses, upgrade misses, decisions, coherence invalidations, inclusion
     victims and dirty evictions;
   * the sensitivity variants (no inclusion model, no coherence model).

### 7.3 Identifying the sources of deviation

| Suspected source | Signature in the calibration output |
|---|---|
| L1 inclusion | The predicted change in accesses (`phantom − suppressed`) or in inclusion victims misses the recorded change, and the prediction moves by more than the threshold when `model_inclusion` is switched off |
| Coherence | Predicted coherence invalidations miss the recorded ones, `inv_modelled` / `inv_deferred` are large, and the no-coherence variant moves the prediction |
| Modelled features | A high `modelled_candidate_pct` in the cross-replay (decisions resting on modelled rather than recorded state) |
| L3 | `inv_recorded_back` small (expected on Ocean with LRU and priority); a large value would be a separate warning |
| Timing-dependent traffic | Recorded accesses differ between the policies by more than between the null pairs, and the model's predicted access change (phantom − suppressed) does not account for it |
| Policy-dependent streams or timing | A residual error that no mechanism count accounts for, and an asymmetry between the two directions (LRU → priority vs priority → LRU) |

### 7.4 If calibration fails

The shadow model must **not** be used to claim that any replacement preference is better or worse than another:
* no miss reductions from fixed orders, oracles or adaptive policies;
* no "best fixed ranking";
* no "headroom" in misses.

**Still valid:**
* the recorded runs' own tallies;
* the window labels as descriptive statistics of each recorded run (concept A, with its stated assumption);
* pairwise label statistics as a description of what each run's future favoured.

The next step would then be online experiments only, or a better model justified by the mechanism check.

### 7.5 After calibration passes (proposed analyses, each gated on its own approval)

1. **Oracle headroom within the window** (shadow, misses): `opt-window` and `opt-set` against shadow LRU and
   priority, on each trace. These are upper references for this window and for the whole set, under the model. They
   are not achievable bounds: the oracles see the future.
2. **Best fixed class order (B):**
   * all 60 additive orders (those rl_lru's weights can express), and all 720;
   * selected on one trace and evaluated on the other (LRU trace against priority trace, r1 against r2), so that
     the selection itself does not inflate the result;
   * reported against priority, with the threshold of 7.2.
3. **RL disagreement (A):** `labels` on traced rl_lru runs (rl-n4, rl-n4-dm65536, both stored separately and gated)
   against LRU and priority: agreement, pairwise right/wrong by class pair, and the protection opportunity cost.
4. **Reward vs features** ([section 5](#5-distinguishing-reward-signal-and-feature-limits-after-calibration)).
5. **Pairwise learning evidence:** the stability of the class-pair right-rates across traces and epochs.
6. **Phase adaptation (C):**
   * `phase` gives per-epoch comparisons of independent replays. These are indicative only: a switch inherits cache
     contents that independent replays ignore.
   * A causal set-dueling policy (LRU vs priority, DIP-style leader sets) could be replayed in the shadow after
     calibration, as a proper C estimate.
   * **B close to priority is not evidence that the features have no potential:** C remains a separate question.

LU `tiny` remains exploratory: its upstream factorization pivots on the smallest element (`docs/rl/CAPBENCH.md`). Any
LU trace is a second, exploratory check of the same model, never the calibration.

### 7.6 Calibration result (Ocean `small`, four runs, 2026-11-23)

Results: `results/stage7-headroom-calibration/` (read-only; `CALIBRATION-REPORT.txt`, `VALIDATION.txt`, the four
`calibrate-*.txt`, `diagnose-stores.txt`), archived as `results/archive/stage7-headroom-calibration-20261123.tar.gz`
(sha256 `57c88209…6916014`). Binary `25290da9` (this branch), every L2 set traced, LRU ×2 and `rl-n4-prio` ×2.

**Verdict: FAIL** (narrow). The criterion fixed in advance was that *every* cross-prediction be within ±0.5 points.
Seven of eight are; one is not (+0.508). Passing seven of eight does not meet that criterion, and the threshold is
not relaxed after the fact. The per-pair `RESULT` lines in three of the `calibrate-*.txt` files say PASS because
each file judges one pair only; the calibration is the conjunction of the four. The verdict is not INCONCLUSIVE:
the noise floor (0.10 points) is a fifth of the tolerance, and all eight errors have the same sign.

**Validity of the runs** (`VALIDATION.txt`; all critical checks pass):
* 4/4 completed normally with a complete ROI and trace end markers; 10/10 checker runs pass.
* Trace tallies equal `sim.stats` per core, except one fill and one decision at the ROI-end race in LRU r1.
* Traced vs untraced runs (9 LRU, 6 priority): L2 misses, evictions and simulated time are within the untraced
  spread. Coherence invalidations are 1.0-2.8% lower in all four traced runs, just below the untraced minimum
  (plausibly the tracer's lock changing interleavings).

**Self-replays: exact** on all four traces: 5.42-5.53 M decisions, 100% identical victims, and every must-be-zero
counter (modelled candidates, phantom and suppressed accesses, modelled or deferred invalidations, orphans,
unresolved fills) 0.

**Cross-predictions** (contents misses, ROI; priority vs LRU, in points of the LRU misses):

| Pair (LRU, priority) | Measured | From the LRU trace (error) | From the priority trace (error) |
|---|---|---|---|
| r1, r1 | −2.301% | −2.001% (+0.301) | −1.930% (+0.371) |
| r1, r2 | −2.397% | −2.001% (+0.396) | −1.889% (**+0.508**) |
| r2, r1 | −2.227% | −1.984% (+0.243) | −1.930% (+0.297) |
| r2, r2 | −2.323% | −1.984% (+0.339) | −1.889% (+0.433) |

* Noise floor (null pairs): LRU r1 vs r2 −0.076 points, priority r1 vs r2 −0.098 points (both ≤ 0.25).
* The sampled-set and all-set differences agree within 0.001 points (every set is traced).

**Mechanism counts** (pair r1, r1; the other pairs agree within about 0.3 points of error%):

| Count | LRU rec | prio pred | prio rec | error | LRU pred | error |
|---|---|---|---|---|---|---|
| L2 accesses | 11,148,025 | 11,162,098 | 11,151,729 | +0.09% | 11,136,526 | −0.10% |
| **Upgrade misses** | 255,744 | 255,383 | 243,442 | **+4.91%** | 243,827 | **−4.66%** |
| Decisions | 5,525,977 | 5,429,041 | 5,423,990 | +0.09% | 5,515,767 | −0.18% |
| Coherence invalidations | 165,490 | 143,470 | 142,305 | +0.82% | 164,847 | −0.39% |
| Inclusion victims | 112,036 | 127,255 | 128,209 | −0.74% | 113,223 | +1.06% |
| Dirty evictions | 4,756,102 | 4,548,636 | 4,548,024 | +0.01% | 4,749,954 | −0.13% |

* Modelled behaviour: LRU → priority: phantom accesses 22.2-23.0 k, suppressed 8.2-8.5 k, modelled invalidations
  2-45, deferred 0, modelled candidates 8.7%. Priority → LRU: phantom ~8.0 k, suppressed 22.9-23.3 k, modelled
  invalidations 22.4-22.7 k, deferred 0, modelled candidates 9.1-9.2%.
* Sensitivity (pair r1, r1, errors from the LRU / priority trace): full model +0.301 / +0.371; no inclusion model
  −0.149 / +0.373; no coherence model +0.295 / +0.210; neither −0.155 / +0.214. The access, inclusion and
  coherence counts support both modelled rules (within about 1%), so the smaller error without the inclusion model
  is a cancellation, not evidence against the rule.

**The upgrade-miss discrepancy.** Every count but one is within about 1%. The shadow predicts essentially no change
in upgrade misses between the policies (LRU trace replayed as priority: 255,383, vs 255,744 recorded under LRU),
whereas the priority runs record about 12,300 fewer. By the line's state before the store (`diagnose-stores.txt`),
priority has about 12,000 fewer stores that find their line in S; stores finding E are unchanged (126.3-126.6 k);
L2 store accesses fall by about 10,700 and stores that hit M rise by about 4,600. Restoring the missing change
would move the r1, r1 errors to about +0.10 / +0.17 points: about two thirds of the bias. That residual would remain
unexplained.

**Limitations shown by this calibration.**
* The shadow replays the recorded L2 reference stream. Which stores reach the L2, and in what state, depends on L1
  write permission, which depends on the L2 state when the L1 copy was filled; the model did not represent this.
* A line held by both the shadow and the recorded cache took the recorded state, even when the recorded cache had
  dropped and refetched it while the shadow kept its own copy (both points are being investigated on the existing traces).
* Even a passing calibration on LRU and priority would not justify predictions for policies far from the recorded
  one (oracles, the whole-set optimum), whose decisions rest more on modelled features and events.
