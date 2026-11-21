# Stage 5 segmentation faults: investigation and fix

Investigation of the four SIGSEGVs reported in [`STAGE5_VALIDATION.md`](STAGE5_VALIDATION.md) ("Crashes"). Both
crashes are pre-existing races in Sniper's trace front end, not in the RL code. They are fixed by commit
`fa8d21b6` ("Fix trace-monitor teardown and trace-thread registry races"), on `feature/rl-port`. Nothing here
touches MESI, RL decisions or reward logic.

## Summary

| Crash | Mechanism | Introduced by the RL work? | Status |
|---|---|---|---|
| 3 × teardown, `rl_lru` + diagnostics, address 0x120 | `TraceManager::Monitor` thread reads `Sim()` after `Simulator::release()` set it to NULL | No: reproduced deterministically on the pre-RL baseline (`27e0d326`) with LRU. The diagnostics make the window about 3× longer | Fixed in `fa8d21b6`; the reproduction no longer crashes |
| 1 × start-up, LRU, wild address | `TraceManager::getTraceThread()` scanned `m_threads` without a lock while `newThread()` appended to it; a `push_back` reallocation left the reader in freed memory | No: LRU run, code unchanged since the baseline | Race confirmed by ThreadSanitizer and fixed in `fa8d21b6`; the crash itself was not reproduced |
| Hang of a second application run (`traceinput/num_runs` > 1), found during testing | `stop()`'s once-guard is never reset, so the second run's `stop()` returns at once and `wait()` never ends | No: hangs identically on the unfixed binary | Separate fix on a review branch, **not merged**. Stage 6 uses `num_runs = 1` |

Neither crash affected simulated results. A teardown crash happened after the statistics database was complete. A
start-up crash produced no results at all. Before the fix, about 1 in 50 runs with diagnostics, and 1 in 150
without, lost their `sim.stats` export.

## Tools

The host had no gdb, `sudo` needs a password, core dumps are disabled (`ulimit -c 0`), and `ptrace_scope` = 1 makes
`run-sniper --gdb` refuse to start. The work-around:
* gdb 12.1 and five libraries were unpacked without root (`apt-get download` + `dpkg -x`) into the session's
  scratch directory.
* Sniper ran under it through `run-sniper --wrap-sim=<wrapper>`. A parent may trace its child despite
  `ptrace_scope` = 1.

The wrapper has three modes:
* **crash:** stop on a signal and print every thread's stack.
* **delay:** gdb non-stop mode. Only the main thread is held, for N seconds, right after `Simulator::release()`
  returns (`standalone.cc:75`); the other threads keep running.
* **time:** timestamps at `release()`'s return and at the `exit_group` syscall.

## Teardown crash

**Natural occurrences** (all SIGSEGV, signal 11, simulator exit status 139, `Access Address = 0x120`):

| Run | Configuration |
|---|---|
| `s5val-rl-l3-1024k-r-K4` | `-n 4 -c gainestown`, L3 1 MB, `rl_lru`, `rl_diag/enabled=true`, `sharetest r 4` |
| `s5val-rl-l3-1024k-u-K1` | same, `sharetest u 1` |
| `crash-s5bin-rl-512k-u-K3-b7` | L3 512 KB, same policy, `sharetest u 3` |

The last log lines of each are `[MMU] Destroying MMU for core 3`, `[Memory Manager] Core 3 accessed … unique data
cache lines`, `[HUGETLBFS] Service destroyed`. Sniper's handler printed `simulator.h:Simulator::getClockSkewMinimizationServer:51`
← `pthread_thread.cc:PthreadThread::spawnedThreadFunc:21`.

**Code.**
1. `standalone.cc`: `TraceManager::run()`, then `Simulator::release()`, which runs `delete m_singleton;
   m_singleton = NULL;`, then `delete cfg`, then `return` and process exit.
2. `TraceManager::run()` returns from `wait()` as soon as every trace thread has set `m_finished`, polling every
   5 ms. Nothing stops or joins the **trace monitor** thread.
3. `TraceManager::Monitor::run()` loops: `sleep(1)`, then
   `Sim()->getClockSkewMinimizationServer() ? … : 0`, and only after that checks `m_watch_closed` (set only by
   `cleanup()`, which a single-run simulation never calls) and `all_finished`.
