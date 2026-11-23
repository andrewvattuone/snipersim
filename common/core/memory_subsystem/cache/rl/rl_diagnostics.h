#ifndef RL_DIAGNOSTICS_H
#define RL_DIAGNOSTICS_H

/*
 * Stage 4 diagnostics for the two RL replacement cores (docs/rl/POLICY_CORES.md, "Diagnostics").
 *
 * Observation only. An observer is called immediately BEFORE the corresponding core call, with the same arguments,
 * and only reads the core through its const accessors (history, line metadata, weights, configuration). It never
 * changes the core, so decisions are identical with and without it. Where it predicts what the core is about to do
 * (which history entry ages out, whether a block is protected), the prediction is checked against the core's own
 * counters: every per-class total must equal the matching core counter (unit tests and check_rl_policy.py).
 *
 *   RewardObserver   rl_lru: rewards by type and feature class, runs of consecutive passes (I-1), pinned ways,
 *                    ties and small score margins (I-4), weight ranking changes (I-4), victim window position,
 *                    double rewards of protected blocks (I-6)
 *   PaperObserver    sf_lru: history hits and aged-out entries by feature class, runs of consecutive rejections of
 *                    candidate #1 (I-1), pinned ways
 *
 * Call order for one miss, as for the cores: before_miss -> [observe_decision -> before_eviction] -> before_fill.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "rl_events.h"
#include "rl_features.h"
#include "rl_paper_policy.h"
#include "rl_reward_policy.h"

namespace rlrepl
{
// Run-length histogram: 1, 2, 3-4, 5-8, 9-16, 17-32, 33+
inline constexpr std::size_t kRunBuckets = 7;
inline constexpr std::array<std::string_view, kRunBuckets> kRunBucketNames{"1", "2", "3-4", "5-8", "9-16", "17-32", "33+"};

inline std::size_t run_bucket(std::uint32_t n) // n >= 1
{
  std::size_t b = 0;
  std::uint32_t upper = 1;
  while (b + 1 < kRunBuckets && n > upper) {
    ++b;
    upper *= 2;
  }
  return b;
}

// How a run of consecutive passes (or rejections) of one block ended
enum class RunEnd { hit, evicted, invalidated };
inline constexpr std::size_t kRunEnds = 3;
inline constexpr std::array<std::string_view, kRunEnds> kRunEndNames{"hit", "evicted", "invalidated"};

using RunHistogram = std::array<std::array<std::uint64_t, kRunBuckets>, kRunEnds>;

// A "pinned" way: passed over (rejected) at least this many consecutive decisions without a hit
inline constexpr std::uint32_t kPinnedShort = 2;
inline constexpr std::uint32_t kPinnedLong = 8;

// Score difference below which candidates with different features count as indistinguishable (weights are in [-1, 1])
inline constexpr double kSmallMargin = 0.01;

// Event counts by the features they concern: state group S, E, M and sharing group BT, AT. A state I (no information,
// never a resident Sniper block) is not counted in the state group.
struct ClassCounts {
  std::array<std::uint64_t, 3> state{}; // S, E, M
  std::array<std::uint64_t, 2> share{}; // BT, AT

  void add(Features f)
  {
    if (f.state == Mesi::S)
      ++state[0];
    else if (f.state == Mesi::E)
      ++state[1];
    else if (f.state == Mesi::M)
      ++state[2];
    ++share[f.above ? 1 : 0];
  }
  [[nodiscard]] std::uint64_t total() const { return share[0] + share[1]; }
};
inline constexpr std::array<std::string_view, 5> kClassNames{"S", "E", "M", "BT", "AT"};

// Per-way run counters with the single per-set upgrade slot that the reward core uses for its own metadata
class RunTracker
{
  struct Stash {
    bool valid = false;
    std::uint64_t tag = 0;
    std::uint32_t run = 0;
  };
  std::vector<std::vector<std::uint32_t>> run_;
  std::vector<Stash> stash_;

public:
  RunHistogram histogram{};
  std::uint64_t ended_passes = 0; // sum of the lengths of all ended runs (closure: plus open_passes() = passes made)

  RunTracker(std::uint32_t num_sets, std::uint32_t ways) : run_(num_sets, std::vector<std::uint32_t>(ways)), stash_(num_sets) {}

  [[nodiscard]] std::uint32_t run(std::uint32_t set, std::uint32_t way) const { return run_.at(set).at(way); }
  void extend(std::uint32_t set, std::uint32_t way) { ++run_.at(set).at(way); }

  void end(std::uint32_t set, std::uint32_t way, RunEnd how)
  {
    auto& r = run_.at(set).at(way);
    if (r > 0) {
      ++histogram.at(static_cast<std::size_t>(how)).at(run_bucket(r));
      ended_passes += r;
    }
    r = 0;
  }

  [[nodiscard]] std::uint64_t open_passes() const
  {
    std::uint64_t n = 0;
    for (const auto& ways : run_)
      for (std::uint32_t r : ways)
        n += r;
    for (const auto& s : stash_)
      n += s.valid ? s.run : 0;
    return n;
  }

  // Ways of the set whose current run is at least k
  [[nodiscard]] std::uint32_t count_at_least(std::uint32_t set, std::uint32_t k) const
  {
    const auto& ways = run_.at(set);
    return static_cast<std::uint32_t>(std::count_if(ways.begin(), ways.end(), [k](std::uint32_t r) { return r >= k; }));
  }

  // Upgrade: keep the run for the refill (the reward core carries its metadata the same way); `carry` = false ends it
  void upgrade_invalidate(std::uint32_t set, std::uint32_t way, std::uint64_t tag, bool carry)
  {
    if (carry) {
      stash_.at(set) = {true, tag, run_.at(set).at(way)};
      run_.at(set).at(way) = 0;
    } else {
      end(set, way, RunEnd::invalidated);
    }
  }

  void fill(std::uint32_t set, std::uint32_t way, std::uint64_t tag, FillKind kind)
  {
    auto& r = run_.at(set).at(way);
    r = 0;
    auto& s = stash_.at(set);
    if (kind == FillKind::upgrade_refill && s.valid && s.tag == tag) {
      r = s.run;
      s.valid = false;
    }
  }
};

// The entry that EvictionHistory::insert(tag) is about to drop, if any
inline const HistoryEntry* entry_to_age_out(const EvictionHistory& h, std::uint64_t tag)
{
  if (h.capacity() == 0 || h.contains(tag) || h.size() < h.capacity())
    return nullptr;
  return &h.entries().front();
}

inline const HistoryEntry* find_entry(const EvictionHistory& h, std::uint64_t tag)
{
  for (const auto& e : h.entries())
    if (e.tag == tag)
      return &e;
  return nullptr;
}

enum class RewardKind { evict_bad, evict_good, protect_good, protect_wasted };
inline constexpr std::array<std::string_view, 4> kRewardNames{"evict-bad", "evict-good", "protect-good", "protect-wasted"};

struct RewardDiagStats {
  std::array<ClassCounts, 4> rewards{}; // by RewardKind, classified by the features that were rewarded
  std::uint64_t decisions = 0;          // learned decisions (equals RewardStats::decisions)
  std::uint64_t pinned_short = 0;       // sum over decisions: ways of the set with a run >= kPinnedShort
  std::uint64_t pinned_long = 0;        // ... >= kPinnedLong
  std::uint64_t all_tied = 0;           // every window candidate had the same score
  std::uint64_t tied_distinct = 0;      // ... although their features were not all equal
  std::uint64_t small_margin = 0;       // features not all equal, best - worst score < kSmallMargin (ties included)
  std::array<std::uint64_t, 8> victim_position{}; // the victim's window position (0 = LRU); 7 = 7 or later
  std::uint64_t flips_state = 0;        // the order of wS, wE, wM differs from the previous decision on that table
  std::uint64_t flips_sharing = 0;      // the order of wBT, wAT differs
  std::uint64_t double_agree = 0;       // protect-wasted (+1), then the evicted entry aged out (+1)
  std::uint64_t double_contradict = 0;  // protect-wasted (+1), then the evicted block returned (-1)
  std::uint64_t double_snapshot_differs = 0; // the +1 rewarded other features than the eviction recorded
  std::uint64_t double_dropped = 0;     // entry left the history without an outcome (fill without on_miss; expect 0)
  ClassCounts protect_invalidated{};    // protect_invalidation_feedback: protected lines invalidated, by snapshot class
};

class RewardObserver
{
  struct Pending {
    bool valid = false;
    Features snapshot{};
  };

  RunTracker runs_;
  std::vector<std::vector<std::uint64_t>> wasted_; // per set: tags of history entries whose block got protect-wasted
  std::vector<Pending> pending_;            // per set: the decision's victim got protect-wasted
  std::vector<int> prev_state_order_;       // per weight table; -1 = not seen yet
  std::vector<int> prev_share_order_;

  static int compare(double a, double b) { return a < b ? 0 : (a == b ? 1 : 2); }

  bool take_wasted(std::uint32_t set, std::uint64_t tag)
  {
    auto& v = wasted_.at(set);
    auto it = std::find(v.begin(), v.end(), tag);
    if (it == v.end())
      return false;
    v.erase(it);
    return true;
  }

public:
  RewardDiagStats stats;

  RewardObserver(std::uint32_t num_sets, std::uint32_t ways, WeightScope scope)
      : runs_(num_sets, ways), wasted_(num_sets), pending_(num_sets),
        prev_state_order_(scope == WeightScope::global ? 1 : num_sets, -1), prev_share_order_(prev_state_order_.size(), -1)
  {
  }

  [[nodiscard]] const RunTracker& runs() const { return runs_; }

  // Before RewardPolicy::on_miss
  void before_miss(const RewardPolicy& p, std::uint32_t set, std::uint64_t tag)
  {
    if (p.config().mode == Mode::off)
      return;
    if (const HistoryEntry* e = find_entry(p.history(set), tag)) {
      stats.rewards[static_cast<std::size_t>(RewardKind::evict_bad)].add(e->features);
      if (take_wasted(set, tag))
        ++stats.double_contradict;
    }
  }

  // After RewardPolicy::choose, before commit; `info` as given to choose (must not have side effects)
  template <typename Info>
  void observe_decision(const RewardPolicy& p, std::uint32_t set, const std::vector<std::uint32_t>& candidates, Info&& info,
                        const RewardDecision& d)
  {
    if (!d.learned) {
      runs_.end(set, d.victim, RunEnd::evicted); // a single eligible candidate: no decision, but the victim leaves
      return;
    }
    ++stats.decisions;
    stats.pinned_short += runs_.count_at_least(set, kPinnedShort);
    stats.pinned_long += runs_.count_at_least(set, kPinnedLong);

    // Scores of the window, as choose() computed them (selection_score: the weight score for the learned selection,
    // the fixed rank for priority, 0 for random, whose candidates are therefore always all tied)
    const auto& cfg = p.config();
    const auto& w = p.weights(set);
    const auto window = std::clamp<std::size_t>(cfg.window, 1, candidates.size());
    double lo = 0, hi = 0;
    bool same_features = true;
    std::size_t position = 0;
    Features first{};
    for (std::size_t i = 0; i < window; ++i) {
      const Features f = info(candidates[i]);
      const double v = p.selection_score(set, f);
      if (i == 0) {
        lo = hi = v;
        first = f;
      } else {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        same_features &= (f == first);
      }
      if (candidates[i] == d.victim)
        position = i;
    }
    if (lo == hi) {
      ++stats.all_tied;
      if (!same_features)
        ++stats.tied_distinct;
    }
    if (!same_features && hi - lo < kSmallMargin)
      ++stats.small_margin;
    ++stats.victim_position[std::min<std::size_t>(position, stats.victim_position.size() - 1)];

    // Order of the weights within each group, compared with the previous decision that used the same table
    const std::size_t t = prev_state_order_.size() == 1 ? 0 : set;
    const double ws = w.state_weight(Mesi::S), we = w.state_weight(Mesi::E), wm = w.state_weight(Mesi::M);
    const int state_order = 9 * compare(ws, we) + 3 * compare(ws, wm) + compare(we, wm);
    const int share_order = compare(w.share_weight(false), w.share_weight(true));
    if (prev_state_order_[t] >= 0 && prev_state_order_[t] != state_order && uses_state(cfg.mode))
      ++stats.flips_state;
    if (prev_share_order_[t] >= 0 && prev_share_order_[t] != share_order && uses_sharing(cfg.mode))
      ++stats.flips_sharing;
    prev_state_order_[t] = state_order;
    prev_share_order_[t] = share_order;

    // commit(): a protected victim gets protect-wasted (+1); passed-over candidates extend their runs
    const LineMeta& v = p.line(set, d.victim);
    if (v.is_protected) {
      stats.rewards[static_cast<std::size_t>(RewardKind::protect_wasted)].add(v.snapshot);
      pending_.at(set) = {true, v.snapshot};
    }
    runs_.end(set, d.victim, RunEnd::evicted);
    for (const auto& [way, f] : d.passed_over)
      runs_.extend(set, way);
  }

  // Before RewardPolicy::on_eviction
  void before_eviction(const RewardPolicy& p, std::uint32_t set, std::uint64_t tag, Features at_decision)
  {
    auto& pending = pending_.at(set);
    if (p.config().mode == Mode::off) {
      pending.valid = false;
      return;
    }
    const auto& h = p.history(set);
    if (h.contains(tag) && take_wasted(set, tag)) // re-inserting a tag still in the history replaces its entry
      ++stats.double_dropped;
    if (const HistoryEntry* aged = entry_to_age_out(h, tag)) {
      stats.rewards[static_cast<std::size_t>(RewardKind::evict_good)].add(aged->features);
      if (take_wasted(set, aged->tag))
        ++stats.double_agree;
    }
    if (pending.valid) {
      if (h.capacity() > 0)
        wasted_.at(set).push_back(tag);
      if (!(pending.snapshot == at_decision))
        ++stats.double_snapshot_differs;
      pending.valid = false;
    }
  }

  // Before RewardPolicy::on_fill
  void before_fill(const RewardPolicy& p, std::uint32_t set, std::uint32_t way, std::uint64_t tag, FillKind kind)
  {
    runs_.fill(set, way, tag, kind);
    if (p.history(set).contains(tag) && take_wasted(set, tag)) // the core removes the entry silently
      ++stats.double_dropped;
  }

  // Before RewardPolicy::on_hit
  void before_hit(const RewardPolicy& p, std::uint32_t set, std::uint32_t way)
  {
    const LineMeta& l = p.line(set, way);
    if (p.config().mode != Mode::off && l.is_protected)
      stats.rewards[static_cast<std::size_t>(RewardKind::protect_good)].add(l.snapshot);
    runs_.end(set, way, RunEnd::hit);
  }

  // Before RewardPolicy::on_invalidate
  void before_invalidate(const RewardPolicy& p, std::uint32_t set, std::uint32_t way, std::uint64_t tag, InvalidationReason reason)
  {
    const auto& cfg = p.config();
    if (cfg.protect_invalidation_feedback && cfg.mode != Mode::off && reason != InvalidationReason::upgrade && p.line(set, way).is_protected)
      stats.protect_invalidated.add(p.line(set, way).snapshot);
    if (reason == InvalidationReason::upgrade)
      runs_.upgrade_invalidate(set, way, tag, true);
    else
      runs_.end(set, way, RunEnd::invalidated);
  }
};

struct PaperDiagStats {
  ClassCounts history_hits{}; // negative rewards, by the returning block's history entry
  ClassCounts aged_out{};     // entries dropped for space
  std::uint64_t decisions = 0; // checked decisions (equals PaperStats::decisions)
  std::uint64_t pinned_short = 0;
  std::uint64_t pinned_long = 0;
};

// Runs are consecutive rejections as candidate #1: a rejected block keeps its LRU position, so the next decision in
// the set examines it again. An upgrade ends the run (the paper core keeps no per-line state; the refill is at MRU).
class PaperObserver
{
  RunTracker runs_;

public:
  PaperDiagStats stats;

  PaperObserver(std::uint32_t num_sets, std::uint32_t ways) : runs_(num_sets, ways) {}

  [[nodiscard]] const RunTracker& runs() const { return runs_; }

  void before_miss(const PaperPolicy& p, std::uint32_t set, std::uint64_t tag)
  {
    if (p.config().mode == Mode::off)
      return;
    if (const HistoryEntry* e = find_entry(p.history(set), tag))
      stats.history_hits.add(e->features);
  }

  // After PaperPolicy::choose, before commit
  void observe_decision(const PaperPolicy&, std::uint32_t set, const PaperDecision& d)
  {
    if (d.checked) { // pinned ways are sampled before this decision changes any run
      ++stats.decisions;
      stats.pinned_short += runs_.count_at_least(set, kPinnedShort);
      stats.pinned_long += runs_.count_at_least(set, kPinnedLong);
      if (d.second_chance)
        runs_.extend(set, d.first);
    }
    runs_.end(set, d.victim, RunEnd::evicted); // also without a check (a single eligible candidate)
  }

  void before_eviction(const PaperPolicy& p, std::uint32_t set, std::uint64_t tag)
  {
    if (p.config().mode == Mode::off)
      return;
    if (const HistoryEntry* aged = entry_to_age_out(p.history(set), tag))
      stats.aged_out.add(aged->features);
  }

  void before_fill(std::uint32_t set, std::uint32_t way, std::uint64_t tag, FillKind kind) { runs_.fill(set, way, tag, kind); }
  void before_hit(std::uint32_t set, std::uint32_t way) { runs_.end(set, way, RunEnd::hit); }
  void before_invalidate(std::uint32_t set, std::uint32_t way, std::uint64_t tag)
  {
    runs_.upgrade_invalidate(set, way, tag, false);
  }
};
} // namespace rlrepl

#endif
