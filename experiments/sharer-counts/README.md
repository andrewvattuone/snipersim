# Sharer-count experiments

Tools to validate the sharer-count metadata that the coherence-aware RL replacement
policy (senior-design-rl branch) will use, on the 4-core Gainestown configuration:
one private L1-I/L1-D and L2 per core under one shared, inclusive L3.

The simulator side is in `common/core/memory_subsystem`:

| Signal | Where | Realistic? |
|---|---|---|
| L3 presence bits (`m_cached_locs`, one bit per L2 above) | `cache/shared_cache_block_info.h`, kept by `notifyPrevLevelInsert/Evict` | Yes: core-valid bits, local to the L3 |
| L2 delivered sharer hint (`CacheBlockInfo::m_sharer_hint`) | set in `CacheCntlr::copyDataFromNextLevel()` | Yes: reported by the L3 with the fill |
| `oracle-*` counts | `CacheCntlr::queryOracleSharers()` | No: direct reads of other caches, diagnostics only |

Nothing here feeds a replacement decision; LRU is unchanged. The directory also holds a
deterministic coherence regression test and notes on Sniper's tracing and checking hooks.

## Files

| File | Purpose |
|---|---|
| `sharetest.c` | 4-thread sharing microbenchmark, modes `r`, `w`, `p`, `u` (see the header comment) |
| `coherence_regress.c` | 2-thread deterministic coherence test: read, read, write, read on clean or dirty lines |
| `Makefile` | builds both (native, `-no-pie` so arrays can be traced; binaries are git-ignored) |
| `run_validation.sh` | runs the suite into `results/<prefix>-*` and checks every run |
| `check_sharers.py` | checks the invariants in one or more `sim.stats` files and summarizes hint accuracy |
| `run_regress.sh` | runs `coherence_regress` (write vs. read control, clean and dirty, plus a traced 1-line run) |
| `check_regress.py` | checks a write/read pair against MESI: invalidations, coherence misses, downgrades |
| `compare_coherence.py` | before/after table of time, IPC, misses, coherence events and victim states |
| `compare_stats.py` | diffs two runs while ignoring the diagnostic stats |
| `timing.sh` | interleaved host-time comparison of several saved Sniper binaries |

## Usage

From the repository root, after building Sniper:

```
make -C experiments/sharer-counts
experiments/sharer-counts/run_validation.sh hint                  # ~10 min with JOBS=6
experiments/sharer-counts/run_validation.sh --long hint           # + FFT -m 16 runs, ~25 min more
MODES=u FFT=0 experiments/sharer-counts/run_validation.sh hint    # one mode only
experiments/sharer-counts/check_sharers.py results/hint-l3-1024k-r-K4
```

One run by hand (a 512 KB L3 makes the L3 evict shared lines while the L2s still hold them):

```
./run-sniper -n 4 -c gainestown -g perf_model/l3_cache/cache_size=512 -d results/x -- experiments/sharer-counts/sharetest w 4
```

Host timing (copy `build/default/lib/sniper` aside after each build first; the
script restores the original binary when it exits):

```
experiments/sharer-counts/timing.sh 5 /tmp/t sniper.before sniper.after
```

Coherence regression test and tracing:

```
experiments/sharer-counts/run_regress.sh regress -n 2 -c gainestown
SNIPER_COHERENCE_CHECK=1 ./run-sniper ...     # counts write hits that MESI forbids
SNIPER_TRACE_VADDR=0x404100 ./run-sniper ...  # [LINETRACE] of one line (address from nm)
```

`results/` is git-ignored; keep simulation output out of the repository.

## Statistics

Per cache component and core (`L1-D`, `L2`, `L3`, ...), recorded when LRU picks a victim:

* `lru-victim-{M,E,S,other}`: MESI state of the victim.
* `oracle-victim-dir-sharers-{0..4plus}`, `-dir-missing`, `-dir-unavailable`: LLC instances the
  home directory lists. With one shared L3 this is always 1.
* `oracle-victim-upper-sharers-{0..4plus}`, `-upper-unavailable`: distinct cores whose caches
  directly above a shared cache hold the line (probe).
* `cachedlocs-victim-*`: the cache's own presence bitmap against that probe:
  `compared`, `match`, `mismatch`, `stale` (bit set, line absent), `missing` (line present, bit
  clear), `unavailable`, and the per-core count (`sharers-*`, `count-match`, `count-mismatch`).
  The first 20 mismatches are also printed as `[CACHEDLOCS MISMATCH]`.
* `hint-victim-*` (L2): the delivered hint against the oracle count of distinct cores, this one
  included, that hold the line now: `compared`, `missing` (no hint on the block), `unavailable`
  (cache gets no hints), `exact`, `over` (stale-high), `under` (stale-low), `abs-error` (sum of
  |hint - oracle|), `delivered-*` and `oracle-*` histograms, and the heavy-sharing classification
  `heavy-{both,neither,delivered-only,oracle-only}` with heavy iff count > num_cores / 2
  (`delivered-only` / `oracle-only` = ChampSim's `delivered_above_oracle_below` /
  `delivered_below_oracle_above`).

