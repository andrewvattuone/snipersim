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
 *   Fill                   A block is installed:
 *     demand               a miss (or prefetch); history lookup and the -1 "bad eviction" feedback apply
 *     upgrade_refill       the same block re-inserted after an upgrade invalidation; carries its metadata over,
 *                          produces no feedback
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
