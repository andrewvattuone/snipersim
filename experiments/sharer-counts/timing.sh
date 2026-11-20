#!/bin/bash
# Host-cost comparison of several Sniper binaries on one workload.
#
# Usage: experiments/sharer-counts/timing.sh REPS OUTDIR BINARY... [-- RUN-SNIPER-ARGS...]
#   BINARY  copies of build/default/lib/sniper saved from different builds
#   default workload: -n 4 -c gainestown -- ./test/fft/fft -p 4 -m 10 -t
#
# Runs strictly one at a time and interleaves the binaries in every repetition,
# so drift on the host affects all of them alike. Each binary is copied over
# build/default/lib/sniper for its runs; the original file is restored at the end
# (a later `make` would otherwise see an up-to-date binary from another build).
# Results go to results/timing-<basename of OUTDIR>-<binary>-<rep>.
# Writes OUTDIR/timing.tsv: binary, rep, wall s, user s, sys s, max RSS KB,
# simulated time (ns). Use REPS >= 5: run-to-run spread is large (WSL: +-15%).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
REPS=$1; OUT=$(mkdir -p "$2" && cd "$2" && pwd); shift 2
BINS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do BINS+=("$(readlink -f "$1")"); shift; done
[ "${1:-}" = "--" ] && shift
ARGS=("$@")
[ ${#ARGS[@]} = 0 ] && ARGS=(-n 4 -c gainestown -- ./test/fft/fft -p 4 -m 10 -t)

cd "$ROOT" || exit 1
TARGET=build/default/lib/sniper
cp -p "$TARGET" "$OUT/sniper.original"
trap 'cp -p "$OUT/sniper.original" "$TARGET"' EXIT

printf 'binary\trep\twall_s\tuser_s\tsys_s\tmaxrss_kb\tsim_ns\n' > "$OUT/timing.tsv"
for rep in $(seq 1 "$REPS"); do
   for bin in "${BINS[@]}"; do
      tag=$(basename "$bin")
      cp "$bin" "$TARGET"
      d=results/timing-$(basename "$OUT")-$tag-$rep
      rm -rf "$d"
      /usr/bin/time -f "%e %U %S %M" -o "$OUT/time.txt" ./run-sniper -d "$d" "${ARGS[@]}" > "$OUT/$tag-$rep.log" 2>&1
      status=$?
      read -r wall user sys rss < <(tail -1 "$OUT/time.txt")
      sim=$(python3 -c "import sys,re
for l in open('$d/simulation/sim.stats'):
    if l.startswith('performance_model.elapsed_time ='):
        print(max(int(x) for x in l.split('=')[1].split(','))/1e6)" 2>/dev/null)
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tag" "$rep" "$wall" "$user" "$sys" "$rss" "$sim" >> "$OUT/timing.tsv"
      echo "$tag rep$rep exit=$status wall=$wall user=$user"
   done
done
python3 - "$OUT/timing.tsv" <<'EOF'
import csv, statistics as st, sys
rows = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
for b in dict.fromkeys(r['binary'] for r in rows):
    sel = [r for r in rows if r['binary'] == b]
    f = lambda k: [float(r[k]) for r in sel if r[k]]
    m = lambda xs: f'{st.mean(xs):.2f} +- {st.stdev(xs) if len(xs) > 1 else 0:.2f}'
    print(f'{b:24s} wall {m(f("wall_s"))} s  user {m(f("user_s"))} s  maxRSS {m([x / 1024 for x in f("maxrss_kb")])} MB  sim {m(f("sim_ns"))} ns')
EOF
