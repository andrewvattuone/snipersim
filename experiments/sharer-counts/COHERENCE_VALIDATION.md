# MESI fill-state fix: validation

Sniper granted a private cache the shared cache's own E/M state on a read fill,
even while other cores held the line. A later write by that core then hit
silently and never invalidated the other copies. This file documents the
failure, the root cause, the fix and its validation (2026-11-19, branch
`senior-design-rl`).

## Commits

| Commit | Content |
|---|---|
| `7500506e` | upstream Sniper 9.0 |
| `9cc4fdb8` | SDE archive extraction fix (no simulator change) |
| `aedc1882` | MESI victim-state stats in LRU (stats only; protocol code identical to `7500506e`) |
| `6b5cbd68` | sharer-count instrumentation, `m_cached_locs` validation (HEAD at the time of writing) |
| uncommitted | private-L2 sharer hint, `experiments/sharer-counts`, line tracer and coherence check, **the fix** |

The bug is present in `aedc1882` and `6b5cbd68` (tested with saved builds of both)
and so in upstream Sniper 9.0: `cache_cntlr.cc` there has the same protocol code.
Suggested commit split for review: (1) sharer hint + experiments, (2) diagnostics
(line tracer, `SNIPER_COHERENCE_CHECK`, regression test), (3) the fix alone
(`cache_cntlr.{h,cc}`, about 30 lines; `otherUpperCacheHolds()` and the block
marked "MESI fill fix" in `copyDataFromNextLevel()`).

## Original failure

`coherence_regress dirty write`: core 0 writes a line (so the L3 copy is M), core 0
reads it, core 1 reads it, core 1 writes it, core 0 reads it again. Trace before
the fix (2-core Gainestown, `SNIPER_TRACE_VADDR`):

```
step 2  c1 READ   L3 hit (M): c0 L1-D M->S, c0 L2 M->S              correct
                  c1 L2 fill: copies the L3's state M -> INSERT M     wrong: c0 still holds S
                  c1 L1-D copies M from its L2
step 3  c1 WRITE  hits its own L1-D in M: no coherence action        missed invalidation of c0
step 4  c0 READ   hits its stale S copy                              missed coherence miss
```

On a clean line (L3 copy E) the reader got E next to S copies; the write still went
to the L3 (only M is writable in Sniper) and invalidated correctly, but the re-read
then received M, setting up the same silent write.

Affected: every hierarchy with private caches above a shared cache. Tested failing:
Gainestown (shared L3, 2 and 4 cores) and silvermont (L2 shared by 2 cores; the L1-D
inherits the L2's state). Configs with the same structure: beckton, dunnington,
dunnington16, cortex-a53/a72, riscv (not run; the ARM/RISC-V ones cannot run the
x86 test). Not affected: hierarchies whose last private level is the LLC (private L3
per core, or two levels with a private L2): their states come from the directory.

Application data was never wrong: Sniper's caches hold no data (data arrays exist
only with fault injection) and values come from the frontend's functional execution.
Wrong were the private-cache MESI states, the coherence events (invalidations,
coherence misses, downgrades) and the timing that depends on them.

## Root cause

`CacheCntlr::copyDataFromNextLevel()` set the filled line's state to
`m_next_cache_cntlr->getCacheState(address)`. For a shared cache that state describes
the cache towards the directory (E after a clean DRAM fill, M once any core wrote
the line), not the permission of one core. The shared cache's read-hit paths
("reading MODIFIED data" / "reading EXCLUSIVE data") correctly downgrade every other
holder to S, but nothing adjusted what the requester received. L1-D fills from the
private L2 copy that state again.

## The fix

In `copyDataFromNextLevel()`, for a READ fill from a cache shared by several cores
whose state is E or M: if any other cache directly above that shared cache holds the
line, grant S. Otherwise keep the shared cache's state.

```cpp
const CacheState::cstate_t next_cstate = cstate;
if (mem_op_type == Core::READ && m_next_cache_cntlr->m_shared_cores > 1
    && (cstate == CacheState::EXCLUSIVE || cstate == CacheState::MODIFIED)
    && m_next_cache_cntlr->otherUpperCacheHolds(address, this))
   cstate = CacheState::SHARED;
```

`otherUpperCacheHolds()` probes the shared cache's list of upper (master) caches,
skipping the requester's master. It stands in for the snoop responses the shared
cache has just collected while downgrading those holders (or its presence bits), so
it does not depend on `ENABLE_TRACK_SHARING_PREVCACHES` and adds no latency: the
snoop is already modeled by the read-hit path.

Why this is enough, and what it deliberately does not change:

