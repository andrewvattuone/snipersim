#ifndef RL_REWARD_POLICY_H
#define RL_REWARD_POLICY_H

/*
 * Our modified coherence-aware RL replacement core (the senior-design "rl_mesi" algorithm): learned feature weights,
 * reward feedback through a per-set eviction history and through protection decisions, and selection of the
 * highest-scoring block among the N oldest candidates.
 *
 * Ported from ChampSim replacement/rl_mesi/rl_core.h (mesi-implementation, 148b9c3), core algorithm only:
 *   - kept: weight table and its EMA update, scoring, window selection with ties to the older way, protection
 *     feedback, history feedback, decision-time feature snapshots, zero initial weights, mode off = base policy;
 *   - not ported (Stage 1 scope): Iprev, adaptive threshold, BRRIP set dueling, epsilon exploration;
 *   - changed: invalidations and upgrades are reported explicitly (rl_events.h) instead of being inferred from fills;
 *     recency comes from the base policy's candidate list instead of a private LRU; per-set weights are available as
 *     an option (default global, as ChampSim).
 * Details and the remaining differences: docs/rl/POLICY_CORES.md.
 *
 *   V(b) = wM*xM + wE*xE + wS*xS + wAT*xAT + wBT*xBT      (higher = more attractive to evict)
 *   w_i += alpha * (r - w_i)                              for the features active in the rewarded snapshot
 *
 *   evicted block refilled while still in the history   -1
 *   evicted block's history entry ages out               +1
 *   protected (passed-over) block hit                    -1
 *   protected block evicted without a hit                +1
 *   coherence / back invalidation, upgrade               none
 *
 * Event order on a miss (as in ChampSim, where the tag check precedes find_victim):
 *   on_miss(set, tag) -> [choose -> commit -> on_eviction(victim)] -> on_fill(set, way, tag, demand)
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "rl_events.h"
#include "rl_eviction_history.h"
#include "rl_features.h"

namespace rlrepl
{
// Who shares a weight table: one per cache (ChampSim rl_mesi, the default) or one per set (the paper's scope)
enum class WeightScope { global, per_set };

inline std::optional<WeightScope> parse_weight_scope(std::string_view s)
{
  if (s == "global")
    return WeightScope::global;
  if (s == "per_set")
    return WeightScope::per_set;
  return std::nullopt;
}
inline constexpr std::string_view name(WeightScope s) { return s == WeightScope::global ? "global" : "per_set"; }

struct RewardConfig {
  Mode mode = Mode::both;
  std::uint32_t window = 4;     // N: how many of the base policy's first candidates are compared (>= 1)
  double alpha = 0.1;           // learning rate, in [0, 1]
  bool protect_feedback = true; // learn from passed-over (protected) blocks
  WeightScope scope = WeightScope::global;
};

/*
 * One weight per feature value: "how good is it to evict a block with this feature?" The state group (wS, wE, wM) and
 * the sharing group (wBT, wAT) each have exactly one active value per block, so a score is the sum of two entries.
 * A block whose state is I (no information) contributes no state weight, as in ChampSim.
 */
class WeightTable
{
  std::array<double, 3> w_state_{}; // S, E, M
  std::array<double, 2> w_share_{}; // below (BT), above (AT)

  static std::optional<std::size_t> state_index(Mesi s)
  {
    switch (s) {
    case Mesi::S:
      return 0;
    case Mesi::E:
      return 1;
    case Mesi::M:
      return 2;
    case Mesi::I:
    default:
      return std::nullopt;
    }
  }

public:
  [[nodiscard]] double score(Mode m, Features f) const
  {
    double v = 0;
    if (uses_state(m))
      if (auto i = state_index(f.state))
        v += w_state_[*i];
    if (uses_sharing(m))
      v += w_share_[f.above ? 1 : 0];
    return v;
  }

  // Move the weights of the features active in `f` toward r (exponential moving average)
  void reward(Mode m, Features f, double r, double alpha)
  {
    if (uses_state(m))
      if (auto i = state_index(f.state)) {
        auto& w = w_state_[*i];
        w += alpha * (r - w);
      }
    if (uses_sharing(m)) {
      auto& w = w_share_[f.above ? 1 : 0];
      w += alpha * (r - w);
    }
  }

  [[nodiscard]] double state_weight(Mesi s) const
  {
    auto i = state_index(s);
    return i ? w_state_[*i] : 0.0;
  }
  [[nodiscard]] double share_weight(bool above) const { return w_share_[above ? 1 : 0]; }

  // Initialisation (all weights start at 0) and tests
  void set_state_weight(Mesi s, double v)
  {
    if (auto i = state_index(s))
      w_state_[*i] = v;
  }
  void set_share_weight(bool above, double v) { w_share_[above ? 1 : 0] = v; }
};