4. A monitor that wakes after `m_singleton = NULL` and before the process exits calls the getter on a null
   `Simulator*`. It reads `m_clock_skew_minimization_server` at offset **0x120** (checked with gdb in both
   binaries), which is exactly the reported address.

The null check in the monitor tests the server, not `Sim()`. A monitor that wakes earlier, during `~Simulator`,
finds `Sim()` still set but the server already NULL. It then reads the per-core times of cores `~Simulator` has
already deleted: a silent use-after-free read, which ends the same way once the fix is in.

**Is `PthreadThread`'s commented-out join the answer?** No. `~PthreadThread` would run only when a `TraceThread`
(between runs, `cleanup()`) or the `Monitor` is deleted. In a single-run simulation neither happens, so enabling
the join would not change this path. Between runs it could block forever on a reader parked in a pipe read, the
case `TraceManager::wait()` already guards against with its bounded wait.

**Deterministic reproduction** (delay mode, main thread held 2 s after `release()`). Every run crashed:

| Binary | Configuration | Result |
|---|---|---|
| pre-RL baseline `27e0d326` | LRU, `coherence_regress clean write 256`, 2 cores | SIGSEGV, thread 4 |
| Stage 5 `20ef93de` | LRU, same | SIGSEGV |
| Stage 5 | `rl_lru`, diagnostics **off**, same | SIGSEGV |
| Stage 5 | `rl_lru` + diagnostics, `sharetest u 3`, 4 cores, 512 KB L3 | SIGSEGV |

Full stack of the faulting thread (baseline binary, LRU). Only two threads were left, main and this one:

```
Thread 4 "sniper" received signal SIGSEGV, Segmentation fault.
#0  Simulator::getClockSkewMinimizationServer (this=0x0) at common/system/simulator.h:51
#1  TraceManager::Monitor::run (this=0x1a3bfc0) at common/trace_frontend/trace_manager.cc:775
        last_sig = 18446744073709551615, still = 0, reported = false
#2  PthreadThread::spawnedThreadFunc (vp=<optimized out>) at common/misc/pthread_thread.cc:19
#3  ?? () from libc.so.6
  Id   Target Id                     Frame
  1    Thread … (LWP …) "sniper"     (running)
* 4    Thread … (LWP …) "sniper"     Simulator::getClockSkewMinimizationServer (this=0x0) …
```

**Why the natural crashes were all `rl_lru` + diagnostics.** The crash needs the monitor's once-a-second wake-up
to fall between `release()` and process exit, so the rate is roughly that window divided by 1 s. Window measured
under gdb (`sharetest u 3`, 4 cores, 512 KB L3, 2 runs each):

| Configuration | `release()` returns → `exit_group` |
|---|---|
| LRU | 5.9, 5.9 ms |
| `rl_lru`, diagnostics off | 5.9, 7.4 ms |
| `rl_lru`, diagnostics on | 14.5, 19.2 ms |
| `sf_lru`, diagnostics on | 21.6, 16.2 ms |

* **What lengthens it:** the write oracle's 64 static hash tables (`rl_write_oracle.cc`). With diagnostics they
  held **68,315** heap records at `release()` (0 without), freed by static destructors during process exit,
  after `Sim()` is NULL.
* **The diagnostics' other state** (observers, departure maps, snapshot files) is destroyed inside `~Simulator`.
  It lengthens the silent use-after-free window above, not the NULL window.
* **No diagnostic code is on the faulting path.** The diagnostics add no callback, thread, file operation or lock
  that the monitor reaches; their contribution is the longer exit only.
* **Expected rates:** about 1.5–2% with diagnostics and about 0.6% without. Observed: 3 of ~93 `rl_lru` +
  diagnostics runs, 0 of 33 `sf_lru` + diagnostics, 0 of ~105 LRU-type runs. That is consistent.
* **Under gdb:** 36 natural runs of the crashing configuration did not crash (P ≈ 0.5 at 2%). No natural crash was
  captured under gdb, so the all-thread stack above is the deterministic one.

**Distinguishing diagnostics on and off by natural crash counts** would need hundreds of runs per configuration
(rates of 0.6% against 2%). The delay test answers the question directly: the race exists with diagnostics off and
with LRU.

## Start-up crash

