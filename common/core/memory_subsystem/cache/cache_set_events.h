#ifndef CACHE_SET_EVENTS_H
#define CACHE_SET_EVENTS_H

/*
 * Classification of cache fills and invalidations, for replacement policies that learn from cache events (the RL
 * policies, docs/rl/POLICY_CORES.md). The cache controller (CacheCntlr) classifies each event; Cache passes it down
 * to the CacheSet hooks (cache_set.h), which are no-ops for every existing replacement policy.
 *
 * The classification is from the point of view of the cache whose line changes:
 *
 *   DEMAND_FILL            a miss in this cache is being filled. The set reports onMiss(tag) BEFORE it selects a
 *                          victim, then onEviction and onFill.
 *   UPGRADE_REFILL         the line this cache dropped with INV_UPGRADE is re-inserted with write permission. Not a
 *                          miss: no onMiss.
 *
 *   INV_UPGRADE            this cache drops its own copy because its own write is not permitted in S/E (Sniper has no
 *                          silent S/E -> M at a private level); an UPGRADE_REFILL of the same line follows.
 *   INV_COHERENCE          another cache made this copy invalid: another core's write (directory INV/FLUSH, a shared
 *                          level invalidating its other upper caches), a sibling cache's write (CacheCntlr
 *                          Transition::UPGRADE seen from the invalidated cache), or an accelerator write below an L1.
 *   INV_BACK_INVALIDATION  an inclusive lower level evicted the line (Transition::BACK_INVAL / EVICT).
 *   INV_UNCLASSIFIED       a caller that does not classify (TLBs, page-walk caches, other cache models).
 */

namespace CacheSetEvent
{
   enum fill_t
   {
      DEMAND_FILL = 0,
      UPGRADE_REFILL,
      NUM_FILL_KINDS
   };

   enum invalidation_t
   {
      INV_UNCLASSIFIED = 0,
      INV_COHERENCE,
      INV_BACK_INVALIDATION,
      INV_UPGRADE,
      NUM_INVALIDATION_REASONS
   };

   inline const char* fillName(fill_t f)
   {
      return f == UPGRADE_REFILL ? "upgrade-refill" : "demand";
   }

   inline const char* invalidationName(invalidation_t r)
   {
      switch (r)
      {
         case INV_COHERENCE:         return "coherence";
         case INV_BACK_INVALIDATION: return "back-invalidation";
         case INV_UPGRADE:           return "upgrade";
         case INV_UNCLASSIFIED:
         default:                    return "unclassified";
      }
   }
}

#endif /* CACHE_SET_EVENTS_H */
