#ifndef RL_WRITE_ORACLE_H
#define RL_WRITE_ORACLE_H

/*
 * Oracle record of data writes per cache line, for the Stage 4 RL diagnostics only (docs/rl/POLICY_CORES.md).
 *
 * It answers two questions no cache could answer locally:
 *   writtenByOtherSince   did another core write the line after this cache dropped it?  (audit I-2: a later miss
 *                         that the policy blames on its eviction would have been a coherence miss anyway)
 *   writtenBySince        did this core write the line since it was filled?            (audit I-3: an M victim that
 *                         was granted M on a read and never written locally)
 *
 * Every data store is recorded where the L1 performs it (CacheCntlr::accessCache, after write permission was
 * obtained), with a global sequence number; a cache reads now() when a line leaves or arrives. The record is never
 * used by a replacement decision, changes no simulator state and charges no time. It is inactive (one relaxed load
 * per store) unless a cache with rl_diag/enabled = true called enable().
 *
 * Writes in Sniper need M, and M excludes every other copy, so for a line that this cache held, "a write by another
 * core since t" can only happen after the line left this cache. Per line it keeps the last writer and the last write
 * by any other core, which answers both questions exactly for any number of writers.
 */

#include "fixed_types.h"

#include <atomic>

namespace RlWriteOracle
{
extern std::atomic<bool> g_enabled;

inline bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

// Called by every cache using the oracle; all of them (and the L1s that record writes) must use one line size
void enable(UInt32 line_size);

// A data store by `core` to the line containing `address`
void noteWrite(IntPtr address, core_id_t core);

// Current sequence number (0 before the first write)
UInt64 now();

// Did a core other than `core` write the line after sequence number `since`?
bool writtenByOtherSince(IntPtr address, core_id_t core, UInt64 since);

// Did `core` write the line after `since`? Only meaningful while `core` holds the line (see above).
bool writtenBySince(IntPtr address, core_id_t core, UInt64 since);
} // namespace RlWriteOracle

#endif /* RL_WRITE_ORACLE_H */
