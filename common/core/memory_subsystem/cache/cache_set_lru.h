#ifndef CACHE_SET_LRU_H
#define CACHE_SET_LRU_H

#include "cache_set.h"

class CacheSetInfoLRU : public CacheSetInfo
{
   public:
      CacheSetInfoLRU(String name, String cfgname, core_id_t core_id, UInt32 associativity, UInt8 num_attempts);
      virtual ~CacheSetInfoLRU();
      void increment(UInt32 index)
      {
         LOG_ASSERT_ERROR(index < m_associativity, "Index(%d) >= Associativity(%d)", index, m_associativity);
         ++m_access[index];
      }
      void incrementAttempt(UInt8 attempt)
      {
         if (m_attempts)
            ++m_attempts[attempt];
         else
            LOG_ASSERT_ERROR(attempt == 0, "No place to store attempt# histogram but attempt != 0");
      }

      void recordVictimState(CacheState::cstate_t state)
      {
         switch (state)
         {
            case CacheState::MODIFIED:
               ++m_victim_modified;
               break;

            case CacheState::EXCLUSIVE:
               ++m_victim_exclusive;
               break;

            case CacheState::SHARED:
               ++m_victim_shared;
               break;

            default:
               ++m_victim_other;
               break;
         }
      }
   private:
      const UInt32 m_associativity;
      UInt64* m_access;
      UInt64* m_attempts;

       // Count eviction victims by coherence state
      UInt64 m_victim_modified = 0;
      UInt64 m_victim_exclusive = 0;
      UInt64 m_victim_shared = 0;
      UInt64 m_victim_other = 0;
};

class CacheSetLRU : public CacheSet
{
   public:
      CacheSetLRU(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts, bool is_tlb_set);
      virtual ~CacheSetLRU();

      virtual UInt32 getReplacementIndex(CacheCntlr *cntlr);
      void updateReplacementIndex(UInt32 accessed_index);
      
      // Override to return actual LRU bits
      virtual UInt8 getRecencyBits(UInt32 way) const override { return m_lru_bits[way]; }

   protected:
      const UInt8 m_num_attempts;
      UInt8* m_lru_bits;
      CacheSetInfoLRU* m_set_info;
      void moveToMRU(UInt32 accessed_index);
};

#endif /* CACHE_SET_LRU_H */