* Existing holders: already downgraded to S (writing back M data) by the shared
  cache before the fill; the fix only corrects the requester.
* L1-D: copies its private L2's state, which is now S; no separate change needed.
* Writes, upgrades, migration: unchanged. A write fill gets M after the shared cache
  invalidated the other copies. A write to an S copy takes the existing upgrade path
  (L2 invalidates its copy, the L3 write-hit or E-upgrade path invalidates the others).
* No other holders: unchanged. The reader keeps E, or M if the shared cache's copy is
  dirty. In Sniper only M is writable and a private M line must sit on an M line
  below it (the next level asserts this on writebacks), so granting E here would add
  an L3 round trip to the first write; M is kept to avoid unnecessary upgrades.
  Consequence: a private M means "exclusive, writable, and the line is dirty with
  respect to memory", not "written by this core" (see limitations).
* The shared cache's own state towards the directory is untouched.

## Validation

Commands (from the repository root; results go to the git-ignored `results/`):

```
make && make -C experiments/sharer-counts
export SNIPER_COHERENCE_CHECK=1          # count silent write hits that MESI forbids
E=experiments/sharer-counts
$E/run_regress.sh fix-g2 -n 2 -c gainestown
$E/run_regress.sh fix-g4 -n 4 -c gainestown
$E/run_regress.sh fix-silvermont -n 2 -c silvermont
$E/run_regress.sh fix-privl3 -n 2 -c gainestown -g perf_model/l3_cache/shared_cores=1
$E/run_regress.sh fix-2level -n 2 -c gainestown -g perf_model/cache/levels=2
JOBS=6 $E/run_validation.sh --long fixed
$E/compare_coherence.py results/hint-l3-1024k-u-K4 results/fixed-l3-1024k-u-K4
SNIPER_BIN=/path/to/saved/sniper $E/run_regress.sh old -n 2 -c gainestown   # another build
```

### Deterministic regression test

`run_regress.sh` runs `coherence_regress` with 256 lines with step 3 as write and as
read; `check_regress.py` requires the write to add >= 90% of 256 to core 0's copies
removed by coherence and step-4 misses (L1-D and private L2) and to core 1's
downgrades, plus a correct data check.

| Hierarchy | Before (clean / dirty) | After (clean / dirty) |
|---|---|---|
| Gainestown, 2 cores, shared L3 | pass / **fail** | pass / pass |
| Gainestown, 4 cores, shared L3 | pass / **fail** | pass / pass |
| silvermont, shared L2 (L1-D rows) | pass / **fail** | pass / pass |
| Gainestown, private L3 per core | pass / pass | pass / pass |
| Gainestown, 2 levels (private L2 is the LLC) | pass / pass | pass / pass |

"Before" was run with builds of `aedc1882`, `6b5cbd68` and the uncommitted tree
without the fix, with identical outcomes. The data check passed in every run. Dirty
write, Gainestown 2 cores, write minus read control: copies removed at core 0 +256 /
+256 (L1-D / L2), core 0 step-4 misses +260 / +255, core 1 downgrades +256, against
deltas of -10 to +2 before.

Traces after the fix (same steps): step 2 grants S (`l3 state M, granted S`); step 3
misses in core 1's L1-D, invalidates core 0's L1-D and L2 and fills M; step 4 misses,
downgrades core 1 to S and fills S. Corner cases traced on one `sharetest` line:
a solo reader keeps E; each further reader gets S and downgrades the holders; the
last reader's write invalidates all three other copies (u, K = 4); ownership
migration invalidates the previous M owner with a writeback (p, K = 2); an L3
eviction back-invalidates all four S copies and sends INV_REP to the directory
(r, K = 4, 512 KB L3).

### Coherence check (`SNIPER_COHERENCE_CHECK=1`)

Write hits in a private cache while another core's private copy exists below the same
shared cache (counted per write hit; oracle probe, no state or timing change):

