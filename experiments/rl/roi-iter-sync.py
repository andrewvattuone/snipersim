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
# boundary therefore moves by about ins_global instructions or less after its marker (up to 1.05 M seen); the shift
# is printed. Verified: fast-forward -> detailed at such a point and detailed -> fast-forward at the end pass every
# checker for LU tiny with all five policies. Not separately tested: whether roi-iter.py's immediate switch alone
# also breaks them.
import sim


class RoiIterSync:
  def setup(self, args):
    args = dict(enumerate((args or '').split(':')))
    self.start_warmup = int(args.get(0, '') or -1)
    self.start_detailed = int(args.get(1, '') or -1)
    self.end_detailed = int(args.get(2, '') or -1)
    self.snapshot = int(args.get(3, '') or -1)
    self.state = 'init'
    self.pending = []        # queued (action, iteration, icount at the marker), applied in order
    print('[ROIITERSYNC] warm-up from iteration %d, detailed from iteration %d to the end of iteration %d'
          % (self.start_warmup, self.start_detailed, self.end_detailed))

  def hook_magic_marker(self, core, thread, a, b, s):
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
    shift = sim.stats.icount() - at
    if action == 'warmup':
      print('[ROIITERSYNC] Start of iteration %d: going to WARMUP (%d instructions after the marker)' % (b, shift))
      sim.control.set_instrumentation_mode(sim.control.WARMUP)
      self.state = 'warmup'
    elif action == 'snapshot':
      print('[ROIITERSYNC] Start of iteration %d: statistics snapshot iter-%d-begin (%d instructions after the marker)'
            % (b, b, shift))
      sim.stats.write('iter-%d-begin' % b)
    elif action == 'detailed':
      print('[ROIITERSYNC] Start of iteration %d: beginning ROI (%d instructions after the marker)' % (b, shift))
      sim.control.set_roi(True)
      self.state = 'detailed'
    else:
      print('[ROIITERSYNC] End of iteration %d: ending ROI (%d instructions after the marker)' % (b, shift))
      sim.control.set_roi(False)
      self.state = 'done'


sim.util.register(RoiIterSync())
