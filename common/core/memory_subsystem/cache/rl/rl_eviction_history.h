#ifndef RL_EVICTION_HISTORY_H
#define RL_EVICTION_HISTORY_H

/*
 * Per-set Eviction History Table ([P24] §II-B), used by both cores.
 *
 * Ported from ChampSim (sf::eviction_history in replacement/souza_freitas/sf_core.h and rlm::eviction_history in
 * replacement/rl_mesi/rl_core.h, which have identical semantics):
 *   - entries are kept in update order: front() is the least recently updated;
 *   - inserting a tag that is already present first removes it, so it becomes the most recently updated;
 *   - when full, the least recently updated entry is dropped and returned ("aged out");
 *   - tags are unique.
 * The sf version returned only whether an entry was dropped; here the dropped entry is returned (the reward core
 * needs its saved features, the paper core only counts it).
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "rl_features.h"

namespace rlrepl
{
struct HistoryEntry {
  std::uint64_t tag;
  Features features; // the evicted block's features at decision time
};

class EvictionHistory
{
  std::size_t capacity_;
  std::vector<HistoryEntry> entries_;

  auto find_(std::uint64_t tag) const
  {
    return std::find_if(entries_.begin(), entries_.end(), [tag](const HistoryEntry& e) { return e.tag == tag; });
  }

public:
  explicit EvictionHistory(std::size_t capacity) : capacity_(capacity) { entries_.reserve(capacity); }

  [[nodiscard]] std::size_t capacity() const { return capacity_; }
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  [[nodiscard]] bool contains(std::uint64_t tag) const { return find_(tag) != entries_.end(); }
  [[nodiscard]] const std::vector<HistoryEntry>& entries() const { return entries_; }

  // Remove the entry for this tag; returns it if it was present
  std::optional<HistoryEntry> remove(std::uint64_t tag)
  {
    auto it = find_(tag);
    if (it == entries_.end())
      return std::nullopt;
    HistoryEntry found = *it;
    entries_.erase(it);
    return found;
  }

  // Record an evicted block; returns the least recently updated entry if it had to be dropped to make room
  std::optional<HistoryEntry> insert(std::uint64_t tag, Features f)
  {
    remove(tag);
    if (capacity_ == 0)
      return std::nullopt;
    std::optional<HistoryEntry> aged_out;
    if (entries_.size() >= capacity_) {
      aged_out = entries_.front();
      entries_.erase(entries_.begin());
    }
    entries_.push_back({tag, f});
    return aged_out;
  }
};
} // namespace rlrepl

#endif
