#ifndef RL_PAPER_POLICY_H
#define RL_PAPER_POLICY_H

/*
 * Souza & Freitas reproduction core: history-ratio weights and the next-attempt (one second chance) selection.
 *
 *   [P24] M. A. Souza, H. C. Freitas, "Reinforcement Learning-Based Cache Replacement Policies for Multicore
 *         Processors," IEEE Access 12, 2024.
 *   [T21] M. A. Souza, PhD thesis, PUC Minas, 2021, Ch. 7.
 *
 * Ported from ChampSim replacement/souza_freitas/sf_core.h (mesi-implementation, 148b9c3). The weight, rule and
 * empty-history logic is unchanged; differences are listed in docs/rl/POLICY_CORES.md:
 *   - the base policy is abstracted as an ordered candidate list (LRU via rl_lru_order.h);
 *   - RL-Both composition is selectable (Combine::And = ChampSim, Combine::Or = the figure read literally);
 *   - the paper's +1/-1 rewards are counted at demand fills (misses), not on every access.
 *
 * As in the paper, the rewards change nothing directly: the weights are recomputed from the per-set history at
 * every decision, and a -1 acts only by removing the returning block's history entry. No update rule is invented.
 * Decisions use only local information (the candidate's state and its delivered sharer class).
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "rl_events.h"
#include "rl_eviction_history.h"
#include "rl_features.h"

namespace rlrepl
{
// The paper's contradiction ([P24] §II-C text vs. its example and Figure 2): text = evict a candidate that matches the
// highest weight, example = keep it. See docs/rl/ALGORITHM_AUDIT.md §2.2.
enum class Rule { text, example };
// RL-Both: And = evict only if both checks say evict (ChampSim); Or = evict if either says evict. Under Rule::example,
// Or is "keep only if both the state and the sharing class match", i.e. Figure 2 read literally.
enum class Combine { And, Or };

inline std::optional<Rule> parse_rule(std::string_view s)
{
  if (s == "text")
    return Rule::text;
  if (s == "example")
    return Rule::example;
  return std::nullopt;
}
inline std::optional<Combine> parse_combine(std::string_view s)
{
  if (s == "and")
    return Combine::And;
  if (s == "or")
    return Combine::Or;
  return std::nullopt;
}
inline constexpr std::string_view name(Rule r) { return r == Rule::text ? "text" : "example"; }
inline constexpr std::string_view name(Combine c) { return c == Combine::And ? "and" : "or"; }

struct PaperConfig {
  Mode mode = Mode::both;
  Rule rule = Rule::text;
  Combine combine = Combine::And;
};

/*
 * Weight vector ([P24] §II-B): each weight is the fraction of history entries in that class, so each group sums to 1.
 * State weights are computed over the entries whose state is known (not I), AT/BT over all entries. The *_empty flags
 * mean "no distribution yet", which selects the paper's initial bias.
 */
struct RatioWeights {
  double m = 0, e = 0, s = 0;
  double at = 0, bt = 0;
  bool state_empty = true;
  bool bitset_empty = true;
};

inline RatioWeights compute_ratio_weights(const EvictionHistory& h)
{
  std::size_t nm = 0, ne = 0, ns = 0, nat = 0;
  for (const auto& entry : h.entries()) {
    nm += entry.features.state == Mesi::M;
    ne += entry.features.state == Mesi::E;
    ns += entry.features.state == Mesi::S;
    nat += entry.features.above;
  }
  RatioWeights w;
  if (const auto known = nm + ne + ns; known > 0) {
    w.m = static_cast<double>(nm) / static_cast<double>(known);
    w.e = static_cast<double>(ne) / static_cast<double>(known);
    w.s = static_cast<double>(ns) / static_cast<double>(known);
    w.state_empty = false;
  }
  if (!h.empty()) {
    w.at = static_cast<double>(nat) / static_cast<double>(h.size());
    w.bt = 1.0 - w.at;
    w.bitset_empty = false;
  }
  return w;
}

// "Highest" counts ties as highest (ChampSim). Only meaningful for S, E, M.
inline bool state_is_highest(const RatioWeights& w, Mesi state)
{
  const double own = state == Mesi::M ? w.m : (state == Mesi::E ? w.e : w.s);
  return own == std::max({w.m, w.e, w.s});
}

inline bool class_is_highest(const RatioWeights& w, bool above) { return above ? (w.at >= w.bt) : (w.bt >= w.at); }

// text: evict a highest-weight element; example: keep it
inline bool apply_rule(Rule r, bool is_highest) { return r == Rule::text ? is_highest : !is_highest; }

// State check. No state information (I) accepts; no distribution yet uses the initial bias: evict only E.
inline bool state_says_evict(const RatioWeights& w, Rule r, Mesi state)
{
  if (state == Mesi::I)
    return true;
  if (w.state_empty)
    return state == Mesi::E;
  return apply_rule(r, state_is_highest(w, state));
}

// Sharing check. No distribution yet uses the initial bias: evict only below-threshold blocks.
inline bool bitset_says_evict(const RatioWeights& w, Rule r, bool above)
{
  if (w.bitset_empty)
    return !above;
  return apply_rule(r, class_is_highest(w, above));
}