Per L2 controller, at fill time: `sharer-hint-fills`, `-fills-write`, `-fill-{1,2,3,4plus}`,
`-write-with-sharers` (must be 0), `-exclusive-with-sharers` (read fill granted E/M while other
cores hold the line, see below), `-write-hit-resets` (write hit on M that lowered a hint > 1).

With `SNIPER_COHERENCE_CHECK=1`, per private cache: `coherence-check-stale-write-hits`, write
hits while another core's private copy exists below the same shared cache (must be 0 under MESI;
see the known limitation below).

`check_sharers.py` fails a run on any broken invariant, any presence-bitmap mismatch, a
nonzero `write-with-sharers` or a nonzero `coherence-check-stale-write-hits`. An inexact hint is
expected and only reported.

## Delivered hint semantics

Same as ChampSim's `delivered_sharers` (`src/cache.cc`, `replacement/rl_mesi/rl_core.h`):

* On a fill, the L3 reports how many cores other than the requester its presence bits list; the
  hint is that number + 1, so it includes the requester and is at least 1. The requester's bit is
  never set at that moment (the L2 drops its copy before forwarding a miss and is registered at
  the L3 only by the insert), and it is excluded by core id regardless.
* Write fills, and upgrades (in Sniper an upgrade invalidates the L2 copy and refills it), give 1:
  the L3 has invalidated every other copy before it responds.
* A write hit on a line the L2 holds in M sets the hint to 1 (ChampSim's
  `coherence_set_modified`).
* Invalidation, back-invalidation, eviction and slot reuse reset it to 0 (unknown); downgrades
  leave it unchanged.
* Cores that join or leave later are not seen, so the hint goes stale in both directions.

No latency is charged: the hint is a few bits on the fill response that is already modeled. It
needs the L3 presence bits, so it exists only with `ENABLE_TRACK_SHARING_PREVCACHES`, and only
for a cache whose next level is shared (`shared_cores > 1`); L1 caches get no hint yet. The
per-core reading of the bits assumes one private L2 per core under the L3 (at most 8).

## Results (2026-11-19, 4-core Gainestown)

Every run passes `check_sharers.py`: the presence bitmap matched the probe at every victim, and
no write fill delivered a hint with other sharers listed.

| Workload | L2 victims compared | Hint exact | MAE | Heavy class differs |
|---|---|---|---|---|
| FFT -m 16, 8 MB L3 | 437,975 | 94.4% | 0.061 | 0.45% |
| FFT -m 16, 512 KB L3 | 62,615 | 99.8% | 0.002 | 0.04% |
| sharetest r, K = 1..4, 1 MB L3 | 21-36k per run | 99.5 / 92.1 / 90.2 / 81.4% | <= 0.25 | up to 13% (K = 4) |
| sharetest w, K = 1..4, 1 MB L3 | 15-38k per run | 99.6 / 95.4 / 94.6 / 82.3% | <= 0.37 | up to 15% (K = 4) |
| sharetest p, K = 1..4, 1 MB L3 | 16-26k per run | >= 99.3% | <= 0.011 | < 0.5% |
| sharetest u, K = 1..4, 1 MB L3 | 17-34k per run | 99.7 / 92.4 / 88.6 / 61.6% | <= 0.66 | up to 27% (K = 4) |
| all modes, 512 KB L3 | 2.5-3.8k per run | >= 99.3% | <= 0.016 | < 0.6% |

With a 512 KB L3 most lines leave the L2 through L3 back-invalidation, so few L2 victims remain
and they are mostly private. Read sharing makes the hint stale-low for early readers (later
readers join unseen) and stale-high for later ones (earlier holders leave unseen).

## Known limitation: L2 MESI state with a shared L3

Sniper copies the L3's state into the L2 on a fill. The L3's state describes the L3 against the
directory and is nearly always E or M, so a read fill can arrive in E/M while other cores hold
the line in S (`sharer-hint-exclusive-with-sharers`, about 2,050 per extra reader in `r`). A
later write by that core is then a silent hit and the other copies are not invalidated: mode `u`
shows `write-hit-resets` = 2016 and almost no `coherency-invalidates` at the other cores. The
hint follows the protocol (1 after the write), the oracle sees the stale copies, which is most of
the `u` error. The L2 MESI state and the coherence events in this configuration are therefore not
those of a MESI protocol. A possible fix is to grant S on a read fill when the L3's presence bits
list another core; it changes the simulated protocol and has not been made. `run_regress.sh`
reproduces the failure deterministically: on dirty lines the write leaves core 0's copies valid.
