#include "cache_set_rl.h"
#include "config.h"
#include "config.hpp"
#include "log.h"
#include "simulator.h"
#include "stats.h"

#include <cmath>
#include <cstdio>

// Coherence-aware RL replacement (sf_lru, rl_lru). See cache_set_rl.h and docs/rl/POLICY_CORES.md.

namespace
{
String cfgString(const String &key, const char *fallback)
{
   return Sim()->getCfg()->hasKey(key) ? Sim()->getCfg()->getString(key) : String(fallback);
}

template <typename T, typename Parse>
T parseKey(const String &key, const char *fallback, Parse parse)
{
   const String value = cfgString(key, fallback);
   auto parsed = parse(std::string_view(value.c_str()));
   if (!parsed)
      LOG_PRINT_ERROR("%s: unrecognized value '%s'", key.c_str(), value.c_str());
   return *parsed;
}

// Signed fixed-point (parts per million) for weights; stats are 64-bit and stored signed in the database
UInt64 ppm(double value)
{
   return (UInt64)(SInt64)std::llround(value * 1e6);
}

enum : int
{
   STAT_MAX_PASSES,
   STAT_W_S, STAT_W_E, STAT_W_M, STAT_W_BT, STAT_W_AT,
   STAT_SF_AVG_M, STAT_SF_AVG_E, STAT_SF_AVG_S, STAT_SF_AVG_AT, STAT_SF_AVG_BT,
};

UInt64 readStat(String, UInt32, String, UInt64 arg)
{
   const CacheSetInfoRL::StatSource *src = reinterpret_cast<const CacheSetInfoRL::StatSource *>(arg);
   CacheSetInfoRL &info = *src->info;
   ScopedLock sl(info.lock());
   if (src->what == STAT_MAX_PASSES)
      return info.reward().stats.max_passes;
   if (src->what >= STAT_W_S && src->what <= STAT_W_AT)
   {
      // Global scope: the one table; per-set scope: the mean over the sets
      const rlrepl::RewardPolicy &p = info.reward();
      const UInt32 tables = p.config().scope == rlrepl::WeightScope::global ? 1 : info.registeredSets();
      double sum = 0;
      for (UInt32 set = 0; set < tables; ++set)
      {
         const rlrepl::WeightTable &w = p.weights(set);
         switch (src->what)
         {
            case STAT_W_S:  sum += w.state_weight(rlrepl::Mesi::S); break;
            case STAT_W_E:  sum += w.state_weight(rlrepl::Mesi::E); break;
            case STAT_W_M:  sum += w.state_weight(rlrepl::Mesi::M); break;
            case STAT_W_BT: sum += w.share_weight(false); break;
            default:        sum += w.share_weight(true); break;
         }
      }
      return ppm(tables ? sum / tables : 0);
   }
   const rlrepl::PaperStats &s = info.paper().stats;
   if (s.weighted_decisions == 0)
      return 0;
   const double n = (double)s.weighted_decisions;
   switch (src->what)
   {
      case STAT_SF_AVG_M:  return ppm(s.sum_m / n);
      case STAT_SF_AVG_E:  return ppm(s.sum_e / n);
      case STAT_SF_AVG_S:  return ppm(s.sum_s / n);
      case STAT_SF_AVG_AT: return ppm(s.sum_at / n);
      default:             return ppm(s.sum_bt / n);
   }
}
} // namespace

