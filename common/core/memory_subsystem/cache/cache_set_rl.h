#ifndef CACHE_SET_RL_H
#define CACHE_SET_RL_H

/*
 * Coherence-aware RL replacement policies (Stage 3 of the RL port, docs/rl/POLICY_CORES.md):
 *
 *   replacement_policy = sf_lru   Souza & Freitas reproduction (rlrepl::PaperPolicy): LRU proposes, the
 *                                 history-ratio weights accept candidate #1 or give it its one second chance
 *   replacement_policy = rl_lru   our modified algorithm (rlrepl::RewardPolicy): highest learned score among the
 *                                 N least recently used ways
 *
 * CacheSetInfoRL is the per-cache object: it owns the one learning core (global weights, per-set histories and per-line
 * metadata inside it), its lock and its statistics, and keeps every CacheSetInfoLRU statistic (lru-victim-*, oracle-*,
 * hint-*, access-mru-*). CacheSetRL is one set: it keeps that set's LRU order (rlrepl::LruOrder, the same ranks as
 * CacheSetLRU) and connects the Stage 2 event hooks to the core. All decisions use the block's own coherence state and
 * its delivered sharer hint; the oracle sharer count is only recorded for diagnostics, as with LRU.
 *
 * Stage 4 diagnostics (rl_diag/enabled = true, default off): RlDiagnostics holds the core observer
 * (rl/rl_diagnostics.h), the miss classification, the write-oracle checks (rl_write_oracle.h) and the weight
 * snapshot file. All of it is observation only: decisions are identical with diagnostics on or off.
 */

#include "cache_set.h"
#include "cache_set_lru.h"
#include "lock.h"
#include "rl_diagnostics.h"
#include "rl_lru_order.h"
#include "rl_paper_policy.h"
#include "rl_reward_policy.h"

#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

// Stage 4 diagnostics of one cache (see docs/rl/POLICY_CORES.md, "Diagnostics"); used under CacheSetInfoRL::lock()
struct RlDiagnostics
{
   std::unique_ptr<rlrepl::RewardObserver> reward; // rl_lru
   std::unique_ptr<rlrepl::PaperObserver> paper;   // sf_lru
   core_id_t core_id = INVALID_CORE_ID;

   // How a line last left this cache, and the write-oracle sequence number at that moment
   enum departure_t : UInt8 { REPLACED, COHERENCE, BACK_INVALIDATION };
   struct Departure
   {
      departure_t how;
      UInt64 seq;
   };
   std::unordered_map<IntPtr, Departure> departed; // by tag: lines that left and have not come back

   // Demand misses (onMiss) by why the line was not here
   struct Misses
   {
      UInt64 total = 0;
      UInt64 cold = 0;                       // never held by this cache
      UInt64 replaced_in_history = 0;        // evicted, still in the history: the policy is blamed (-1)
      UInt64 replaced_not_in_history = 0;    // evicted, entry aged out (or mode off)
      UInt64 coherence = 0;                  // invalidated by another core's write
      UInt64 back_invalidation = 0;          // invalidated by the inclusive next level
      UInt64 replaced_in_history_other_write = 0;     // ... another core wrote the line after the eviction (I-2)
      UInt64 replaced_not_in_history_other_write = 0;
      UInt64 inconsistent = 0;               // in the history but last left otherwise (expect 0)
   } misses;
   rlrepl::ClassCounts history_hit_other_write; // replaced_in_history_other_write by the history entry's features

   // Victims in M (audit I-3): M from a read fill and never written by this core while resident
   UInt64 victims_m = 0;
   UInt64 victims_m_unwritten = 0;

   // Periodic snapshots of the weights and counters, one CSV row every snapshot_interval full-set decisions
   UInt64 snapshot_interval = 0;
   FILE *snapshots = nullptr;
   UInt64 snapshot_rows = 0;
};

class CacheSetInfoRL : public CacheSetInfoLRU
{
   public:
      enum algorithm_t { PAPER, REWARD };

      CacheSetInfoRL(String name, String cfgname, core_id_t core_id, UInt32 associativity, UInt32 num_sets, algorithm_t algorithm);
      virtual ~CacheSetInfoRL();

      // Called once by every CacheSetRL of this cache, in creation order: returns its set index in the core
      UInt32 registerSet();
      UInt32 registeredSets() const { return m_registered_sets; }