// Per-way learning metadata for the block currently installed there
struct LineMeta {
  bool is_protected = false; // passed over by a decision and neither hit nor evicted since
  Features snapshot{};       // its features when it was (last) passed over
  std::uint32_t passes = 0;  // decisions that passed it over since it was installed (diagnostic only)
};

struct RewardStats {
  std::uint64_t decisions = 0;
  std::uint64_t chose_oldest = 0;
  std::uint64_t chose_other = 0;
  std::uint64_t evict_bad = 0;      // -1: evicted block refilled while in the history
  std::uint64_t evict_good = 0;     // +1: history entry aged out
  std::uint64_t protect_good = 0;   // -1: protected block hit
  std::uint64_t protect_wasted = 0; // +1: protected block evicted without a hit
  std::uint64_t passes = 0;         // passed-over events
  std::uint32_t max_passes = 0;     // most times one block was passed over (repeated protection, audit I-1)
  std::array<std::uint64_t, 3> invalidations{}; // by InvalidationReason
  std::uint64_t upgrade_carryovers = 0;         // metadata restored on an upgrade refill
  std::uint64_t upgrade_refills_unmatched = 0;  // upgrade refill without a matching upgrade invalidation
  std::uint64_t upgrade_stash_overwritten = 0;  // upgrade invalidation while another one in the set awaited its refill
  std::uint64_t refill_removals = 0;            // history entry found at a fill not preceded by on_miss (expected 0)
};

// Outcome of choose(): the victim and the candidates older than it that it was preferred over, with their features
struct RewardDecision {
  std::uint32_t victim = 0;
  bool learned = false; // false when mode is off or there is a single candidate: the base policy's choice, unrecorded
  std::vector<std::pair<std::uint32_t, Features>> passed_over;
  Features victim_features{}; // recorded into the history when the eviction happens
};

class RewardPolicy
{
  struct PendingUpgrade {
    bool valid = false;
    std::uint64_t tag = 0;
    LineMeta meta{};
  };

  RewardConfig cfg_;
  std::uint32_t ways_;
  std::vector<WeightTable> weights_; // one (global) or one per set
  std::vector<std::vector<LineMeta>> lines_;
  std::vector<EvictionHistory> history_;
  std::vector<PendingUpgrade> pending_upgrade_;

  WeightTable& table(std::uint32_t set) { return weights_.at(cfg_.scope == WeightScope::global ? 0 : set); }
  LineMeta& line_(std::uint32_t set, std::uint32_t way) { return lines_.at(set).at(way); }

  void reward(std::uint32_t set, Features f, double r) { table(set).reward(cfg_.mode, f, r, cfg_.alpha); }

public:
  RewardStats stats;

  RewardPolicy(RewardConfig cfg, std::uint32_t num_sets, std::uint32_t ways, WeightTable initial = {})
      : cfg_(cfg), ways_(ways), weights_(cfg.scope == WeightScope::global ? 1 : num_sets, initial),
        lines_(num_sets, std::vector<LineMeta>(ways)), history_(num_sets, EvictionHistory{2 * static_cast<std::size_t>(ways)}),
        pending_upgrade_(num_sets)
  {
  }

  [[nodiscard]] const RewardConfig& config() const { return cfg_; }
  [[nodiscard]] const WeightTable& weights(std::uint32_t set = 0) const
  {
    return weights_.at(cfg_.scope == WeightScope::global ? 0 : set);
  }
  [[nodiscard]] const LineMeta& line(std::uint32_t set, std::uint32_t way) const { return lines_.at(set).at(way); }
  [[nodiscard]] const EvictionHistory& history(std::uint32_t set) const { return history_.at(set); }

  /*
   * Pick a victim without changing anything. `candidates` is the base policy's preference order (LRU: oldest first;
   * must not be empty); `info(way)` returns a candidate's current features. Among the first N candidates the highest
   * score wins; scanning in order with a strict comparison sends ties to the older candidate. With all weights equal
   * (initially all 0) this is the base policy's choice.
   */
  template <typename Info>
  [[nodiscard]] RewardDecision choose(std::uint32_t set, const std::vector<std::uint32_t>& candidates, Info&& info) const
  {
    RewardDecision d;
    d.victim = candidates.at(0);
    if (cfg_.mode != Mode::off && candidates.size() >= 2) {
      d.learned = true;
      const auto window = std::clamp<std::size_t>(cfg_.window, 1, candidates.size());
      const auto& w = weights(set);
      std::vector<Features> f(window);
      for (std::size_t i = 0; i < window; ++i)
        f[i] = info(candidates[i]);

      std::size_t chosen = 0;
      double best = w.score(cfg_.mode, f[0]);
      for (std::size_t i = 1; i < window; ++i)
        if (const double v = w.score(cfg_.mode, f[i]); v > best) {
          best = v;
          chosen = i;
        }
      d.victim = candidates[chosen];
      for (std::size_t i = 0; i < chosen; ++i)
        d.passed_over.emplace_back(candidates[i], f[i]);
    }
    d.victim_features = info(d.victim);
    return d;
  }

