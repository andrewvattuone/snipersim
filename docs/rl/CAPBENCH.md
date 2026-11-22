# CAP Bench in Sniper: setup, review and reproduction limits

Stage 6 priority after the Ocean/Barnes pilot ([`STAGE6_PILOTS.md`](STAGE6_PILOTS.md)): run the CAP Bench kernels
that Souza & Freitas used ([P24]; thesis [T21], see [`ALGORITHM_AUDIT.md`](ALGORITHM_AUDIT.md) Section 2) on this
Sniper fork. This page covers the tooling, its review, and what can and cannot be reproduced. **No policy comparison
has been run on CAP Bench.** The only CAP Bench simulations are three LRU qualification runs.

## Source and build

* **Source:** <https://github.com/cart-pucminas/CAPBenchmarks>, revision `ceb6f53bfffcf48dacd9e6e0078ada1edac567f2`
  (2022-11-26), cloned to `../capbench` beside this tree. That checkout is never modified.
* **Build:** `experiments/rl/build_capbench.sh [SRC_DIR [OUT_DIR]]`.
  * **Revision:** it clones the pinned revision if `SRC_DIR` is missing. It refuses a checkout at another
    revision or with modified tracked files.
  * **Export:** it exports `x86/` at the pinned revision into `OUT_DIR`, applies `experiments/rl/capbench-roi.patch`
    there, builds there, then makes `OUT_DIR` read-only.
  * **Output directory:** the default `OUT_DIR` is `../capbench-build-<first 8 hex digits of the patch md5>`.
    `run_stage6.py` derives the same name from the patch it has, so a binary is always used with the patch it was
    built from. An existing `OUT_DIR`, or one inside this tree, is refused.
  * **`BUILD_INFO`:** written next to `bin/`. It holds the revision, patch md5, compiler, flags, `sim_api.h` md5
    and the binary md5s. The runner copies its first line into each manifest entry (`workload_build`).
* **Flags:** the repository's `x86/makefile` flags without `-Werror`: `gcc -Wall -Wextra -O3 -fopenmp`, plus the
  include paths of `x86/include` and this tree's `include/` (for `sim_api.h`). Toolchain: GCC 11.4.0.
  * Dropping `-Werror` changes no generated code: the kernels the patch does not touch (FAST, FN, GF, IS, TSP) have
    the same md5 as in a build with `-Werror -Wno-error`.
* **Current build:** `../capbench-build-b9c60223` (patch md5 `b9c60223…`).
* **Earlier build:** `../capbench-build` (patch `96a07190…`, no iteration markers) was used by the archived
  qualification runs and is kept for their provenance.

## Patch (`capbench-roi.patch`)

The authors' own ROI adaptation is not public. The patch therefore adds:

