#!/usr/bin/env python3
"""Stage 6 experiment runner for the RL replacement policies (docs/rl/ALGORITHM_AUDIT.md, Section 9, Stage 6).

Usage: run_stage6.py OUTDIR --workloads sharetest-r,sharetest-u --l3 8192 --reps 2 [--jobs 5] [--timeout SECONDS]
                     [--policies lru,sf-text,sf-example,rl-n2,rl-n4] [--dry-run]
       (also lru-hist, the controls rl-n4-frozen, rl-n4-prio, rl-n4-rand, and the Stage 7 configurations
       rl-n4-dm65536, rl-n4-ema001: see POLICIES)

Every simulation gets the same explicit configuration apart from the policy keys: 4-core Gainestown, private L2 as
the only cache with the policy under test, prefetchers off, traceinput/num_runs = 1, the given shared L3 size, and
SNIPER_RL_EVENT_CHECK=1 / SNIPER_COHERENCE_CHECK=1 for the checkers. The RL policies run with rl_diag/enabled =
true (observation only; it changes no decision). Each run directory is OUTDIR/<workload>-l3-<KB>-<policy>-r<rep>.
Workloads marked roi run with --roi: only the program's region of interest is simulated in detail (the caches are
warmed before it) and sim.stats covers that region alone. A workload's own run-sniper options (WORKLOADS[...]['sniper'])
can bound that region differently, and its roi_end names how the region must end (recorded per run as roi_end):
  application        the program's SimRoiEnd() marker (default);
  instruction-limit  Sniper's stop-by-icount ends it after N instructions summed over all cores (a fixed instruction
                     count, not a fixed amount of work: compare per-instruction rates, not completion time);
  iteration-marker   experiments/rl/roi-iter-sync.py (with --roi-script) runs complete iterations between the program's
                     SimMarker(1, i) / SimMarker(2, i) markers in detail (a fixed amount of work).
CAP Bench workloads also carry input = paper (the inputs of Souza & Freitas 2024) or exploratory (reduced inputs).

Timeout: each workload has its own bound (WORKLOADS[...]['timeout'], about 2x its measured wall time with several
simulations in parallel); --timeout overrides it for every workload. The bound used is recorded per run. On a
timeout the run's whole session (run-sniper, the simulator, the SDE front end and its pind) is killed.

Checks: as soon as a simulation completes, check_sharers.py and check_rl_events.py (and, for the RL policies,
check_rl_policy.py) run on it alone. A check passes only if the checker exits 0 and reports RESULT PASS and no
RESULT FAIL. Their output is appended to OUTDIR/checkers.txt.

OUTDIR/manifest.jsonl gets one line per run, written as it finishes: name, policy, workload, l3, rep, the exact
command line, stdin file, the git commit, the simulator and workload binary checksums, timeout, exit status,
wall-clock seconds, timed_out, the verdict of each check, and status. It also records the policy's configured options
(policy_options) and the rl_lru settings the simulator itself reported at start-up (rl_settings, from its [RL]
lines: mode, window, alpha, protection, scope, control settings, estimator and horizon; estimator 'ema' when no
STAGE7 line was printed, which is the default). Status is "ok" only if the simulator exited 0
within the timeout, the statistics exist, no "Internal exception" was logged, for ROI workloads the region was
entered and left, and every check passed; otherwise "FAILED" or "FAILED (checks)". summarize_stage6.py uses only
"ok" runs.

An existing run directory is never reused, overwritten or recorded again: it is skipped and reported.

Exit status: 0 only if every requested run was simulated in this invocation and ended "ok"; 1 if any run failed;
3 if none failed but some were skipped (with every run skipped, the batch did nothing new and says so).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
E = os.path.join(ROOT, 'experiments', 'sharer-counts')
L2 = 'perf_model/l2_cache'
# SPLASH-2 from https://github.com/snipersim/benchmarks, built by build_splash2.sh outside this tree
SPLASH2 = os.environ.get('SPLASH2_CODES', os.path.join(os.path.dirname(ROOT), 'sniper-benchmarks', 'splash2', 'splash2', 'codes'))
INPUTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'inputs')

# The five initial configurations; every learning parameter is the approved default, spelled out
POLICIES = {
    'lru':        [f'{L2}/replacement_policy=lru'],
    'sf-text':    [f'{L2}/replacement_policy=sf_lru', f'{L2}/sf/mode=both', f'{L2}/sf/rule=text', f'{L2}/sf/combine=and'],
    'sf-example': [f'{L2}/replacement_policy=sf_lru', f'{L2}/sf/mode=both', f'{L2}/sf/rule=example', f'{L2}/sf/combine=and'],
    'rl-n2':      [f'{L2}/replacement_policy=rl_lru', f'{L2}/rl/mode=both', f'{L2}/rl/window=2', f'{L2}/rl/alpha=0.1',
                   f'{L2}/rl/protect_feedback=true', f'{L2}/rl/weight_scope=global'],
    'rl-n4':      [f'{L2}/replacement_policy=rl_lru', f'{L2}/rl/mode=both', f'{L2}/rl/window=4', f'{L2}/rl/alpha=0.1',
                   f'{L2}/rl/protect_feedback=true', f'{L2}/rl/weight_scope=global'],
    # Not a policy under test: LRU's victim sequence (alpha 0 keeps every weight 0, so every decision is LRU's;
    # Stage 3 verified the identical victim sequence, POLICY_CORES.md) with the eviction history and rl_diag, to
    # measure reuse just beyond the L2's capacity (misses on blocks still in the 16-entry per-set history).
    'lru-hist':   [f'{L2}/replacement_policy=rl_lru', f'{L2}/rl/mode=both', f'{L2}/rl/window=2', f'{L2}/rl/alpha=0',
                   f'{L2}/rl/protect_feedback=true', f'{L2}/rl/weight_scope=global'],
}
# Controls for the learning question (docs/rl/POLICY_CORES.md, "Controls for the learning question"; CAPBENCH.md):
# rl-n4's window, candidates, history, protection and diagnostics, with no weight updates and another selection rule.
RL_N4_BASE = POLICIES['rl-n4']
# Frozen weights: the mean of the second-half in-ROI weight snapshots of the 12 core-runs of the LU tiny rl-n4 pilot
# (results/stage6-capbench-lu-pilot), so an in-sample control for that workload, not an independent one.
FROZEN_LU_TINY = {'S': -0.09, 'E': -0.38, 'M': -0.76, 'BT': -0.64, 'AT': 0.0}
POLICIES.update({
    'rl-n4-frozen': RL_N4_BASE + [f'{L2}/rl/learning=false']
                    + [f'{L2}/rl/initial_weights/{k}={v}' for k, v in FROZEN_LU_TINY.items()],
    # Fixed priority: S > E > M, then above > below the sharing threshold, ties to the older candidate (priority_rank)
    'rl-n4-prio':   RL_N4_BASE + [f'{L2}/rl/learning=false', f'{L2}/rl/selection=priority'],
    # Randomized: the oldest candidate, or with probability 0.083 (rl-n4's measured rate of leaving LRU on LU tiny)
    # one of the other three, by a seeded per-cache draw
    'rl-n4-rand':   RL_N4_BASE + [f'{L2}/rl/learning=false', f'{L2}/rl/selection=random', f'{L2}/rl/random_rate=0.083',
                                  f'{L2}/rl/seed=1'],
})
# Stage 7 (docs/rl/STAGE7.md): rl-n4 with another weight estimator, learning on, zero initial weights (no LU input).
#   rl-n4-dm65536  rl/estimator=discounted_mean, rl/horizon=65536 decisions (rl/alpha=0.1 stays in the list, unused)
#   rl-n4-ema001   the algorithm's EMA with rl/alpha=0.01 instead of 0.1 (the smaller-alpha comparator)
POLICIES.update({
    'rl-n4-dm65536': RL_N4_BASE + [f'{L2}/rl/estimator=discounted_mean', f'{L2}/rl/horizon=65536'],
    'rl-n4-ema001':  [f'{L2}/rl/alpha=0.01' if o == f'{L2}/rl/alpha=0.1' else o for o in RL_N4_BASE] + [f'{L2}/rl/estimator=ema'],
})
assert POLICIES['rl-n4-ema001'].count(f'{L2}/rl/alpha=0.01') == 1 and f'{L2}/rl/alpha=0.1' not in POLICIES['rl-n4-ema001']
# Workloads: name -> command after `--`, whether to simulate the ROI only, and an optional stdin file.
# The SPLASH-2 arguments are those of the benchmarks repository's splashrun, with 4 threads.
S2 = lambda d, *a: [os.path.join(SPLASH2, d)] + list(a)
# CAP Bench from https://github.com/cart-pucminas/CAPBenchmarks, built by build_capbench.sh outside this tree into a
# directory named after the md5 of the patch it applied, so a build can only be used with the patch it was made from
CAPB_PATCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'capbench-roi.patch')
CAPB = os.environ.get('CAPB_BIN', os.path.join(os.path.dirname(ROOT), 'capbench-build-'
                      + hashlib.md5(open(CAPB_PATCH, 'rb').read()).hexdigest()[:8], 'bin'))
CB = lambda k, cls: [os.path.join(CAPB, k + '.intel'), '--class', cls, '--nthreads', '4']
# timeout: per-run bound in seconds, from the measured wall time (comment) with 4-5 simulations in parallel.
WORKLOADS = {
    'sharetest-r': dict(cmd=[os.path.join(E, 'sharetest'), 'r', '4'], timeout=600),                    # 32-35 s
    'sharetest-w': dict(cmd=[os.path.join(E, 'sharetest'), 'w', '4'], timeout=600),
    'sharetest-p': dict(cmd=[os.path.join(E, 'sharetest'), 'p', '4'], timeout=600),
    'sharetest-u': dict(cmd=[os.path.join(E, 'sharetest'), 'u', '4'], timeout=600),                    # 32-35 s
    'fft-m12':     dict(cmd=[os.path.join(ROOT, 'test', 'fft', 'fft'), '-p', '4', '-m', '12'], timeout=600),  # ~51 s
    'fft-m16':     dict(cmd=[os.path.join(ROOT, 'test', 'fft', 'fft'), '-p', '4', '-m', '16', '-t'], timeout=2400),  # 828-890 s
    'radix-small': dict(cmd=S2('kernels/radix/RADIX', '-n262144', '-p4', '-t'), roi=True, timeout=900),       # 170 s
    'lu-small':    dict(cmd=S2('kernels/lu/contiguous_blocks/LU', '-n512', '-p4', '-t'), roi=True, timeout=2400),  # 768 s
    'ocean-small': dict(cmd=S2('apps/ocean/contiguous_partitions/OCEAN', '-n258', '-p4'), roi=True, timeout=3000),  # 1443 s
    'barnes-4096': dict(cmd=S2('apps/barnes/BARNES'), roi=True, stdin=os.path.join(INPUTS, 'barnes-4096-p4.in'),
                        timeout=2400),                                                                 # 1103 s
    # Not measured: about 5x barnes-4096 by instruction count (1.85 G vs 0.38 G)
    'barnes-small': dict(cmd=S2('apps/barnes/BARNES'), roi=True, stdin=os.path.join(INPUTS, 'barnes-16384-p4.in'),
                         timeout=10800),
    # CAP Bench x86/OpenMP kernels (build_capbench.sh; docs/rl/CAPBENCH.md), 4 threads. ROI instruction counts (SDE,
    # 4 threads) in comments.
    # Paper inputs, first 500 M ROI instructions only (results/stage6-capbench-qual; the full kernels are 8-55 G).
    # IS's 5 G-instruction initialization is fast-forwarded (--no-cache-warming): cold caches at the ROI.
    'capb-is-500m': dict(cmd=CB('is', 'standard'), roi=True, input='paper', roi_end='instruction-limit', timeout=3600,
                         sniper=['-s', 'stop-by-icount:500000000', '--no-cache-warming']),   # > 3,600 s: timed out
    'capb-lu-500m': dict(cmd=CB('lu', 'standard'), roi=True, input='paper', roi_end='instruction-limit', timeout=3600,
                         sniper=['-s', 'stop-by-icount:500000000']),                          # 1,720 s
    'capb-km-500m': dict(cmd=CB('km', 'paper'), roi=True, input='paper', roi_end='instruction-limit', timeout=3600,
                         sniper=['-s', 'stop-by-icount:500000000']),                          # 624 s
    # FAST and GF qualification (results/stage6-fastgf-qual): paper inputs (8192 x 8192 image; GF 11 x 11 mask), first
    # 250 M ROI instructions. The ~64 M-pixel random-image initialization is fast-forwarded (--no-cache-warming): the
    # 64 MB image exceeds the L3, and each thread's rows are untouched since initialization, so warming changes little.
    'capb-fast-250m': dict(cmd=CB('fast', 'standard'), roi=True, input='paper', roi_end='instruction-limit',
                           timeout=3000, sniper=['-s', 'stop-by-icount:250000000', '--no-cache-warming']),
    'capb-gf-250m': dict(cmd=CB('gf', 'standard'), roi=True, input='paper', roi_end='instruction-limit',
                         timeout=3000, sniper=['-s', 'stop-by-icount:250000000', '--no-cache-warming']),
    # The same, first 500 M ROI instructions: at 250 M the cold L2s had barely filled (FAST 6, GF 111 replacements)
    'capb-fast-500m': dict(cmd=CB('fast', 'standard'), roi=True, input='paper', roi_end='instruction-limit',
                           timeout=3000, sniper=['-s', 'stop-by-icount:500000000', '--no-cache-warming']),
    'capb-gf-500m': dict(cmd=CB('gf', 'standard'), roi=True, input='paper', roi_end='instruction-limit',
                         timeout=3000, sniper=['-s', 'stop-by-icount:500000000', '--no-cache-warming']),
    # Paper input, two complete K-Means iterations in detail (1.78 G + 1.77 G), with a statistics snapshot at the start
    # of iteration 2; `measure` is iteration 2 alone (warm caches, reuse of the points). Initialization (7 M) is
    # fast-forwarded. Not measured: ~0.84 MIPS (capb-km-500m) -> ~4,200 s.
    'capb-km-paper-it2': dict(cmd=CB('km', 'paper'), roi=True, input='paper', roi_end='iteration-marker', timeout=9000,
                              measure='iter-2-begin:roi-end',
                              sniper=['-s', 'experiments/rl/roi-iter-sync::1:2:2', '--roi-script', '--no-cache-warming']),
    # Exploratory input: LU `tiny` (512 x 512), the whole factorization (ROI 0.71 G of 0.72 G instructions). Exploratory
    # also because upstream LU pivots on the smallest-magnitude element (docs/rl/CAPBENCH.md): the factorization is
    # numerically meaningless and its data-dependent swaps may differ from a correct LU's. Not measured: 0.3-0.8 MIPS
    # -> ~900-2,400 s.
    'capbx-lu-tiny': dict(cmd=CB('lu', 'tiny'), roi=True, input='exploratory', timeout=5400),
    # Smoke test of the iteration markers: LU `tiny`, steps 2-4 detailed (~5 M instructions), measure step 4 alone
    'capbx-lu-tiny-smoke': dict(cmd=CB('lu', 'tiny'), roi=True, input='exploratory', roi_end='iteration-marker',
                                timeout=600, measure='iter-4-begin:roi-end',
                                sniper=['-s', 'experiments/rl/roi-iter-sync::2:4:4', '--roi-script', '--no-cache-warming']),
}
ROI_END_SIGNS = {'instruction-limit': '[STOPBYICOUNT] Ending ROI after', 'iteration-marker': '[ROIITERSYNC] switch end '}
SWITCH_RE = re.compile(r'\[ROIITERSYNC\] switch (\w+) iteration (-?\d+) marker-icount (\d+) switch-icount (\d+) offset (\d+)')
MARKERS_RE = re.compile(r'\[ROIITERSYNC\] markers begin (\d+) end (\d+) first (\S+) last (\S+) order-violations (\d+)')
COMMON = ['traceinput/num_runs=1',
          'perf_model/l1_icache/prefetcher=none', 'perf_model/l1_dcache/prefetcher=none', f'{L2}/prefetcher=none']


def md5(path):
    return hashlib.md5(open(path, 'rb').read()).hexdigest() if os.path.exists(path) else None


def build_info(binary):
    """First line of the BUILD_INFO beside a build_capbench.sh build (source revision and patch md5), if any."""
    f = os.path.join(os.path.dirname(os.path.dirname(binary)), 'BUILD_INFO')
    return open(f).readline().strip() if os.path.exists(f) else None


def command(policy, workload, l3, outdir):
    cmd = ['./run-sniper', '-n', '4', '-c', 'gainestown']
    for g in COMMON + [f'perf_model/l3_cache/cache_size={l3}'] + POLICIES[policy]:
        cmd += ['-g', g]
    if policy != 'lru':
        cmd += ['-g', f'{L2}/rl_diag/enabled=true']
    if WORKLOADS[workload].get('roi') and '--roi-script' not in WORKLOADS[workload].get('sniper', []):
        cmd += ['--roi']
    cmd += WORKLOADS[workload].get('sniper', [])
    return cmd + ['-d', outdir, '--'] + WORKLOADS[workload]['cmd']


_manifest_lock = threading.Lock()
_checkers_lock = threading.Lock()


def interval_dir(outdir, region):
    """OUTDIR/<run>/measure: the statistics of the measured interval (dumpstats --partial) beside a copy of sim.cfg,
    laid out as a run directory so that the summarizer and the checkers can read it. None if it cannot be made."""
    sim = os.path.join(outdir, 'simulation')
    r = subprocess.run(['python3', os.path.join(ROOT, 'tools', 'dumpstats.py'), '-d', sim, '--partial', region],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0 or '=' not in r.stdout:
        return None
    d = os.path.join(outdir, 'measure', 'simulation')
    os.makedirs(d)
    open(os.path.join(d, 'sim.stats'), 'w').write(r.stdout)
    shutil.copy(os.path.join(sim, 'sim.cfg'), d)
    open(os.path.join(outdir, 'measure', 'REGION'), 'w').write(region + '\n')
    return os.path.dirname(d)


def run_checks(outdir, policy, checkers_txt=None, label=''):
    """Run each applicable checker on one run directory; {script: True/False}. Output goes to checkers_txt."""
    verdicts, text = {}, ''
    scripts = ['check_sharers.py', 'check_rl_events.py'] + (['check_rl_policy.py'] if policy != 'lru' else [])
    for script in scripts:
        r = subprocess.run(['python3', os.path.join(E, script), outdir], cwd=ROOT, capture_output=True, text=True)
        verdicts[script] = r.returncode == 0 and 'RESULT PASS' in r.stdout and 'RESULT FAIL' not in r.stdout
        text += f'### {script} (exit {r.returncode}, {"PASS" if verdicts[script] else "FAIL"})\n{r.stdout}{r.stderr}\n'
    if checkers_txt:
        with _checkers_lock, open(checkers_txt, 'a') as f:
            f.write(f'##### {time.strftime("%Y-%m-%d %H:%M:%S")} {os.path.basename(outdir)}{label}\n{text}')
    return verdicts


def session_members(sid):
    """PIDs of the processes in session sid (field 6 of /proc/PID/stat)."""
    pids = []
    for d in os.listdir('/proc'):
        if d.isdigit():
            try:
                stat = open(f'/proc/{d}/stat').read()
            except OSError:
                continue
            if int(stat[stat.rindex(')') + 2:].split()[3]) == sid:
                pids.append(int(d))
    return pids


RL_LINE_RE = re.compile(r'\[RL\] (\S+) core (\d+): (.*)$', re.M)


def rl_settings(text):
    """The rl_lru/sf_lru settings each cache printed at start-up ([RL] lines of the run log): {cache: {key: value}}, merged
    over the cores when they agree (else one entry per core). rl_lru's estimator is 'ema' unless a STAGE7 line says
    otherwise (the default prints none)."""
    per = {}
    for cache, core, rest in RL_LINE_RE.findall(text):
        if 'diagnostics on' in rest:
            continue
        d = per.setdefault((cache, int(core)), {})
        if rest.startswith('rl_lru') and 'policy' not in d:
            d.update(policy='rl_lru', estimator='ema')
        elif rest.startswith('sf_lru'):
            d['policy'] = 'sf_lru'
        d.update(re.findall(r'([A-Za-z_]+)=(\S+)', rest))
        if 'initial weights' in rest:
            d['initial_weights'] = rest.split('initial weights ', 1)[1].strip()
        if 'stream ' in rest:
            d['stream'] = rest.split('stream ', 1)[1].split(')', 1)[0]
    out = {}
    for cache in sorted({c for c, _ in per}):
        cores = {k[1]: v for k, v in per.items() if k[0] == cache}
        common = {k: v for k, v in cores[min(cores)].items() if k != 'stream' and all(c.get(k) == v for c in cores.values())}
        per_core = {c: {k: v for k, v in d.items() if common.get(k) != v} for c, d in sorted(cores.items())}
        out[cache] = dict(common, cores=sorted(cores), per_core={c: d for c, d in per_core.items() if d})
    return out


def run_one(job, manifest, checkers_txt):
    timeout = job['timeout_s']
    name, cmd, outdir = job['name'], job['cmd'], job['dir']
    if os.path.exists(outdir):
        job['status'] = 'skipped (exists)'
        print(f'{time.strftime("%H:%M:%S")} {name:34s} skipped: directory exists, not rerun or recorded', flush=True)
        return job
    env = dict(os.environ, SNIPER_RL_EVENT_CHECK='1', SNIPER_COHERENCE_CHECK='1')
    env.pop('SNIPER_BUILD_DIR', None)  # always the tree's build/default
    t0 = time.time()
    timed_out = False
    stdin = open(job['stdin']) if job.get('stdin') else subprocess.DEVNULL
    with open(outdir + '.log', 'w') as log:
        p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdin=stdin, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            for sig in (signal.SIGTERM, signal.SIGKILL):   # the simulator and front end are in the same group
                try:
                    os.killpg(p.pid, sig)
                except ProcessLookupError:
                    break
                try:
                    p.wait(timeout=30)
                    break
                except subprocess.TimeoutExpired:
                    pass
            # The leader can exit before the rest of its group, and SDE's pind runs in its own group of the same
            # session: wait until nothing of the session is left
            for _ in range(60):
                left = session_members(p.pid)
                if not left:
                    break
                for pid in left:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                time.sleep(1)
            rc = None
    if stdin is not subprocess.DEVNULL:
        stdin.close()
    wall = time.time() - t0
    text = open(outdir + '.log', errors='ignore').read()
    stats = os.path.exists(os.path.join(outdir, 'simulation', 'sim.stats'))
    crashed = 'Internal exception' in text
    # The detailed models must have been switched on and off once, and the region must have ended the expected way
    roi_end = None
    if job['roi'] and 'Disabling performance models' in text:
        roi_end = next((k for k, v in ROI_END_SIGNS.items() if v in text), 'application')
    roi_seen = not job['roi'] or ('Enabling performance models' in text and roi_end == job['roi_end_expected'])
    # roi-iter-sync: every deferred switch (marker and switch instruction counts) and the marker order
    switches = [dict(action=m[0], iteration=int(m[1]), marker_icount=int(m[2]), switch_icount=int(m[3]), offset=int(m[4]))
                for m in SWITCH_RE.findall(text)]
    mk = MARKERS_RE.search(text)
    markers = dict(begin=int(mk[1]), end=int(mk[2]), first=mk[3], last=mk[4], order_violations=int(mk[5])) if mk else None
    if job['roi_end_expected'] == 'iteration-marker':
        actions = [x['action'] for x in switches]
        want = ['detailed'] + (['snapshot'] if job.get('measure') else []) + ['end']
        roi_seen = roi_seen and actions == want and markers is not None and markers['order_violations'] == 0
    ok = rc == 0 and stats and not crashed and not timed_out and roi_seen
    checks = run_checks(outdir, job['policy'], checkers_txt) if ok else {}
    checks_ok = bool(checks) and all(checks.values())
    # The measured interval (e.g. one iteration) gets its own statistics and the same checkers
    measure_checks = None
    if ok and job.get('measure'):
        d = interval_dir(outdir, job['measure'])
        measure_checks = run_checks(d, job['policy'], checkers_txt, f' (interval {job["measure"]})') if d else {}
        checks_ok = checks_ok and bool(measure_checks) and all(measure_checks.values())
    job.update(rl_settings=rl_settings(text))
    job.update(exit=rc, wall_s=round(wall, 1), timed_out=timed_out, crashed=crashed, stats=stats, roi_seen=roi_seen,
               roi_end=roi_end, roi_switches=switches, roi_markers=markers, measure_checks=measure_checks,
               checks=checks, status='ok' if ok and checks_ok else ('FAILED (checks)' if ok else 'FAILED'))
    with _manifest_lock, open(manifest, 'a') as f:
        f.write(json.dumps(job) + '\n')
    print(f'{time.strftime("%H:%M:%S")} {name:34s} {job["status"]:6s} exit={rc} wall={wall:6.1f}s'
          + (' TIMEOUT' if timed_out else '') + (' CRASH' if crashed else '') + ('' if roi_seen else ' ROI-BOUNDARY')
          + ''.join(f' {k[:-3]}:FAIL' for k, v in checks.items() if not v)
          + ''.join(f' interval-{k[:-3]}:FAIL' for k, v in (measure_checks or {}).items() if not v)
          + ''.join(f' {x["action"]}@{x["iteration"]}+{x["offset"]}' for x in switches), flush=True)
    return job


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--workloads', default='sharetest-r,sharetest-u')
    ap.add_argument('--policies', default=','.join(POLICIES))
    ap.add_argument('--l3', default='8192')
    ap.add_argument('--reps', type=int, default=2)
    ap.add_argument('--jobs', type=int, default=5)
    ap.add_argument('--timeout', type=int, default=None, help='override every workload\'s own bound (seconds)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    for w in a.workloads.split(','):
        spec = WORKLOADS[w]
        for f in [spec['cmd'][0]] + ([spec['stdin']] if spec.get('stdin') else []):
            if not os.path.exists(f):
                sys.exit(f'{w}: {f} not found')
    for p in a.policies.split(','):
        POLICIES[p]
    outdir = os.path.abspath(a.outdir)
    git = lambda *c: subprocess.run(['git'] + list(c), cwd=ROOT, capture_output=True, text=True)
    provenance = dict(commit=git('rev-parse', 'HEAD').stdout.strip(),
                      branch=git('rev-parse', '--abbrev-ref', 'HEAD').stdout.strip(),
                      tree_clean=git('diff', '--quiet', 'HEAD', '--', 'common', 'simulator', 'sift', 'config').returncode == 0,
                      # the experiment tooling itself (this runner, the build scripts, the patch, the inputs)
                      tooling_clean=git('diff', '--quiet', 'HEAD', '--', 'experiments').returncode == 0
                      and not git('ls-files', '--others', '--exclude-standard', '--', 'experiments').stdout.strip(),
                      binary_md5=md5(os.path.join(ROOT, 'build', 'default', 'lib', 'sniper')),
                      env='SNIPER_RL_EVENT_CHECK=1 SNIPER_COHERENCE_CHECK=1')
    jobs = []
    for rep in range(1, a.reps + 1):          # repetition-major, so the first repetition completes first
        for l3 in a.l3.split(','):
            for w in a.workloads.split(','):
                for p in a.policies.split(','):
                    name = f'{w}-l3-{l3}-{p}-r{rep}'
                    d = os.path.join(outdir, name)
                    spec = WORKLOADS[w]
                    jobs.append(dict(name=name, policy=p, workload=w, l3=int(l3), rep=rep, dir=d, cmd=command(p, w, l3, d),
                                     policy_options=list(POLICIES[p]),
                                     roi=bool(spec.get('roi')), stdin=spec.get('stdin'),
                                     stdin_md5=md5(spec['stdin']) if spec.get('stdin') else None,
                                     workload_md5=md5(spec['cmd'][0]), workload_build=build_info(spec['cmd'][0]),
                                     input=spec.get('input'),
                                     roi_end_expected=spec.get('roi_end', 'application') if spec.get('roi') else None,
                                     measure=spec.get('measure'),
                                     timeout_s=a.timeout or spec['timeout'],
                                     **provenance))
    if a.dry_run:
        for j in jobs:
            print(' '.join(j['cmd']) + (f' < {j["stdin"]}' if j['stdin'] else '') + f'   # timeout {j["timeout_s"]} s')
        return 0
    os.makedirs(outdir, exist_ok=True)
    manifest = os.path.join(outdir, 'manifest.jsonl')
    checkers_txt = os.path.join(outdir, 'checkers.txt')
    bounds = sorted({j['timeout_s'] for j in jobs})
    print(f'{time.strftime("%H:%M:%S")} {len(jobs)} simulations, {a.jobs} in parallel, timeout {"/".join(map(str, bounds))} s',
          flush=True)
    with ThreadPoolExecutor(a.jobs) as ex:
        done = list(ex.map(lambda j: run_one(j, manifest, checkers_txt), jobs))
    skipped = [j for j in done if j['status'] == 'skipped (exists)']
    ran = [j for j in done if j not in skipped]
    ok = [j for j in ran if j['status'] == 'ok']
    failed = [j for j in ran if j['status'] != 'ok']
    print(f'{time.strftime("%H:%M:%S")} simulated {len(ran)} of {len(done)} requested runs: {len(ok)} ok, '
          f'{len(failed)} failed, {len(skipped)} skipped (directory exists)', flush=True)
    for j in failed:
        print(f'  FAILED {j["name"]}: {j["status"]}' + ''.join(f', {k} FAIL' for k, v in j['checks'].items() if not v))
    if failed:
        return 1
    if skipped:
        print('  NO NEW RESULTS: every requested run already existed; nothing was simulated' if not ran else
              '  INCOMPLETE BATCH: skipped runs were not simulated in this invocation', flush=True)
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main())
