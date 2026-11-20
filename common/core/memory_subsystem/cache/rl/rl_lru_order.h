#ifndef RL_LRU_ORDER_H
#define RL_LRU_ORDER_H

/*
 * Candidate generation for the base policy LRU.
 *
 * Mirrors Sniper's CacheSetLRU recency bookkeeping exactly (common/core/memory_subsystem/cache/cache_set_lru.cc):
 * one rank per way, 0 = most recently used, associativity-1 = least recently used; the ranks are initialised to the
 * way index (so the highest way starts as LRU) and moveToMRU() ages every younger way by one. CacheSetLRU itself is
 * not modified or used; this is a copy of its ordering logic so the cores can be tested without Sniper.
 *
 * The cores do not depend on LRU: they consume a list of candidate ways in the base policy's preference order (best
 * victim first). Another base policy only has to produce such a list (see docs/rl/POLICY_CORES.md for the caveat on
 * SRRIP's next attempt).
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace rlrepl
{
class LruOrder
{
  std::vector<std::uint32_t> rank_; // per way: 0 = MRU, ways-1 = LRU

public:
  explicit LruOrder(std::uint32_t ways) : rank_(ways) { std::iota(rank_.begin(), rank_.end(), 0u); }

  [[nodiscard]] std::uint32_t ways() const { return static_cast<std::uint32_t>(rank_.size()); }
  [[nodiscard]] std::uint32_t rank(std::uint32_t way) const { return rank_.at(way); }

  // CacheSetLRU::moveToMRU
  void touch(std::uint32_t way)
  {
    const std::uint32_t r = rank_.at(way);
    for (auto& other : rank_)
      if (other < r)
        ++other;
    rank_.at(way) = 0;
  }

  /*
   * Up to n eligible ways, least recently used first. `eligible(way)` lets the adapter exclude ways that may not be
   * replaced (Sniper: CacheSet::isValidReplacement, i.e. not SHARED_UPGRADING). Ranks are a permutation, so there are
   * no ties.
   */
  template <typename Eligible>
  [[nodiscard]] std::vector<std::uint32_t> candidates(std::size_t n, Eligible&& eligible) const
  {
    std::vector<std::uint32_t> order;
    order.reserve(rank_.size());
    for (std::uint32_t way = 0; way < rank_.size(); ++way)
      if (eligible(way))
        order.push_back(way);
    std::sort(order.begin(), order.end(), [this](std::uint32_t a, std::uint32_t b) { return rank_[a] > rank_[b]; });
    if (order.size() > n)
      order.resize(n);
    return order;
  }

  [[nodiscard]] std::vector<std::uint32_t> candidates(std::size_t n) const
  {
    return candidates(n, [](std::uint32_t) { return true; });
  }
};
} // namespace rlrepl

#endif
