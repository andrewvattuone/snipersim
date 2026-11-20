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
      // ORACLE instrumentation: sharer counts of actual eviction victims
      static const UInt32 ORACLE_SHARER_BUCKETS = 5; // 0, 1, 2, 3, 4+
      void recordVictimSharers(const OracleSharerInfo &info)
      {
         recordSharers(info.dir_status, info.dir_sharers, m_oracle_dir_sharers, &m_oracle_dir_missing, m_oracle_dir_unavailable);
         recordSharers(info.upper_status, info.upper_sharers, m_oracle_upper_sharers, NULL /* never NOT_FOUND */, m_oracle_upper_unavailable);
         recordCachedLocs(info);
         recordSharerHint(info);
      }
   private:
      // Local presence bitmap (m_cached_locs) vs. oracle probe of the same block
      void recordCachedLocs(const OracleSharerInfo &info)
      {
         if (info.cv_status != OracleSharerInfo::OK)
         {
            ++m_cv_unavailable;
            return;
         }
         ++m_cv_compared;
         if (info.cv_mask == info.probe_mask)
            ++m_cv_match;
         else
            ++m_cv_mismatch;
         if (info.cv_mask & ~info.probe_mask)
            ++m_cv_stale;   // bit set, but that cache does not hold the line
         if (info.probe_mask & ~info.cv_mask)
            ++m_cv_missing; // cache holds the line, but its bit is clear
         // Per-core count, comparable to oracle-victim-upper-sharers (shared caches only)
         if (info.upper_status == OracleSharerInfo::OK)
         {
            ++m_cv_sharers[info.cv_sharers < ORACLE_SHARER_BUCKETS ? info.cv_sharers : ORACLE_SHARER_BUCKETS - 1];
            if (info.cv_sharers == info.upper_sharers)
               ++m_cv_count_match;
            else
               ++m_cv_count_mismatch;
         }
      }

      // Delivered sharer hint (local block metadata) vs. oracle count of holding cores
      void recordSharerHint(const OracleSharerInfo &info)
      {
         if (info.hint_status == OracleSharerInfo::UNAVAILABLE)
         {
            ++m_hint_unavailable;
            return;
         }
         if (info.hint_status == OracleSharerInfo::NOT_FOUND)
         {
            ++m_hint_missing;
            return;
         }
         ++m_hint_compared;
         ++m_hint_delivered[info.hint < ORACLE_SHARER_BUCKETS ? info.hint : ORACLE_SHARER_BUCKETS - 1];
         ++m_hint_oracle[info.hint_oracle < ORACLE_SHARER_BUCKETS ? info.hint_oracle : ORACLE_SHARER_BUCKETS - 1];
         if (info.hint == info.hint_oracle)
            ++m_hint_exact;
         else if (info.hint > info.hint_oracle)
            ++m_hint_over;  // stale-high: other holders have left
         else
            ++m_hint_under; // stale-low: other holders joined after our fill
         m_hint_abs_error += info.hint > info.hint_oracle ? info.hint - info.hint_oracle : info.hint_oracle - info.hint;
         bool heavy_delivered = info.hint > info.heavy_threshold, heavy_oracle = info.hint_oracle > info.heavy_threshold;
         if (heavy_delivered && heavy_oracle)
            ++m_hint_heavy_both;
         else if (heavy_delivered)
            ++m_hint_heavy_delivered_only; // ChampSim: delivered_above_oracle_below
         else if (heavy_oracle)
            ++m_hint_heavy_oracle_only;    // ChampSim: delivered_below_oracle_above
         else
            ++m_hint_heavy_neither;
      }

      static void recordSharers(OracleSharerInfo::status_t status, UInt32 count,
                                UInt64 *buckets, UInt64 *missing, UInt64 &unavailable)
      {
         switch (status)
         {
            case OracleSharerInfo::OK:
               ++buckets[count < ORACLE_SHARER_BUCKETS ? count : ORACLE_SHARER_BUCKETS - 1];
               break;
            case OracleSharerInfo::NOT_FOUND:
               LOG_ASSERT_ERROR(missing != NULL, "Unexpected NOT_FOUND oracle sharer status");
               ++*missing;
               break;
            default:
               ++unavailable;
               break;
         }
      }

      const UInt32 m_associativity;
      UInt64* m_access;
      UInt64* m_attempts;

       // Count eviction victims by coherence state
      UInt64 m_victim_modified = 0;
      UInt64 m_victim_exclusive = 0;
      UInt64 m_victim_shared = 0;
      UInt64 m_victim_other = 0;

      UInt64 m_oracle_dir_sharers[ORACLE_SHARER_BUCKETS] = {};
      UInt64 m_oracle_dir_missing = 0;
      UInt64 m_oracle_dir_unavailable = 0;
      UInt64 m_oracle_upper_sharers[ORACLE_SHARER_BUCKETS] = {};
      UInt64 m_oracle_upper_unavailable = 0;

      UInt64 m_cv_compared = 0, m_cv_match = 0, m_cv_mismatch = 0;
      UInt64 m_cv_stale = 0, m_cv_missing = 0, m_cv_unavailable = 0;
      UInt64 m_cv_sharers[ORACLE_SHARER_BUCKETS] = {};
      UInt64 m_cv_count_match = 0, m_cv_count_mismatch = 0;

      UInt64 m_hint_compared = 0, m_hint_missing = 0, m_hint_unavailable = 0;
      UInt64 m_hint_exact = 0, m_hint_over = 0, m_hint_under = 0, m_hint_abs_error = 0;
      UInt64 m_hint_delivered[ORACLE_SHARER_BUCKETS] = {}, m_hint_oracle[ORACLE_SHARER_BUCKETS] = {};
      UInt64 m_hint_heavy_both = 0, m_hint_heavy_neither = 0, m_hint_heavy_delivered_only = 0, m_hint_heavy_oracle_only = 0;
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
