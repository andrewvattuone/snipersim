#!/usr/bin/env python3
"""Stage 6 experiment runner for the RL replacement policies (docs/rl/ALGORITHM_AUDIT.md, Section 9, Stage 6).

Usage: run_stage6.py OUTDIR --workloads sharetest-r,sharetest-u --l3 8192 --reps 2 [--jobs 5] [--timeout SECONDS]
                     [--policies lru,sf-text,sf-example,rl-n2,rl-n4] [--dry-run]

Every simulation gets the same explicit configuration apart from the policy keys: 4-core Gainestown, private L2 as
the only cache with the policy under test, prefetchers off, traceinput/num_runs = 1, the given shared L3 size, and
SNIPER_RL_EVENT_CHECK=1 / SNIPER_COHERENCE_CHECK=1 for the checkers. The RL policies run with rl_diag/enabled =
true (observation only; it changes no decision). Each run directory is OUTDIR/<workload>-l3-<KB>-<policy>-r<rep>.
Workloads marked roi run with --roi: only the program's region of interest is simulated in detail (the caches are
warmed before it) and sim.stats covers that region alone.

Timeout: each workload has its own bound (WORKLOADS[...]['timeout'], about 2x its measured wall time with several
simulations in parallel); --timeout overrides it for every workload. The bound used is recorded per run. On a
timeout the run's whole session (run-sniper, the simulator, the SDE front end and its pind) is killed.

Checks: as soon as a simulation completes, check_sharers.py and check_rl_events.py (and, for the RL policies,
check_rl_policy.py) run on it alone. A check passes only if the checker exits 0 and reports RESULT PASS and no
RESULT FAIL. Their output is appended to OUTDIR/checkers.txt.

OUTDIR/manifest.jsonl gets one line per run, written as it finishes: name, policy, workload, l3, rep, the exact
command line, stdin file, the git commit, the simulator and workload binary checksums, timeout, exit status,
wall-clock seconds, timed_out, the verdict of each check, and status. Status is "ok" only if the simulator exited 0
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
}
# Workloads: name -> command after `--`, whether to simulate the ROI only, and an optional stdin file.
# The SPLASH-2 arguments are those of the benchmarks repository's splashrun, with 4 threads.
S2 = lambda d, *a: [os.path.join(SPLASH2, d)] + list(a)
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
}
COMMON = ['traceinput/num_runs=1',
          'perf_model/l1_icache/prefetcher=none', 'perf_model/l1_dcache/prefetcher=none', f'{L2}/prefetcher=none']


def md5(path):
    return hashlib.md5(open(path, 'rb').read()).hexdigest() if os.path.exists(path) else None


def command(policy, workload, l3, outdir):
    cmd = ['./run-sniper', '-n', '4', '-c', 'gainestown']
    for g in COMMON + [f'perf_model/l3_cache/cache_size={l3}'] + POLICIES[policy]:
        cmd += ['-g', g]
    if policy != 'lru':
        cmd += ['-g', f'{L2}/rl_diag/enabled=true']
    if WORKLOADS[workload].get('roi'):
        cmd += ['--roi']
    return cmd + ['-d', outdir, '--'] + WORKLOADS[workload]['cmd']


_manifest_lock = threading.Lock()
_checkers_lock = threading.Lock()


def run_checks(outdir, policy, checkers_txt=None):
    """Run each applicable checker on one run directory; {script: True/False}. Output goes to checkers_txt."""
    verdicts, text = {}, ''
    scripts = ['check_sharers.py', 'check_rl_events.py'] + (['check_rl_policy.py'] if policy != 'lru' else [])
    for script in scripts:
        r = subprocess.run(['python3', os.path.join(E, script), outdir], cwd=ROOT, capture_output=True, text=True)
        verdicts[script] = r.returncode == 0 and 'RESULT PASS' in r.stdout and 'RESULT FAIL' not in r.stdout
        text += f'### {script} (exit {r.returncode}, {"PASS" if verdicts[script] else "FAIL"})\n{r.stdout}{r.stderr}\n'
    if checkers_txt:
        with _checkers_lock, open(checkers_txt, 'a') as f:
            f.write(f'##### {time.strftime("%Y-%m-%d %H:%M:%S")} {os.path.basename(outdir)}\n{text}')
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
    # With --roi the detailed models must have been switched on and off by the program's markers
    roi_seen = not job['roi'] or ('Enabling performance models' in text and 'Disabling performance models' in text)
    ok = rc == 0 and stats and not crashed and not timed_out and roi_seen
    checks = run_checks(outdir, job['policy'], checkers_txt) if ok else {}
    checks_ok = bool(checks) and all(checks.values())
    job.update(exit=rc, wall_s=round(wall, 1), timed_out=timed_out, crashed=crashed, stats=stats, roi_seen=roi_seen,
               checks=checks, status='ok' if ok and checks_ok else ('FAILED (checks)' if ok else 'FAILED'))
    with _manifest_lock, open(manifest, 'a') as f:
        f.write(json.dumps(job) + '\n')
    print(f'{time.strftime("%H:%M:%S")} {name:34s} {job["status"]:6s} exit={rc} wall={wall:6.1f}s'
          + (' TIMEOUT' if timed_out else '') + (' CRASH' if crashed else '')
          + ''.join(f' {k[:-3]}:FAIL' for k, v in checks.items() if not v), flush=True)
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
                                     roi=bool(spec.get('roi')), stdin=spec.get('stdin'),
                                     stdin_md5=md5(spec['stdin']) if spec.get('stdin') else None,
                                     workload_md5=md5(spec['cmd'][0]), timeout_s=a.timeout or spec['timeout'],
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