| Run | Configuration | Signal / status | Address |
|---|---|---|---|
| `s5val-lru-l3-512k-u-K3` | `-n 4 -c gainestown`, L3 512 KB, LRU, `sharetest u 3` | SIGSEGV, simulator 139 (recorder 127) | 0x6ff81b3094de |

* **Backtrace:** `trace_thread.h:TraceThread::getThread:291` ← `MemoryManager::coreInitiateMemoryAccess:625` ←
  `Core::initiateMemoryAccess` ← … ← `TraceThread::m_run_func_default`.
* **Last log lines:** `[TraceThread] Thread 1 (app 0) started`, `Creating new application with app id = 0`; thread
  2 started at the moment of the crash.
* **Mechanism (code inspection).** `coreInitiateMemoryAccess` calls `TraceManager::getTraceThread(app, thread)` on
  every memory access, from every core thread. That function scans `std::vector<TraceThread*> m_threads` without
  `m_lock`. `newThread()` appends to it under `m_lock`.
* **How it faults:** a `push_back` that reallocates frees the old buffer while another core thread scans it. The
  reader loads a stale or garbage `TraceThread*` and faults in the inlined `getThread()` at a wild address. The
  vector grows from capacity 2 to 4 when the third trace thread (thread 2) is added, which is what the log shows.
* **Relation to the teardown crash:** a different mechanism; both are thread-lifetime and synchronization flaws in
  the trace front end. `trace_manager.cc`, `trace_thread.*` and `memory_manager.cc`'s lookup are unchanged since
  the baseline, apart from the inert line-watch calls.
* **ThreadSanitizer confirms the race.** It was run on an 8-core thread-creation stress program, with ASLR off
  because GCC 11's TSan cannot map memory on this kernel otherwise. On the unfixed build it reports:
  * `newThread()` → `push_back` → `_M_realloc_insert` freeing the vector's old buffer;
  * racing with `getTraceThread()` reading `size()` and elements, from `coreInitiateMemoryAccess` on another core
    thread, including a read of the freed buffer.

  The fixed build, in the same workload at the same point, reports no race on the registry.
* **The crash itself was not reproduced.** Freed memory usually still holds the old pointers, and the crash needs
  the buffer reused before the reader finishes. So the fix provably removes the race and its use-after-free; that
  this race caused the one observed crash is very likely, not proven.

## The fix (commit `fa8d21b6`)

Four files, +108/−31. Every change is outside the RL code.

**Teardown: the monitor shutdown protocol.**
* **The rule.** The trace monitor thread, which is never joined, touches the Simulator (`Sim()`, cores, the thread
  manager) only while holding `m_watch_lock`, and only after checking that `m_watch_closed` is false. This applies
  in its periodic check and in `reportHang()`.
* **The boundary.** `Simulator::release()` calls `TraceManager::closeWatch()`, which sets the flag under the same
  lock, before deleting anything. Both release paths go through it: the normal end of a run (`standalone.cc`) and
  a Python script's abort (`py_hooks.cc`).
* **Why it is correct.** The lock orders every monitor operation against the close. An operation that took the
  lock first finishes before the Simulator is deleted. One that takes it later sees the flag and returns without
  touching the Simulator.
* **No deadlock.**
  * `m_watch_lock` is a leaf: nothing else is acquired while it is held. The calls made under it are
    `getGlobalTime`, the performance-model time, `printState`, `getThreadState` and the bounded signal-based stack
    dump, and none takes a lock.
  * `release()` therefore waits at most for one bounded monitor operation. There is no thread join, which could
    block on a reader parked in a pipe read.
* **Other changes.** The hang-report interval is read when the TraceManager is constructed, because the monitor
  used to read it through `Sim()` at start-up. `m_num_threads_started`, which the monitor polls without a lock, is
  atomic.

**Start-up: the thread registry.**
* **The rule.** `m_threads` is read and written only while holding a new leaf lock, `m_threads_lock`.
  * `newThread()` appends under it and `cleanup()` swaps the list out under it.
  * `getTraceThread()` scans under it; nothing it calls takes a lock.
  * Every other loop over the list iterates a copy taken under it (`threadsSnapshot()`), so the work done on each
    thread happens without the lock. That also fixes two pre-existing loops that iterated the list while it could
    grow: `start()` spawning threads, and `endApplication()` calling `signalDone()`, which can restart an
    application.
