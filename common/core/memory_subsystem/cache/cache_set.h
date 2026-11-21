#ifndef CACHE_SET_H
#define CACHE_SET_H

#include "fixed_types.h"
#include "cache_block_info.h"
#include "cache_base.h"
#include "cache_set_events.h"
#include "lock.h"
#include "random.h"
#include "log.h"

#include <cstring>

// Per-cache object to store replacement-policy related info (e.g. statistics),
// can collect data from all CacheSet* objects which are per set and implement the actual replacement policy
class CacheSetInfo
{
   public:
      virtual ~CacheSetInfo() {}
};

// Everything related to cache sets
class CacheSet
{
   public:

      static CacheSet* createCacheSet(String cfgname, core_id_t core_id, String replacement_policy, CacheBase::cache_t cache_type, UInt32 associativity, UInt32 blocksize, CacheSetInfo* set_info = NULL, bool is_tlb_set = false);
      static CacheSetInfo* createCacheSetInfo(String name, String cfgname, core_id_t core_id, String replacement_policy, UInt32 associativity);
      static CacheBase::ReplacementPolicy parsePolicyType(String policy);
      static UInt8 getNumQBSAttempts(CacheBase::ReplacementPolicy, String cfgname, core_id_t core_id);

   protected:

      CacheBlockInfo** m_cache_block_info_array;
      char* m_blocks;
      UInt32 m_associativity;
      UInt32 m_blocksize;
      Lock m_lock;
      bool m_is_tlb_set;
      UInt32 inserts;
      UInt32 evictions;
      UInt32 invalidations;
      

   public:

      CacheSet(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, bool is_tlb_set);
      virtual ~CacheSet();

      UInt32 getBlockSize() { return m_blocksize; }
      UInt32 getAssociativity() { return m_associativity; }
      Lock& getLock() { return m_lock; }

      void read_line(UInt32 line_index, UInt32 offset, Byte *out_buff, UInt32 bytes, bool update_replacement);
      void write_line(UInt32 line_index, UInt32 offset, Byte *in_buff, UInt32 bytes, bool update_replacement);
      CacheBlockInfo* find(IntPtr tag, UInt32* line_index = NULL);
      // TLB-aware find that matches both tag AND page_size to avoid false matches
      // between entries with the same tag but different VPNs (e.g., 4KB vs 2MB pages)
      CacheBlockInfo* findTLB(IntPtr tag, int page_size, UInt32* line_index = NULL);
      bool invalidate(IntPtr& tag, CacheSetEvent::invalidation_t reason = CacheSetEvent::INV_UNCLASSIFIED);
      // TLB-aware invalidate that matches both tag AND page_size
      bool invalidateTLB(IntPtr tag, int page_size);
      void insert(CacheBlockInfo* cache_block_info, Byte* fill_buff, bool* eviction, CacheBlockInfo* evict_block_info, Byte* evict_buff, CacheCntlr *cntlr = NULL,
                  CacheSetEvent::fill_t fill = CacheSetEvent::DEMAND_FILL);

      CacheBlockInfo* peekBlock(UInt32 way) const { return m_cache_block_info_array[way]; }

      char* getDataPtr(UInt32 line_index, UInt32 offset = 0);
      UInt32 getBlockSize(void) const { return m_blocksize; }

      virtual UInt32 getReplacementIndex(CacheCntlr *cntlr) = 0;
      virtual void updateReplacementIndex(UInt32) = 0;

      /*
       * Cache-event hooks for replacement policies that learn from cache events (RL policies, docs/rl/POLICY_CORES.md).
       * No-ops here, so every existing policy is unaffected. Order for one insert():
       *    onMiss(tag)                    demand fill only, before getReplacementIndex selects a victim
       *    getReplacementIndex()          victim selection (as before)
       *    onEviction(way, victim)        a valid block is replaced; `victim` still holds its state and sharer hint
       *    onFill(way, tag, fill)         after the new block is installed (its coherence state is set by the
       *                                   controller afterwards)
       * Hits that update recency keep arriving through updateReplacementIndex(); invalidate() reports
       * onInvalidate(way, tag, reason) before the block is invalidated.
       */
      virtual void onMiss(IntPtr tag) {}
      virtual void onEviction(UInt32 way, const CacheBlockInfo* victim) {}
      virtual void onFill(UInt32 way, IntPtr tag, CacheSetEvent::fill_t fill) {}
      virtual void onInvalidate(UInt32 way, IntPtr tag, CacheSetEvent::invalidation_t reason) {}
      
      // Get LRU/recency bits for a way (0 = MRU, higher = older)
      // Default implementation returns way index (no recency info)
      virtual UInt8 getRecencyBits(UInt32 way) const { return (UInt8)way; }

      bool isValidReplacement(UInt32 index);

      uint64_t countPageWalkCacheBlocks();

};

#endif /* CACHE_SET_H */