inline bool should_evict(const PaperConfig& cfg, const RatioWeights& w, Features f)
{
  switch (cfg.mode) {
  case Mode::state:
    return state_says_evict(w, cfg.rule, f.state);
  case Mode::bitset:
    return bitset_says_evict(w, cfg.rule, f.above);
  case Mode::both: {
    const bool by_state = state_says_evict(w, cfg.rule, f.state);
    const bool by_sharing = bitset_says_evict(w, cfg.rule, f.above);
    return cfg.combine == Combine::And ? (by_state && by_sharing) : (by_state || by_sharing);
  }
  case Mode::off:
  default:
    return true;
  }
}

struct PaperStats {
  std::uint64_t decisions = 0;          // RL checks performed
  std::uint64_t first_accepted = 0;     // candidate #1 evicted
  std::uint64_t second_chances = 0;     // candidate #1 kept, candidate #2 evicted unchecked
  std::uint64_t empty_history_decisions = 0;
  std::uint64_t negative_rewards = 0;   // demand fill of a tag found in the history (-1, entry removed)
  std::uint64_t positive_rewards = 0;   // demand fill of a tag not in the history (+1, no effect)
  std::uint64_t history_aged_out = 0;   // least recently updated entry dropped to make room
  std::uint64_t weighted_decisions = 0; // decisions with a non-empty history, for the averages below
  double sum_m = 0, sum_e = 0, sum_s = 0, sum_at = 0, sum_bt = 0;
};

struct PaperDecision {
  std::uint32_t victim = 0;
  std::uint32_t first = 0;     // the base policy's first choice
  bool checked = false;        // the RL check ran (mode != off and at least two candidates)
  bool second_chance = false;  // first choice rejected, second evicted without another check
  bool empty_history = false;
  RatioWeights weights{};      // the weights the check used
  Features victim_features{};  // recorded into the history when the eviction happens
};

class PaperPolicy
{
  PaperConfig cfg_;
  std::vector<EvictionHistory> history_;

public:
  PaperStats stats;

  PaperPolicy(PaperConfig cfg, std::uint32_t num_sets, std::uint32_t ways)
      : cfg_(cfg), history_(num_sets, EvictionHistory{2 * static_cast<std::size_t>(ways)})
  {
  }

  [[nodiscard]] const PaperConfig& config() const { return cfg_; }
  [[nodiscard]] const EvictionHistory& history(std::uint32_t set) const { return history_.at(set); }

  /*
   * Next attempt ([P24] §II-C/D, [T21] Fig. 7.1). `candidates` is the base policy's preference order (best victim
   * first; must not be empty); `info(way)` returns a candidate's current features. Candidate #1 is checked once; if
   * rejected it gets its second chance and candidate #2 is evicted without a check. Nothing is changed here, so the
   * caller (and the base policy) see the rejected way untouched: with LRU it stays at the LRU position.
   */
  template <typename Info>
  [[nodiscard]] PaperDecision choose(std::uint32_t set, const std::vector<std::uint32_t>& candidates, Info&& info) const
  {
    PaperDecision d;
    d.first = d.victim = candidates.at(0);
    if (cfg_.mode != Mode::off && candidates.size() >= 2) {
      const auto& h = history_.at(set);
      d.checked = true;
      d.empty_history = h.empty();
      d.weights = compute_ratio_weights(h);
      if (!should_evict(cfg_, d.weights, info(d.first))) {
        d.second_chance = true;
        d.victim = candidates[1];
      }
    }
    d.victim_features = info(d.victim);
    return d;
  }

  // The decision was carried out: statistics only
  void commit(const PaperDecision& d)
  {
    if (!d.checked)
      return;
    ++stats.decisions;
    (d.second_chance ? stats.second_chances : stats.first_accepted)++;
    if (d.empty_history) {
      ++stats.empty_history_decisions;
    } else {
      ++stats.weighted_decisions;
      stats.sum_m += d.weights.m;
      stats.sum_e += d.weights.e;
      stats.sum_s += d.weights.s;
      stats.sum_at += d.weights.at;
      stats.sum_bt += d.weights.bt;
    }
  }

  // A replacement eviction: the victim's tag and decision-time features enter the history
  void on_eviction(std::uint32_t set, std::uint64_t tag, Features at_decision)
  {
    if (cfg_.mode == Mode::off)
      return;
    if (history_.at(set).insert(tag, at_decision).has_value())
      ++stats.history_aged_out;
  }

  /*
   * A block was installed. Its history entry, if any, is removed ([P24] §II-B). For a demand fill (a miss) the paper's
   * reward is counted: -1 if the tag was in the history, +1 otherwise. Neither changes a weight directly.
   */
  void on_fill(std::uint32_t set, std::uint64_t tag, FillKind kind)
  {
    if (cfg_.mode == Mode::off)
      return;
    const bool was_in_history = history_.at(set).remove(tag).has_value();
    if (kind != FillKind::demand)
      return;
    (was_in_history ? stats.negative_rewards : stats.positive_rewards)++;
  }
};
} // namespace rlrepl

#endif
