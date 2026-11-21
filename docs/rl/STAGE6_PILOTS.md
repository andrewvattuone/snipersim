# Stage 6 pilots: initial performance evaluation

Pilot experiments for Stage 6 of [`ALGORITHM_AUDIT.md`](ALGORITHM_AUDIT.md) (Section 9, "Experiments A"), on
`feature/rl-port` (`9bcf5e45`: the RL policies plus the trace front-end fix `fa8d21b6`). They are bounded pilots for
review, not the benchmark campaign. The RL algorithms and their default parameters are unchanged.

**Status: pilot findings only.** Every policy result below comes from **two repetitions** of one configuration
(8 MB L3). The repetitions bound the run-to-run variation of the simulator, but two samples give no confidence
interval and cover no input or configuration variation. None of the differences is a statistically established
performance improvement, and none is attributed to learning.

## Method

**Policies** (private L2 only; every other cache stays LRU):

| Name | Configuration |
|---|---|
| `lru` | `replacement_policy=lru` |
| `sf-text` | `sf_lru`, `sf/mode=both`, `sf/rule=text`, `sf/combine=and` |
| `sf-example` | `sf_lru`, `sf/mode=both`, `sf/rule=example`, `sf/combine=and` |
| `rl-n2` | `rl_lru`, `rl/window=2`, `mode=both`, `alpha=0.1`, `protect_feedback=true`, `weight_scope=global` |
| `rl-n4` | as `rl-n2` with `rl/window=4` |

**Fixed for every run:**
* `-n 4 -c gainestown`, the given `perf_model/l3_cache/cache_size`, `traceinput/num_runs=1`;
* `perf_model/{l1_icache,l1_dcache,l2_cache}/prefetcher=none`;
* `SNIPER_RL_EVENT_CHECK=1` and `SNIPER_COHERENCE_CHECK=1`, which only add statistics and probes.

The four RL policies also run with `rl_diag/enabled=true`. It changes no decision (Stage 4), but adds host time.
The recorded `sim.cfg` of each RL run differs from the LRU run's only in these keys and in run-sniper's temporary
trace prefix.

**Our configuration against the paper's** ([P24] / [T21], see `ALGORITHM_AUDIT.md` Section 2):

| | This study | Souza & Freitas |
|---|---|---|
| Cores | 4 (Gainestown) | 16 |
| Policy applied to | private L2 only (256 kB, 8-way); L1-D stays LRU | private L1 and L2 together |
| L3 | shared 16-way, real: 8 MB (pilots) and 1 MB | 16 MB shared, "perfect" (every request hits) |
| DRAM | modelled | effectively never reached |
| Protocol | Sniper MESI, delivered sharer hints | MESI, directory |
| Prefetching | off | off |
| Simulator | this Sniper fork | Sniper 7.3 |

**Tools** (`experiments/rl/`):
* **`run_stage6.py`** runs the matrix (workloads × L3 sizes × policies × repetitions) with a per-run timeout.
  * It never reuses a run directory.
  * It writes `manifest.jsonl`: per run, the exact command, git commit, binary checksum, exit status, wall time
    and status. A run is `ok` only if it exited 0, wrote statistics and logged no exception.
  * It runs `check_sharers.py`, `check_rl_events.py` and `check_rl_policy.py` on every completed run
    (`checkers.txt`).
* **`summarize_stage6.py`** computes per-run metrics (`metrics.csv`), the mean and spread per configuration, the
  change against LRU, and the RL behaviour.
  * Its extraction was checked against an independent recomputation from the raw `sim.stats` (IPC, L2 MPKI, L2
    misses, invalidations, decisions).
  * Metrics cover all cores, over the whole run, or over the region of interest for `--roi` workloads (then
    `sim.stats` is exactly the `roi-begin`..`roi-end` delta). IPC is the sum of instructions divided by the global
    cycle count, i.e. aggregate over the 4 cores.
  * It checks that the runs of each workload and L3 size share one commit and binary and that their `sim.cfg`
    differ only in the policy keys, and reports failed, timed-out, or statistics-less runs instead of using them.
