# Stage 6 pilots: initial performance evaluation

Pilot experiments for Stage 6 of [`ALGORITHM_AUDIT.md`](ALGORITHM_AUDIT.md) (Section 9, "Experiments A"), on
`feature/rl-port` (`9bcf5e45`: the RL policies plus the trace front-end fix `fa8d21b6`). They are bounded pilots for
review, not the benchmark campaign. The RL algorithms and their default parameters are unchanged.

**Status: pilot findings only.** Every policy result below, including Pilot 3 (Ocean/Barnes), comes from **two
repetitions** of one configuration (8 MB L3). The repetitions bound the run-to-run variation of the simulator, but two samples give no confidence
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
* **`run_stage6.py`** runs the matrix (workloads × L3 sizes × policies × repetitions).
  * **Timeouts:** each workload has its own per-run bound, about 2 × its measured wall time with 4-5 simulations
    in parallel. Ocean `small` gets 3,000 s (measured 1,443 s) and Barnes 4,096 gets 2,400 s (measured 1,103 s).
    `--timeout` overrides every bound. A timed-out run's whole session is killed, including SDE's `pind`.
  * **Checks:** as each simulation completes, `check_sharers.py` and `check_rl_events.py` run on it alone, plus
    `check_rl_policy.py` for the RL policies. A check passes only if the checker exits 0 and reports
    `RESULT PASS` and no `RESULT FAIL`. The output is appended to `checkers.txt`.
  * **Manifest:** `manifest.jsonl` has one line per run, written as it finishes. It records the exact command,
    stdin file, git commit, clean-tree flag, the simulator, workload and input checksums, the timeout, the exit
    status, the wall time, the verdict of each check, and the status.
  * **Status:** `ok` only if the run exited 0 in time, wrote statistics, logged no exception, entered and left
    the ROI (for `--roi` workloads) and passed every check. Anything else is `FAILED` or `FAILED (checks)`.
  * **No overwriting:** an existing run directory is never reused, overwritten or recorded again; it is skipped.
  * **Exit status:** 0 only if every requested run was simulated now and is `ok`, 1 if any failed, and 3 if
    some were skipped. With every run skipped it prints "NO NEW RESULTS", so an empty batch cannot pass for a
    completed one.
* **`summarize_stage6.py`** computes per-run metrics (`metrics.csv`), the mean and spread per configuration, the
  change against LRU, and the RL behaviour.
  * Its extraction was checked against an independent recomputation from the raw `sim.stats` (IPC, L2 MPKI, L2
    misses, invalidations, decisions).
  * Metrics cover all cores, over the whole run, or over the region of interest for `--roi` workloads (then
    `sim.stats` is exactly the `roi-begin`..`roi-end` delta). IPC is the sum of instructions divided by the global
    cycle count, i.e. aggregate over the 4 cores.
  * **Runs it uses:** only `ok` runs with a recorded passing verdict for every applicable check. The verdicts
    come from the manifest, or for the three earlier result sets, written before per-run verdicts, from the
    per-run `RESULT` lines of their `checkers.txt`. Failed, unchecked and statistics-less runs are listed and not
    used.
  * **Consistency checks**, each an error:
    * the runs come from more than one commit or simulator binary;
    * a run was made with uncommitted simulator changes;
    * within a workload and L3 size, a `sim.cfg` differs beyond the policy keys, or the workload command, binary
      or input differs.
  * **Exit status:** 0 only for a valid comparison. Otherwise 1 (runs not used) plus 2 (consistency errors), with
    an `ERROR` line per problem and a final "NOT A VALID COMPARISON". The tables are still printed, for
    diagnosis.
* **Provenance:** the manifest's `commit` and `binary_md5` identify the simulator. The `git_revision` in each
  run's `sim.info` is not reliable: it is a CMake configure-time label (`build/default/config/sniper.py`), still
  `12eda347` from Stage 4 although the binary was rebuilt from `9bcf5e45`.

Results are in the git-ignored `results/stage6-pilot/`, `results/stage6-fft-pilot/`, `results/stage6-splash2-qual/`
and `results/stage6-ocean-barnes-pilot/`. Each has a `PROVENANCE.txt` and is read-only. Archives are in
`results/archive/`, with the SHA-256 of each in the `.sha256` file beside it:
* `stage6-pilots-20261121.tar.gz`, for the first two;
* `stage6-splash2-qual-20261121.tar.gz`;
* `stage6-ocean-barnes-pilot-20261121.tar.gz`.

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