  /*
   * The decision was carried out (in Sniper, as soon as it is taken). Passed-over candidates become protected with
   * their decision-time features; their recency is NOT changed (audit I-1). If the victim itself was protected, the
   * protection was wasted: +1 to its saved features.
   */
  void commit(std::uint32_t set, const RewardDecision& d)
  {
    if (!d.learned)
      return;
    ++stats.decisions;
    (d.passed_over.empty() ? stats.chose_oldest : stats.chose_other)++;

    for (const auto& [way, f] : d.passed_over) {
      auto& l = line_(set, way);
      ++l.passes;
      ++stats.passes;
      stats.max_passes = std::max(stats.max_passes, l.passes);
      if (cfg_.protect_feedback) {
        l.is_protected = true;
        l.snapshot = f;
      }
    }

    auto& v = line_(set, d.victim);
    if (v.is_protected) {
      ++stats.protect_wasted;
      reward(set, v.snapshot, +1.0);
      v.is_protected = false;
    }
  }

  // A replacement eviction: the victim enters the history; an entry pushed out ages out without returning (+1).
  // On a miss, on_miss for the incoming tag must come first (see on_miss).
  void on_eviction(std::uint32_t set, std::uint64_t tag, Features at_decision)
  {
    if (cfg_.mode == Mode::off)
      return;
    if (auto aged = history_.at(set).insert(tag, at_decision)) {
      ++stats.evict_good;
      reward(set, aged->features, +1.0);
    }
  }

  /*
   * A demand miss on `tag` that will be filled into `set`, reported BEFORE the victim for that fill is chosen. If the
   * tag is in the history, the earlier eviction was bad: -1 to the saved features, entry removed. This is ChampSim's
   * order (rl_mesi on_access on the tag check, then find_victim / replacement_cache_fill at the fill), so the victim is
   * chosen with the -1 already applied and the victim's insertion cannot push out the returning block's entry.
   * Not called for an upgrade refill (the block was never evicted).
   */
  void on_miss(std::uint32_t set, std::uint64_t tag)
  {
    if (cfg_.mode == Mode::off)
      return;
    if (auto entry = history_.at(set).remove(tag)) {
      ++stats.evict_bad;
      reward(set, entry->features, -1.0);
    }
  }

  /*
   * A block was installed in `way`. Demand fill: the way's metadata starts fresh (the history was handled by on_miss).
   * Upgrade refill: the block never left from the policy's point of view; its metadata from before the upgrade
   * invalidation is restored, no feedback. A history entry for the tag can only be found here if the fill was not
   * preceded by on_miss; it is removed silently, as ChampSim's on_fill does, and counted.
   */
  void on_fill(std::uint32_t set, std::uint32_t way, std::uint64_t tag, FillKind kind)
  {
    auto& l = line_(set, way);
    l = LineMeta{};

    if (kind == FillKind::upgrade_refill) {
      auto& p = pending_upgrade_.at(set);
      if (p.valid && p.tag == tag) {
        l = p.meta;
        p.valid = false;
        ++stats.upgrade_carryovers;
      } else {
        ++stats.upgrade_refills_unmatched; // a pending upgrade of another block stays pending
      }
    }

    if (history_.at(set).remove(tag).has_value())
      ++stats.refill_removals;
  }

  // A hit that updates recency. A protected block that is used again shows the protection was right: -1.
  void on_hit(std::uint32_t set, std::uint32_t way)
  {
    auto& l = line_(set, way);
    if (cfg_.mode == Mode::off || !l.is_protected)
      return;
    ++stats.protect_good;
    reward(set, l.snapshot, -1.0);
    l.is_protected = false;
  }

  /*
   * A block left without a replacement decision. Never an eviction: no history entry, no feedback, and a later miss on
   * it is not blamed on the policy (it is not in the history). For an upgrade, the metadata is kept for the refill.
   */
  void on_invalidate(std::uint32_t set, std::uint32_t way, std::uint64_t tag, InvalidationReason reason)
  {
    ++stats.invalidations.at(static_cast<std::size_t>(reason));
    auto& l = line_(set, way);
    if (reason == InvalidationReason::upgrade) {
      // One pending upgrade per set. A second one before the first refill (possible only if several hardware threads
      // or cores share this cache, see POLICY_CORES.md) replaces it; the first refill then starts fresh.
      auto& p = pending_upgrade_.at(set);
      if (p.valid)
        ++stats.upgrade_stash_overwritten;
      p = {true, tag, l};
    }
    l = LineMeta{};
  }
};
} // namespace rlrepl

#endif