* **Provenance:** the manifest's `commit` and `binary_md5` identify the simulator. The `git_revision` in each
  run's `sim.info` is not reliable: it is a CMake configure-time label (`build/default/config/sniper.py`), still
  `12eda347` from Stage 4 although the binary was rebuilt from `9bcf5e45`.

Results are in the git-ignored `results/stage6-pilot/` and `results/stage6-fft-pilot/` (each with a
`PROVENANCE.txt`; made read-only) and in `results/archive/stage6-pilots-20261121.tar.gz` (SHA-256 in the `.sha256`
file beside it).

## Pilot 1: `sharetest` (20 runs)

`sharetest r 4` and `u 4`, 8 MB L3, 5 policies × 2 repetitions. All 20 runs completed (32-35 s each, 5 in
parallel); every checker passed; 0 stale write hits; no-eligible-victim 0.

| Workload | Policy | IPC vs LRU | L2 misses vs LRU | Differs from LRU |
|---|---|---|---|---|
| `r` | sf text / sf example / rl N=2 / rl N=4 | +0.03 / +0.02 / +0.01 / +0.04 % | −0.03 / **−0.93** / −0.04 / +0.03 % | 38 / 99 / 18 / 26 % |
| `u` | same order | +0.01 / +0.02 / 0.00 / +0.04 % | −0.02 / **−0.56** / −0.03 / −0.01 % | 35 / 99 / 14 / 37 % |

**Variation between repetitions:** at most 0.065% in IPC and 0.07% in L2 misses. So every IPC difference is noise,
and only `sf-example`'s miss reduction is outside it.

**Limitations: `sharetest` cannot separate replacement policies.**
* **Its L2 misses are not reuse misses.** 52-55% are cold. Almost all the rest come from each thread streaming
  its private 1 MB buffer, whose reuse distance exceeds the L2. Only 0.1% of misses hit the eviction history.
* **Its features are nearly constant:** 91-92% of victims are M, and 2-3% have a heavy delivered hint.
* **No DRAM writes:** the footprint (about 4 MB) fits in the 8 MB L3.
* **The one real effect is not learning.** `sf-example`'s miss reduction comes from evicting the second-oldest
  block 99% of the time (the `example` + AND corner case, I-9).
* **Use:** `sharetest` stays a coherence and correctness microbenchmark, not a performance workload.

## Pilot 2: FFT `-m 16` (10 runs)

`test/fft/fft -p 4 -m 16 -t` (SPLASH-2 FFT, 2^16 complex points, with its inverse-FFT self-test), 8 MB L3,
5 policies × 2 repetitions.
* **Completion:** all 10 completed in 828-890 s each, 5 in parallel; the self-test passed in every run.
* **Checks:** every checker passed, with 0 stale write hits and no-eligible-victim 0.
* **Region measured:** the whole run, including initialization and the self-test (`test/fft` has ROI markers,
  but these runs did not use `--roi`). Any later FFT runs should use `--roi` for consistency with SPLASH-2 below.

| Policy | IPC | vs LRU | L2 MPKI | L2 misses vs LRU | L2 dirty evictions vs LRU | L2 coherence invalidations | Differs from LRU |
|---|---|---|---|---|---|---|---|
| LRU | 5.491 | — | 1.972 | — | — | 392 | — |
| sf text | 5.496 | **+0.10%** | 1.985 | **+0.67%** | +3.2% | 1871 | 72% |
| sf example | 5.493 | +0.04% | 1.977 | +0.28% | +1.2% | 709 | 99.8% |
| rl N=2 | 5.494 | +0.06% | 1.970 | **−0.12%** | +0.3% | 407 | 14% |
| rl N=4 | 5.493 | +0.04% | 1.969 | **−0.14%** | +0.2% | 398 | 20% |

**Variation between repetitions:**
* about 0.01-0.03% in IPC, simulated time and L3 misses;
* 0.002-0.18% in L2 misses;
* 1-11% in L2 coherence invalidations (a few hundred events).

**What is meaningful:**
* **L2 misses:** `rl_lru` −0.12/−0.14% and `sf_lru` text +0.67% are outside the noise. All are small.
* **IPC:** the gains are 2-5 × the noise but at most 0.1%. FFT's L2 MPKI is about 2, so L2 misses barely matter
  to performance.