**Build** (`experiments/rl/build_splash2.sh`):
* **Revision:** without an existing checkout it clones the repository and checks out the pinned revision. It refuses
  an existing checkout at another revision, or with modified tracked files, instead of changing it.
* **Verification:** a fresh GitHub clone rebuilt with it gives programs identical in machine code and read-only
  data to those used here; only the debug info differs, because it records the build path. The Sniper tree is
  unchanged by a build.
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

## Pilot 3: Ocean `small` and Barnes 4,096 (20 runs)

**Every policy comparison in this section is a preliminary two-repetition result.** The figures are means of two
runs. "Spread" is (max − min) / mean over the two runs. Even a difference larger than the spread is not a
statistically established effect.

**Method.** The five policies above × `ocean-small` and `barnes-4096` (both `--roi`) × 2 repetitions, 8 MB L3,
with the fixed configuration of the Method section. Five simulations ran in parallel, with timeouts of 3,000 s
(Ocean) and 2,400 s (Barnes).
* **Command:** `run_stage6.py results/stage6-ocean-barnes-pilot --workloads ocean-small,barnes-4096 --policies
  lru,sf-text,sf-example,rl-n2,rl-n4 --reps 2 --jobs 5`.
* **When:** 2026-11-21, 14:20-15:59 (1 h 39 min).
* **Simulator:** `feature/rl-port` at `7a871fd7`, with a clean tree. This is the tooling commit; the simulator
  sources are those of `9bcf5e45`, and the binary md5 is `192619ee…`, the same as in the earlier pilots.
* **Analysis:** metrics from `summarize_stage6.py` (exit 0: the 20 runs share one commit and one binary, and
  within each workload their `sim.cfg` differ only in the policy keys). The additional diagnostics below come
  from the same `sim.stats` and from the RL weight snapshots in each run's `telemetry/` directory.

**Completion and correctness:**
* **Runs:** all 20 are `ok`: exit 0, no timeout, no crash, models enabled and disabled once at the ROI markers.
* **Checks:** all 56 applicable checker verdicts are `PASS`. There are 0 stale write hits and no-eligible-victim
  is 0.
* **Wall time per run:** Ocean 1,475-1,624 s and Barnes 1,291-1,481 s.
* **Shutdown message:** 7 of the 20 runs, LRU among them, log "channel to the simulator lost" from worker-thread
  SIFT streams. It appears after the ROI and after the program's own final output, as in the qualification run.

**Results** (ROI, all 4 cores; change against LRU; spread in parentheses):

| Ocean | IPC | L2 MPKI | L2 dirty evictions | L2 coherence invalidations | L2 downgrades | L3 misses | DRAM reads / writes |
|---|---|---|---|---|---|---|---|
| LRU | 2.363 (0.20%) | 21.82 (0.10%) | 4.74 M | 133 k | 409 k | 1.875 M | 1.555 M / 1.227 M |
| sf text | +0.22% | +11.4% | +15.5% | −2.1% | −11.5% | +0.07% | −0.14% / −0.09% |
| sf example | 0.00% | +17.9% | +22.0% | −5.4% | +3.4% | −0.14% | −0.43% / +0.02% |
| rl N=2 | −0.03% | −0.70% | −1.0% | −1.2% | −0.6% | +0.13% | +0.09% / −0.01% |
| rl N=4 | +0.04% | −0.85% | −1.4% | −2.1% | +2.5% | −0.01% | −0.19% / +0.12% |

| Barnes | IPC | L2 MPKI | L2 dirty evictions | L2 coherence invalidations | L2 downgrades | L3 misses | DRAM reads / writes |
|---|---|---|---|---|---|---|---|
| LRU | 5.088 (0.06%) | 0.525 (1.1%) | 38.9 k | 48.2 k | 111 k | 9.9 k | 8.4 k / 0 |
| sf text | −0.10% | +4.0% | +18.9% | −3.8% | −0.6% | −0.3% | −0.1% / 0 |
| sf example | −0.06% | +5.6% | +26.5% | −4.4% | +1.0% | −0.7% | −0.2% / 0 |
| rl N=2 | +0.02% | −0.13% | −2.1% | +0.2% | +0.5% | −0.9% | −0.7% / 0 |
| rl N=4 | −0.07% | +1.0% | −2.1% | +1.9% | +2.2% | −0.6% | −0.8% / 0 |

