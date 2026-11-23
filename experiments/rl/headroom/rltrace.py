"""Reader for the headroom-diagnostic trace rl-trace.bin (docs/rl/HEADROOM.md).

The record format is defined in common/core/memory_subsystem/cache/rl/rl_trace_core.h: fixed 16-byte little-endian
records (type u8, core u8, a u8, b u8, aux u32, tag u64) in the order the simulator produced them. The record index
is the time axis of every offline analysis.
"""

import hashlib
import json
import mmap
import os
import re
import struct
import subprocess
import sys

RECORD = struct.Struct('<BBBBIQ')  # type, core, a, b, aux, tag
assert RECORD.size == 16

T_HEADER, T_ACCESS, T_WAY, T_DECISION, T_FILL, T_INVAL, T_L1, T_ROI, T_END = range(9)
TYPE_NAMES = ('header', 'access', 'way', 'decision', 'fill', 'inval', 'l1', 'roi', 'end')
MAGIC = 0x3145434152544C52
SCHEMA_VERSION = 1
NO_WAY = 0xFF
NO_TAG = (1 << 64) - 1

# access flags (field a)
ACC_HIT, ACC_STORE, ACC_FROM_L1I, ACC_PREFETCH, ACC_READEX, ACC_OVERLAP = 1, 2, 16, 32, 64, 128
ACC_PRE_SHIFT = 2
# l1 flags (field b)
L1_STORE, L1_EPOCH_FIRST, L1_STORE_FILTER = 1, 2, 4
# states
S_I, S_S, S_E, S_M = 0, 1, 2, 3
STATE_NAMES = 'ISEM'
# fill kinds and invalidation reasons (CacheSetEvent)
FILL_DEMAND, FILL_UPGRADE = 0, 1
INV_UNCLASSIFIED, INV_COHERENCE, INV_BACK, INV_UPGRADE = 0, 1, 2, 3
INV_NAMES = ('unclassified', 'coherence', 'back-invalidation', 'upgrade')


class TraceError(Exception):
    pass


def encode(records):
    """Bytes of a list of (type, core, a, b, aux, tag) tuples (used by the synthetic tests)."""
    return b''.join(RECORD.pack(*r) for r in records)


class Trace:
    """A trace: its records (a list of (type, core, a, b, aux, tag) tuples), geometry, sampling and sidecar."""

    def __init__(self, path=None, data=None):
        if data is None:
            self.path = os.path.abspath(path)
            with open(self.path, 'rb') as f:
                with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                    data = m[:]
        else:
            self.path = path or '<memory>'
        if len(data) % RECORD.size:
            raise TraceError(f'{self.path}: size is not a multiple of {RECORD.size} bytes')
        self.rec = list(RECORD.iter_unpack(data))
        r = self.rec
        if len(r) < 3 or r[0][0] != T_HEADER or r[0][5] != MAGIC:
            raise TraceError(f'{self.path}: not an rl-trace file (bad magic)')
        self.version = r[0][4]
        if self.version != SCHEMA_VERSION:
            raise TraceError(f'{self.path}: schema version {self.version}, reader supports {SCHEMA_VERSION}')
        self.cores, self.ways = r[0][2], r[0][3]
        if r[1][0] != T_HEADER or r[1][1] != 1:
            raise TraceError(f'{self.path}: second header record missing')
        self.log_block, self.sets = r[1][2], r[1][4]
        self.modulus, self.residue = r[1][5] >> 32, r[1][5] & 0xFFFFFFFF
        self.complete = r[-1][0] == T_END and r[-1][5] == len(r) - 1
        self.sidecar = None
        if data is not None and path:
            js = os.path.splitext(os.path.abspath(path))[0] + '.json'
            if os.path.exists(js):
                with open(js) as f:
                    self.sidecar = json.load(f)

    def __len__(self):
        return len(self.rec)

    def sampled_sets(self):
        return [s for s in range(self.sets) if s % self.modulus == self.residue]

    def roi(self):
        """(begin, end) record indices of the region of interest: after the first ROI-begin marker, before the last
        ROI-end marker; the whole trace if there are no markers."""
        begins = [i for i, x in enumerate(self.rec) if x[0] == T_ROI and x[2] == 1]
        ends = [i for i, x in enumerate(self.rec) if x[0] == T_ROI and x[2] == 0]
        if not begins and not ends:
            return 0, len(self.rec)
        return (begins[0] if begins else 0), (ends[-1] if ends else len(self.rec))

    def counts(self):
        c = [0] * len(TYPE_NAMES)
        for x in self.rec:
            c[x[0]] += 1
        return {TYPE_NAMES[k]: v for k, v in enumerate(c) if v}


