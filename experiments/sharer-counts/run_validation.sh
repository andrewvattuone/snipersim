#!/bin/bash
# Run the sharer-count validation suite and check every result.
#
# Usage: experiments/sharer-counts/run_validation.sh [--long] [PREFIX]
#   PREFIX  name prefix for the result directories under results/ (default: sharers)
#   --long  also run FFT -m 16 with the default 8 MB L3 (many L2 victims) and with
#           a 512 KB L3 (real multi-sharer L3 victims); ~25 min more
# Environment: JOBS     simulations run in parallel (default 4)
#              MODES    sharetest modes (default "r w p u")
#              L3_SIZES L3 sizes in KB (default "1024 512")
#              FFT      0 to skip the FFT runs (default 1)
#
# Suite (all 4-core Gainestown):
#   FFT -p 4 -m 10 (default 8 MB L3; no L3 evictions)
#   sharetest r|w|p|u x K=1..4, L3 = 1 MB and 512 KB (512 KB evicts shared lines
#   from L3 while the L2s still hold them, so multi-sharer L3 victims occur)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
LONG=0
if [ "${1:-}" = "--long" ]; then LONG=1; shift; fi
PREFIX=${1:-sharers}
JOBS=${JOBS:-4}
MODES=${MODES:-r w p u}
L3_SIZES=${L3_SIZES:-1024 512}
FFT=${FFT:-1}

make -s -C "$HERE" sharetest || exit 1
cd "$ROOT" || exit 1

run() { # NAME CMD...
   local name=$1; shift
   ./run-sniper -n 4 -c gainestown -d "results/$PREFIX-$name" "$@" > "results/$PREFIX-$name.log" 2>&1
   echo "$name exit=$?"
}

declare -a RUNS=()
# Long runs first, so they overlap with the short ones
[ $LONG = 1 ] && [ "$FFT" = 1 ] && RUNS+=("fft-m16|-- ./test/fft/fft -p 4 -m 16 -t"
                        "fft-m16-l3-512k|-g perf_model/l3_cache/cache_size=512 -- ./test/fft/fft -p 4 -m 16 -t")
[ "$FFT" = 1 ] && RUNS+=("fft-m10|-- ./test/fft/fft -p 4 -m 10 -t")
for l3 in $L3_SIZES; do
   for mode in $MODES; do
      for k in 1 2 3 4; do
         RUNS+=("l3-${l3}k-$mode-K$k|-g perf_model/l3_cache/cache_size=$l3 -- $HERE/sharetest $mode $k")
      done
   done
done

mkdir -p results
dirs=()
for r in "${RUNS[@]}"; do
   name=${r%%|*}; args=${r#*|}
   dirs+=("results/$PREFIX-$name")
   # shellcheck disable=SC2086
   run "$name" $args &
   while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
done
wait

grep -l "CACHEDLOCS MISMATCH\|ERROR\|ssertion" results/$PREFIX-*.log && echo "^^ logs with errors or mismatch reports"
python3 "$HERE/check_sharers.py" -q "${dirs[@]}"
