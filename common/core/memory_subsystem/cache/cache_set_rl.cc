#include "cache_set_rl.h"
#include "config.h"
#include "config.hpp"
#include "core.h"
#include "core_manager.h"
#include "log.h"
#include "magic_server.h"
#include "rl_write_oracle.h"
#include "simulator.h"
#include "stats.h"

#include <cmath>
#include <cstdio>
#include <string>

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
      return ppm(info.meanWeight(src->what));
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

      // Experimental controls (docs/rl/POLICY_CORES.md, "Controls for the learning question"); the defaults leave the
      // algorithm unchanged. Initial weights are separate keys because a comma-separated value is a per-core list.
      c.selection = parseKey<rlrepl::Selection>(cfgname + "/rl/selection", "learned", rlrepl::parse_selection);
      c.learning = Sim()->getCfg()->getBoolDefault(cfgname + "/rl/learning", true);
      c.random_rate = Sim()->getCfg()->getFloatDefault(cfgname + "/rl/random_rate", 0.0);
      LOG_ASSERT_ERROR(c.random_rate >= 0.0 && c.random_rate <= 1.0, "%s/rl/random_rate must be in [0, 1] (%f)", cfgname.c_str(), c.random_rate);
      LOG_ASSERT_ERROR(c.selection == rlrepl::Selection::random || c.random_rate == 0.0,
                       "%s/rl/random_rate is only used with rl/selection=random", cfgname.c_str());
      const SInt64 seed = Sim()->getCfg()->getIntDefault(cfgname + "/rl/seed", 0);
      LOG_ASSERT_ERROR(seed >= 0, "%s/rl/seed must be >= 0 (%ld)", cfgname.c_str(), (long)seed);
      // One reproducible stream per cache: the configured seed mixed with the core id
      c.seed = rlrepl::splitmix64((std::uint64_t)seed ^ ((std::uint64_t)core_id << 32));
      // Experimental Stage 7 variant (docs/rl/STAGE7.md); the default is the algorithm's EMA
      c.estimator = parseKey<rlrepl::Estimator>(cfgname + "/rl/estimator", "ema", rlrepl::parse_estimator);
      const bool horizon_set = Sim()->getCfg()->hasKey(cfgname + "/rl/horizon");
      c.horizon = Sim()->getCfg()->getFloatDefault(cfgname + "/rl/horizon", 65536.0);
      LOG_ASSERT_ERROR(std::isfinite(c.horizon) && c.horizon >= 1.0, "%s/rl/horizon must be a finite number >= 1 (%f)", cfgname.c_str(), c.horizon);
      LOG_ASSERT_ERROR(c.estimator == rlrepl::Estimator::discounted_mean || !horizon_set,
                       "%s/rl/horizon is only used with rl/estimator=discounted_mean", cfgname.c_str());
      rlrepl::WeightTable initial;
      const char *const weight_keys[5] = {"S", "E", "M", "BT", "AT"};
      double w[5];
      bool initial_set = false;
      for (int i = 0; i < 5; ++i)
      {
         const String key = cfgname + "/rl/initial_weights/" + weight_keys[i];
         initial_set |= Sim()->getCfg()->hasKey(key);
         w[i] = Sim()->getCfg()->getFloatDefault(key, 0.0);
         LOG_ASSERT_ERROR(w[i] >= -1.0 && w[i] <= 1.0, "%s must be in [-1, 1] (%f)", key.c_str(), w[i]);
      }
      initial.set_state_weight(rlrepl::Mesi::S, w[0]);
      initial.set_state_weight(rlrepl::Mesi::E, w[1]);
      initial.set_state_weight(rlrepl::Mesi::M, w[2]);
      initial.set_share_weight(false, w[3]);
      initial.set_share_weight(true, w[4]);

      m_reward.reset(new rlrepl::RewardPolicy(c, num_sets, associativity, initial));
      printf("[RL] %s core %d: rl_lru mode=%s window=%u alpha=%g protect_feedback=%d weight_scope=%s history=%u/set threshold=>%u of %u cores\n",
             name.c_str(), core_id, std::string(rlrepl::name(c.mode)).c_str(), c.window, c.alpha, (int)c.protect_feedback,
             std::string(rlrepl::name(c.scope)).c_str(), 2 * associativity, m_num_cores / 2, m_num_cores);
      if (c.selection != rlrepl::Selection::learned || !c.learning || initial_set)
         printf("[RL] %s core %d: rl_lru CONTROL selection=%s learning=%d random_rate=%g seed=%ld (stream %016llx) initial weights S=%g E=%g M=%g BT=%g AT=%g\n",
                name.c_str(), core_id, std::string(rlrepl::name(c.selection)).c_str(), (int)c.learning, c.random_rate, (long)seed,
                (unsigned long long)c.seed, w[0], w[1], w[2], w[3], w[4]);
      if (c.estimator != rlrepl::Estimator::ema)
         printf("[RL] %s core %d: rl_lru STAGE7 estimator=%s horizon=%g decisions (alpha unused)\n", name.c_str(), core_id,
                std::string(rlrepl::name(c.estimator)).c_str(), c.horizon);
   }
   registerStats(name, core_id);
   setupDiagnostics(name, cfgname, core_id, associativity, num_sets);
}