def sha256(path, block=1 << 22):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(block)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def provenance(argv, inputs):
    """Provenance lines for an output: command, tool source checksums, tool tree commit, inputs with checksums."""
    here = os.path.dirname(os.path.abspath(__file__))
    lines = ['# command: ' + ' '.join(argv)]
    for f in sorted(os.listdir(here)):
        if f.endswith('.py'):
            lines.append(f'# tool {f} sha256 {sha256(os.path.join(here, f))}')
    try:
        rev = subprocess.run(['git', '-C', here, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(['git', '-C', here, 'status', '--porcelain', '--', here], capture_output=True, text=True).stdout.strip()
        lines.append(f'# tool tree {rev}{" (tool directory modified)" if dirty else ""}')
    except OSError:
        pass
    for p in inputs:
        lines.append(f'# input {p} sha256 {sha256(p)} bytes {os.path.getsize(p)}')
    lines.append(f'# python {sys.version.split()[0]}')
    return lines


def load_stats(run_dir):
    """sim.stats of a run directory (the ROI region by default), as {name: [per-core values]}."""
    path = os.path.join(run_dir, 'sim.stats')
    if not os.path.exists(path):
        path = os.path.join(run_dir, 'simulation', 'sim.stats')
    s = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'^(\S+)\s*=\s*(.*)$', line.strip())
            if m:
                try:
                    s[m.group(1)] = [float(x) for x in m.group(2).split(',')]
                except ValueError:
                    pass
    return s


def trace_tallies(tr, start=None, stop=None):
    """Per-core tallies of the recorded run over [start, stop) (default: the ROI), in the simulator's statistic units:
    the counted L2 accesses and misses of the sampled sets, and the fills, decisions and invalidations.

    load_misses / store_misses are Sniper's statistic definitions: a store hit whose line is still being filled
    (MSHR overlap) counts as a store miss. contents_misses counts only accesses that found the line absent or without
    permission, the definition a shadow cache can reproduce; the difference is store_overlap_misses."""
    b, e = tr.roi()
    start = b if start is None else start
    stop = e if stop is None else stop
    keys = ('loads', 'stores', 'load_misses', 'store_misses', 'store_misses_upgrade', 'store_overlap_misses', 'contents_misses',
            'fill_demand', 'fill_upgrade', 'decisions',
            'inv_unclassified', 'inv_coherence', 'inv_back-invalidation', 'inv_upgrade', 'l1')
    out = {c: dict.fromkeys(keys, 0) for c in range(tr.cores)}
    for t, c, a, bb, aux, tag in tr.rec[start:stop]:
        o = out[c]
        if t == T_ACCESS:
            if a & ACC_PREFETCH:
                continue
            store, hit = a & ACC_STORE, a & ACC_HIT
            o['stores' if store else 'loads'] += 1
            if not hit:
                o['contents_misses'] += 1
                o['store_misses' if store else 'load_misses'] += 1
                if store and ((a >> ACC_PRE_SHIFT) & 3) in (S_S, S_E):
                    o['store_misses_upgrade'] += 1
            elif store and a & ACC_OVERLAP:
                o['store_misses'] += 1
                o['store_overlap_misses'] += 1
        elif t == T_FILL:
            o['fill_upgrade' if bb & 1 else 'fill_demand'] += 1
        elif t == T_DECISION:
            o['decisions'] += 1
        elif t == T_INVAL:
            o['inv_' + INV_NAMES[bb]] += 1
        elif t == T_L1:
            o['l1'] += 1
    return out


# sim.stats name of each tally (L2 per-core counters; rl-event-* exist with SNIPER_RL_EVENT_CHECK=1)
STAT_OF_TALLY = {
    'loads': 'loads', 'stores': 'stores', 'load_misses': 'load-misses', 'store_misses': 'store-misses',
    'fill_demand': 'rl-event-fill-demand', 'fill_upgrade': 'rl-event-fill-upgrade-refill', 'decisions': 'rl-event-eviction',
    'inv_coherence': 'rl-event-inv-coherence', 'inv_back-invalidation': 'rl-event-inv-back-invalidation',
    'inv_upgrade': 'rl-event-inv-upgrade', 'inv_unclassified': 'rl-event-inv-unclassified',
}


def check_against_stats(tr, stats, cache='L2'):
    """Compare the recorded tallies with the simulator's statistics. Exact equality is expected when every set is
    traced (set_modulus = 1); with sampling the trace holds the sampled sets only. Returns (rows, all_equal)."""
    tal = trace_tallies(tr)
    rows, ok = [], True
    for core in range(tr.cores):
        for k, stat in STAT_OF_TALLY.items():
            name = f'{cache}.{stat}'
            if name not in stats:
                continue
            v = stats[name][core] if core < len(stats[name]) else 0
            got = tal[core][k]
            up = None
            if k == 'store_misses':
                up = sum(stats.get(f'{cache}.store-misses-{x}', [0] * (core + 1))[core] for x in 'SE')
            rows.append((core, k, got, int(v), got == int(v)))
            ok &= got == int(v)
            if up is not None:
                rows.append((core, 'store_misses_upgrade', tal[core]['store_misses_upgrade'], int(up), tal[core]['store_misses_upgrade'] == int(up)))
                ok &= tal[core]['store_misses_upgrade'] == int(up)
    return rows, ok
