#ifndef RL_TRACE_H
#define RL_TRACE_H

/*
 * Headroom-diagnostic trace of one cache level (docs/rl/HEADROOM.md), opt-in and observation only:
 *
 *   -g perf_model/l2_cache/rl_trace/enabled=true      (default false)
 *   -g perf_model/l2_cache/rl_trace/set_modulus=8      trace set s iff s mod set_modulus == set_residue (default 8)
 *   -g perf_model/l2_cache/rl_trace/set_residue=0      (default 0)
 *
 * Every cache of that configuration (one L2 per core) records, for its sampled sets, the counted accesses, the
 * replacement decisions with every way's state, the fills and the invalidations; the L1 controllers record the L1
 * accesses of lines in those sets that the offline model needs (rl/rl_trace_core.h, L1Filter). Output:
 * rl-trace.bin (records, rl/rl_trace_core.h) and rl-trace.json (geometry, sampling, record counts) in the output
 * directory. The trace is never read by the simulator: it changes no replacement decision, coherence state or timing.
 * Disabled (the default), each hook costs one relaxed atomic load.
 */

#include "cache_base.h"
#include "cache_set_events.h"
#include "cache_state.h"
#include "fixed_types.h"

#include <atomic>

class CacheSet;

namespace RlTrace
{
extern std::atomic<bool> g_enabled;

inline bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

// Called by every non-TLB Cache at construction. Returns true if this cache is traced (cfgname/rl_trace/enabled);
// the first traced cache opens the trace, the others must have the same configuration and geometry.
bool configure(String cfgname, String name, core_id_t core_id, UInt32 num_sets, UInt32 associativity, UInt32 blocksize,
               CacheBase::hash_t hash, String replacement_policy);

// Is set `set` of the traced caches sampled?
bool sampled(UInt32 set);

// Hooks of a traced Cache (caller checks sampled(set)). `set_obj` is the set before the event.
void access(core_id_t core, UInt32 set, CacheSet &set_obj, IntPtr tag, bool store, bool read_ex, bool hit, CacheState::cstate_t pre,
            bool from_l1i, bool prefetch, bool overlap);

// Decision-time view of a set, taken by beforeInsert and consumed by afterInsert of the same insert
struct InsertView
{
   bool full = false;
   UInt32 ranks = 0;
   UInt32 eligible = 0;
};
InsertView beforeInsert(core_id_t core, UInt32 set, CacheSet &set_obj);
void afterInsert(core_id_t core, UInt32 set, CacheSet &set_obj, const InsertView &view, IntPtr tag, CacheSetEvent::fill_t fill,
                 bool eviction, IntPtr victim_tag);
void beforeInvalidate(core_id_t core, UInt32 set, CacheSet &set_obj, IntPtr tag, CacheSetEvent::invalidation_t reason);

// Hook of every L1 controller (CacheCntlr::accessCache): an L1 access by `core` to the line containing `address`
void noteL1Access(IntPtr address, core_id_t core, bool l1d, bool store);
} // namespace RlTrace

#endif /* RL_TRACE_H */