* **Lock order.** `m_lock` → `m_threads_lock` and `m_lock` → `m_watch_lock`. The two leaf locks are never nested.
  So `createTraceBasedApplication()`, which calls `getTraceThread()` holding `m_lock`, cannot deadlock.
* **Lifetime.** A `TraceThread*` from a snapshot or `getTraceThread()` stays valid until `cleanup()`, which runs
  only between application runs.
* **No lock per memory access.** The memory-access path looks the trace thread up only when userspace MimicOS is
  enabled (off by default), and no longer dereferences a null result.

**Behavior changes.**
* `endApplication()` does not visit threads created during its own loop (application restarts only).
* After the Simulator is released, the monitor prints no hang report.
* A hang report in progress at that moment delays the release until it finishes.
* No simulated result changes.

## Tests of the fix

* **Teardown, deterministic.** gdb in non-stop mode held only the main thread for 2 s or 5 s after `release()`.
  The patched binary exits normally (LRU, 2 cores; `rl_lru` + diagnostics, 4 cores), where the unpatched one
  crashed 4 of 4 times.
* **Teardown, fault injection.** A test-only build, not committed, put delays into each ordering of a monitor
  operation against `release()`. All four behave as the protocol requires:
  * the monitor is mid-check: `release()` waited 1.65 s for it;
  * a hang report is printing: it completed, then the watch closed;
  * a report started before the release but takes the lock after it: it saw the watch closed and skipped;
  * control: normal.
* **Start-up.** ThreadSanitizer: no registry race (see above). A native 8-core run of the thread-creation stress
  program (24 threads in 4 waves) completed 3 of 3 times.
* **No change in results.** Single-core FFT replays (LRU, `rl_lru`, `sf_lru`, learning off; 8 MB and 512 KB L3)
  are identical to Stage 3: 10 runs, plus the post-merge smoke replay.
* **Regressions.** All pass every checker (`check_sharers`, `check_rl_events`, `check_rl_policy`), with 0 stale write
  hits:
  * coherence regressions: 2-core LRU, 2- and 4-core `rl_lru`;
  * 4-core FFT with `rl_lru` and `sharetest u 4` with `sf_lru`;
  * the SIGUSR1 hang report;
  * 6 Python-abort runs.
* **Build.** No compiler warnings.

## Repeated application runs (separate, not merged)

With `traceinput/num_runs = 2` the simulator hung after the second run, on the unpatched binary as well. gdb
shows the main thread in `Semaphore::wait()` inside `TraceManager::wait()`, at `standalone.cc:67`.

The cause is per-run state that `cleanup()` never resets:
* `m_stop_entered`: the second run's `stop()` returns at once, so `m_done` is never signalled;
* `m_shutdown_requested`: `isDone()` is true for the whole second run;
* the reader pool: its entries point into the deleted first-run `TraceThread`s.

A separate patch resets this state between runs. It is on a review branch and not merged. **Stage 6 uses
independent single-run simulations (`num_runs = 1`), which do not depend on it.**

## Remaining limitations

* **Remaining ThreadSanitizer reports**, all pre-existing and present in both builds:
  * **512 "unlock of an unlocked mutex"** from Sniper's per-set cache locks (`common/misc/setlock.h`): pthread
    mutexes kept in a `std::vector`, a pattern TSan evidently does not follow.
  * **Cache-state and timing races** (`RobTimer::doIssue`, `CacheBlockInfo` state changes,
    `MemoryManager::handleMsgFromNetwork`). These are probably accesses that the same set locks protect and that
    TSan cannot see, so real races cannot be told from false positives. If real, they would add multicore
    nondeterminism, equally for LRU and RL. Single-core replay is deterministic, and Stage 6 uses repetitions.
  * **The monitor's lock-free reads** of per-core time and the core pointer: diagnostic only.
  * **`Core::hookPeriodicIns*`**: only Python periodic scripts use it.
* **A trace thread still running** when `TraceManager::wait()` gives up (10 s) could still touch a released
  Simulator. Unchanged and never observed.
* **Not tested:**
  * the Python-abort release path under the fault-injection hooks;
  * multi-threaded repeated runs, since recording a multi-threaded trace with response files fails offline;
  * SMT.
