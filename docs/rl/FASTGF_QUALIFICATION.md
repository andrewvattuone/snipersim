# FAST and Gaussian Filter (GF): a quick qualification study

*2026-11-24. This is a short workload qualification for the advisor meeting. It is not a policy comparison and not a
full evaluation. Results: `results/stage6-fastgf-qual/` (read-only, git-ignored, archived).*

> **Scope.** Every conclusion here is **preliminary**. It holds only for this project's configuration: 4 cores, the
> replacement policy on the private L2 only, a real 8 MB L3, and about 500 M instructions from cold caches.
>
> It is **not** a conclusion about Souza and Freitas's experiments. They used:
> * 16 cores;
> * the policy on both L1 and L2;
> * a perfect 16 MB L3;
> * Sniper 7.3 with Pin;
> * their own ROI adaptation, which is not public.
>
> See "Relation to the paper" below.

## Question

Could either CAP Bench kernel, FAST corner detection or the Gaussian filter (GF), be a useful workload for later
coherence-aware replacement experiments? A useful workload needs:
1. enough L2 replacement pressure for the victim choice to matter;
2. a mix of MESI states among the victims;
3. blocks that more than one core holds (sharer hints of 2 or more);
4. reuse just beyond the L2's capacity, which a better victim choice could capture.

## What was run

| | |
|---|---|
| Kernels | `fast.intel --class standard` and `gf.intel --class standard`, `--nthreads 4` |
| Inputs | The paper's: an 8192 × 8192 image; GF also uses an 11 × 11 mask ([P24] Sec. III-C; `CAPBENCH.md`, "Inputs") |
| Build | The existing `../capbench-build-b9c60223`: CAP Bench `ceb6f53b` + `capbench-roi.patch` b9c60223 (no rebuild) |
| System | The validated Stage 6 setup: 4-core Gainestown, private L2 of 256 KB 8-way, shared inclusive L3 of 8 MB, prefetchers off, `num_runs=1`, both checkers enabled |
| Policy | `lru-hist` only: the same LRU victims (window 2, α = 0, no learning) with the eviction history and diagnostics, used to measure reuse just beyond the L2 |
| ROI | The kernel's own timed region (patch markers), cut off after the first **250 M** or **500 M** instructions (`-s stop-by-icount:250000000` / `500000000`) |
| Before the ROI | The random-image initialization is fast-forwarded with `--no-cache-warming` (FAST: about 1.8 G instructions). The 64 MB image is larger than the L3, so warming would change little |
| Simulator | `build/default/lib/sniper` md5 ae285766, the binary of the Stage 7 Ocean pilot (not rebuilt) |
| Harness | `experiments/rl/run_stage6.py` with four new workload entries (12 added lines): `capb-fast-250m`, `capb-gf-250m`, `capb-fast-500m`, `capb-gf-500m`. These runs used the file before it was committed; it is committed unchanged (md5 44ab4229) |
| Repetitions | 1 per kernel and ROI length. Two batches of 2 simulations in parallel (20 host cores, 13 GB free) |

**Coverage.** These runs see only the start of each kernel:
* **FAST (measured):** SDE `-mix`, run natively, counts about 8.89 G instructions per thread in the kernel (35.6 G in
  total). 250 M is **about 0.7%** of the ROI, roughly the first 14 image rows of each thread's 2,048.
* **GF (estimate):** 8182² output pixels × 121 multiply-adds is about 8.1 G inner iterations, so 250 M is well under
  1% of the filter loop. It never reaches the output-printing loop described below.

Both kernels do the same work for every image row, so a short prefix should show their steady-state access pattern.
That is an assumption, not something measured here.

The 250 M runs left the cold L2s barely full (6 and 111 replacements in total), so each kernel was also run to
500 M instructions, the top of the planned range. The 500 M runs repeat the first 250 M and add 250 M more. That
was the only way to see behaviour after the caches fill.

## Status of the runs

| Run | Status | Wall time | Checkers |
|---|---|---|---|
| `capb-fast-250m-…-lru-hist-r1` | **complete**, ok | 345 s | 3/3 pass |
| `capb-gf-250m-…-lru-hist-r1` | **complete**, ok | 516 s | 3/3 pass |
| `capb-fast-500m-…-lru-hist-r1` | **complete**, ok | 586 s | 3/3 pass |
| `capb-gf-500m-…-lru-hist-r1` | **complete**, ok | 921 s | 3/3 pass |