1. **ROI markers** in each kernel's `main.c`. `SimRoiStart()` goes immediately before, and `SimRoiEnd()`
   immediately after, the last `start = timer_get()` .. `end = timer_get()` pair. That pair is exactly the interval
   the kernel times and reports as "total time".

   | Kernel | ROI = call(s) between the markers | Before the ROI (not simulated in detail) |
   |---|---|---|
   | FAST | `fast(img, imgsize, mask)` | random image and mask |
   | FN | `friendly_numbers(start, end)` | — |
   | GF | `gauss_filter(img, imgsize, mask, masksize)` | random image, mask |
   | IS | `integer_sort(a, n)` | 2^k normally distributed integers (5 G instructions at `standard`) |
   | KM | `kmeans(points, npoints, ncentroids, 16)` | random points |
   | LU | `lower_upper(m, l, u)` | random matrix |
   | TSP | spawning, running and joining the worker threads in `run_tsp` | cost matrix |

   * **Thread creation:** the OpenMP thread team (and TSP's threads) is first created inside the ROI. No kernel
     runs a parallel region before it.
2. **Iteration markers**, used only with `roi-iter-sync` (below). `SimMarker(1, i)` / `SimMarker(2, i)` go at the
   start and end of:
   * KM iteration `i`: one `populate()` + `compute_centroids()` pass;
   * LU elimination step `i` (1-based): pivot search, row and column swap, row reduction.
3. **A `paper` problem class** for the two kernels whose paper input matches no class at this revision:
   * FN: 8,000,001 .. 8,016,384;
   * KM: 16,384 points, 512 centroids.

   All other classes are unchanged.

## Inputs: the paper's versus reduced exploratory inputs

| Kernel | Paper ([P24] Sec. III-C, [T21] Sec. 4.2.2) | Repository class at `ceb6f53b` | Our class |
|---|---|---|---|
| FAST | 8192 × 8192 image | `standard` (8192) | `standard` |
| FN | 8×10^6+1 .. 8×10^6+2^14 | none now; the pre-2015 `standard` (changed in `b45d979`) | `paper` |
| GF | 8192 × 8192 image, 11 × 11 mask | `standard` (11, 8192) | `standard` |
| IS | 2^25 integers | `standard` (33,554,432, max 2^20) | `standard` |
| KM | 2^14 points in R^16, 512 centroids | none: `standard` has 1,024 centroids, `small` 8,192 points | `paper` |
| LU | 1536 × 1536 | `standard` | `standard` |
| TSP | 17 towns | `standard` (17, seed 12345) | `standard` |

**Labels in the runner.** CAP Bench workloads in `run_stage6.py` carry `input = paper` or `input = exploratory`,
recorded in the manifest and printed by the summarizer.
* **Naming:** `capb-*` workloads use the paper's inputs. `capbx-*` workloads use reduced inputs, so far only LU
  `tiny` (512 × 512).
* **Exploratory results are not reproductions** of the paper.

**Measured sizes** (SDE instruction counts, 4 threads; ROI = instructions between the ROI markers, measured with
scratch builds that exit at each marker; whole-program counts with `OMP_WAIT_POLICY=passive` are within 1% of the
default, so spin-waiting is not a significant part):

| Workload | Whole program | ROI | Notes |
|---|---|---|---|
| KM `paper` | 54.7 G | 54.7 G | 31 iterations; iteration 1 ends at 1.78 G, iteration 2 at 3.55 G (1.77 G each) |
| KM `small` | 17.7 G | 17.7 G | 20 iterations, 0.89 G each |
| KM `tiny` | 3.8 G | 3.8 G | 17 iterations, 0.23 G each |
| LU `standard` | 18.4 G | ≈ 18.3 G | 1,535 steps |
| LU `small` | 5.5 G | 5.48 G | 1,023 steps |
| LU `tiny` | 0.72 G | 0.71 G | 511 steps; initialization 8 M |
| IS `standard` | 8.3 G | not measured | main thread 8.1 G; worker threads 0.08 G each |

## Bounding the simulated region

The runner records how each run's region ended (`roi_end`), requires the workload's expected ending, and the
summarizer refuses to compare runs whose regions ended differently.

| `roi_end` | Mechanism | Same work in every run? |
|---|---|---|
| `application` | `--roi`: cache warm-up before `SimRoiStart()`, detailed until `SimRoiEnd()` | yes: the whole kernel |
| `instruction-limit` | `--roi -s stop-by-icount:N`: detailed from `SimRoiStart()` for N instructions summed over all cores, then the run stops | **no** (see below) |
| `iteration-marker` | `--roi-script --no-cache-warming -s experiments/rl/roi-iter-sync::B:C[:D]`: fast-forward to iteration B, detailed through the end of iteration C, optional snapshot `iter-D-begin` | yes, up to the boundary shift below |

**Instruction limits do not compare equal work.** N counts every instruction on every core.
* **What varies:** a policy that changes timing changes how far each thread gets in those N instructions. It also
  changes how many instructions go into waiting (OpenMP spinning at barriers, lock retries).
* **Consequences:**
  * Simulated time and IPC over an instruction-limited region are not comparable across policies.
  * Only per-instruction rates (MPKI, miss ratios, decision outcomes) roughly are.
  * The summarizer prints this caveat in the header of such workloads.
* **Use:** the archived 500 M qualification runs are LRU workload characterisations only.

**Iteration-bounded regions** (`experiments/rl/roi-iter-sync.py`, a variant of Sniper's `scripts/roi-iter.py`).
* **Fixed work:** every run simulates the same complete iterations.
* **Deferred switch:** each mode switch is queued at the marker and applied at the next periodic instruction
  callback (every 1 M instructions summed over all cores, where `stop-by-icount` also switches). Each boundary
  therefore lies up to about 1 M instructions after its marker, printed per run (≤ 1.05 M seen): ≤ 0.06% of a KM
  `paper` iteration.
* **No warm-up mode in the middle of a run.** Switching from cache warm-up to detailed simulation in the middle of
  a parallel region makes the Stage 6 checkers fail. The L1-D sharer invariant and several paired L2 counters
  (evictions vs history inserts, decisions vs victim positions, upgrade invalidations vs refills) come out 1-5
  events apart.
  * **Where it happens:** with `roi-iter-sync` and with Sniper's own `stop-by-icount:N:roi+M`, so it is a
    property of that mode transition, not of the scripts.
  * **What passes:** fast-forward → detailed at the same point (LU `tiny`, steps 2-4, all five policies), and
    detailed → fast-forward at the end.
  * **Consequence:** warm the caches by simulating earlier iterations in detail, then measure the later ones
    alone through the `iter-D-begin` snapshot.
  * **Not investigated:** the cause inside the simulator's warm-up mode.

**Measuring one iteration alone** (workload key `measure`, for example `iter-2-begin:roi-end` for KM iteration 2).
* **The runner:**
  * **Checks on the full region:** it runs the checkers on the complete detailed region (`sim.stats`).
  * **Interval statistics:** it writes the interval's statistics (`tools/dumpstats.py --partial`) to
    `<run>/measure/simulation/sim.stats`, beside a copy of `sim.cfg`, and runs the same checkers on them
    (`measure_checks`). A run is `ok` only if both sets pass.
* **The summarizer:**
  * **`--region FROM:TO`:** it reads every run's statistics for that interval, so IPC, MPKI, misses, evictions,
    invalidations, victim states, hints and the RL diagnostics all come from the same interval.
  * **Refusals:** it refuses a run whose `measure` differs from the requested region, or whose interval checks
    failed.
  * **RL max-passes:** this is a maximum, not a counter, so it is left out of interval summaries.
* **Recorded per run** (manifest `roi_switches`): every deferred switch (`detailed`, `snapshot`, `end`), with the
  instruction count, summed over all cores, at the marker and at the switch, and the offset between them.
  * **Marker check:** `roi_markers` counts every begin and end marker and any departure from the order
    `SimMarker(1, 1), SimMarker(2, 1), SimMarker(1, 2), ...`.
  * **Requirements:** an iteration-marker run is `ok` only if its switches occurred in the expected order and
    there were no marker-order violations.
* **Validated** (LU `tiny` smoke test: steps 2-4 detailed, step 4 measured; `lru`, `lru-hist`, `rl-n2`):
  * **Markers and switches:** all 511 begin and 511 end markers arrived in order; switches detailed → snapshot → end.
  * **Additivity:** `roi-begin:iter-4-begin` plus `iter-4-begin:roi-end` equals `sim.stats` for every counter
    (4,092 keys). The 13 exceptions are timestamps and derived values (`*_begin`, `*_end`, `ipc`, `corefreq`,
    `l1misslat`, ...), none of which the summarizer uses.
  * **Consistency:** `measure/simulation/sim.stats` equals the `--region` statistics, and the checkers pass on the
    interval.
* **Offsets:** 0.37-1.15 M instructions after their markers, summed over the 4 cores.
  * **KM:** negligible at 1.77 G per iteration (< 0.07% at each end).
  * **LU:** not negligible at 1.4 M per elimination step. For LU, measure many steps or the whole ROI.
* **Run-to-run variation:** in this 4 M-instruction window, LRU's step-4 L2 MPKI varied from 2.50 to 3.38 over
  three identical runs. Such short windows cannot compare policies.

**`lru-hist`** (`run_stage6.py`; not a policy under test): `rl_lru` with `rl/alpha=0`, window 2 and the diagnostics.
* **Victims:** the weights stay 0, so every decision is LRU's (Stage 3 verified the identical victim sequence; the
  smoke runs show 0% of decisions differing from LRU).
* **Measurement:** the eviction history (16 entries per set) and the miss classification are kept. They measure
  reuse *just beyond* the L2's capacity: misses on blocks evicted within the set's last 16 evictions, which a
  replacement policy could in principle have kept.
* **Within capacity:** plain LRU's `L2.access-mru-0..7` (hits by recency position, 7 = least recent) shows only
  reuse within the L2's capacity.
* **Checked:** for an instruction-limited run, `sim.stats` is exactly the `roi-begin`..`roi-end` delta: all 3,981
  keys of the KM run.

## Review of the tooling

* **Region start:**
  * **`application`:** starts in the main thread before any parallel region (OpenMP's threads are not yet
    created), so the switch happens at a quiescent point. It ends after the join or the end of the last parallel
    region.
  * **`instruction-limit`:** ends at a periodic callback in the middle of the computation; the checkers passed
    for KM and LU.
  * **`iteration-marker`:** starts from fast-forward at a periodic callback after the marker.
  * **Cold start:** IS used `--no-cache-warming` (its 5 G-instruction initialization would take hours in
    cache-only mode), so its caches start cold. KM and LU warm their caches before the ROI with `--roi`.
* **Incomplete runs and failures:**
  * A run is `ok` only if:
    * it exits 0 within its timeout, with statistics, without an internal exception;
    * its models were switched on and off;
    * its region ended the expected way;
    * every checker passed.
  * **A timeout** kills the run's whole session. IS `standard`, 500 M (3,600 s) is recorded `FAILED`, with no
    statistics and no processes left behind.
  * **A run whose region ends unexpectedly** (for example a kernel shorter than its instruction limit) is
    `FAILED`, not silently a different measurement.
  * **The summarizer** lists failed runs and exits 1.
* **Output validation:**
  * **What is checked under Sniper:** only that the region was entered and left, the simulator checkers, and the
    statistics.
  * **No self-tests:** CAP Bench has none, and a truncated or iteration-bounded run is stopped or fast-forwarded,
    so its later output is not printed. LU, KM and IS print only timings; FAST (corner count) and TSP (shortest
    path) print a result that a full run could compare with a native run.
  * **Native checks** (scratch builds outside this tree, not part of the tooling):
    * **LU** completes every elimination step (`lower_upper` returns 0) at `tiny`, `small` and `standard`, with
      no non-finite values. The values grow to 10^13-10^15, a consequence of the pivot choice below. 6 repeated
      runs at 4 threads and 3 at 1 thread gave the same result.
    * **KM** converges in 17 / 20 / 31 iterations (`tiny` / `small` / `paper`).
      * **`paper` input:** 3 runs at 4 threads, 1 at 2 and 1 at 1 gave the same 31 iterations and the identical
        final assignment (hash of the point-to-centroid map).
      * **Convergence:** the sum of squared distances falls monotonically from 4.72 × 10^13 after iteration 1 to
        4.15 × 10^13 at convergence, and no cluster is ever empty.
      * **The input** is uniform random 16-dimensional points (`randnum() & 0xffff`), so the result is a correct
        Lloyd's run on data with no natural clusters: meaningful as a computation, not as clustering.
      * **Fixed work:** the iteration count is fixed by the data and the seed, so each iteration is the same work in
        every run.
    * **IS's result is not sorted** (see below). **IS is excluded** from the qualification and from any policy
      comparison.
* **Provenance and overwriting:**
  * **Overwriting:** a run directory is never reused, and a build directory is never overwritten.
  * **Manifest:** each line records the commit, `tree_clean` (simulator sources), `tooling_clean` (everything
    under `experiments/`, including untracked files), the simulator md5, the workload binary md5, `workload_build`
    (source revision and patch md5), `input`, the expected and actual `roi_end`, the full command and the timeout.
  * **Summarizer errors:**
    * runs made with uncommitted tooling;
    * runs whose run-sniper options differ beyond the policy keys (scripts and their arguments included);
    * mixed region endings.
  * **The archived qualification runs** were made with uncommitted tooling. The md5s of that tooling are in
    `results/stage6-capbench-qual.command`.

### Upstream warnings and defects (kept, not fixed)

* **LU, `lower_upper.c:93`** (`-Wabsolute-value`): `fabs(MATRIX(m,pipvt,pjpvt) > fabs(MATRIX(m,ipvt,jpvt)))`.
  * **What it is:** a misplaced parenthesis. The comparison is evaluated first, and `fabs` of its 0/1 result
    decides the branch, so the merge compares a thread's signed candidate with the absolute value of the global
    one. This is defined behaviour (no undefined behaviour).
  * **The search above it** (`fabs(...) < fabs(...)`) keeps the *smallest*-magnitude element, so LU pivots on
    tiny values. The factorization is numerically meaningless (values grow to ~10^14) but completes. Its access
    pattern is still that of a full-pivoting Gaussian elimination: pivot search over the submatrix, row and column
    swap, row reduction.
  * **Pivot choice:** it is data-dependent, so the swap targets are too. A correct LU (largest-magnitude pivot)
    would choose different rows and columns to swap at each step. The upstream code therefore has the same kind of
    accesses as a correct LU, but not necessarily the same addresses in the swaps. With values of ~10^14 it is also
    not a numerically valid factorization.
  * **Consequence:** the upstream LU is kept unchanged for now. **Every LU measurement is exploratory, at every
    input class**, including `standard`; none reproduces the paper's LU or stands in for a correct LU.
  * **Ties:** the merge's result could in principle depend on the order in which threads enter the critical
    section, but the repeated runs above gave identical results.
* **TSP, `tsp.c:246`** (`-Wmisleading-indentation`): in `get_next_partition_block_size` only `ret.start = ...` is
  guarded by the `if`.
  * **Harmless part:** when no partitions remain, `start` stays −1 and the caller (`repopulate_queue`) stops on
    `start < 0`, so the unguarded lines do no harm.
  * **The `else` branch** sets `end = start + nb_partitions − start = nb_partitions`, one past the last partition
    index for a final partial block. That is a possible off-by-one, not investigated: TSP is not in the
    qualification set.
* **IS, `bucket-sort.c` (no warning):**
  * **Missing barrier:** the master thread fills the buckets (`#pragma omp master`, which has no barrier). The
    other threads meanwhile enter the `omp for schedule(dynamic)` that sorts the buckets, possibly before they are
    filled.
  * **Wrong offsets:** the rebuild offsets are `indexes[i] = indexes[i-1] + size(bucket i)`, where `bucket i-1`
    is presumably intended.
  * **Natively, the output is not sorted:** at `tiny` with 4 threads, about 4.09 M of 8.39 M adjacent pairs are
    out of order, a different count in each of 3 runs; with 1 thread, 55,103.
  * **Consequence:** IS's ROI does timing-dependent work with a wrong result.

## LRU qualification so far (`results/stage6-capbench-qual`, archived)

Paper inputs, 8 MB L3, first 500 M ROI instructions. These are workload characterisations, not comparisons.

| | KM `paper` | LU `standard` | IS `standard` |
|---|---|---|---|
| Status | ok, all checks pass | ok, all checks pass | timeout (3,600 s) inside the ROI, no statistics |
| IPC / L1-D MPKI / L2 MPKI | 6.87 / 1.0 / 0.06 | 2.60 / 11.7 / 11.7 | — |
| L2 miss rate | 6% | 99.9% | — |
| L2 victims S/E/M % | 1/1/98 | 2/42/56 | — |
| Heavy sharer hint | 0% | 1.4% | — |
| L2 invalidations / downgrades | 8.4 k / 12 k | 1.1 k / 160 k | — |
| L3 misses, DRAM reads / writes | 2.1 k, 2 k / 0 | 5.4 M, 3.5 M / 1.9 M | — |
| Simulation rate | 0.84 MIPS | 0.29 MIPS | — |

* **KM's 500 M** cover only 28% of its first iteration. Each point is read once per iteration, so a single
  partial iteration cannot show reuse.
* **LU `standard` streams:** its 9.4 MB matrix exceeds every cache, and the reuse distance between elimination
  steps is far beyond the L2.

## Reproduction status

**Documented by Souza & Freitas** ([P24] Sec. III, [T21] Sec. 4.2):
* **Simulator and cores:** Sniper 7.3 (August 2019), Pin front end; 16 x86 cores at 2.66 GHz, no SMT, 22 nm.
* **L1:** private, 32 kB data and 32 kB instruction, 8-way.
* **L2:** private, 256 kB, 8-way (or shared 4 MB in the thesis).
* **Latencies (CACTI 6.0):** 4 / 16 / 42 cycles (shared L2 21).
* **L3:** 16 MB, 16-way, shared, modelled as perfect (every request hits).
* **Other memory settings:** 64-byte blocks (the texts say "bits"), MESI directory, no prefetching, inclusive
  hierarchy.
* **Policies:** applied to L1 and L2 simultaneously. Baselines LFU, LRU, MRU, NRU, PLRU, SRRIP; variants RL-State,
  RL-Bitset, RL-Both.
* **Sharing threshold:** 9 or more of 16 cores.
* **Workloads:** the seven kernels and the inputs in the table above.
* **Thesis-only experiments:** block sizes of 64/128/256 and L2 associativity of 4/8/16.
* **ROI:** "slightly adapted for use with the Sniper simulator – we delimited the region of interest, speeding up
  simulations".
* **Metrics:** L1 and L2 data miss rates, coherence invalidations and write-backs, IPC, energy (McPAT).
* **LRU baseline per kernel** (thesis Fig. 4.1): for example L2 miss rates of 99.8% (FAST), 51.7% (IS), 85.8%
  (KM) and 11.9% (LU), and 103 M L1 accesses for LU.

**Recovered from the CAP Bench source** (not stated in the papers):
* the repository revision and its class tables;
* that FN's paper range is the pre-2015 `standard` class;
* that KM's paper input mixes `standard`'s point count and `small`'s centroid count;
* the seeds (0; TSP 12345);
* the OpenMP structure of each kernel;
* the upstream defects above;
* that the repository has no ROI markers.

**Our modifications and approximations:**
* **ROI and markers:** our ROI is the kernel's own timed region; the iteration markers are also ours.
* **Hardware model:**
  * 4 cores instead of 16 (sharing threshold > 2 of 4);
  * policy on the private L2 only (L1-D stays LRU);
  * a real 8 MB L3 instead of a perfect 16 MB one;
  * this tree's Gainestown latencies, not the CACTI values above;
  * this Sniper fork with the SDE front end, not Sniper 7.3 with Pin.
* **Software:** GCC 11.4 without `-Werror`; the `paper` classes for FN and KM.
* **Region size:** an instruction or iteration bound instead of the whole kernel.

**Still unavailable:**
* **The authors' ROI:** where exactly the region was. Their counts imply it was far smaller than the whole kernel
  at the stated inputs (for example 103 M L1 accesses for LU, against about 18 G instructions here), unless the
  counts are per core or sampled.
* **Configuration:** their Sniper configuration files and the code of their policies in Sniper.
* **Software:** their compiler and flags, and thread counts per kernel.
* **Run settings:** whether the perfect L3 was a configuration or a code change, and the number of repetitions.
* **Results:** the raw results behind the figures.

**Worth requesting from the authors** (M. A. Souza, H. C. Freitas):
1. Their Sniper 7.3 tree or patch with the RL-State / RL-Bitset / RL-Both implementations and the base policies.
2. The CAP Bench sources as simulated: ROI markers, any input or code changes, and the commit they started from.
3. The Sniper configuration files: core model, cache hierarchy and latencies, how the perfect L3 was configured,
   and the directory and MESI settings.
4. The exact command lines: inputs, thread counts, `--roi` or other ROI options, warm-up and fast-forward.
5. Compiler version and flags.
6. Raw per-kernel, per-policy outputs (`sim.out` / `sim.stats`) behind the figures, including L1/L2 accesses and
   misses, invalidations, write-backs, IPC and energy.
7. The number of runs per configuration and how variability was handled.
8. Whether they observed the LU pivot and IS sorting defects, and whether their copies differ.

## LRU qualification 2 (`results/stage6-capbench-qual2`, archived)

The two experiments recommended after the tooling review, each with `lru` and with `lru-hist` (the same LRU victim
sequence plus the eviction history, so that reuse just beyond the L2's capacity can be measured). One run each; all
checks pass.

| | KM `paper`, iteration 2 alone | LU `tiny` (exploratory), whole ROI |
|---|---|---|
| Measured region | `iter-2-begin:roi-end`, 1.77 G instructions | complete kernel ROI, 0.717 G instructions |
| IPC / L1-D MPKI / L2 MPKI | 6.92 / 1.05 / 0.115 | 8.83 / 9.47 / 1.43 |
| L2 evictions per 1,000 instructions | 0.02 | 0.85 |
| L2 hits at LRU stack positions 4-7 (of 0-7) | 0.1% | 78.9% (17.3% at position 7) |
| LRU misses on blocks evicted within the set's last 16 evictions | 0.019 MPKI | 0.75 MPKI |
| L2 victims S / E / M | 1 / 93 / 6% | 9 / 1 / 89% |
| Wall time | 5,178-5,232 s | 1,305-1,310 s |

* **KM** has almost no replacement pressure. Its L2 misses are mostly coherence misses; removing every capacity
  miss would save about 0.1% of the run time. It does not justify a policy comparison.
* **LU `tiny`** reuses data just past the L2's capacity, and evicted Shared blocks return less often (59%) than
  Modified ones (92%). It was chosen for the five-policy pilot below.
* **KM's iteration markers** were all seen in order (31 begin, 31 end). The deferred switches lay 0.26-1.22 M
  instructions after their markers, under 0.07% of an iteration.

## LU `tiny` five-policy pilot (`results/stage6-capbench-lu-pilot`, archived): EXPLORATORY

**Exploratory:** the upstream LU pivot defect (`lower_upper.c:93`, above) makes the factorization numerically
unreliable and may change its data-dependent memory access pattern. None of these results reproduces the paper's LU
or stands for a correct LU. They describe this program, unchanged from upstream apart from the markers.

**Setup:**
* **Runs:** `lru`, `sf-text`, `sf-example`, `rl-n2`, `rl-n4`, three repetitions each (15 runs), 4 at a time.
* **Machine:** 4 Gainestown cores, the policy on the private L2 only, 8 MB shared L3, prefetchers off.
* **Workload:** `capbx-lu-tiny` (`lu.intel --class tiny --nthreads 4`), the complete kernel ROI to the end of the
  application.
* **Builds:** one simulator binary (md5 `192619ee…`) and one LU binary (md5 `1c8b6c6b…`) for every run.
* **Outcome:** no crashes, timeouts or failed checks; 0 stale write hits; wall time 1,216-1,345 s per run.

**Every repetition** (from `metrics-per-run.csv`):

| Policy | Rep | Instructions (M) | IPC | Simulated time (µs) | L2 misses | L2 demand misses | L2 dirty evictions | L2 invalidations | L2 downgrades | Victims S / M % | Differ from LRU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| lru | 1 | 716.42 | 8.8305 | 30,500 | 1,028,306 | 838,594 | 543,039 | 205,942 | 340,995 | 9.5 / 89.3 | – |
| lru | 2 | 716.20 | 8.8317 | 30,487 | 1,020,087 | 832,999 | 542,491 | 201,294 | 332,008 | 9.4 / 89.4 | – |
| lru | 3 | 716.27 | 8.8225 | 30,522 | 1,053,762 | 864,242 | 566,613 | 206,155 | 345,936 | 9.3 / 89.5 | – |
| sf-text | 1 | 721.25 | 8.7429 | 31,013 | 1,511,217 | 1,324,649 | 1,035,495 | 200,557 | 341,798 | 5.2 / 94.3 | 93.2% |
| sf-text | 2 | 723.16 | 8.7501 | 31,070 | 1,514,339 | 1,328,609 | 1,040,496 | 200,000 | 337,300 | 5.1 / 94.3 | 93.4% |
| sf-text | 3 | 722.30 | 8.7455 | 31,049 | 1,513,313 | 1,324,841 | 1,030,599 | 205,082 | 348,829 | 5.2 / 94.2 | 92.9% |
| sf-example | 1 | 720.02 | 8.6784 | 31,190 | 1,787,542 | 1,603,227 | 1,309,799 | 203,347 | 344,445 | 4.0 / 95.3 | 92.2% |
| sf-example | 2 | 719.46 | 8.6753 | 31,178 | 1,792,691 | 1,609,978 | 1,317,813 | 201,684 | 345,089 | 4.1 / 95.3 | 92.3% |
| sf-example | 3 | 719.28 | 8.6748 | 31,172 | 1,790,920 | 1,607,801 | 1,315,435 | 201,565 | 345,656 | 4.1 / 95.2 | 92.6% |
| rl-n2 | 1 | 716.59 | 8.8249 | 30,526 | 1,031,168 | 847,490 | 558,434 | 197,784 | 335,752 | 9.6 / 89.3 | 6.3% |
| rl-n2 | 2 | 714.80 | 8.8252 | 30,450 | 1,006,130 | 816,438 | 524,237 | 202,822 | 336,917 | 9.9 / 89.0 | 6.4% |
| rl-n2 | 3 | 714.31 | 8.8222 | 30,439 | 1,001,445 | 814,641 | 525,679 | 199,562 | 334,619 | 10.0 / 89.0 | 6.4% |
| rl-n4 | 1 | 715.73 | 8.8408 | 30,435 | 969,444 | 782,712 | 494,800 | 195,766 | 329,767 | 10.8 / 88.0 | 8.0% |
| rl-n4 | 2 | 716.70 | 8.8355 | 30,495 | 995,106 | 812,303 | 527,004 | 190,778 | 329,845 | 10.6 / 88.3 | 8.4% |
| rl-n4 | 3 | 715.45 | 8.8339 | 30,447 | 978,458 | 796,784 | 511,801 | 192,961 | 333,866 | 10.7 / 88.3 | 8.5% |

* **L3 misses and DRAM traffic** are the same for every policy (33.4 k L3 misses, 33.4 k DRAM reads, 0 writes).
  They are the cold misses; the 8 MB L3 holds the whole working set.

**Means, change vs LRU:**

| | sf-text | sf-example | rl-n2 | rl-n4 |
|---|---|---|---|---|
| Simulated time | +1.77% | +2.22% | −0.10% | −0.14% |
| IPC | −0.93% | −1.72% | −0.05% | +0.10% |
| L2 misses | +46.3% | +73.1% | −2.0% | −5.1% |
| L2 dirty evictions | +88% | +139% | −2.7% | −7.2% |
| L2 invalidations | −1.3% | −1.1% | −2.2% | −5.5% |
| L2 downgrades | +0.9% | +1.6% | −1.1% | −2.5% |

**Denominators** (shown on the `lru-hist` qualification run: 716.16 M instructions, 6,800,396 L2 accesses):
* **L2 misses: 1,020,957** (1.426 MPKI) = load misses 783,887 + store misses 237,070. They fall into three kinds:
  * **Demand misses: 833,842.** The block is absent (state I): load-I 783,887 + store-I 49,955. Only these need a
    replacement decision, and they are the policy diagnostics' "misses" (`rl-diag-misses` = `rl-event-fill-demand`).
  * **Upgrade misses: 186,485.** A store to a block present in S (186,465) or E (20). It is refilled in place
    (`rl-event-fill-upgrade-refill`); no victim is chosen.
  * **Other: 630.** Store misses recorded on blocks in M, with no fill of either kind.
* **Eviction-history returns: 538,082.** Demand misses on a block evicted within its set's last 16 evictions
  (`rl-diag-miss-replaced-in-history`). This equals the S/E/M sum of `reward-evict-bad`. As a share, it is:
  * 64.5% of the 833,842 demand misses (the figure in the policy summaries);
  * 52.7% of all 1,020,957 L2 misses;
  * 0.751 MPKI, against 1.426 MPKI for all L2 misses;
  * 88.5% of the 607,671 evictions;
  * 7.9% of the L2 accesses.

**Instructions, IPC and simulated time:**
* **They agree exactly in every run.** Cycles = simulated time × 2.66 GHz, and IPC = instructions / cycles,
  summed over the 4 cores.
* **The instruction count is not fixed.** The ROI is the same work, but the sf rules retire 0.46-0.95% more
  instructions than LRU (719.3-723.2 M vs 716.2-716.4 M). The rl policies are within ±0.3%.
* **Extra instructions track extra time.** Across the 15 runs they rise with simulated time: about 0.7 extra
  instructions per extra core-cycle, r = 0.88. The extra goes to cores 1-3, while core 0 retires slightly fewer.
* **Likely cause: OpenMP waiting.** The run records no futex waits and spin-loop detection is off, so libgomp's
  barrier and critical-section waits spin in user code and count as retired instructions. This is consistent with
  the data but not proven.
* **Consequence:** IPC includes waiting instructions, so a slower policy loses less IPC than run time (sf-text:
  +0.83% instructions, +1.77% time, −0.93% IPC). For this fixed-work ROI, **simulated time** is the performance
  measure; absolute miss counts are cleaner than MPKI.

**Statistical evidence versus exploratory findings:**
* **Run-to-run variability of LRU:** across its three repetitions, L2 misses vary by 3.3% of the mean (standard
  deviation 1.7%), dirty evictions by 4.4%, simulated time by 0.11%.
* **The sf rules are clearly worse.** Their misses, dirty evictions and time are far outside LRU's range in every
  repetition.
* **rl-n4 at the L2 (indicative, not established):**
  * **Lower in every repetition:** L2 misses (−5.1%), dirty evictions (−7.2%) and invalidations (−5.5%). All
    three rl-n4 runs fall outside LRU's three-run range.
  * **Welch t statistics:** −4.2, −3.2 and −5.3, about 3.7 degrees of freedom.
  * **Why this is not significance:** three repetitions, about ten metrics examined, no correction for multiple
    comparisons, and run-to-run variation (two identical runs differ by a few instructions before the ROI) that is
    not shown to be the only source of difference.
* **rl-n4's performance change is not separable from noise.** Simulated time −0.14% overlaps LRU's range
  (t ≈ −2.1); IPC +0.10% is at the edge of it.
* **rl-n2:** every difference is within LRU's variability.

**Exploratory observations (mechanism, not tested):**
* **More Shared victims:** rl-n4 evicts more Shared blocks than LRU (10.6-10.8% vs 9.3-9.5% of victims, about
  +4.2 k) and fewer Modified ones (about −39.5 k).
* **Shared blocks are the better victims here.** Under every policy, evicted Shared and above-threshold blocks
  return within the history much less often (59-64%) than Modified ones (91-96%).
* **The learned weights point the same way.** In every rl-n4 run and core, wM is the most negative weight; wS is
  near 0, and wAT is positive on cores 0-2 (second-half means; positive = evict more readily).
* **Mostly LRU:** all candidates tie in 83% (rl-n4) and 88% (rl-n2) of decisions, which then go to the oldest.
  The policies leave LRU in only 8.3% and 6.4% of decisions.
* **The sf rules do the opposite.** sf-text and sf-example evict Modified blocks preferentially (94-95% of victims)
  and nearly double the misses.
* **No performance payoff here.** L3 never misses, so a saved L2 miss saves only an L3 hit, and the kernel is
  compute-bound (IPC 8.8 on 4 cores). Even a real L2 effect cannot change the run time much on this input.

**What the pilot does not show:**
* **Learning, specifically, is not shown to help.** rl-n4 departs from LRU on 8.3% of decisions, and its learned
  preference (Shared and above-threshold before Modified, within the 4 oldest) could come from a fixed rule.
* **The sf rules are not that control.** They are a different, ratio-based mechanism.
* **What can separate learning from a fixed preference:** the controls below.

## Controls for the learning question (`review/stage6-controls`)

`rl_lru` options that keep rl-n4's window, candidates, eligibility, history and diagnostics, and change only how
the victim is chosen among the 4 oldest eligible candidates. Implementation, tests and defaults:
[`POLICY_CORES.md`](POLICY_CORES.md#controls-for-the-learning-question).

| Control | `run_stage6.py` policy | Victim among the 4 oldest | Weight updates |
|---|---|---|---|
| Frozen weights | `rl-n4-frozen` | highest score under a fixed, recorded weight vector | none |
| Fixed priority | `rl-n4-prio` | S before E before M; above-threshold first within a state; then the oldest | none (weights unused) |
| Randomized | `rl-n4-rand` | the oldest, or with probability 0.083 a uniformly drawn other one (seeded) | none (weights unused) |

* **Frozen weights:**
  * **Source of the vector:** wS −0.09, wE −0.38, wM −0.76, wBT −0.64, wAT 0.00. It is the mean of the
    second-half (in-ROI) weight snapshots of the 12 core-runs of the rl-n4 pilot above.
  * **Not an independent test:** the vector comes from this workload, so it is an in-sample control. It asks
    whether continuing to update the weights adds anything to their average, not whether learned weights
    transfer.
  * **Core 3 differs:** it learned wAT −0.39 to −0.46 in all three runs, against +0.03 to +0.26 on cores 0-2.
    The pooled vector, applied to every core, does not follow that.
* **Fixed priority:** a hypothesis stated before seeing a comparison, not fitted. It is deterministic, with no
  learning.
* **Randomized:**
  * **Rate:** the probability equals rl-n4's measured rate of departing from LRU (8.3%). The run reports its own
    actual rate (`rl-chose-other` / `rl-decisions`), which is slightly lower: with a single eligible candidate,
    nothing is drawn.
  * **What it controls for:** a non-LRU perturbation at the same rate but uninformed by MESI state or sharing.
    It does not mimic *where* rl-n4's departures happen.
* **Status:** implemented and tested, not yet run on LU `tiny`.
  * **Tests:** 12 unit-test cases, plus a byte-identical differential test of 608 core configurations against
    51c74b99.
  * **Replays:** deterministic replays show the existing policies unchanged on the new build. They also show the
    random control repeating exactly with the same seed.
  * **Smoke run:** a 4-core smoke run of all three passed every checker.
  * **Details:** POLICY_CORES.md.
* **Reading frozen weights:** the summaries show `rl-weight-*-ppm` as end − begin, i.e. 0 for a frozen weight.
  Read the vector from the start-up CONTROL line or the weight snapshots.

## Proposed ablation (not run)

* **Policies:** `lru`, `rl-n4`, `rl-n4-frozen`, `rl-n4-prio`, `rl-n4-rand` on `capbx-lu-tiny`.
* **Setup:** the pilot's configuration, input and ROI. Every policy runs on one new simulator build from
  `review/stage6-controls`, including LRU and rl-n4, so that the comparison does not mix binaries.
* **Initial size:** two repetitions (10 runs), about 1,250 s each.
  * At 4 in parallel: three rounds, about 65-70 min.
  * At 5 in parallel, if the machine allows: two rounds, about 45 min.
* **Readout:**
  * **Cache level:** L2 misses and demand misses, dirty evictions, invalidations, the Shared share of victims,
    the history-return rates by class, and the rate of departing from LRU.
  * **Simulated time:** reported but not expected to move on this input.
  * **Comparisons:** each control is compared with both LRU and rl-n4, against LRU's three-run spread from the
    pilot (3.3% in L2 misses).
* **Interpretation:**
  * **rl-n4 ≈ `rl-n4-prio` (or ≈ `rl-n4-frozen`), both better than LRU:** the gain comes from a fixed preference
    for evicting Shared blocks, not from online learning.
  * **rl-n4 clearly better than both:** adaptation within the run adds something (more repetitions would then be
    needed to show it).
  * **`rl-n4-rand` ≈ rl-n4:** the effect is any perturbation of LRU, not a coherence-aware choice.
  * **All within LRU's spread:** the pilot's −5% was not robust.
* **Independent confirmation, later:**
  * **A corrected LU variant.** The minimal fix is in `lower_upper.c` (`>` instead of `<` in the pivot search,
    and the parenthesis at line 93), built as a separate, labelled variant; the factorization must then be checked
    natively.
  * **Or Ocean `small`** (SPLASH-2, validated in Pilot 3; correct output, no known defect).
    * **Reuse near the L2's capacity edge:** 72.6% of LRU's L2 hits are at stack positions 4-7, and rl-n4's
      eviction-history returns are 9.6 MPKI.
    * **rl-n4 there:** −0.85% L2 misses, within noise.
    * **Cost:** about 1,550 s per run.
    * **What it would show:** the same five-policy ablation on it would test whether any control ranking seen on
      LU carries over to a correct program.
  * **The corrected LU variant is not implemented yet.**