| Workload | Before | After |
|---|---|---|
| `coherence_regress` dirty write, 256 lines | 278 (256 + ~20 from thread start-up) | 0 |
| `coherence_regress` clean write | 19 (thread start-up) | 0 |
| sharetest u, K = 4 | 2,016 (L2) + ~30 (L1-D) | 0 |
| sharetest r / w, K = 4 | ~5 per core (start-up) | 0 |
| FFT -m 10, 4 cores | 13,242 (13,146 L1-D + 96 L2; ~5% of the worker cores' stores) | 0 |
| FFT -m 16, 4 cores | 435 | 0 |
| all 35 runs of the validation suite | (not run with the check) | 0 |

The check cannot see hierarchies without a shared cache below the private ones
(private L3, two levels); there the directory keeps coherence and the regression
deltas above are the evidence.

### Sharer-count suite

`run_validation.sh --long fixed`: all 35 runs PASS. The L3 presence bitmap matched the
oracle at every victim (0 mismatches), no write fill delivered a hint with other
sharers listed, `sharer-hint-exclusive-with-sharers` = 0 and
`sharer-hint-write-hit-resets` = 0 in every run (a private M line can no longer
coexist with other copies). Hint accuracy is unchanged where the bug did not matter
and better where it distorted the oracle:

| Workload | Exact before -> after | MAE before -> after |
|---|---|---|
| FFT -m 16, 8 MB L3 | 94.4% -> 94.3% | 0.061 -> 0.061 |
| sharetest u, K = 2 | 92.4% -> 96.3% | 0.079 -> 0.041 |
| sharetest u, K = 4 | 61.6% -> 70.7% | 0.655 -> 0.395 |
| sharetest r, K = 4 | 81.4% -> 83.0% | 0.250 -> 0.228 |

### Before / after

"Before" are the runs of the hint build (same protocol code as the unfixed tree),
"after" the fixed build; one run each unless noted. Two runs of the same unfixed
build differ by up to 0.4% in simulated time and up to 22% in sharetest's L2 victim
M/E/S counts, which depend on how L2 evictions and L3 back-invalidations interleave;
FFT's miss and victim counts vary by under 0.6% and its invalidations by about 2%.

| Workload | Sim time | IPC | L2 misses | L2 invalidations* | L2 downgrades | L2 victims S |
|---|---|---|---|---|---|---|
| FFT -m 10 (3 runs each, means) | 365,100 -> 365,200 ns | 3.057 -> 3.055 | | | | |
| FFT -m 16, 8 MB L3 | +0.0% | -0.0% | +0.1% | 1,877 -> 1,982 | +0.9% | 14,090 -> 30,238 |
| FFT -m 16, 512 KB L3 | +0.2% | -0.2% | +0.1% | 408 -> 516 | +2.5% | 105 -> 187 |
| sharetest r, K = 4 | -0.0% | -0.1% | +0.1% | 34 -> 48 | +0.6% | noisy |
| sharetest u, K = 2 | +0.1% | -0.3% | +1.6% | 98 -> 2,148 | +76% | noisy |
| sharetest u, K = 4 | +0.3% | -0.5% | +3.0% | 200 -> 6,261 | +92% | noisy |
| `coherence_regress` dirty write | +0.5% | -0.2% | +12.2% | 18 -> 287 | +59% | |

\* coherency invalidates + writebacks (invalidation of M copies) + upgrades.

The added events are the coherence traffic MESI requires and Sniper had omitted: in
u (K = 4) the last reader's write now invalidates the other 3 x 2,048 copies, their
re-reads miss (+3% L2 misses) and downgrade the writer (+92% downgrades). FFT's
large phase shares little, so its timing does not move, but about 16,000 of its L2
victims that were labelled M or E while other cores held them are now S. On these
workloads the timing effect is within or close to noise; sharing-heavy kernels and
short runs (FFT -m 10 had ~5% of stores affected) can move more.

## Limitations and remaining issues

* **M is not "written by this core".** A read of a line that is dirty in the shared
  cache and held by no other core still receives M (see above). For an RL feature
  that follows ChampSim (M = this copy was written), use a per-block "written
  locally" bit set on write hits and write fills, or decide to grant E and make E
  writable at private levels (a protocol and timing change). Until then a private
  M means exclusive + dirty somewhere below.
* **E -> M costs an L3 round trip.** Only M is writable in Sniper, so the first write
  to an E line in L1/L2 goes to the shared cache (real MESI upgrades silently).
  Pre-existing; affects timing, not states.
* **`coherency-downgrades` counts no-op snoops.** A read hit on an E/M line in the
  shared cache sends a SHARED update to every upper cache; caches already in S count
  an S -> S "downgrade". Pre-existing; inflates the counter, changes no state.
* **Snoops are broadcast.** The shared cache updates every upper cache, not only the
  ones its presence bits list (TODO in `updateCacheBlock`); latency is the maximum.
* **Same-core L1-I copies count as other holders** at a shared L2 (silvermont): the
  L1-D then gets S and a write upgrades. Correct MESI; rare (code and data on one line).
* **Not tested:** configs other than those listed, more than one shared LLC with a
  directory between them (the fix applies within each shared cache), and the
  `perfect`, non-`coherent` and passthrough cache options.
* The probe in `otherUpperCacheHolds()` is functional (no modeled lookup); the
  matching snoop latency is charged by the shared cache's read-hit path.