CacheSetInfoRL::CacheSetInfoRL(String name, String cfgname, core_id_t core_id, UInt32 associativity, UInt32 num_sets, algorithm_t algorithm)
   : CacheSetInfoLRU(name, cfgname, core_id, associativity, 1)
   , m_algorithm(algorithm)
   , m_num_sets(num_sets)
   , m_registered_sets(0)
   , m_num_cores(Config::getSingleton()->getApplicationCores())
{
   LOG_ASSERT_ERROR(num_sets > 0, "%s: replacement policy %s needs the number of sets of its cache (not available here)",
                    name.c_str(), algorithm == PAPER ? "sf_lru" : "rl_lru");

   if (m_algorithm == PAPER)
   {
      rlrepl::PaperConfig c;
      c.mode = parseKey<rlrepl::Mode>(cfgname + "/sf/mode", "both", rlrepl::parse_mode);
      c.rule = parseKey<rlrepl::Rule>(cfgname + "/sf/rule", "text", rlrepl::parse_rule);
      c.combine = parseKey<rlrepl::Combine>(cfgname + "/sf/combine", "and", rlrepl::parse_combine);
      m_paper.reset(new rlrepl::PaperPolicy(c, num_sets, associativity));
      printf("[RL] %s core %d: sf_lru mode=%s rule=%s combine=%s history=%u/set threshold=>%u of %u cores\n", name.c_str(), core_id,
             std::string(rlrepl::name(c.mode)).c_str(), std::string(rlrepl::name(c.rule)).c_str(), std::string(rlrepl::name(c.combine)).c_str(),
             2 * associativity, m_num_cores / 2, m_num_cores);
   }
   else
   {
      rlrepl::RewardConfig c;
      c.mode = parseKey<rlrepl::Mode>(cfgname + "/rl/mode", "both", rlrepl::parse_mode);
      const SInt64 window = Sim()->getCfg()->getIntDefault(cfgname + "/rl/window", 4);
      LOG_ASSERT_ERROR(window >= 1, "%s/rl/window must be >= 1 (%ld)", cfgname.c_str(), (long)window);
      c.window = (std::uint32_t)window;
      c.alpha = Sim()->getCfg()->getFloatDefault(cfgname + "/rl/alpha", 0.1);
      LOG_ASSERT_ERROR(c.alpha >= 0.0 && c.alpha <= 1.0, "%s/rl/alpha must be in [0, 1] (%f)", cfgname.c_str(), c.alpha);
      c.protect_feedback = Sim()->getCfg()->getBoolDefault(cfgname + "/rl/protect_feedback", true);
      c.scope = parseKey<rlrepl::WeightScope>(cfgname + "/rl/weight_scope", "global", rlrepl::parse_weight_scope);
      m_reward.reset(new rlrepl::RewardPolicy(c, num_sets, associativity));
      printf("[RL] %s core %d: rl_lru mode=%s window=%u alpha=%g protect_feedback=%d weight_scope=%s history=%u/set threshold=>%u of %u cores\n",
             name.c_str(), core_id, std::string(rlrepl::name(c.mode)).c_str(), c.window, c.alpha, (int)c.protect_feedback,
             std::string(rlrepl::name(c.scope)).c_str(), 2 * associativity, m_num_cores / 2, m_num_cores);
   }
   registerStats(name, core_id);
}

CacheSetInfoRL::~CacheSetInfoRL()
{
   for (StatSource *s : m_stat_sources)
      delete s;
}

UInt32
CacheSetInfoRL::registerSet()
{
   ScopedLock sl(m_lock);
   LOG_ASSERT_ERROR(m_registered_sets < m_num_sets, "RL replacement: more sets (%u) than sized for (%u)", m_registered_sets + 1, m_num_sets);
   return m_registered_sets++;
}