CacheSetInfoRL::~CacheSetInfoRL()
{
   for (StatSource *s : m_stat_sources)
      delete s;
   if (m_diag && m_diag->snapshots)
      fclose(m_diag->snapshots);
}

double
CacheSetInfoRL::meanWeight(int what) const
{
   // Global scope: the one table; per-set scope: the mean over the sets
   const rlrepl::RewardPolicy &p = *m_reward;
   const UInt32 tables = p.config().scope == rlrepl::WeightScope::global ? 1 : m_registered_sets;
   double sum = 0;
   for (UInt32 set = 0; set < tables; ++set)
   {
      const rlrepl::WeightTable &w = p.weights(set);
      switch (what)
      {
         case STAT_W_S:  sum += w.state_weight(rlrepl::Mesi::S); break;
         case STAT_W_E:  sum += w.state_weight(rlrepl::Mesi::E); break;
         case STAT_W_M:  sum += w.state_weight(rlrepl::Mesi::M); break;
         case STAT_W_BT: sum += w.share_weight(false); break;
         default:        sum += w.share_weight(true); break;
      }
   }
   return tables ? sum / tables : 0;
}

void
CacheSetInfoRL::setupDiagnostics(String name, String cfgname, core_id_t core_id, UInt32 associativity, UInt32 num_sets)
{
   if (!Sim()->getCfg()->getBoolDefault(cfgname + "/rl_diag/enabled", false))
      return;
   m_diag.reset(new RlDiagnostics());
   RlDiagnostics &d = *m_diag;
   d.core_id = core_id;
   if (m_algorithm == PAPER)
      d.paper.reset(new rlrepl::PaperObserver(num_sets, associativity));
   else
      d.reward.reset(new rlrepl::RewardObserver(num_sets, associativity, m_reward->config().scope));

   const SInt64 interval = Sim()->getCfg()->getIntDefault(cfgname + "/rl_diag/snapshot_interval", 1000);
   LOG_ASSERT_ERROR(interval >= 0, "%s/rl_diag/snapshot_interval must be >= 0 (%ld)", cfgname.c_str(), (long)interval);
   d.snapshot_interval = (UInt64)interval;
   String file = "-";
   if (d.snapshot_interval > 0)
   {
      file = Sim()->getConfig()->formatOutputFileName(String("rl-snapshots-") + name + "-" + String(std::to_string(core_id).c_str()) + ".csv");
      d.snapshots = fopen(file.c_str(), "w");
      LOG_ASSERT_ERROR(d.snapshots != NULL, "Cannot open %s", file.c_str());
      if (m_algorithm == PAPER)
         fprintf(d.snapshots, "decisions,instructions,in_roi,history_entries,frac_M,frac_E,frac_S,frac_AT,"
                              "first_accepted,second_chances,reward_negative,reward_positive,history_aged_out\n");
      else
         fprintf(d.snapshots, "decisions,instructions,in_roi,wS,wE,wM,wBT,wAT,evict_bad,evict_good,protect_good,protect_wasted,"
                              "passes,max_passes,all_tied,tied_distinct,small_margin,flips_state,flips_sharing\n");
   }
   printf("[RL] %s core %d: diagnostics on, snapshot every %lu full-set decisions (%s)\n", name.c_str(), core_id,
          (unsigned long)d.snapshot_interval, file.c_str());
   registerDiagnosticStats(name, core_id);
}

