#ifndef RL_FEATURES_H
#define RL_FEATURES_H

/*
 * Features shared by both coherence-aware replacement cores (rl_paper_policy.h and rl_reward_policy.h):
 * the block's MESI state and whether its sharer count is above the threshold.
 *
 * Simulator-independent and header-only. Sniper's CacheState is mapped to rlrepl::Mesi by the cache-set
 * adapter (Stage 2); nothing here includes Sniper headers. See docs/rl/POLICY_CORES.md.
 */

#include <bitset>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rlrepl
{
// Numeric values mirror ChampSim's champsim::coherence_state (and the sf::mesi / rlm::mesi enums).
// I means "no state information" (a non-coherent cache); a resident block in Sniper is always S, E or M.
enum class Mesi : std::uint8_t { I = 0, S, E, M };

// The two features of a block at the moment a decision is taken
struct Features {
  Mesi state = Mesi::I;
  bool above = false; // sharer count above the threshold ("AT"); false is "BT"

  friend bool operator==(const Features& a, const Features& b) { return a.state == b.state && a.above == b.above; }
};

// Fixed threshold ([P24] §II-A): above iff the sharer count is strictly greater than half the cores.
// For an integer count, count > num_cores / 2 (integer division) is the same test for odd and even core counts.
inline bool above_threshold(std::uint32_t sharer_count, std::uint32_t num_cores) { return sharer_count > num_cores / 2; }

inline std::uint32_t count_sharers(std::uint64_t sharer_mask) { return static_cast<std::uint32_t>(std::bitset<64>{sharer_mask}.count()); }

// Feature extraction from a state and a sharer count. In Sniper's private L2 the count is the delivered hint
// (CacheBlockInfo::getSharerHint(): requester included, 0 = no hint), which classifies as below the threshold,
// as ChampSim's delivered_sharers = 0 does.
inline Features make_features(Mesi state, std::uint32_t sharer_count, std::uint32_t num_cores)
{
  return {state, above_threshold(sharer_count, num_cores)};
}

// Which features a policy uses: RL-State, RL-Bitset, RL-Both; off = the base policy unchanged
enum class Mode { off, state, bitset, both };

inline bool uses_state(Mode m) { return m == Mode::state || m == Mode::both; }
inline bool uses_sharing(Mode m) { return m == Mode::bitset || m == Mode::both; }

inline std::optional<Mode> parse_mode(std::string_view s)
{
  if (s == "off")
    return Mode::off;
  if (s == "state")
    return Mode::state;
  if (s == "bitset")
    return Mode::bitset;
  if (s == "both")
    return Mode::both;
  return std::nullopt;
}

inline constexpr std::string_view name(Mode m)
{
  switch (m) {
  case Mode::off:
    return "off";
  case Mode::state:
    return "state";
  case Mode::bitset:
    return "bitset";
  case Mode::both:
  default:
    return "both";
  }
}
} // namespace rlrepl

#endif