**Per-repetition values:**

| | Ocean IPC | Ocean L2 MPKI | Barnes IPC |
|---|---|---|---|
| LRU | 2.3608 / 2.3656 | 21.81 / 21.83 | 5.0861 / 5.0890 |
| sf text | 2.3677 / 2.3690 | 24.32 / 24.30 | 5.0790 / 5.0864 |
| sf example | 2.3620 / 2.3645 | 25.69 / 25.78 | 5.0857 / 5.0837 |
| rl N=2 | 2.3624 / 2.3623 | 21.70 / 21.63 | 5.0924 / 5.0846 |
| rl N=4 | 2.3633 / 2.3651 | 21.67 / 21.60 | 5.0905 / 5.0771 |

**Other measurements:**
* **Simulated time** mirrors IPC: same magnitude, opposite sign.
* **L1-D MPKI:** unchanged in Ocean, 40.5 for every policy. In Barnes it is 1.3-3.7% lower than LRU under every
  policy. The L1-D always uses LRU, and LRU's own two runs differ by 1.9% (7.71 / 7.56), so this is not analysed
  further.
* **Run-to-run variation:** the simulator is not deterministic between runs, LRU included. IPC spreads are
  0.01-0.26%, and L2 miss spreads 0.1-2%.

**What the comparisons show**, with two repetitions each:
* **IPC:**
  * No policy is separated from LRU on Barnes; every difference is within ±0.1%.
  * On Ocean, both `sf-text` runs exceed both LRU runs (+0.22%), but LRU's own two runs differ by 0.20%.
* **L2 misses:**
  * `rl_lru` is lower than LRU on Ocean in both repetitions (−0.70% for N=2, −0.85% for N=4), with no IPC change.
  * `sf_lru` raises L2 misses on both workloads: +11-18% on Ocean, +4-6% on Barnes.
* **IPC barely follows the L2.** On Ocean, `sf-example`'s 18% more L2 misses leave IPC unchanged. The extra misses
  are L3 hits: L3 loads rise by 1.06 M while L3 misses fall 0.14%. Why their latency does not show in IPC has not
  been determined. Find out before IPC is used as the main metric for Ocean.

**L3 and DRAM, corrected interpretation.** A first reading of these results said the 8 MB L3 holds both working
sets. That is wrong for Ocean, as the qualification section already showed.
* **Ocean overflows the L3.** With LRU in the ROI:
  * L3: 5.95 M accesses and 1.875 M misses (32%; 0.54 M load misses and 1.33 M store misses).
  * L3 evictions: 1.42 M (1.23 M modified and 0.20 M exclusive).
  * DRAM: 1.555 M reads and 1.228 M writes. The writes equal the L3's modified evictions.
  * DRAM reads are 0.32 M fewer than L3 misses; that difference was not analysed.
* **Correct statement:** the L3 and DRAM traffic is large, but almost the same under every L2 policy (within
  ±0.5%). The policy changes which blocks miss in the L2 and how many L2-L3 transfers occur, but not which blocks
  the L3 holds and evicts.
* **Barnes does fit:** 9.9 k L3 misses (cold), no L3 evictions and no DRAM writes in the ROI.

**What the policies see:**
* **Victim states and sharer hints:**
  * **Ocean:** victims are 5% S, 9% E and 86% M. 97% have a delivered sharer hint of 1 and about 1% a heavy hint
    (> 2 of 4 cores). The delivered hint equals the oracle count for 98% of victims.
  * **Barnes:** victims are 54% S, 1% E and 45% M.
    * Delivered hints: 66% 1, 22% 2, 7% 3 and 4% 4+. 10.8% are heavy under LRU (8-12% across policies).
    * The delivered hint equals the oracle count for 66-71% of victims; otherwise it is lower.
  * **Policy shifts:** the policies change these mixes by only a few points.
* **Decisions:**

  | | Ocean decisions | Ocean differs from LRU | Barnes decisions | Barnes differs from LRU |
  |---|---|---|---|---|
  | sf text | 6.17 M | 84% | 97 k | 85% |
  | sf example | 6.56 M | 100% | 101 k | 99% |
  | rl N=2 | 5.47 M | 14% | 86 k | 29% |
  | rl N=4 | 5.46 M | 17% | 86 k | 45% |

  * **Barnes is thin:** about 21,000 decisions per core over 512 sets, about 42 per set over the whole ROI.
