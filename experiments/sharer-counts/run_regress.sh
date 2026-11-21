#!/bin/bash
# Run the coherence regression test (coherence_regress.c) and check it.
#
# Usage: experiments/sharer-counts/run_regress.sh PREFIX [RUN-SNIPER-ARGS...]
#   default config: -n 2 -c gainestown
# Environment: NLINES (default 256), SNIPER_BIN = a saved Sniper binary to use
#   instead of the current build (copied over build/default/lib/sniper and
#   restored on exit; do not run other simulations at the same time).
# For each of clean/dirty it runs step 3 as write and as read, compares the two
# with check_regress.py, and runs a 1-line traced copy of the "write" case
# (SNIPER_TRACE_VADDR) whose [LINETRACE] output is in results/PREFIX-*-trace.log
# (only builds that have the line tracer print it).
# The write/read runs also count every event of the N regression lines (SNIPER_WATCH_VADDR /
# SNIPER_WATCH_LINES, linewatch.csv), which check_regress.py uses for its per-line check.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PREFIX=$1; shift
ARGS=("$@")
[ ${#ARGS[@]} = 0 ] && ARGS=(-n 2 -c gainestown)
N=${NLINES:-256}

make -s -C "$HERE" coherence_regress || exit 1
cd "$ROOT" || exit 1
mkdir -p results
if [ -n "${SNIPER_BIN:-}" ]; then
   TARGET=build/default/lib/sniper
   SAVED=$(mktemp)
   cp -p "$TARGET" "$SAVED"
   trap 'cp -p "$SAVED" "$TARGET"; rm -f "$SAVED"' EXIT
   cp "$SNIPER_BIN" "$TARGET"
fi
VADDR=$(nm "$HERE/coherence_regress" | awk '$3 == "regress_lines" {print "0x" $1}')

for kind in clean dirty; do
   for step3 in write read; do
      SNIPER_WATCH_VADDR=$VADDR SNIPER_WATCH_LINES=$N \
      ./run-sniper "${ARGS[@]}" -d "results/$PREFIX-$kind-$step3" -- "$HERE/coherence_regress" $kind $step3 "$N" \
         > "results/$PREFIX-$kind-$step3.log" 2>&1 &
   done
   SNIPER_TRACE_VADDR=$VADDR ./run-sniper "${ARGS[@]}" -d "results/$PREFIX-$kind-trace" -- "$HERE/coherence_regress" $kind write 1 \
      > "results/$PREFIX-$kind-trace.log" 2>&1 &
done
wait

status=0
for kind in clean dirty; do
   python3 "$HERE/check_regress.py" "results/$PREFIX-$kind-write" "results/$PREFIX-$kind-read" "$N" || status=1
done
exit $status