void
CacheSetInfoRL::registerDiagnosticStats(String name, core_id_t core_id)
{
   RlDiagnostics &d = *m_diag;
   const String p = String(m_algorithm == PAPER ? "sf-" : "rl-") + "diag-";
   auto reg = [&](const String &metric, const UInt64 *value)
   {
      registerStatsMetric(name, core_id, p + metric, const_cast<UInt64 *>(value)); // read-only use
   };
   auto regClasses = [&](const String &metric, const rlrepl::ClassCounts &c)
   {
      for (std::size_t i = 0; i < 3; ++i)
         reg(metric + "-" + String(std::string(rlrepl::kClassNames[i]).c_str()), &c.state[i]);
      for (std::size_t i = 0; i < 2; ++i)
         reg(metric + "-" + String(std::string(rlrepl::kClassNames[3 + i]).c_str()), &c.share[i]);
   };
   auto regRuns = [&](const rlrepl::RunTracker &t)
   {
      for (std::size_t e = 0; e < rlrepl::kRunEnds; ++e)
         for (std::size_t b = 0; b < rlrepl::kRunBuckets; ++b)
            reg(String("run-") + std::string(rlrepl::kRunEndNames[e]).c_str() + "-" + std::string(rlrepl::kRunBucketNames[b]).c_str(),
                &t.histogram[e][b]);
      reg("run-ended-passes", &t.ended_passes);
   };

   if (m_algorithm == PAPER)
   {
      const rlrepl::PaperDiagStats &s = d.paper->stats;
      regClasses("history-hit", s.history_hits);
      regClasses("aged-out", s.aged_out);
      reg("decisions", &s.decisions);
      reg("pinned-short-sum", &s.pinned_short);
      reg("pinned-long-sum", &s.pinned_long);
      regRuns(d.paper->runs());
   }
   else
   {
      const rlrepl::RewardDiagStats &s = d.reward->stats;
      for (std::size_t k = 0; k < s.rewards.size(); ++k)
         regClasses(String("reward-") + std::string(rlrepl::kRewardNames[k]).c_str(), s.rewards[k]);
      reg("decisions", &s.decisions);
      reg("pinned-short-sum", &s.pinned_short);
      reg("pinned-long-sum", &s.pinned_long);
      reg("all-tied", &s.all_tied);
      reg("tied-distinct", &s.tied_distinct);
      reg("small-margin", &s.small_margin);
      for (std::size_t i = 0; i < s.victim_position.size(); ++i)
         reg(String("victim-position-") + std::to_string(i).c_str(), &s.victim_position[i]);
      reg("flips-state", &s.flips_state);
      reg("flips-sharing", &s.flips_sharing);
      reg("double-agree", &s.double_agree);
      reg("double-contradict", &s.double_contradict);
      reg("double-snapshot-differs", &s.double_snapshot_differs);
      reg("double-dropped", &s.double_dropped);
      regRuns(d.reward->runs());
   }
   reg("misses", &d.misses.total);
   reg("miss-cold", &d.misses.cold);
   reg("miss-replaced-in-history", &d.misses.replaced_in_history);
   reg("miss-replaced-not-in-history", &d.misses.replaced_not_in_history);
   reg("miss-coherence", &d.misses.coherence);
   reg("miss-back-invalidation", &d.misses.back_invalidation);
   reg("miss-replaced-in-history-other-write", &d.misses.replaced_in_history_other_write);
   reg("miss-replaced-not-in-history-other-write", &d.misses.replaced_not_in_history_other_write);
   reg("miss-inconsistent", &d.misses.inconsistent);
   regClasses("history-hit-other-write", d.history_hit_other_write);
   regClasses("evicted", d.evicted);
   reg("eviction-features-changed", &d.eviction_features_changed);
   reg("victims-M", &d.victims_m);
   reg("victims-M-unwritten", &d.victims_m_unwritten);
}