* **Checks:** every run entered and left its ROI and ended at the instruction limit.
* **Checkers:** `check_sharers`, `check_rl_events` and `check_rl_policy` all report PASS.
* **Stale writes:** none.
* **The summarizer exits 2** only because the harness had uncommitted workload entries when the runs were made
  (`tooling_clean false`). That is the same situation as the earlier CAP Bench qualification. The metrics themselves
  are unaffected.

## Correctness review of the kernels (upstream, CAP Bench `ceb6f53b`)

**Native checks** (scratch builds; the same binaries):
* **FAST:** the corner count is 4,728,352 in all three 4-thread runs and in a 1-thread run.
* **GF:** the full output image has the same md5 (`f95004d1…`) in two 4-thread runs and a 1-thread run.

Both kernels are deterministic at this build. Under Sniper the runs are truncated, so no output can be compared
there.

**FAST** (`src/FAST/fast.c`):
* **F1, a data race in the source, not visible in this build:** `z` (and `k`) are missing from the `private(...)`
  list, so all threads share `z`.
  * GCC 11 `-O3` appears to keep `z` in a register: the native results are deterministic, and the outlined
    function calls `omp_get_thread_num` once, before the loop. The disassembly was not checked instruction by
    instruction.
  * A different compiler or flags could expose the race.
* **F2, false sharing (a performance artefact that matters here):**
  * The per-thread `corners[MAX_THREADS]` ints share cache lines: 16 counters per 64 B.
  * `corners[tid]++` compiles to a memory increment (`addl $0x1,(%rax,%rbx,4)`, run about 4.7 M times over the
    whole ROI).
  * **Inference** (from the code, not checked by address): this is the only shared data the loop writes, so it is
    almost certainly the source of FAST's coherence traffic below.
* **F3, minor:**
  * pixels come from `char val = randnum() & 0xff; img = val > 0 ? val : -val` on a signed `char`, so the image is
    0..127 with -128 left as is;
  * the bounds test does not check `x < imgsize`, so neighbours past the right edge wrap to the next row;
  * the accumulator names are swapped (a brighter pixel increments `accumDarker`).

  None of these changes the access pattern materially.

**GF** (`src/GF/gauss-filter.c`, `main.c`):
* **G1, the ROI includes printing every output pixel:**
  * `gauss_filter` writes all 67 M output values to stderr with `fprintf` *inside* the timed region, after the
    parallel loop.
  * A full-ROI GF measurement with this source is therefore partly a single-threaded I/O formatting loop.
  * Whether the paper's GF numbers include it is unknown, because the authors' ROI adaptation is not public.
  * These truncated runs never reach it.
* **G2, the mask formula:** `-((i*i + j*j)/2.0*SD*SD)` computes −r²·SD²/2 instead of −r²/(2·SD²). With SD = 0.8 the
  kernel is wider than intended, though it is still normalised. Same access pattern.
* **G3:** the 64 MB output buffer is `calloc`ed inside the ROI.

## Results (measured, 1 run each, 4 cores, LRU victims)

*Ocean and Barnes are the archived LRU r1 runs of `results/stage6-ocean-barnes-pilot`; LU and KM are those of
`results/stage6-capbench-qual`.* IPC is summed over the 4 cores. Sharer hints are the delivered hints: 1 = only
the requester, ≥ 2 = another L2 also held the line.

| | Ocean `small` | Barnes 4096 | LU `std` 500M | KM `paper` 500M | **FAST 250M** | **FAST 500M** | **GF 250M** | **GF 500M** |
|---|---|---|---|---|---|---|---|---|
| ROI instructions | 273 M | 372 M | 501 M | 501 M | 251 M | 501 M | 251 M | 501 M |
| IPC (sum) | 2.36 | 5.09 | 2.60 | 6.87 | 7.37 | 7.40 | 2.88 | 2.88 |
| L1-D MPKI | 40.5 | 7.7 | 11.7 | 1.0 | 0.38 | 0.40 | 13.2 | 13.2 |
| **L2 MPKI** | **21.8** | **0.52** | **11.7** | 0.06 | **0.23** | **0.23** | **0.054** | **0.042** |
| L2 miss rate | 53% | 6.8% | 99.9% | 6.1% | 59% | 58% | 0.4% | 0.3% |
| **L2 replacements per k-instr** | **20.2** | **0.23** | **7.8** | 0.008 | 0.00002 (6 total) | 0.004 (2,048) | 0.0004 (111) | 0.009 (4,560) |
| L2 victims S/E/M % | 5/9/86 | 54/1/45 | 2/42/56 | 1/1/98 | (n = 6) | 15/77/8 | 16/78/5 | 7/62/32 |
| Victims with heavy hint (≥ 3 of 4) | 1.2% | 10.7% | 1.4% | 0% | 0% | 6.0% | 3.6% | 2.5% |
| Fills with hint ≥ 2 | 5.1% | 44.7% | 1.0% | 33.6% | 40.5% | 41.9% | 2.1% | 1.4% |
| L2 invalidations per k-instr | 0.49 | 0.13 | 0.002 | 0.017 | 0.091 | 0.096 | ≈ 0 (17) | ≈ 0 (17) |
| Misses that are coherence misses | 2.8%¹ | 30%¹ | – | – | 68% | 73% | 0% | 0% |
| **Misses on blocks still in the 16-entry history** (reuse just beyond the L2) | 46%¹ | 46%¹ | – | – | 0% | **0.1%** | 0% | **0.1%** |
| L3 misses | 1.88 M | 9.9 k | 5.38 M | 2.1 k | 10.7 k | 18.1 k | 13.3 k | 20.7 k |