void
CacheSetInfoRL::registerStats(String name, core_id_t core_id)
{
   const String p = m_algorithm == PAPER ? "sf-" : "rl-";
   auto callback = [&](const char *metric, int what)
   {
      StatSource *holder = new StatSource{this, what};
      m_stat_sources.push_back(holder);
      Sim()->getStatsManager()->registerMetric(new StatsMetricCallback(name, core_id, p + metric, readStat, (UInt64)holder));
   };

   if (m_algorithm == PAPER)
   {
      rlrepl::PaperStats &s = m_paper->stats;
      registerStatsMetric(name, core_id, p + "decisions", &s.decisions);
      registerStatsMetric(name, core_id, p + "first-accepted", &s.first_accepted);
      registerStatsMetric(name, core_id, p + "second-chances", &s.second_chances);
      registerStatsMetric(name, core_id, p + "empty-history-decisions", &s.empty_history_decisions);
      registerStatsMetric(name, core_id, p + "reward-negative", &s.negative_rewards);
      registerStatsMetric(name, core_id, p + "reward-positive", &s.positive_rewards);
      registerStatsMetric(name, core_id, p + "history-aged-out", &s.history_aged_out);
      registerStatsMetric(name, core_id, p + "history-refill-removals", &s.refill_removals);
      registerStatsMetric(name, core_id, p + "weighted-decisions", &s.weighted_decisions);
      callback("avg-weight-M-ppm", STAT_SF_AVG_M);
      callback("avg-weight-E-ppm", STAT_SF_AVG_E);
      callback("avg-weight-S-ppm", STAT_SF_AVG_S);
      callback("avg-weight-AT-ppm", STAT_SF_AVG_AT);
      callback("avg-weight-BT-ppm", STAT_SF_AVG_BT);
   }
   else
   {
      rlrepl::RewardStats &s = m_reward->stats;
      registerStatsMetric(name, core_id, p + "decisions", &s.decisions);
      registerStatsMetric(name, core_id, p + "chose-oldest", &s.chose_oldest);
      registerStatsMetric(name, core_id, p + "chose-other", &s.chose_other);
      registerStatsMetric(name, core_id, p + "reward-evict-bad", &s.evict_bad);
      registerStatsMetric(name, core_id, p + "reward-evict-good", &s.evict_good);
      registerStatsMetric(name, core_id, p + "reward-protect-good", &s.protect_good);
      registerStatsMetric(name, core_id, p + "reward-protect-wasted", &s.protect_wasted);
      registerStatsMetric(name, core_id, p + "passes", &s.passes);
      callback("max-passes", STAT_MAX_PASSES);
      registerStatsMetric(name, core_id, p + "inv-coherence", &s.invalidations[(int)rlrepl::InvalidationReason::coherence]);
      registerStatsMetric(name, core_id, p + "inv-back-invalidation", &s.invalidations[(int)rlrepl::InvalidationReason::back_invalidation]);
      registerStatsMetric(name, core_id, p + "inv-upgrade", &s.invalidations[(int)rlrepl::InvalidationReason::upgrade]);
      registerStatsMetric(name, core_id, p + "upgrade-carryovers", &s.upgrade_carryovers);
      registerStatsMetric(name, core_id, p + "upgrade-refills-unmatched", &s.upgrade_refills_unmatched);
      registerStatsMetric(name, core_id, p + "upgrade-stash-overwritten", &s.upgrade_stash_overwritten);
      registerStatsMetric(name, core_id, p + "history-refill-removals", &s.refill_removals);
      callback("weight-S-ppm", STAT_W_S);
      callback("weight-E-ppm", STAT_W_E);
      callback("weight-M-ppm", STAT_W_M);
      callback("weight-BT-ppm", STAT_W_BT);
      callback("weight-AT-ppm", STAT_W_AT);
   }
   registerStatsMetric(name, core_id, p + "history-inserts", &adapter.history_inserts);
   registerStatsMetric(name, core_id, p + "full-set-decisions", &adapter.decisions_with_eviction);
   registerStatsMetric(name, core_id, p + "eviction-without-decision", &adapter.eviction_without_decision);
   registerStatsMetric(name, core_id, p + "no-eligible-victim", &adapter.no_eligible_victim);
   registerStatsMetric(name, core_id, p + "inv-unclassified", &adapter.unclassified_invalidations);
   registerStatsMetric(name, core_id, p + "candidate-state-unknown", &adapter.candidate_state_unknown);
}

CacheSetRL::CacheSetRL(CacheBase::cache_t cache_type, UInt32 associativity, UInt32 blocksize, CacheSetInfoRL *set_info, bool is_tlb_set)
   : CacheSet(cache_type, associativity, blocksize, is_tlb_set)
   , m_info(set_info)
   , m_set(registerWith(set_info))
   , m_lru(associativity)
{
}

CacheSetRL::~CacheSetRL()
{
}

UInt32
CacheSetRL::registerWith(CacheSetInfoRL *set_info)
{
   LOG_ASSERT_ERROR(set_info != NULL, "RL replacement needs its CacheSetInfoRL");
   return set_info->registerSet();
}

rlrepl::Mesi
CacheSetRL::toMesi(CacheState::cstate_t cstate)
{
   switch (cstate)
   {
      case CacheState::MODIFIED:
      case CacheState::OWNED: // not produced by this MESI protocol; dirty, so M
         return rlrepl::Mesi::M;
      case CacheState::EXCLUSIVE:
         return rlrepl::Mesi::E;
      case CacheState::SHARED:
      case CacheState::SHARED_UPGRADING:
         return rlrepl::Mesi::S;
      default:
         return rlrepl::Mesi::I;
   }
}

// The block's own coherence state (including M granted on a read) and its delivered sharer hint; never the oracle
rlrepl::Features
CacheSetRL::features(UInt32 way)
{
   const CacheBlockInfo *block = m_cache_block_info_array[way];
   const rlrepl::Mesi state = toMesi(block->getCState());
   if (state == rlrepl::Mesi::I)
      ++m_info->adapter.candidate_state_unknown;
   return rlrepl::make_features(state, block->getSharerHint(), m_info->numCores());
}

