// Shared building blocks: features and threshold, eviction history, LRU candidate order.

#include "rl_eviction_history.h"
#include "rl_events.h"
#include "rl_features.h"
#include "rl_lru_order.h"
#include "rl_test.h"

using namespace rlrepl;

TEST_CASE("threshold: above iff the sharer count is strictly greater than half the cores")
{
  // Ported from ChampSim 446/447: 8 of 16 is below, 9 of 16 above; 1 of 2 below, 2 of 2 above
  REQUIRE_FALSE(above_threshold(8, 16));
  REQUIRE(above_threshold(9, 16));
  REQUIRE_FALSE(above_threshold(1, 2));
  REQUIRE(above_threshold(2, 2));
  REQUIRE(above_threshold(1, 1));
  REQUIRE_FALSE(above_threshold(0, 1));

  // Gainestown, 4 cores: heavy iff 3 or 4 sharers (the boundary differs in [T21] Ch. 6; audit I-13)
  REQUIRE_FALSE(above_threshold(0, 4));
  REQUIRE_FALSE(above_threshold(1, 4));
  REQUIRE_FALSE(above_threshold(2, 4));
  REQUIRE(above_threshold(3, 4));
  REQUIRE(above_threshold(4, 4));

  // Odd core count: strictly more than 1.5 of 3
  REQUIRE_FALSE(above_threshold(1, 3));
  REQUIRE(above_threshold(2, 3));

  REQUIRE(count_sharers(0b1011) == 3);
  REQUIRE_FALSE(above_threshold(count_sharers(0xff), 16));
  REQUIRE(above_threshold(count_sharers(0x1ff), 16));
}

TEST_CASE("features: a missing delivered hint (0) classifies as below the threshold")
{
  REQUIRE(make_features(Mesi::M, 0, 4) == (Features{Mesi::M, false}));
  REQUIRE(make_features(Mesi::S, 3, 4) == (Features{Mesi::S, true}));
  REQUIRE(make_features(Mesi::E, 2, 4) == (Features{Mesi::E, false}));
}

TEST_CASE("features: modes and their parsing")
{
  REQUIRE(parse_mode("off") == Mode::off);
  REQUIRE(parse_mode("state") == Mode::state);
  REQUIRE(parse_mode("bitset") == Mode::bitset);
  REQUIRE(parse_mode("both") == Mode::both);
  REQUIRE_FALSE(parse_mode("LRU").has_value());
  REQUIRE_FALSE(parse_mode("").has_value());
  REQUIRE(name(Mode::bitset) == "bitset");
  REQUIRE(uses_state(Mode::both));
  REQUIRE(uses_sharing(Mode::both));
  REQUIRE(uses_state(Mode::state));
  REQUIRE_FALSE(uses_sharing(Mode::state));
  REQUIRE_FALSE(uses_state(Mode::bitset));
  REQUIRE_FALSE(uses_state(Mode::off));
  REQUIRE_FALSE(uses_sharing(Mode::off));
  REQUIRE(name(InvalidationReason::back_invalidation) == "back_invalidation");
}

TEST_CASE("history: holds its capacity and drops the least recently updated entry")
{
  // Ported from ChampSim 446 ("history holds 2x associativity and drops the least recently updated entry")
  constexpr std::size_t ways = 4;
  EvictionHistory h{2 * ways};
  REQUIRE(h.capacity() == 8);
  for (std::uint64_t tag = 0; tag < 2 * ways; ++tag)
    REQUIRE_FALSE(h.insert(tag, {Mesi::E, false}).has_value());
  REQUIRE(h.size() == 2 * ways);

  auto aged = h.insert(100, {Mesi::M, true});
  REQUIRE(aged.has_value());
  REQUIRE(aged->tag == 0);
  REQUIRE(aged->features == (Features{Mesi::E, false}));
  REQUIRE(h.size() == 2 * ways);
  REQUIRE_FALSE(h.contains(0));
  REQUIRE(h.contains(1));
  REQUIRE(h.contains(100));

  // Re-recording a tag makes it the most recently updated, with the new features, and drops nothing
  REQUIRE_FALSE(h.insert(1, {Mesi::S, false}).has_value());
  REQUIRE(h.entries().back().tag == 1);
  REQUIRE(h.entries().back().features == (Features{Mesi::S, false}));
  aged = h.insert(101, {Mesi::S, false});
  REQUIRE(aged->tag == 2);
  REQUIRE(h.contains(1));
  REQUIRE_FALSE(h.contains(2));
}

TEST_CASE("history: lookup and removal return the saved entry once")
{
  EvictionHistory h{4};
  h.insert(7, {Mesi::M, true});
  h.insert(8, {Mesi::E, false});
  REQUIRE(h.contains(7));
  auto e = h.remove(7);
  REQUIRE(e.has_value());
  REQUIRE(e->tag == 7);
  REQUIRE(e->features == (Features{Mesi::M, true}));
  REQUIRE_FALSE(h.contains(7));
  REQUIRE_FALSE(h.remove(7).has_value());
  REQUIRE(h.size() == 1);
  REQUIRE_FALSE(h.remove(12345).has_value());
}

TEST_CASE("history: tags stay unique and a zero-capacity history stores nothing")
{
  EvictionHistory h{3};
  h.insert(5, {Mesi::E, false});
  h.insert(5, {Mesi::M, false});
  h.insert(5, {Mesi::S, true});
  REQUIRE(h.size() == 1);
  REQUIRE(h.entries().front().features == (Features{Mesi::S, true}));

  EvictionHistory none{0};
  REQUIRE_FALSE(none.insert(1, {Mesi::E, false}).has_value());
  REQUIRE(none.empty());
}

TEST_CASE("lru order: mirrors Sniper CacheSetLRU ranks and moveToMRU")
{
  LruOrder lru{4};
  // CacheSetLRU initialises m_lru_bits[i] = i: way 3 is the least recently used
  for (std::uint32_t w = 0; w < 4; ++w)
    REQUIRE(lru.rank(w) == w);
  REQUIRE(lru.candidates(4) == (std::vector<std::uint32_t>{3, 2, 1, 0}));

  lru.touch(3); // ranks: way3 0, way0 1, way1 2, way2 3
  REQUIRE(lru.rank(3) == 0);
  REQUIRE(lru.rank(0) == 1);
  REQUIRE(lru.rank(2) == 3);
  REQUIRE(lru.candidates(4) == (std::vector<std::uint32_t>{2, 1, 0, 3}));

  // Touching the MRU way changes nothing; candidates(n) truncates to the n oldest
  lru.touch(3);
  REQUIRE(lru.candidates(2) == (std::vector<std::uint32_t>{2, 1}));
  REQUIRE(lru.candidates(1) == (std::vector<std::uint32_t>{2}));
}

TEST_CASE("lru order: ineligible ways (Sniper SHARED_UPGRADING) are skipped")
{
  LruOrder lru{4};
  auto c = lru.candidates(4, [](std::uint32_t w) { return w != 3; });
  REQUIRE(c == (std::vector<std::uint32_t>{2, 1, 0}));
  REQUIRE(lru.candidates(4, [](std::uint32_t) { return false; }).empty());
}