¹ From the `rl-n4` r1 runs (the lru runs have no diagnostics). The rl-n4 victims are close to LRU's but not
identical. LU and KM were run with plain `lru`, which has no history diagnostics.

**After the caches fill** (difference between the 500 M and 250 M runs of the same deterministic setup; an
estimate, since the two runs are separate simulations):

| | FAST | GF |
|---|---|---|
| L2 replacements per k-instr | 0.008 | 0.018 |
| L3 misses per k-instr | 0.030 | 0.030 |

That is about 2,500× (FAST) and 1,100× (GF) less replacement pressure than Ocean, and 13–30× less than Barnes.

## What the numbers show

* **Replacement pressure is negligible in both kernels.**
  * **The stencil window fits in the L2,** as predicted from the code before the runs. FAST's 7-row neighbourhood
    is about 56 KB per thread and GF's 11-row window about 88 KB, both well under 256 KB.
  * **What the L2 sees is a stream:** each output row brings in one new 8 KB image row, which is reused only for the
    next 7 (FAST) or 11 (GF) output rows.
  * **GF:** the L1-D misses (13 MPKI) are almost all absorbed by the L2 (0.3% miss rate).
  * **The cost is low:** the L3 sees about 0.03 misses per k-instruction from each kernel.
* **No reuse beyond the L2's capacity.**
  * Only 0.1% of misses are on a block still in the 16-entry eviction history, against 46% on Ocean and Barnes.
  * An evicted image row is not touched again, so LRU's victims are already dead blocks. A better victim choice
    has almost nothing to win.
* **FAST's "sharing" is false sharing of one counter line, not data sharing.**
  * About 41% of FAST's fills carry a hint ≥ 2, and 68–73% of its L2 misses are coherence misses.
  * These are the `corners[]` ping-pong (F2, inferred): a single hot line that is never a replacement victim.
  * The image itself is read privately, apart from a 3-row halo at each thread boundary.
  * This traffic is a property of the upstream code, not of the replacement policy.
* **GF has no sharing to speak of:** 17 invalidations in 500 M instructions, 1.4% of fills with another holder.
* **Victim states:** mostly E (the read-only image, held privately).
  * GF also has M victims (32% at 500 M): its output rows are written and evicted.
  * So only the E/M distinction appears, and the shared-state signal the policy relies on is nearly absent.
* **Timing (inference):** with L2 MPKI of 0.04–0.23 and about 0.03 L3 misses per k-instruction, both kernels are
  compute-bound in this window. GF's IPC of 2.9 for 4 cores is presumably limited by its floating-point inner loop.
  Even a perfect L2 policy could hardly move their execution time.

## Preliminary verdict for coherence-aware replacement (this configuration only)

**In this configuration, neither FAST nor GF looks promising** with the paper's inputs: 4 cores, policy on the
private L2 only, 8 MB L3, measured on the first 500 M ROI instructions, 1 run each. Each fails the first two
criteria:
* replacement pressure is 3–4 orders of magnitude below Ocean;
* there is essentially no reuse beyond the L2.

GF also fails the sharing criterion. FAST's apparent sharing is a false-sharing artefact on a never-evicted line.

They are **not** recommended for this project's next policy comparisons in this configuration. They could serve as
**negative controls** here: a correct L2 policy should leave their misses and run time unchanged.

## Relation to the paper (hypotheses, not tested)

These runs do not reproduce or test the paper's FAST and GF experiments. The paper's setup differs in ways that
could matter:
* **The policy was applied to the L1s as well** ([T21] Sec. 4.2; `CAPBENCH.md`, "Reproduction status").
  * GF's L1-D MPKI is 13 here.
  * An L1 policy acts on exactly the window that the L2 absorbs in our setup, so it could matter far more there
    than an L2-only policy does here.
  * This is the most important difference for these two kernels.
