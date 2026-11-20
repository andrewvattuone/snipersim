#ifndef RL_EVENTS_H
#define RL_EVENTS_H

/*
 * Cache events the replacement cores distinguish. The Stage 2 Sniper adapter must classify every fill and
 * invalidation it reports; getting this wrong would turn coherence activity into replacement feedback.
 *
 *   Replacement eviction   A victim chosen by the policy is replaced by a fill (CacheSet::insert). Reported with
 *                          on_eviction(); the only event that adds an eviction-history entry.
 *   Invalidation           A block leaves without a replacement decision. Never an eviction, never in the history:
 *     coherence            another core's write (directory INV / FLUSH, Transition::COHERENCY)
 *     back_invalidation    an inclusive lower level evicted the line (Transition::BACK_INVAL)
 *     upgrade              this cache drops its own copy to obtain write permission (Transition::UPGRADE, and the
 *                          self-invalidations before forwarding a write); the block comes back with an upgrade_refill
 *   Miss                   A demand miss (or prefetch) whose block is about to be filled. Reported with on_miss(tag)
 *                          BEFORE any victim is chosen for that fill: the history lookup, the removal of a returning
 *                          block's entry and its -1 "bad eviction" feedback happen here, as at ChampSim's tag check.
 *   Fill                   A block is installed (after the eviction, if any):
 *     demand               the fill completing a miss already reported with on_miss
 *     upgrade_refill       the same block re-inserted after an upgrade invalidation; no on_miss, carries its
 *                          metadata over, produces no feedback
 *
 * Order for one miss:  on_miss -> [choose -> commit -> on_eviction(victim)] -> on_fill
 * Reporting the miss after the eviction would let the victim's history insertion age out (+1) an entry that the
 * returning block was about to free, or age out the returning block's own entry and lose its -1.
 *
 * Write-backs from an upper level (update_replacement = false in Sniper) and accesses that do not update
 * recency are not reported at all.
 */

#include <string_view>

namespace rlrepl
{
enum class InvalidationReason { coherence, back_invalidation, upgrade };
enum class FillKind { demand, upgrade_refill };

inline constexpr std::string_view name(InvalidationReason r)
{
  switch (r) {
  case InvalidationReason::coherence:
    return "coherence";
  case InvalidationReason::back_invalidation:
    return "back_invalidation";
  case InvalidationReason::upgrade:
  default:
    return "upgrade";
  }
}
} // namespace rlrepl

#endif
