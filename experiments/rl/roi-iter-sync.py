# Iteration-bounded ROI for the Stage 6 runner (run_stage6.py), a variant of Sniper's scripts/roi-iter.py.
#
#   run-sniper --roi-script --no-cache-warming -s experiments/rl/roi-iter-sync:A:B:C[:D]
#
# The program marks iteration i with SimMarker(1, i) at its start and SimMarker(2, i) at its end (capbench-roi.patch).
# Fast-forward until iteration A starts, cache warm-up from there, detailed simulation from the start of iteration B
# to the end of iteration C, then fast-forward to the end of the program. A empty: no warm-up (fast-forward straight
# into detailed). D (optional, B < D <= C): also write a statistics snapshot `iter-D-begin` at the start of
# iteration D, so that the iterations D..C can be measured alone (dumpstats --partial iter-D-begin:roi-end) while the
# earlier detailed iterations warm the caches.
#
# Use A empty. Switching from cache warm-up to detailed in the middle of a parallel region (here, or with Sniper's own
# stop-by-icount N:roi+M) leaves the L1-D sharer bookkeeping and the L2 paired counters a few events apart in the
# roi-begin..roi-end statistics, and the Stage 6 checkers fail; fast-forward -> detailed at the same point passes.
#
# Difference from roi-iter.py: a marker only queues a mode switch; the switch itself happens at the next periodic
# instruction callback (core/hook_periodic_ins, every ins_global = 1 M instructions summed over all cores), the
# same point at which Sniper's stop-by-icount switches, rather than inside one thread's marker instruction. Each
# boundary therefore lies up to about ins_global instructions after its marker.
#
# Every switch is logged on one line that run_stage6.py records in the manifest (roi_switches):
#   [ROIITERSYNC] switch <action> iteration <i> marker-icount <m> switch-icount <s> offset <s - m>
# (action: warmup, detailed, snapshot, end; icount = instructions summed over all cores since the start), and at the
# end of the simulation every marker seen is summarized, with any departure from the order
# SimMarker(1, 1), SimMarker(2, 1), SimMarker(1, 2), ... :
#   [ROIITERSYNC] markers begin <n1> end <n2> first <i> last <j> order-violations <k>
import sim


class RoiIterSync:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    self.start_warmup = int(args.get(0, '') or -1)
    self.start_detailed = int(args.get(1, '') or -1)
    self.end_detailed = int(args.get(2, '') or -1)
    self.snapshot = int(args.get(3, '') or -1)
    self.state = 'init'
    self.pending = []        # queued (action, iteration, icount at the marker, state after), applied in order
    self.seen = {1: 0, 2: 0}
    self.first = self.last = None
    self.expect = (1, None)  # next marker expected: (kind, iteration); None = any iteration
    self.violations = 0
    print('[ROIITERSYNC] warm-up from iteration %d, detailed from iteration %d to the end of iteration %d, snapshot at %d'
          % (self.start_warmup, self.start_detailed, self.end_detailed, self.snapshot))

  def hook_magic_marker(self, core, thread, a, b, s):
    if a not in (1, 2):
      return
    self.seen[a] += 1
    self.first = b if self.first is None else self.first
    self.last = b
    kind, it = self.expect
    if a != kind or (it is not None and b != it):
      self.violations += 1
      if self.violations <= 5:
        print('[ROIITERSYNC] marker order: got SimMarker(%d, %d), expected SimMarker(%d, %s)' % (a, b, kind, it))
    self.expect = (2, b) if a == 1 else (1, b + 1)

    # The state the queued switches will leave, so that a marker arriving before the next callback is still seen
    state = self.pending[-1][3] if self.pending else self.state
    at = sim.stats.icount()
    if state == 'done':
      return
    if a == 2 and b == self.end_detailed and state == 'detailed':
      self.pending.append(('end', b, at, 'done'))
    elif a == 1 and b == self.snapshot and state == 'detailed':
      self.pending.append(('snapshot', b, at, 'detailed'))
    elif a == 1 and b == self.start_detailed and state != 'detailed':
      self.pending.append(('detailed', b, at, 'detailed'))
    elif a == 1 and b == self.start_warmup and state == 'init':
      self.pending.append(('warmup', b, at, 'warmup'))

  def hook_periodic_ins(self, icount):
    while self.pending:
      self.apply(*self.pending.pop(0))

  def apply(self, action, b, at, _):
    now = sim.stats.icount()
    print('[ROIITERSYNC] switch %s iteration %d marker-icount %d switch-icount %d offset %d' % (action, b, at, now, now - at))
    if action == 'warmup':
      sim.control.set_instrumentation_mode(sim.control.WARMUP)
      self.state = 'warmup'
    elif action == 'snapshot':
      sim.stats.write('iter-%d-begin' % b)
    elif action == 'detailed':
      sim.control.set_roi(True)
      self.state = 'detailed'
    else:
      sim.control.set_roi(False)
      self.state = 'done'

  def hook_sim_end(self):
    if self.pending:
      print('[ROIITERSYNC] %d switches still queued at the end: %s' % (len(self.pending), [p[:2] for p in self.pending]))
    print('[ROIITERSYNC] markers begin %d end %d first %s last %s order-violations %d'
          % (self.seen[1], self.seen[2], self.first, self.last, self.violations))


sim.util.register(RoiIterSync())