      algorithm_t algorithm() const { return m_algorithm; }
      rlrepl::Mode mode() const { return m_algorithm == PAPER ? m_paper->config().mode : m_reward->config().mode; }
      UInt32 numCores() const { return m_num_cores; }
      rlrepl::PaperPolicy &paper() { return *m_paper; }
      rlrepl::RewardPolicy &reward() { return *m_reward; }
      Lock &lock() { return m_lock; }
      RlDiagnostics *diag() { return m_diag.get(); } // null unless rl_diag/enabled

      // Weights for the statistics and snapshots: the global table, or the mean over the sets (per-set scope)
      double meanWeight(int what) const;
      // One snapshot row (caller holds lock())
      void writeSnapshot();

      // Adapter statistics (prefixed sf- or rl-)
      struct AdapterStats
      {
         UInt64 history_inserts = 0;           // replacement evictions recorded into a history (mode != off)
         UInt64 decisions_with_eviction = 0;   // getReplacementIndex calls on a full set
         UInt64 eviction_without_decision = 0; // onEviction without a preceding decision for that way (expect 0)
         UInt64 no_eligible_victim = 0;        // every way SHARED_UPGRADING: fell back to way 0 (expect 0)
         UInt64 unclassified_invalidations = 0;// INV_UNCLASSIFIED, treated as a coherence invalidation (expect 0)
         UInt64 candidate_state_unknown = 0;   // candidate whose state maps to no feature (expect 0)
      } adapter;

      // A statistic read through a callback (weights, max passes): which value of which cache
      struct StatSource
      {
         CacheSetInfoRL *info;
         int what;
      };

   private:
      const algorithm_t m_algorithm;
      const UInt32 m_num_sets;
      UInt32 m_registered_sets;
      UInt32 m_num_cores;
      std::unique_ptr<rlrepl::PaperPolicy> m_paper;
      std::unique_ptr<rlrepl::RewardPolicy> m_reward;
      Lock m_lock;
      std::vector<StatSource *> m_stat_sources;
      std::unique_ptr<RlDiagnostics> m_diag;

      void registerStats(String name, core_id_t core_id);
      void setupDiagnostics(String name, String cfgname, core_id_t core_id, UInt32 associativity, UInt32 num_sets);
      void registerDiagnosticStats(String name, core_id_t core_id);
};

class CacheSetRL : public CacheSet
{
   public:
      CacheSetRL(CacheBase::cache_t cache_type, UInt32 associativity, UInt32 blocksize, CacheSetInfoRL *set_info, bool is_tlb_set);
      virtual ~CacheSetRL();

      virtual UInt32 getReplacementIndex(CacheCntlr *cntlr) override;
      virtual void updateReplacementIndex(UInt32 accessed_index) override;
      virtual UInt8 getRecencyBits(UInt32 way) const override { return (UInt8)m_lru.rank(way); }

      virtual void onMiss(IntPtr tag) override;
      virtual void onEviction(UInt32 way, const CacheBlockInfo *victim) override;
      virtual void onFill(UInt32 way, IntPtr tag, CacheSetEvent::fill_t fill) override;
      virtual void onInvalidate(UInt32 way, IntPtr tag, CacheSetEvent::invalidation_t reason) override;

      // Sniper coherence state -> feature state; SHARED_UPGRADING (never a candidate) counts as S
      static rlrepl::Mesi toMesi(CacheState::cstate_t cstate);

   private:
      CacheSetInfoRL *m_info;
      const UInt32 m_set;
      rlrepl::LruOrder m_lru;

      // Decision taken by getReplacementIndex, consumed by onEviction of the same way (CacheSet::insert is atomic)
      struct Pending
      {
         bool valid = false;
         UInt32 way = 0;
         rlrepl::Features features{};
      } m_pending;

      // Write-oracle sequence number when each way was filled (diagnostics only)
      std::vector<UInt64> m_fill_seq;

      rlrepl::Features features(UInt32 way);          // counts candidate-state-unknown
      rlrepl::Features featuresOf(UInt32 way) const;  // no side effect (diagnostics)
      IntPtr addressOf(IntPtr tag) const { return tag * m_blocksize; }
      void classifyMiss(RlDiagnostics &d, IntPtr tag);
      static UInt32 registerWith(CacheSetInfoRL *set_info);
};

#endif /* CACHE_SET_RL_H */