* **Feature variation:** the share of decisions in which every candidate has the same score.
  * **Ocean:** 80% for `rl-n2` and 71% for `rl-n4`.
  * **Barnes:** 47% (N=2) and 20% (N=4).
  * With equal scores, `rl_lru` keeps the base choice, the LRU block.
* **Protection outcomes:**
  * **Wasted protections**, as a share of resolved protect rewards:
    * `rl-n2`: 85% on Ocean and 74% on Barnes;
    * `rl-n4`: 80% on Ocean and 63% on Barnes.
  * **Contradicted wasted protections:** 40-42% (Ocean) and 70-73% (Barnes) of the protect-wasted rewards are
    contradicted later by the block's return (I-6).
  * **`sf_lru` protection runs:**
    * Runs ending with the protected block invalidated: 20% (text) and 35% (example) on Ocean, 16% and 34% on
      Barnes, against 72% in FFT.
    * Runs of 5 or more consecutive rejections: 45% and 82% on Ocean, 19% and 35% on Barnes.
  * **`rl_lru` pass runs:** up to 100 consecutive passes (Ocean, N=2), but only 1-6% of runs reach 5 passes.

**Reward denominators** (repetition 1 of `rl-n2`, summed over the four L2s, ROI deltas; repetition 2 and N=4 are
within a few percent). Each L2 keeps a 16-entry eviction history per set.
* **Bad eviction (−1):** a demand miss on a block whose entry is still in its set's history.
* **Good eviction (+1):** an entry pushed out of a full history (aged out) without returning.
* **Upgrade refills** are not policy misses. Neither are invalidations, which create no history entry.

| | Barnes | Ocean |
|---|---|---|
| Replacement evictions = history inserts = decisions | 85,055 | 5,475,890 |
| Bad evictions (returned while in the history) | 67,087 (78.9% of inserts) | 2,635,893 (48.1%) |
| Good evictions (aged out) | 1,612 (1.9% of inserts) | 2,807,229 (51.3%) |
| Inserts − bad − good (unresolved at the ROI end, net of entries carried in from the warm-up) | 16,356 (19.2%) | 32,768 (0.6%) |
| Policy demand misses (L2 misses minus upgrade refills) | 147,674 | 5,659,870 |
| of which in the history (= bad evictions) | 45.4% | 46.6% |
| of which replaced and no longer in the history (aged out, then reused) | 112 (0.1%) | 2,609,198 (46.1%) |
| of which cold / coherence-invalidated | 24.5% / 30.0% | 4.5% / 2.9% |

* **Barnes's "98% bad"** is 67,087 / (67,087 + 1,612). Its denominator is the resolved eviction rewards only,
  not all evictions or all rewards.
  * As a share of all evictions, bad is 79%: the same event as the "79% eviction-history reuse", whose
    denominator is history inserts.
  * As a share of all 85,197 rewards (with 4,270 protect-good and 12,228 protect-wasted), it is 78.7%.
* **Why good rewards are rare in Barnes.** A good reward needs a set to receive 16 more evictions before the
  block returns. With about 42 evictions per set in the whole ROI, histories seldom fill, and almost every evicted
  block (all but 112) that returns does so within the history. Nearly every possible victim earns −1, so the
  reward says little about which candidate was the better choice.
* **Ocean's good rewards are mostly contradicted later.** 2.61 M misses are on blocks that had already aged out
  of the history. That is 93% of the 2.81 M good rewards; the two counts do not pair one-to-one at the ROI
  boundaries. Ocean's reuse distance exceeds the 16-entry history, so "aged out" usually does not mean "not
  needed".

**Weights** (`rl_lru`, one weight vector per L2: `weight_scope=global` shares it across sets, not cores; snapshot
every 1,000 decisions). Five weights are learned: S, E, M, below threshold (BT) and above threshold (AT).
* **Ocean** has about 1,360 snapshots per core.
  * **They change often.** Each weight changes sign on 14-23% of snapshot intervals, i.e. about every 5-7
    snapshots, and moves over most of [−1, 1].
  * **But their averages repeat.** The second-half means are positive and agree across the 4 cores and both
    repetitions: wE +0.25-0.37, wS +0.13-0.26, and wM, wBT, wAT +0.08-0.20. E and S blocks are therefore, on
    average, the preferred victims over M.
  * So the weights are not a random walk. They fluctuate strongly around a reproducible tendency.
