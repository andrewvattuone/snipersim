#include "cache_set_lru.h"
#include "log.h"
#include "stats.h"

// Implements LRU replacement, optionally augmented with Query-Based Selection [Jaleel et al., MICRO'10]

CacheSetLRU::CacheSetLRU(
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts, bool is_tlb_set)
   : CacheSet(cache_type, associativity, blocksize, is_tlb_set)
   , m_num_attempts(num_attempts)
   , m_set_info(set_info)
{
   m_lru_bits = new UInt8[m_associativity];
   for (UInt32 i = 0; i < m_associativity; i++)
      m_lru_bits[i] = i;
}

CacheSetLRU::~CacheSetLRU()
{
   delete [] m_lru_bits;
}

UInt32
CacheSetLRU::getReplacementIndex(CacheCntlr *cntlr)
{
   // First try to find an invalid block
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         // Mark our newly-inserted line as most-recently used
         moveToMRU(i);
         return i;
      }
   }

   // Make m_num_attemps attempts at evicting the block at LRU position
   for(UInt8 attempt = 0; attempt < m_num_attempts; ++attempt)
   {
      UInt32 index = 0;
      UInt8 max_bits = 0;
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_lru_bits[i] > max_bits && isValidReplacement(i))
         {
            index = i;
            max_bits = m_lru_bits[i];
         }
      }
      LOG_ASSERT_ERROR(index < m_associativity, "Error Finding LRU bits");

      bool qbs_reject = false;
      if (attempt < m_num_attempts - 1)
      {
         LOG_ASSERT_ERROR(cntlr != NULL, "CacheCntlr == NULL, QBS can only be used when cntlr is passed in");
         qbs_reject = cntlr->isInLowerLevelCache(m_cache_block_info_array[index]);
      }

      if (qbs_reject)
      {
         // Block is contained in lower-level cache, and we have more tries remaining.
         // Move this block to MRU and try again
         moveToMRU(index);
         cntlr->incrementQBSLookupCost();
         continue;
      }
      else
      {
         // Get the coherence state of the selected victim
         CacheState::cstate_t state =
            m_cache_block_info_array[index]->getCState();

         // Record which MESI state was evicted
         m_set_info->recordVictimState(state);

         // ORACLE instrumentation (observation only, does not affect the choice)
         m_set_info->recordVictimSharers(
            (cntlr && !m_is_tlb_set) ? cntlr->queryOracleSharers(m_cache_block_info_array[index]) : OracleSharerInfo());

         // Preserve the original LRU replacement behavior
         moveToMRU(index);
         m_set_info->incrementAttempt(attempt);
         return index;
      }
   }

   LOG_PRINT_ERROR("Should not reach here");
}

void
CacheSetLRU::updateReplacementIndex(UInt32 accessed_index)
{
   m_set_info->increment(m_lru_bits[accessed_index]);
   moveToMRU(accessed_index);
}

void
CacheSetLRU::moveToMRU(UInt32 accessed_index)
{
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (m_lru_bits[i] < m_lru_bits[accessed_index])
         m_lru_bits[i] ++;
   }
   m_lru_bits[accessed_index] = 0;
}

CacheSetInfoLRU::CacheSetInfoLRU(String name, String cfgname, core_id_t core_id, UInt32 associativity, UInt8 num_attempts)
   : m_associativity(associativity)
   , m_attempts(NULL)
{
   m_access = new UInt64[m_associativity];
   for(UInt32 i = 0; i < m_associativity; ++i)
   {
      m_access[i] = 0;
      registerStatsMetric(name, core_id, String("access-mru-")+itostr(i), &m_access[i]);
   }

   if (num_attempts > 1)
   {
      m_attempts = new UInt64[num_attempts];
      for(UInt32 i = 0; i < num_attempts; ++i)
      {
         m_attempts[i] = 0;
         registerStatsMetric(name, core_id, String("qbs-attempt-")+itostr(i), &m_attempts[i]);
      }
   }

   // Register coherence-state victim counters
   registerStatsMetric(name, core_id, "lru-victim-M", &m_victim_modified);
   registerStatsMetric(name, core_id, "lru-victim-E", &m_victim_exclusive);
   registerStatsMetric(name, core_id, "lru-victim-S", &m_victim_shared);
   registerStatsMetric(name, core_id, "lru-victim-other", &m_victim_other);

   // ORACLE sharer counts of LRU victims (direct simulator-state reads, unmodeled)
   for (UInt32 i = 0; i < ORACLE_SHARER_BUCKETS; ++i)
   {
      String bucket = (i == ORACLE_SHARER_BUCKETS - 1) ? itostr(i) + "plus" : itostr(i);
      registerStatsMetric(name, core_id, "oracle-victim-dir-sharers-" + bucket, &m_oracle_dir_sharers[i]);
      registerStatsMetric(name, core_id, "oracle-victim-upper-sharers-" + bucket, &m_oracle_upper_sharers[i]);
      registerStatsMetric(name, core_id, "cachedlocs-victim-sharers-" + bucket, &m_cv_sharers[i]);
   }
   // Local presence bitmap (m_cached_locs) checked against the oracle probe
   registerStatsMetric(name, core_id, "cachedlocs-victim-compared", &m_cv_compared);
   registerStatsMetric(name, core_id, "cachedlocs-victim-match", &m_cv_match);
   registerStatsMetric(name, core_id, "cachedlocs-victim-mismatch", &m_cv_mismatch);
   registerStatsMetric(name, core_id, "cachedlocs-victim-stale", &m_cv_stale);
   registerStatsMetric(name, core_id, "cachedlocs-victim-missing", &m_cv_missing);
   registerStatsMetric(name, core_id, "cachedlocs-victim-unavailable", &m_cv_unavailable);
   registerStatsMetric(name, core_id, "cachedlocs-victim-count-match", &m_cv_count_match);
   registerStatsMetric(name, core_id, "cachedlocs-victim-count-mismatch", &m_cv_count_mismatch);
   registerStatsMetric(name, core_id, "oracle-victim-dir-missing", &m_oracle_dir_missing);
   registerStatsMetric(name, core_id, "oracle-victim-dir-unavailable", &m_oracle_dir_unavailable);
   registerStatsMetric(name, core_id, "oracle-victim-upper-unavailable", &m_oracle_upper_unavailable);
};

CacheSetInfoLRU::~CacheSetInfoLRU()
{
   delete [] m_access;
   if (m_attempts)
      delete [] m_attempts;
}
