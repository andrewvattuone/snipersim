#!/bin/bash
# Build the CAP Bench x86 (OpenMP) kernels with Sniper ROI and iteration markers, OUTSIDE this tree.
#
# Usage: experiments/rl/build_capbench.sh [SRC_DIR [OUT_DIR]]
#   SRC_DIR  CAP Bench checkout (default: ../capbench next to this Sniper tree)
#   OUT_DIR  build directory (default: ../capbench-build-<first 8 hex digits of the patch md5>); must not exist yet.
#            The default name ties a build to the patch it was made with; run_stage6.py looks for that name.
#
# Source: https://github.com/cart-pucminas/CAPBenchmarks at revision $REV. If SRC_DIR does not exist it is cloned and
# checked out at $REV; an existing SRC_DIR at another revision, or with modified tracked files, is refused, not changed.
# The checkout itself is never modified: x86/ at $REV is exported to OUT_DIR, capbench-roi.patch is applied there,
# and the kernels are built there (OUT_DIR/bin/<kernel>.intel). Nothing is installed and no Sniper is built.
# docs/rl/CAPBENCH.md describes the patch (ROI boundaries, iteration markers, the `paper` input class).
#
# Flags: the repository's (x86/makefile: gcc -Wall -Wextra -Werror -O3 -fopenmp) without -Werror. GCC 11 reports
# two upstream warnings (LU lower_upper.c:93, TSP tsp.c:246; analysed in CAPBENCH.md). They are reported, not
# fixed, so the code is the code the authors published.
set -euo pipefail
REV=ceb6f53bfffcf48dacd9e6e0078ada1edac567f2
SNIPER_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PATCH=$SNIPER_ROOT/experiments/rl/capbench-roi.patch
PATCH_MD5=$(md5sum "$PATCH" | cut -c1-32)
SRC=${1:-$(dirname "$SNIPER_ROOT")/capbench}
OUT=${2:-$(dirname "$SNIPER_ROOT")/capbench-build-${PATCH_MD5:0:8}}
CFLAGS_UPSTREAM="-Wall -Wextra -O3"
if [ ! -e "$SRC" ]; then
  git clone https://github.com/cart-pucminas/CAPBenchmarks.git "$SRC"
  git -C "$SRC" checkout --quiet --detach "$REV"
fi
have=$(git -C "$SRC" rev-parse HEAD)
[ "$have" = "$REV" ] || { echo "CAP Bench at $have, expected $REV; not changing an existing checkout" >&2; exit 1; }
[ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ] || { echo "tracked files modified in $SRC" >&2; exit 1; }
[ ! -e "$OUT" ] || { echo "$OUT exists; not overwriting a build (remove it or choose another OUT_DIR)" >&2; exit 1; }
case "$(cd "$(dirname "$OUT")" && pwd)/" in "$SNIPER_ROOT"/*) echo "OUT_DIR must be outside $SNIPER_ROOT" >&2; exit 1;; esac

mkdir -p "$OUT/bin"
OUT=$(cd "$OUT" && pwd)
git -C "$SRC" archive "$REV" x86 | tar -x -C "$OUT"
patch -d "$OUT" -p0 --quiet < "$PATCH"
make -C "$OUT/x86" all BINDIR="$OUT/bin" CFLAGS="$CFLAGS_UPSTREAM -I $OUT/x86/include -I $SNIPER_ROOT/include" \
  >"$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log" >&2; exit 1; }
echo "warnings:"; grep -E "warning:" "$OUT/build.log" | sed "s#$OUT/##" || true
{ echo "CAP Bench $REV + capbench-roi.patch $PATCH_MD5"
  echo "$(gcc --version | head -1); CFLAGS $CFLAGS_UPSTREAM -fopenmp (+ include paths); sim_api.h md5 $(md5sum "$SNIPER_ROOT/include/sim_api.h" | cut -c1-32)"
  (cd "$OUT/bin" && md5sum fast.intel fn.intel gf.intel is.intel km.intel lu.intel tsp.intel); } | tee "$OUT/BUILD_INFO"
chmod -R a-w "$OUT"
