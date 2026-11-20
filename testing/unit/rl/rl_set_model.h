#ifndef RL_SET_MODEL_H
#define RL_SET_MODEL_H

/*
 * A tiny functional cache model used by the scenario tests. It drives the cores in the order the Stage 2 Sniper adapter
 * will, following CacheSet::insert / CacheSetLRU::getReplacementIndex:
 *
 *   fill(tag):  demand fill: policy.on_miss(set, tag) first, before anything else     (ChampSim: tag check)
 *               if an invalid way exists: take the lowest one, move it to MRU          (no decision, no eviction)
 *               else: candidates = LRU order of the valid ways, oldest first
 *                     d = policy.choose(...); policy.commit(...)
 *                     policy.on_eviction(set, victim tag, d.victim_features)
 *                     move the victim way to MRU                                       (CacheSetLRU does this too)
 *               install the block, then policy.on_fill(...)
 *   hit(way):   move to MRU, policy.on_hit                                              (only recency-updating hits)
 *   invalidate: the block becomes invalid, policy.on_invalidate(reason)
 *   upgrade:    invalidate(upgrade), then refill the same tag as M (lowest invalid way, moved to MRU) with
 *               FillKind::upgrade_refill, as Sniper's L2 does for a write to an S/E line
 *
 * It holds no coherence protocol: tests set states and sharer hints directly.
 */

#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "rl_events.h"
#include "rl_features.h"
#include "rl_lru_order.h"
#include "rl_paper_policy.h"
#include "rl_reward_policy.h"

namespace rltest
{
struct Block {
  bool valid = false;
  std::uint64_t tag = 0;
  rlrepl::Mesi state = rlrepl::Mesi::I;
  std::uint32_t hint = 0; // delivered sharer count
};

struct FillResult {
  std::uint32_t way = 0;
  bool evicted = false;
  std::uint64_t evicted_tag = 0;
};

template <typename Policy>
class SetModel
{
public:
  std::vector<std::vector<Block>> blocks;
  std::vector<rlrepl::LruOrder> lru;
  std::uint32_t num_cores;
  Policy& policy;

  SetModel(Policy& p, std::uint32_t sets, std::uint32_t ways, std::uint32_t cores)
      : blocks(sets, std::vector<Block>(ways)), lru(sets, rlrepl::LruOrder{ways}), num_cores(cores), policy(p)
  {
  }

  rlrepl::Features features(std::uint32_t set, std::uint32_t way) const
  {
    const auto& b = blocks.at(set).at(way);
    return rlrepl::make_features(b.state, b.hint, num_cores);
  }

  std::optional<std::uint32_t> find(std::uint32_t set, std::uint64_t tag) const
  {
    for (std::uint32_t w = 0; w < blocks.at(set).size(); ++w)
      if (blocks[set][w].valid && blocks[set][w].tag == tag)
        return w;
    return std::nullopt;
  }

  // Ways oldest first (all valid ways)
  std::vector<std::uint32_t> lru_order(std::uint32_t set) const
  {
    return lru.at(set).candidates(blocks.at(set).size(), [&](std::uint32_t w) { return blocks[set][w].valid; });
  }

  FillResult fill(std::uint32_t set, std::uint64_t tag, rlrepl::Mesi state, std::uint32_t hint,
                  rlrepl::FillKind kind = rlrepl::FillKind::demand)
  {
    FillResult r;
    if (kind == rlrepl::FillKind::demand)
      policy.on_miss(set, tag);
    auto& ways = blocks.at(set);
    std::optional<std::uint32_t> invalid;
    for (std::uint32_t w = 0; w < ways.size() && !invalid; ++w)
      if (!ways[w].valid)
        invalid = w;

    if (invalid) {
      r.way = *invalid;
    } else {
      auto info = [&](std::uint32_t w) { return features(set, w); };
      const auto candidates = lru_order(set);
      if constexpr (std::is_same_v<Policy, rlrepl::RewardPolicy>) {
        auto d = policy.choose(set, candidates, info);
        policy.commit(set, d);
        policy.on_eviction(set, ways[d.victim].tag, d.victim_features);
        r.way = d.victim;
      } else {
        auto d = policy.choose(set, candidates, info);
        policy.commit(d);
        policy.on_eviction(set, ways[d.victim].tag, d.victim_features);
        r.way = d.victim;
      }
      r.evicted = true;
      r.evicted_tag = ways[r.way].tag;
    }
    lru.at(set).touch(r.way);
    ways[r.way] = Block{true, tag, state, hint};
    if constexpr (std::is_same_v<Policy, rlrepl::RewardPolicy>)
      policy.on_fill(set, r.way, tag, kind);
    else
      policy.on_fill(set, tag);
    return r;
  }

  void hit(std::uint32_t set, std::uint32_t way)
  {
    lru.at(set).touch(way);
    if constexpr (std::is_same_v<Policy, rlrepl::RewardPolicy>)
      policy.on_hit(set, way);
  }

  void hit_tag(std::uint32_t set, std::uint64_t tag) { hit(set, find(set, tag).value()); }

  void invalidate(std::uint32_t set, std::uint32_t way, rlrepl::InvalidationReason reason)
  {
    auto& b = blocks.at(set).at(way);
    if constexpr (std::is_same_v<Policy, rlrepl::RewardPolicy>)
      policy.on_invalidate(set, way, b.tag, reason);
    b.valid = false;
  }

  // Sniper upgrade: drop the own copy, then re-insert it as M; a write fill delivers a sharer hint of 1
  FillResult upgrade(std::uint32_t set, std::uint64_t tag)
  {
    invalidate(set, find(set, tag).value(), rlrepl::InvalidationReason::upgrade);
    return fill(set, tag, rlrepl::Mesi::M, 1, rlrepl::FillKind::upgrade_refill);
  }
};
} // namespace rltest

#endif