* **`sf_lru` text has more L2 misses yet the best IPC.** The non-LRU policies cause 8-11% fewer L1-D inclusion
  victims (back invalidations from the L2), but that accounts for only part of the L1-D difference (−486 misses
  of 486,000). The IPC difference has no single clear cause and is not evidence of learning.
* **Coherence invalidations:** `sf_lru` keeps lines that are later invalidated by coherence (4.8 × LRU), but these
  are a few thousand events.
* **L3 and DRAM traffic** are unchanged (DRAM writes 0: the footprint fits in 8 MB).

**What the policies see:**
* **Reuse exists.** 41% of L2 misses are history hits, blocks the policy evicted and needed again: FFT has the
  reuse `sharetest` lacks. Another 22% are cold misses.
* **The features are nearly constant:**
  * 89-92% of victims are M;
  * 0.3-0.5% have a heavy delivered sharer hint, so the sharing feature is almost always "below the threshold".
* **The learning signal is mixed.**
  * `rl_lru`'s rewards are balanced: 185,000 bad evictions against 218,000 good ones.
  * 53% of protect-wasted rewards are contradicted by the block's return (I-6).
* **Repeated protection.**
  * `rl_lru` has pass runs up to 84-98 passes, though only 13-20% of runs reach 5 passes.
  * `sf_lru` rejects candidate #1 five or more times in a row in 72-88% of its runs. With `example`, it evicts
    the second-oldest block 99.8% of the time (I-9).

**Conclusion:** FFT exercises replacement (440,000 decisions, real reuse) but is not memory-bound, and gives the
coherence features almost nothing to distinguish. The policies differ from LRU by at most 0.7% in L2 misses and
0.1% in IPC.

**Follow-up on `sf_lru` text's coherence invalidations** (from the existing statistics, repetition 1; no new runs).
The text rule evicts candidate #1 only if its state and sharing class are the most common in the set's history.
In FFT 89-92% of history entries are M, so S and E blocks are kept and the next block is evicted instead.
* **It protects S blocks:** S victims fall from 29,772 (LRU) to 19,007.
* **The protection is rarely useful.** Of about 7,000 protection runs (consecutive second chances for one block):
  * 5,066 (72%) end with the block invalidated by another core's write;
  * 1,789 (25%) end with its eviction;
  * 167 (2.4%) end with a hit.
* **Hence the invalidation count:** L2 coherence invalidations rise from 413 to 1,840. FFT's transposes read
  blocks that another core then writes.
* **Consequence:** `sf_lru` text's 0.1% IPC gain does not come from protection hits. A dedicated FFT rerun is not
  needed. Watch the same mechanism in the SPLASH-2 comparison: "keep the minority state" protects shared blocks,
  which pays off only if they are re-read before another core writes them.

## SPLASH-2 setup