* **16 cores:**
  * The per-thread stencil window would be the same size, since it depends only on the image width and mask.
  * But each thread's rows, the thread-boundary halos and the `corners[]` false sharing would all change.
* **A perfect 16 MB L3:** every L2 miss costs the same there, which changes timing but not L2 replacement counts.
* **Their ROI** may differ from ours (the patch markers), for example in GF's output loop.

What can be said:
* The thesis's 99.8% L2 miss *rate* for FAST under LRU is compatible with the low miss *count* seen here, if the L1
  filters almost every access and what reaches the L2 is mostly first-touch traffic.
* Our 58% includes false-sharing coherence misses that a 16-core run would see differently.
* Whether FAST or GF has replacement headroom in the paper's setup, especially at the L1, is an open question that
  this study does not answer.

## Limitations

* **One run per kernel and configuration:** no variability estimate. All numbers are characterisations, not
  significance claims.
* **Only the start of each ROI** (≤ 500 M instructions, about 1.4% of FAST's 35.6 G-instruction kernel), from cold
  caches (`--no-cache-warming`). The steady state is inferred from the 250→500 M difference and from the code's
  access pattern. A run over the full ROI was not attempted (FAST alone would take roughly 12 h at 0.85 MIPS).
* **The analysis is analytic:** the GF ROI length is computed from the loop, not counted.
* **Unverified inferences:**
  * that FAST's coherence traffic is `corners[]` (from the code, not checked by address);
  * that the paper's GF timing includes the output loop (from the code; the authors' ROI patch is not public).
* **One configuration:** 4 cores, an 8 MB L3, the policy on the L2 only (the L1s stay LRU), LRU victims only. No
  statement is made about L1 replacement or about the paper's 16-core setup.
* **Ocean and Barnes reuse figures** come from rl-n4 runs, not LRU.

## Recommendations

1. **Do not use FAST or GF for the next L2 policy comparisons in this configuration.** Record them as
   provisionally qualified negative controls. Revisit if the project later studies L1 replacement or the paper's
   16-core setup.
2. **Keep Ocean `small` as the main workload** and Barnes as the sharing-rich secondary. They remain the only
   measured workloads with both L2 replacement pressure and reuse just beyond the L2 (46% history hits).
3. **If more CAP Bench coverage is wanted:** TSP and FN (not yet qualified) are the remaining paper kernels.
   * Neither is obviously memory-bound, so a code reading like the one above (working set against L2 size) should
     come before spending simulation time.
   * A larger image (`large`, 16384) would not help FAST or GF: the window would be 112–176 KB and still fit in the
     L2.
4. **Optional, for the record:** report F2 and G1 upstream, or note them in `CAPBENCH.md`. Neither changes anything
   already measured.

## Remaining work (not done)

* **Not done:**
  * no repetition;
  * no full-ROI run;
  * no warm-cache variant;
  * no address-level confirmation of the `corners[]` attribution.
* **Committed:** this file and the 12 added lines in `run_stage6.py` (4 workload entries) are on branch
  `review/stage6-fastgf-qualification`, based on `feature/rl-port` 2363b52e and not merged.
* **Not committed:** the raw results, which are git-ignored and stay local, together with the archive below.

## Files

* `results/stage6-fastgf-qual/`:
  * the four run directories;
  * `manifest.jsonl` (exact commands, checksums, status);
  * `checkers.txt`;
  * `metrics.csv`, `metrics-250m.csv`, `summary.txt`;
  * `PROVENANCE.txt`;
  * `native/` (SDE mix and native check outputs).
* **Progress logs:** `results/stage6-fastgf-qual.progress.log`, `results/stage6-fastgf-qual.progress-500m.log`.
* **Helper scripts (observation only)** in `results/stage6-fastgf-qual/scripts/`:
  * `hint_hist.py`: the victim hint distributions from `checkers.txt`;
  * `fill_stats.py`: fill hints, invalidations by reason.
* **Archive:** `results/archive/stage6-fastgf-qual-20261124.tar.gz`, sha256 `cf4e23bd…64bc0`.
  * It contains the results directory and both progress logs.
  * Verify from `results/` with `sha256sum -c archive/stage6-fastgf-qual-20261124.tar.gz.sha256`.
  * Each file is also listed in `results/stage6-fastgf-qual/SHA256SUMS` (116 files).
* **`results/stage6-fastgf-qual/SUMMARY.md`** is the archived snapshot of the first draft of this report. Where they
  differ, this committed version supersedes it in scope wording and file status; the measurements are the same.
