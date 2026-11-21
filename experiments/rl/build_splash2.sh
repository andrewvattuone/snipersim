#!/bin/bash
# Build the SPLASH-2 programs used by run_stage6.py from the Sniper benchmarks repository, OUTSIDE this tree.
#
# Usage: experiments/rl/build_splash2.sh [BENCH_DIR]   (default: ../sniper-benchmarks next to this Sniper tree)
#
# Source: https://github.com/snipersim/benchmarks at revision $REV. If BENCH_DIR does not exist it is cloned and
# checked out at $REV; an existing BENCH_DIR at another revision, or with modified tracked files, is refused, not changed.
# Only the SPLASH-2 ROI hooks library and Radix, LU (contiguous blocks), Ocean (contiguous partitions) and Barnes
# are built. Nothing is installed, and no Sniper is built: the hooks are compiled against THIS tree's
# include/sim_api.h, and the programs are run with this tree's run-sniper (run_stage6.py), not with the
# repository's own run-sniper.
#
# Compatibility changes, none of them to a tracked source file:
# * The repository's build helpers (tools/hooks/makebuildscripts.py, env_setup_bm.py, run-sniper, splashrun) are
#   Python 2, which this host does not have. The generated tools/hooks/buildconf.{makefile,sh,py} are written by
#   a Python 3 rendering of makebuildscripts.py below (identical flags), and each program's makefile is invoked
#   directly with the flags of splash2/splash2/codes/Makefile.
# * LU uses PAGE_SIZE, which current glibc headers no longer define: -DPAGE_SIZE=4096 (the host page size).
set -euo pipefail
REV=ba6004fd2b251be0ee5fc9e5d77374f1d4c80317
SNIPER_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BENCH=${1:-$(dirname "$SNIPER_ROOT")/sniper-benchmarks}
if [ ! -e "$BENCH" ]; then
  # A new clone is checked out at the pinned revision; an existing checkout is never changed
  git clone https://github.com/snipersim/benchmarks.git "$BENCH"
  git -C "$BENCH" checkout --quiet --detach "$REV"
fi
BENCH=$(cd "$BENCH" && pwd)
have=$(git -C "$BENCH" rev-parse HEAD)
[ "$have" = "$REV" ] || { echo "benchmarks at $have, expected $REV; not changing an existing checkout (git -C $BENCH checkout $REV)" >&2; exit 1; }
[ -z "$(git -C "$BENCH" status --porcelain --untracked-files=no)" ] || { echo "tracked files modified in $BENCH" >&2; exit 1; }
export BENCHMARKS_ROOT=$BENCH SNIPER_ROOT GRAPHITE_ROOT=$SNIPER_ROOT

H=$BENCH/tools/hooks
( cd "$H" && python3 -I - <<'PY'
# Python 3 rendering of tools/hooks/makebuildscripts.py (same flags, file() -> open())
import os
flags = [
  ('HOOKS_DIR', '${BENCHMARKS_ROOT}/tools/hooks'),
  ('HOOKS_CC', 'gcc'),
  ('HOOKS_CXX', 'g++'),
  ('HOOKS_FC', 'gfortran'),
  ('LD', 'g++'),
  ('HOOKS_CFLAGS', '-I${HOOKS_DIR} -I${GRAPHITE_ROOT}/include'),
  ('HOOKS_CXXFLAGS', '${HOOKS_CFLAGS}'),
  ('HOOKS_LDFLAGS', '-uparmacs_roi_end -uparmacs_roi_start -L${HOOKS_DIR} -lhooks_base -lrt -pthread'),
  ('HOOKS_LDFLAGS_NOROI', '-uparmacs_roi_end -uparmacs_roi_start -L${HOOKS_DIR} -lhooks_base_noroi -lrt -pthread'),
  ('HOOKS_LDFLAGS_DYN', '-uparmacs_roi_end -uparmacs_roi_start -L${HOOKS_DIR} -lhooks_base -lrt -pthread'),
  ('HOOKS_LD_LIBRARY_PATH', ''),
]
message = '# This file is auto-generated (by snipersim experiments/rl/build_splash2.sh, from makebuildscripts.py).'
open('buildconf.sh', 'w').write('\n'.join([message, ''] + ['%s="%s"' % i for i in flags]) + '\n')
open('buildconf.makefile', 'w').write('\n'.join([message, ''] + ['%s:=%s' % i for i in flags]) + '\n')
open('buildconf.py', 'w').write('\n'.join([message, '', str(dict(flags))]) + '\n')
PY
)
make -C "$H" libhooks_base.a libhooks_base_noroi.a

C=$BENCH/splash2/splash2/codes
# As splash2/splash2/codes/Makefile: CFLAGS = -g -O3 -I. -Ipthreads ${HOOKS_CFLAGS}, LDFLAGS = -lm -pthread ${HOOKS_LDFLAGS}
CFLAGS="-g -O3 -I$C -I$C/pthreads -I$H -I$SNIPER_ROOT/include"
LDFLAGS="-lm -pthread -uparmacs_roi_end -uparmacs_roi_start -L$H -lhooks_base -lrt -pthread"
for d in kernels/radix kernels/lu/contiguous_blocks apps/ocean/contiguous_partitions apps/barnes; do
  extra=""
  [ "$d" = kernels/lu/contiguous_blocks ] && extra="-DPAGE_SIZE=4096"
  make -C "$C/$d" all CC=gcc LD=gcc CFLAGS="$CFLAGS $extra" LDFLAGS="$LDFLAGS" MACROS="$C/pthreads/c.m4.null.POSIX" TARGET_ARCH=
done
echo "benchmarks $REV, $(gcc --version | head -1), $(m4 --version | head -1)"
md5sum "$C/kernels/radix/RADIX" "$C/kernels/lu/contiguous_blocks/LU" "$C/apps/ocean/contiguous_partitions/OCEAN" "$C/apps/barnes/BARNES"