* **Barnes** has only 16-27 snapshots per core.
  * Weights are mostly negative: positive in 6-44% of snapshots, second-half means −0.25 to −0.81.
  * Core 0's wE is positive (+0.55-0.75) in all four runs.
* **Not shown:** whether these weights, or their updating, choose better victims than a fixed rule.

**Effects of the selection rule versus effects of learning:**
* **`sf_lru` (both rules)** has no learned parameters. Each decision is a fixed rule applied to the history's
  class counts (text) or to the block's own class (example). All its effects are therefore effects of its
  victim-selection rule:
  * more L2 misses and dirty evictions;
  * fewer coherence invalidations;
  * `sf-text`'s 11.5% fewer Ocean downgrades with +0.22% IPC. The plausible mechanism is that it evicts M blocks
    earlier, so their write-back precedes the neighbour's read. Not verified.
* **`rl_lru`'s** non-LRU choices (14-45% of decisions) come from its weights: with the initial all-zero weights it
  is LRU. Its effects are small: −0.7 to −0.85% L2 misses on Ocean, nothing separable on Barnes.
* **No result here is demonstrably caused by learning.** The difference from LRU could come from a fixed priority
  within the N-candidate window. It could also come from any non-LRU perturbation.
* **The controls needed are not yet run, and none exists as a configuration option today.** `rl/alpha=0` with the
  zero initial weights is exactly LRU, so it is not a no-learning control. Candidate controls:
  1. frozen weights, for example the second-half means above, with no updates;
  2. a fixed state or sharer priority within the same window;
  3. a random choice within the window at the same rate of differing from LRU.

  Each needs a small, separately reviewed simulator option; this would not change the RL algorithm itself.
* **Frequent sign changes and wasted or contradicted protections** are evidence of possible weaknesses in the
  reward and feature design, not proof of them.

**Limitations:**
* two repetitions;
* one L3 size;
* 4 cores, against the paper's 16;
* policy on the L2 only;
* a real (not perfect) L3;
* a single input per program;
* Barnes has too few decisions per set to train or judge the weights.

**Results:**
* **Run directories:** `results/stage6-ocean-barnes-pilot/` (git-ignored, read-only). It holds `manifest.jsonl`,
  `checkers.txt`, `metrics.csv` and `PROVENANCE.txt`.
* **Batch files:** `results/stage6-ocean-barnes-pilot.progress.log` and `.command`. The command file holds the HEAD
  commit and the binary, workload and input md5s.
* **Archive:** `results/archive/stage6-ocean-barnes-pilot-20261121.tar.gz`, with its SHA-256 in the `.sha256`
  beside it.

## Recommendations (written before Pilot 3)

This section records the plan made after the qualification runs; Pilot 3 carried out its "next step". What
follows Pilot 3 is in "After Pilot 3".

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
  * Ocean: about 1,450 s per run, against a 3,000 s timeout.
  * Barnes: about 1,100 s per run, against a 2,400 s timeout.
  * At 8 in parallel (the host has 20 hardware threads, each simulation uses about 2.3), 20 runs take about
    1-1.5 h.
* **Five repetitions:** only after that pilot, for the configurations whose difference from LRU is near the
  measured noise.
* **Not before Ocean/Barnes:** any other SPLASH-2 or PARSEC programs.
  * **Other SPLASH-2:** Cholesky, FMM, Water and the non-contiguous variants build the same way (minutes).
  * **PARSEC 2.1:** canneal, streamcluster and others need the repository's PARSEC download (several GB) and
    `parsecmgmt`, which is also Python 2 driven.

## After Pilot 3

* **Next Stage 6 priority:** reproducing the paper's CAP Bench workloads (FAST, FN, GF, IS, KM, LU, TSP), before
  Stage 7 or any change to the RL algorithm. Setup, tooling review, qualification and reproduction limits:
  [`CAPBENCH.md`](CAPBENCH.md).
* **Not planned:** more repetitions of the Ocean/Barnes matrix. The differences are at the noise level and
  are limited by what the workloads expose, not by the number of samples.
* **Needed before any claim about learning:** the no-learning controls listed in Pilot 3. They are now implemented
  for review, and the CAP Bench LU `tiny` pilot that motivated them is in [`CAPBENCH.md`](CAPBENCH.md).