**Source:** [snipersim/benchmarks](https://github.com/snipersim/benchmarks) at
`ba6004fd2b251be0ee5fc9e5d77374f1d4c80317` (2024-02-10), cloned to `../sniper-benchmarks`, beside this tree and
outside it. No tracked file of that repository is modified.

**Build** (`experiments/rl/build_splash2.sh`, verified from a clean clone):
* Only the ROI hooks library and four programs are built: Radix, LU (contiguous blocks), Ocean (contiguous
  partitions) and Barnes. Nothing is installed, and no Sniper is built or downloaded.
* The hooks are compiled against this tree's `include/sim_api.h`.
* Flags are those of the repository's `splash2/splash2/codes/Makefile`: `gcc -g -O3`, with `m4` and the POSIX
  `c.m4.null.POSIX` macros. Toolchain: GCC 11.4.0 and GNU M4 1.4.18.
* **Compatibility changes**, none to a tracked file:
  * The repository's helpers are Python 2: `makebuildscripts.py`, `env_setup_bm.py`, its `run-sniper` and
    `splashrun`. This host has no Python 2, so `tools/hooks/buildconf.*` is generated by a Python 3 rendering of
    `makebuildscripts.py` with identical flags, and each program's makefile is invoked directly.
  * LU needs `-DPAGE_SIZE=4096`: current glibc no longer defines `PAGE_SIZE`.
* Natively, Radix (`-t`) and LU (`-t`) pass their self-tests.

**Running:** the programs are run by `run_stage6.py` through this tree's `run-sniper`, not through the
repository's launcher.
* **Arguments:** those of `splashrun`, with 4 threads. Barnes reads its parameters from stdin
  (`experiments/rl/inputs/barnes-*-p4.in`, `splashrun`'s format), which reaches the program under Sniper.
* **Simulator check:** `sim.info`'s `snipercmd` names `snipersim/build/default/lib/sniper`, md5 `192619ee…`, the
  build of `feature/rl-port` at `9bcf5e45`.

| Workload | Arguments | Input size | Instructions (SDE, whole program) |
|---|---|---|---|
| `radix-small` | `RADIX -n262144 -p4 -t` | `small`: 2^18 keys (radix 1024) | 47 M |
| `lu-small` | `LU -n512 -p4 -t` | `small`: 512 × 512 matrix, 16 × 16 blocks | 273 M |
| `ocean-small` | `OCEAN -n258 -p4` | `small`: 258 × 258 grid | 273 M |
| `barnes-4096` | `BARNES < barnes-4096-p4.in` | 4,096 bodies, between `test` (1,024) and `small` (16,384) | 381 M |

* **Barnes size:** `small` would be 1.85 G instructions, about 1.5-2 h per simulation.
* **Not built:** Ocean's non-contiguous variant, LU's non-contiguous variant, Cholesky and the other
  applications. They build the same way.

**Region of interest.** The programs call the hooks' `parmacs_roi_begin` and `parmacs_roi_end`
(`MEMSYS_ON`/`MEMSYS_OFF`, which call `SimRoiStart`/`SimRoiEnd`). These markers bracket the parallel phase: from
creating the worker threads to joining them.
* **How it runs:** with `--roi`, the code before the ROI runs in cache-only mode, warming the caches and the
  policies. The ROI runs in detailed mode, and the code after it fast-forwards.
* **Checks:** in every run, the log shows the models enabled and disabled once at the markers. `sim.stats` equals
  `dumpstats --partial roi-begin:roi-end`.
* **Excluded before the ROI:** Radix 0.2 M, Ocean 0.3 M, Barnes 2.9 M and LU 35 M instructions (LU initializes
  the matrix serially).
* **Included in the ROI:** Radix generates its keys inside the ROI. Ocean's ROI includes its first time step.

## Workload qualification (4 LRU runs, 8 MB L3)

One LRU run per program with the same configuration as the pilots. All 4 completed and every checker passed. 0
stale write hits.

| Workload | Instructions (ROI) | IPC (4 cores) | L1-D MPKI | L2 MPKI | L2 miss rate | L2 evictions / kilo-instr | L2 hits in LRU positions 4-7 | L2 coherence invalidations | L2 downgrades | L3 misses | DRAM reads / writes | Victims S/E/M % | Heavy hint | Wall |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `radix-small` | 46 M | 1.78 | 6.5 | 2.6 | 40% | 2.1 | 19% | 2,120 | 11,159 | 35 k | 35 k / 0 | 12/0/88 | 1.3% | 170 s |
| `lu-small` | 238 M | 12.79 | 3.1 | 1.4 | 46% | 1.4 | 38% | 2,284 | 49,788 | 0.6 k | 0.4 k / 0 | 18/0/81 | 1.1% | 768 s |
| `ocean-small` | 273 M | 2.37 | 40.5 | **21.8** | 53% | **20.2** | **73%** | **134,143** | **412,660** | **1.87 M** | **1.54 M / 1.23 M** | 5/9/86 | 1.2% | 1,443 s |
| `barnes-4096` | 372 M | 5.09 | 7.5 | 0.5 | 7% | 0.2 | 7% | **48,716** | 111,920 | 10 k | 8 k / 0 | **54/1/45** | **10.7%** | 1,103 s |
| `fft-m16` (pilot, whole run) | 233 M | 5.49 | 2.1 | 2.0 | 93% | 1.9 | 63% | 413 | 24,374 | 68 k | 52 k / 0 | 7/4/89 | 0.5% | 888 s |

**Column notes:**
* **L2 hits in LRU positions 4-7** is the share of L2 hits on the four least-recently-used ways. A large share
  means the working set sits at the L2's capacity edge, where the choice of victim matters.
* **Wall** is wall-clock time with 4 simulations in parallel (FFT: 5).

**Correctness:**
* Radix and LU pass their self-tests (`-t`) under Sniper.
* Ocean and Barnes have no self-test and print no numeric results. Their output under Sniper matches a native run
  except for timings and front-end messages.
* **Ocean's log:** after the ROI and the program's own output, the three worker threads' SIFT streams report
  "channel to the simulator lost" while the simulator shuts down. This happens after the measured region, the run
  exits 0 and the statistics are complete, so it is noted, not investigated.

**Qualification by program:**
* **Ocean** is the only memory-bound workload.
  * L2 MPKI is 22, and 73% of L2 hits come from the four least-recent ways.
  * Its about 16 MB footprint (the memory manager counts about 62,000 distinct lines per core) also overflows the
    8 MB L3, causing 1.2 M DRAM write-backs.
  * It has heavy coherence traffic: 134,000 invalidations and 413,000 downgrades (nearest-neighbour exchange
    between partitions).
  * The sharing feature is still almost constant (1.2% heavy hints): its sharing is between pairs of cores, not
    many.
* **Barnes** is the only workload whose features vary.
  * 54% of victims are S, and 10.7% have a heavy delivered hint: the tree is read by every core.
  * L2 pressure is low: a 7% miss rate, with hits concentrated near MRU.
  * So it can show whether the policies learn from sharing, but not a performance difference.
* **Radix and LU:** both have low L2 MPKI (2.6, 1.4) and nearly constant features.
  * LU is compute-bound (IPC 12.8 over 4 cores).
  * Radix at `small` fits the L3, and its L2 hits are mostly near MRU.
  * They are a cheap sanity workload (Radix: 170 s) rather than a discriminator.

## Recommendations

**Next workloads:** Ocean and Barnes, in that order.
* **Ocean `small`:** the only workload where L2 replacement can change performance.
  * It runs at the L2 capacity edge and is memory-bound.
  * It has DRAM write-backs and the most coherence traffic.
* **Barnes:** the only workload where the coherence features are informative (S-heavy victims, heavy hints). It
  tests whether the policies use them, even though its performance is insensitive to the L2.
* **Radix:** can be added as a cheap third row (about 3 min per run), but is unlikely to separate the policies.
* **LU:** behaves like FFT, compute-bound with constant features, and adds little.

**Next step:** a 20-run five-policy pilot at 8 MB (Ocean `small` and Barnes 4,096 × 5 policies × 2 repetitions),
before any 1 MB L3 runs.
* **Why not 1 MB first:**
  * Ocean already overflows the 8 MB L3, so a smaller L3 is not needed to create pressure.
  * The L3 is inclusive, so at 1 MB it would hold no more than the four 256 kB L2s together. Its evictions
    would then back-invalidate L2 lines constantly: Ocean has 327 L2 back-invalidations at 8 MB despite 1.4 M L3
    evictions. That measures an L3 effect more than the L2 policy.
* **Then at 1 MB:** one LRU qualification run per workload (2 runs) can follow.
* **Expected cost**, measured LRU times with diagnostics adding a little for the RL policies:
  * Ocean: about 1,450 s per run.
  * Barnes: about 1,100 s per run.
  * At 8 in parallel (the host has 20 hardware threads, each simulation uses about 2.3), 20 runs take about
    1-1.5 h.
* **Five repetitions:** only after that pilot, for the configurations whose difference from LRU is near the
  measured noise.
* **Not before Ocean/Barnes:** any other SPLASH-2 or PARSEC programs.
  * **Other SPLASH-2:** Cholesky, FMM, Water and the non-contiguous variants build the same way (minutes).
  * **PARSEC 2.1:** canneal, streamcluster and others need the repository's PARSEC download (several GB) and
    `parsecmgmt`, which is also Python 2 driven.