void
CacheSetInfoRL::writeSnapshot()
{
   RlDiagnostics &d = *m_diag;
   if (!d.snapshots)
      return;
   Core *core = Sim()->getCoreManager()->getCoreFromID(d.core_id);
   const unsigned long long instructions = core ? (unsigned long long)core->getInstructionCount() : 0ULL;
   const int in_roi = Sim()->getMagicServer()->inROI() ? 1 : 0;
   const unsigned long long decisions = (unsigned long long)adapter.decisions_with_eviction;
   if (m_algorithm == PAPER)
   {
      const rlrepl::PaperStats &s = m_paper->stats;
      UInt64 entries = 0, nm = 0, ne = 0, ns = 0, nat = 0;
      for (UInt32 set = 0; set < m_registered_sets; ++set)
         for (const rlrepl::HistoryEntry &e : m_paper->history(set).entries())
         {
            ++entries;
            nm += e.features.state == rlrepl::Mesi::M;
            ne += e.features.state == rlrepl::Mesi::E;
            ns += e.features.state == rlrepl::Mesi::S;
            nat += e.features.above;
         }
      const double known = (double)(nm + ne + ns);
      fprintf(d.snapshots, "%llu,%llu,%d,%llu,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu\n", decisions, instructions, in_roi,
              (unsigned long long)entries, known ? nm / known : 0.0, known ? ne / known : 0.0, known ? ns / known : 0.0,
              entries ? (double)nat / (double)entries : 0.0, (unsigned long long)s.first_accepted, (unsigned long long)s.second_chances,
              (unsigned long long)s.negative_rewards, (unsigned long long)s.positive_rewards, (unsigned long long)s.history_aged_out);
   }
   else
   {
      const rlrepl::RewardStats &s = m_reward->stats;
      const rlrepl::RewardDiagStats &o = d.reward->stats;
      fprintf(d.snapshots, "%llu,%llu,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu\n", decisions,
              instructions, in_roi, meanWeight(STAT_W_S), meanWeight(STAT_W_E), meanWeight(STAT_W_M), meanWeight(STAT_W_BT),
              meanWeight(STAT_W_AT), (unsigned long long)s.evict_bad, (unsigned long long)s.evict_good, (unsigned long long)s.protect_good,
              (unsigned long long)s.protect_wasted, (unsigned long long)s.passes, s.max_passes, (unsigned long long)o.all_tied,
              (unsigned long long)o.tied_distinct, (unsigned long long)o.small_margin, (unsigned long long)o.flips_state,
              (unsigned long long)o.flips_sharing);
   }
   fflush(d.snapshots);
   ++d.snapshot_rows;
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
   , m_fill_seq(associativity, 0)
{
   if (m_info->diag())
      RlWriteOracle::enable(blocksize);
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
CacheSetRL::featuresOf(UInt32 way) const
{
   const CacheBlockInfo *block = m_cache_block_info_array[way];
   return rlrepl::make_features(toMesi(block->getCState()), block->getSharerHint(), m_info->numCores());
}

rlrepl::Features
CacheSetRL::features(UInt32 way)
{
   const rlrepl::Features f = featuresOf(way);
   if (f.state == rlrepl::Mesi::I)
      ++m_info->adapter.candidate_state_unknown;
   return f;
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
      RlDiagnostics *diag = m_info->diag();
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
         if (diag)
            diag->paper->observe_decision(m_info->paper(), m_set, d);
         m_info->paper().commit(d);
         victim = d.victim;
         victim_features = d.victim_features;
      }
      else
      {
         const rlrepl::RewardDecision d = m_info->reward().choose(m_set, candidates, info);
         if (diag)
            diag->reward->observe_decision(m_info->reward(), m_set, candidates, [this](std::uint32_t way) { return featuresOf(way); }, d);
         m_info->reward().commit(m_set, d);
         victim = d.victim;
         victim_features = d.victim_features;
      }
      ++m_info->adapter.decisions_with_eviction;
      m_pending.valid = true;
      m_pending.way = victim;
      m_pending.features = victim_features;

      if (diag)
      {
         // I-3: an M victim that this core never wrote while it was resident got M on a read
         const CacheBlockInfo *block = m_cache_block_info_array[victim];
         if (block->getCState() == CacheState::MODIFIED)
         {
            ++diag->victims_m;
            if (!RlWriteOracle::writtenBySince(addressOf(block->getTag()), diag->core_id, m_fill_seq[victim]))
               ++diag->victims_m_unwritten;
         }
         if (diag->snapshot_interval && m_info->adapter.decisions_with_eviction % diag->snapshot_interval == 0)
            m_info->writeSnapshot();
      }
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
   RlDiagnostics *diag = m_info->diag();
   if (m_info->algorithm() == CacheSetInfoRL::REWARD)
   {
      ScopedLock sl(m_info->lock());
      if (diag)
         diag->reward->before_hit(m_info->reward(), m_set, accessed_index);
      m_info->reward().on_hit(m_set, accessed_index);
   }
   else if (diag)
   {
      ScopedLock sl(m_info->lock());
      diag->paper->before_hit(m_set, accessed_index);
   }
}

void
CacheSetRL::onMiss(IntPtr tag)
{
   ScopedLock sl(m_info->lock());
   RlDiagnostics *diag = m_info->diag();
   if (diag)
      classifyMiss(*diag, tag); // reads the history before the core removes a returning entry
   if (m_info->algorithm() == CacheSetInfoRL::PAPER)
   {
      if (diag)
         diag->paper->before_miss(m_info->paper(), m_set, tag);
      m_info->paper().on_miss(m_set, tag);
   }
   else
   {
      if (diag)
         diag->reward->before_miss(m_info->reward(), m_set, tag);
      m_info->reward().on_miss(m_set, tag);
   }
}

void
CacheSetRL::classifyMiss(RlDiagnostics &d, IntPtr tag)
{
   ++d.misses.total;
   const rlrepl::EvictionHistory &h = m_info->algorithm() == CacheSetInfoRL::PAPER ? m_info->paper().history(m_set) : m_info->reward().history(m_set);
   const rlrepl::HistoryEntry *entry = rlrepl::find_entry(h, tag);
   auto it = d.departed.find(tag);
   if (it == d.departed.end())
   {
      if (entry)
         ++d.misses.inconsistent;
      ++d.misses.cold;
      return;
   }
   const RlDiagnostics::Departure dep = it->second;
   d.departed.erase(it);
   if (dep.how != RlDiagnostics::REPLACED)
   {
      if (entry)
         ++d.misses.inconsistent;
      ++(dep.how == RlDiagnostics::COHERENCE ? d.misses.coherence : d.misses.back_invalidation);
      return;
   }
   // I-2: would another core's write have invalidated the line had it not been evicted?
   const bool other_write = RlWriteOracle::writtenByOtherSince(addressOf(tag), d.core_id, dep.seq);
   if (entry)
   {
      ++d.misses.replaced_in_history;
      if (other_write)
      {
         ++d.misses.replaced_in_history_other_write;
         d.history_hit_other_write.add(entry->features);
      }
   }
   else
   {
      ++d.misses.replaced_not_in_history;
      if (other_write)
         ++d.misses.replaced_not_in_history_other_write;
   }
}

void
CacheSetRL::onEviction(UInt32 way, const CacheBlockInfo *victim)
{
   ScopedLock sl(m_info->lock());
   rlrepl::Features f;
   if (m_pending.valid && m_pending.way == way)
   {
      f = m_pending.features;
      if (RlDiagnostics *diag = m_info->diag())
         if (!(f == featuresOf(way)))
            ++diag->eviction_features_changed;
   }
   else
   {
      ++m_info->adapter.eviction_without_decision;
      f = features(way);
   }
   m_pending.valid = false;

   if (RlDiagnostics *diag = m_info->diag())
   {
      diag->departed[victim->getTag()] = {RlDiagnostics::REPLACED, RlWriteOracle::now()};
      diag->evicted.add(f);
      if (m_info->algorithm() == CacheSetInfoRL::PAPER)
         diag->paper->before_eviction(m_info->paper(), m_set, victim->getTag());
      else
         diag->reward->before_eviction(m_info->reward(), m_set, victim->getTag(), f);
   }

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
   const rlrepl::FillKind kind = fill == CacheSetEvent::UPGRADE_REFILL ? rlrepl::FillKind::upgrade_refill : rlrepl::FillKind::demand;
   RlDiagnostics *diag = m_info->diag();
   if (diag)
      m_fill_seq[way] = RlWriteOracle::now();
   if (m_info->algorithm() == CacheSetInfoRL::PAPER)
   {
      if (diag)
         diag->paper->before_fill(m_set, way, tag, kind);
      m_info->paper().on_fill(m_set, tag);
   }
   else
   {
      if (diag)
         diag->reward->before_fill(m_info->reward(), m_set, way, tag, kind);
      m_info->reward().on_fill(m_set, way, tag, kind);
   }
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
   if (RlDiagnostics *diag = m_info->diag())
   {
      if (r != rlrepl::InvalidationReason::upgrade) // an upgrade comes back with its refill, without a miss
         diag->departed[tag] = {r == rlrepl::InvalidationReason::coherence ? RlDiagnostics::COHERENCE : RlDiagnostics::BACK_INVALIDATION,
                                RlWriteOracle::now()};
      if (m_info->algorithm() == CacheSetInfoRL::PAPER)
         diag->paper->before_invalidate(m_set, way, tag);
      else
         diag->reward->before_invalidate(m_info->reward(), m_set, way, tag, r);
   }
   // The paper reproduction keeps no per-line state; invalidations never touch either core's history
   if (m_info->algorithm() == CacheSetInfoRL::REWARD)
      m_info->reward().on_invalidate(m_set, way, tag, r);
}