UInt32
CacheSetRL::getReplacementIndex(CacheCntlr *cntlr)
{
   // An invalid way first, exactly as CacheSetLRU
   for (UInt32 i = 0; i < m_associativity; i++)
   {
      if (!m_cache_block_info_array[i]->isValid())
      {
         m_lru.touch(i);
         m_pending.valid = false;
         return i;
      }
   }

   UInt32 victim;
   {
      ScopedLock sl(m_info->lock());
      // Candidates: the eligible ways (not SHARED_UPGRADING), least recently used first
      const std::vector<std::uint32_t> candidates = m_lru.candidates(m_associativity, [this](std::uint32_t way) { return isValidReplacement(way); });
      auto info = [this](std::uint32_t way) { return features(way); };
      rlrepl::Features victim_features;
      if (candidates.empty())
      {
         ++m_info->adapter.no_eligible_victim;
         victim = 0;
         victim_features = features(0);
      }
      else if (m_info->algorithm() == CacheSetInfoRL::PAPER)
      {
         const rlrepl::PaperDecision d = m_info->paper().choose(m_set, candidates, info);
         m_info->paper().commit(d);
         victim = d.victim;
         victim_features = d.victim_features;
      }
      else
      {
         const rlrepl::RewardDecision d = m_info->reward().choose(m_set, candidates, info);
         m_info->reward().commit(m_set, d);
         victim = d.victim;
         victim_features = d.victim_features;
      }
      ++m_info->adapter.decisions_with_eviction;
      m_pending.valid = true;
      m_pending.way = victim;
      m_pending.features = victim_features;
   }

   // Same diagnostics as CacheSetLRU (observation only, do not affect the choice)
   m_info->recordVictimState(m_cache_block_info_array[victim]->getCState());
   m_info->recordVictimSharers((cntlr && !m_is_tlb_set) ? cntlr->queryOracleSharers(m_cache_block_info_array[victim]) : OracleSharerInfo());

   m_lru.touch(victim);
   m_info->incrementAttempt(0);
   return victim;
}

void
CacheSetRL::updateReplacementIndex(UInt32 accessed_index)
{
   m_info->increment(m_lru.rank(accessed_index));
   m_lru.touch(accessed_index);
   if (m_info->algorithm() == CacheSetInfoRL::REWARD)
   {
      ScopedLock sl(m_info->lock());
      m_info->reward().on_hit(m_set, accessed_index);
   }
}

void
CacheSetRL::onMiss(IntPtr tag)
{
   ScopedLock sl(m_info->lock());
   if (m_info->algorithm() == CacheSetInfoRL::PAPER)
      m_info->paper().on_miss(m_set, tag);
   else
      m_info->reward().on_miss(m_set, tag);
}

void
CacheSetRL::onEviction(UInt32 way, const CacheBlockInfo *victim)
{
   ScopedLock sl(m_info->lock());
   rlrepl::Features f;
   if (m_pending.valid && m_pending.way == way)
   {
      f = m_pending.features;
   }
   else
   {
      ++m_info->adapter.eviction_without_decision;
      f = features(way);
   }
   m_pending.valid = false;

   if (m_info->mode() == rlrepl::Mode::off)
      return;
   ++m_info->adapter.history_inserts;
   if (m_info->algorithm() == CacheSetInfoRL::PAPER)
      m_info->paper().on_eviction(m_set, victim->getTag(), f);
   else
      m_info->reward().on_eviction(m_set, victim->getTag(), f);
}

void
CacheSetRL::onFill(UInt32 way, IntPtr tag, CacheSetEvent::fill_t fill)
{
   ScopedLock sl(m_info->lock());
   if (m_info->algorithm() == CacheSetInfoRL::PAPER)
      m_info->paper().on_fill(m_set, tag);
   else
      m_info->reward().on_fill(m_set, way, tag,
                               fill == CacheSetEvent::UPGRADE_REFILL ? rlrepl::FillKind::upgrade_refill : rlrepl::FillKind::demand);
}

void
CacheSetRL::onInvalidate(UInt32 way, IntPtr tag, CacheSetEvent::invalidation_t reason)
{
   ScopedLock sl(m_info->lock());
   rlrepl::InvalidationReason r;
   switch (reason)
   {
      case CacheSetEvent::INV_UPGRADE:           r = rlrepl::InvalidationReason::upgrade; break;
      case CacheSetEvent::INV_BACK_INVALIDATION: r = rlrepl::InvalidationReason::back_invalidation; break;
      case CacheSetEvent::INV_COHERENCE:         r = rlrepl::InvalidationReason::coherence; break;
      default:
         // Should not happen in a coherent L1/L2; no feedback either way
         ++m_info->adapter.unclassified_invalidations;
         r = rlrepl::InvalidationReason::coherence;
         break;
   }
   // The paper reproduction keeps no per-line state; invalidations never touch either core's history
   if (m_info->algorithm() == CacheSetInfoRL::REWARD)
      m_info->reward().on_invalidate(m_set, way, tag, r);
}
