// Souza-Freitas reproduction core (rl_paper_policy.h). Ports ChampSim test/cpp/src/446-souza-freitas.cc where the
// logic is shared, plus tests for the Sniper-port interface (candidate list, AND/OR, rewards at misses).

#include <initializer_list>

#include "rl_paper_policy.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
EvictionHistory history_of(std::initializer_list<HistoryEntry> entries, std::size_t capacity = 16)
{
  EvictionHistory h{capacity};
  for (const auto& e : entries)
    h.insert(e.tag, e.features);
  return h;
}

constexpr Features M_bt{Mesi::M, false}, M_at{Mesi::M, true};
constexpr Features E_bt{Mesi::E, false}, E_at{Mesi::E, true};
constexpr Features S_bt{Mesi::S, false}, S_at{Mesi::S, true};
constexpr Features I_bt{Mesi::I, false};

PaperConfig cfg(Mode m, Rule r = Rule::text, Combine c = Combine::And) { return {m, r, c}; }

// A one-set policy whose history is prefilled through on_eviction (as a real cache would build it)
PaperPolicy policy_with(PaperConfig c, std::initializer_list<HistoryEntry> entries, std::uint32_t ways = 8)
{
  PaperPolicy p{c, 1, ways};
  for (const auto& e : entries)
    p.on_eviction(0, e.tag, e.features);
  return p;
}

// Candidate table: way i has features ways[i]; counts how often a candidate's features are read
struct Table {
  std::vector<Features> ways;
  mutable std::vector<int> reads = std::vector<int>(8, 0);
  Features operator()(std::uint32_t w) const
  {
    ++reads.at(w);
    return ways.at(w);
  }
};
} // namespace

TEST_CASE("paper: weights are ratios of history entries and each group sums to 1")
{
  auto h = history_of({{1, M_at}, {2, M_bt}, {3, E_bt}, {4, S_bt}});
  auto w = compute_ratio_weights(h);
  REQUIRE_APPROX(w.m, 0.5);
  REQUIRE_APPROX(w.e, 0.25);
  REQUIRE_APPROX(w.s, 0.25);
  REQUIRE_APPROX(w.m + w.e + w.s, 1.0);
  REQUIRE_APPROX(w.at, 0.25);
  REQUIRE_APPROX(w.bt, 0.75);
  REQUIRE_APPROX(w.at + w.bt, 1.0);
  REQUIRE_FALSE(w.state_empty);
  REQUIRE_FALSE(w.bitset_empty);
}

TEST_CASE("paper: state weights ignore entries without state information")
{
  auto h = history_of({{1, I_bt}, {2, {Mesi::I, true}}, {3, E_bt}});
  auto w = compute_ratio_weights(h);
  REQUIRE_APPROX(w.e, 1.0);
  REQUIRE_APPROX(w.m, 0.0);
  REQUIRE_APPROX(w.at, 1.0 / 3.0); // AT/BT over all three entries

  auto only_i = compute_ratio_weights(history_of({{1, I_bt}}));
  REQUIRE(only_i.state_empty);      // no state distribution: initial bias for the state check
  REQUIRE_FALSE(only_i.bitset_empty);
}

TEST_CASE("paper: empty history evicts Exclusive / below-threshold blocks under both rules")
{
  // Ported from ChampSim 446
  auto w = compute_ratio_weights(EvictionHistory{16});
  REQUIRE(w.state_empty);
  REQUIRE(w.bitset_empty);
  for (auto r : {Rule::text, Rule::example}) {
    REQUIRE(should_evict(cfg(Mode::state, r), w, E_bt));
    REQUIRE(should_evict(cfg(Mode::state, r), w, E_at));
    REQUIRE_FALSE(should_evict(cfg(Mode::state, r), w, M_bt));
    REQUIRE_FALSE(should_evict(cfg(Mode::state, r), w, S_bt));
    REQUIRE(should_evict(cfg(Mode::bitset, r), w, M_bt));
    REQUIRE_FALSE(should_evict(cfg(Mode::bitset, r), w, E_at));
    // Both: AND needs E and below; OR needs E or below
    REQUIRE(should_evict(cfg(Mode::both, r, Combine::And), w, E_bt));
    REQUIRE_FALSE(should_evict(cfg(Mode::both, r, Combine::And), w, E_at));
    REQUIRE_FALSE(should_evict(cfg(Mode::both, r, Combine::And), w, M_bt));
    REQUIRE(should_evict(cfg(Mode::both, r, Combine::Or), w, E_at));
    REQUIRE(should_evict(cfg(Mode::both, r, Combine::Or), w, M_bt));
    REQUIRE_FALSE(should_evict(cfg(Mode::both, r, Combine::Or), w, M_at));
  }

  // Through the policy: an M first candidate gets its second chance, the decision is counted as empty-history
  PaperPolicy p{cfg(Mode::state), 1, 8};
  auto d = p.choose(0, {0, 1}, Table{{M_bt, M_bt}});
  REQUIRE(d.empty_history);
  REQUIRE(d.second_chance);
  REQUIRE(d.victim == 1);
  p.commit(d);
  REQUIRE(p.stats.empty_history_decisions == 1);
  REQUIRE(p.stats.second_chances == 1);
}

TEST_CASE("paper: rule text evicts the highest-weight state, rule example keeps it")
{
  // wM is the highest, as in the paper's §II-C example. Ported from ChampSim 446.
  auto h = history_of({{1, M_bt}, {2, M_bt}, {3, E_bt}});
  auto w = compute_ratio_weights(h);
  REQUIRE(should_evict(cfg(Mode::state, Rule::text), w, M_bt));
  REQUIRE_FALSE(should_evict(cfg(Mode::state, Rule::text), w, E_bt));
  REQUIRE_FALSE(should_evict(cfg(Mode::state, Rule::example), w, M_bt));
  REQUIRE(should_evict(cfg(Mode::state, Rule::example), w, E_bt));

  // The worked example: LRU picks an M block, wM is highest, so the second LRU block is chosen (example rule)
  auto ex = policy_with(cfg(Mode::state, Rule::example), {{1, M_bt}, {2, M_bt}, {3, E_bt}});
  REQUIRE(ex.choose(0, {0, 1}, Table{{M_bt, E_bt}}).victim == 1);
  auto tx = policy_with(cfg(Mode::state, Rule::text), {{1, M_bt}, {2, M_bt}, {3, E_bt}});
  REQUIRE(tx.choose(0, {0, 1}, Table{{M_bt, E_bt}}).victim == 0);
}

TEST_CASE("paper: sharing check under both rules")
{
  // AT is the majority class
  auto w = compute_ratio_weights(history_of({{1, E_at}, {2, M_at}, {3, S_bt}}));
  REQUIRE(should_evict(cfg(Mode::bitset, Rule::text), w, S_at));
  REQUIRE_FALSE(should_evict(cfg(Mode::bitset, Rule::text), w, S_bt));
  REQUIRE_FALSE(should_evict(cfg(Mode::bitset, Rule::example), w, S_at));
  REQUIRE(should_evict(cfg(Mode::bitset, Rule::example), w, S_bt));
}

TEST_CASE("paper: ties count as highest")
{
  // Ported from ChampSim 446: wM = wE, wAT = wBT
  auto w = compute_ratio_weights(history_of({{1, M_at}, {2, E_bt}}));
  REQUIRE(state_says_evict(w, Rule::text, Mesi::M));
  REQUIRE(state_says_evict(w, Rule::text, Mesi::E));
  REQUIRE_FALSE(state_says_evict(w, Rule::text, Mesi::S));
  REQUIRE(bitset_says_evict(w, Rule::text, true));
  REQUIRE(bitset_says_evict(w, Rule::text, false));
  REQUIRE_FALSE(bitset_says_evict(w, Rule::example, true));

  // A three-way state tie: text evicts every candidate #1, example keeps every candidate #1 (audit I-9)
  auto three = compute_ratio_weights(history_of({{1, M_bt}, {2, E_bt}, {3, S_bt}}));
  for (auto s : {Mesi::M, Mesi::E, Mesi::S}) {
    REQUIRE(state_says_evict(three, Rule::text, s));
    REQUIRE_FALSE(state_says_evict(three, Rule::example, s));
  }
}

TEST_CASE("paper: RL-Both with AND evicts only when both checks say evict")
{
  // Ported from ChampSim 446. State: E highest. Sharing: AT highest.
  auto w = compute_ratio_weights(history_of({{1, E_at}, {2, E_at}, {3, M_bt}}));
  const auto both = cfg(Mode::both, Rule::text, Combine::And);
  REQUIRE(should_evict(both, w, E_at));
  REQUIRE_FALSE(should_evict(both, w, E_bt)); // sharing says keep
  REQUIRE_FALSE(should_evict(both, w, M_at)); // state says keep
  REQUIRE_FALSE(should_evict(both, w, M_bt));
  REQUIRE(should_evict(cfg(Mode::state), w, E_bt));
  REQUIRE(should_evict(cfg(Mode::bitset), w, M_at));
}

TEST_CASE("paper: RL-Both with OR evicts when either check says evict")
{
  auto w = compute_ratio_weights(history_of({{1, E_at}, {2, E_at}, {3, M_bt}}));
  const auto text_or = cfg(Mode::both, Rule::text, Combine::Or);
  REQUIRE(should_evict(text_or, w, E_at));
  REQUIRE(should_evict(text_or, w, E_bt));
  REQUIRE(should_evict(text_or, w, M_at));
  REQUIRE_FALSE(should_evict(text_or, w, M_bt));

  // Example + OR = Figure 2 read literally: keep only if the state AND the sharing class both match the highest
  const auto ex_or = cfg(Mode::both, Rule::example, Combine::Or);
  const auto ex_and = cfg(Mode::both, Rule::example, Combine::And);
  REQUIRE_FALSE(should_evict(ex_or, w, E_at)); // both match: keep
  REQUIRE(should_evict(ex_or, w, E_bt));       // class differs: evict
  REQUIRE(should_evict(ex_or, w, M_at));       // state differs: evict
  // ChampSim's example + AND keeps a block if either matches
  REQUIRE_FALSE(should_evict(ex_and, w, E_bt));
  REQUIRE_FALSE(should_evict(ex_and, w, M_at));
  REQUIRE(should_evict(ex_and, w, M_bt));
}

TEST_CASE("paper: INVALID state means no information and accepts candidate #1")
{
  // Ported from ChampSim 446
  auto p = policy_with(cfg(Mode::state), {{1, M_bt}});
  auto d = p.choose(0, {0, 1}, Table{{I_bt, E_bt}});
  REQUIRE(d.victim == 0);
  p.commit(d);
  REQUIRE(p.stats.first_accepted == 1);
}

TEST_CASE("paper: exactly one second chance, and the second candidate is not checked")
{
  // E is highest (text: evict only E). Candidate #1 (M) is rejected; candidate #2 (S) would be rejected too, and
  // candidate #3 (E) would be accepted, but the second choice is evicted unconditionally.
  auto p = policy_with(cfg(Mode::state, Rule::text), {{1, E_bt}, {2, E_bt}, {3, M_bt}});
  Table t{{M_bt, S_bt, E_bt}};
  auto d = p.choose(0, {0, 1, 2}, t);
  REQUIRE(d.checked);
  REQUIRE(d.second_chance);
  REQUIRE(d.first == 0);
  REQUIRE(d.victim == 1);
  REQUIRE(d.victim_features == S_bt);
  REQUIRE(t.reads[2] == 0); // the third candidate is never looked at
  p.commit(d);
  REQUIRE(p.stats.decisions == 1);
  REQUIRE(p.stats.second_chances == 1);
  REQUIRE(p.stats.first_accepted == 0);
}

TEST_CASE("paper: a rejected candidate keeps its LRU position and is checked again at the next miss")
{
  // Text rule, wE highest: an M block at the LRU position is rejected at every miss while the weights do not change
  // (ChampSim behavior; no promotion. Audit I-1 / I-11.)
  PaperPolicy p{cfg(Mode::state, Rule::text), 1, 4};
  rltest::SetModel<PaperPolicy> m{p, 1, 4, 4};
  m.fill(0, 100, Mesi::M, 1); // way 0, oldest
  m.fill(0, 101, Mesi::E, 1);
  m.fill(0, 102, Mesi::E, 1);
  m.fill(0, 103, Mesi::E, 1);
  // Empty history: the M block is rejected (initial bias), 101 (E) evicted
  auto r = m.fill(0, 104, Mesi::E, 1);
  REQUIRE(r.evicted_tag == 101);
  REQUIRE(m.lru_order(0).front() == 0); // block 100 still least recently used
  REQUIRE(m.lru.at(0).rank(0) == 3);

  // History now {101: E}: wE = 1, M still not highest
  r = m.fill(0, 105, Mesi::E, 1);
  REQUIRE(r.evicted_tag == 102);
  r = m.fill(0, 106, Mesi::E, 1);
  REQUIRE(r.evicted_tag == 103);
  REQUIRE(m.find(0, 100).has_value());
  REQUIRE(m.lru_order(0).front() == 0);
  REQUIRE(p.stats.second_chances == 3);
  REQUIRE(p.stats.first_accepted == 0);
}

TEST_CASE("paper: mode off returns the base policy's choice and records nothing")
{
  // Ported from ChampSim 446
  auto p = policy_with(cfg(Mode::off), {{1, E_bt}});
  REQUIRE(p.history(0).empty()); // off: on_eviction records nothing
  auto d = p.choose(0, {0, 1}, Table{{M_at, E_bt}});
  REQUIRE(d.victim == 0);
  REQUIRE_FALSE(d.checked);
  p.commit(d);
  p.on_miss(0, 1);
  p.on_fill(0, 1);
  REQUIRE(p.stats.decisions == 0);
  REQUIRE(p.stats.positive_rewards == 0);
}

TEST_CASE("paper: a single candidate is accepted without a check")
{
  auto p = policy_with(cfg(Mode::state), {{1, E_bt}});
  auto d = p.choose(0, {5}, Table{{M_bt, M_bt, M_bt, M_bt, M_bt, M_bt}});
  REQUIRE(d.victim == 5);
  REQUIRE_FALSE(d.checked);
}

TEST_CASE("paper: history per set, capacity 2 x associativity, isolated between sets")
{
  PaperPolicy p{cfg(Mode::both), 2, 4};
  REQUIRE(p.history(0).capacity() == 8);
  REQUIRE(p.history(1).capacity() == 8);
  for (std::uint64_t t = 0; t < 9; ++t)
    p.on_eviction(0, t, E_bt);
  REQUIRE(p.history(0).size() == 8);
  REQUIRE(p.history(1).empty());
  REQUIRE(p.stats.history_aged_out == 1);
  REQUIRE_FALSE(p.history(0).contains(0));

  // Set 1's weights are unaffected by set 0's history: still the initial bias
  auto d = p.choose(1, {0, 1}, Table{{M_bt, E_bt}});
  REQUIRE(d.empty_history);
}

TEST_CASE("paper: rewards are counted at misses; only the removal affects the weights")
{
  auto p = policy_with(cfg(Mode::state), {{7, M_bt}, {8, E_bt}});
  REQUIRE_APPROX(compute_ratio_weights(p.history(0)).m, 0.5);

  p.on_miss(0, 7); // returning evicted block: -1, entry removed
  REQUIRE(p.stats.negative_rewards == 1);
  REQUIRE_FALSE(p.history(0).contains(7));
  REQUIRE_APPROX(compute_ratio_weights(p.history(0)).e, 1.0);

  p.on_fill(0, 7);
  p.on_miss(0, 7); // not in the history any more: +1, nothing changes
  REQUIRE(p.stats.positive_rewards == 1);
  REQUIRE(p.history(0).size() == 1);

  p.on_fill(0, 8); // a fill without on_miss (not a miss): no reward; an entry found here is removed and counted
  REQUIRE(p.stats.negative_rewards == 1);
  REQUIRE(p.stats.positive_rewards == 1);
  REQUIRE(p.stats.refill_removals == 1);
  REQUIRE(p.history(0).empty());
}

TEST_CASE("paper: weight averages are accumulated only for decisions with a history")
{
  auto p = policy_with(cfg(Mode::both), {{1, M_bt}, {2, E_at}});
  auto d = p.choose(0, {0, 1}, Table{{M_bt, E_bt}});
  p.commit(d);
  REQUIRE(p.stats.weighted_decisions == 1);
  REQUIRE_APPROX(p.stats.sum_m, 0.5);
  REQUIRE_APPROX(p.stats.sum_at, 0.5);
}

TEST_CASE("paper: parses its configuration")
{
  REQUIRE(parse_rule("text") == Rule::text);
  REQUIRE(parse_rule("example") == Rule::example);
  REQUIRE_FALSE(parse_rule("").has_value());
  REQUIRE(parse_combine("and") == Combine::And);
  REQUIRE(parse_combine("or") == Combine::Or);
  REQUIRE_FALSE(parse_combine("AND").has_value());
  REQUIRE(name(Rule::example) == "example");
  REQUIRE(name(Combine::Or) == "or");
  PaperConfig defaults;
  REQUIRE(defaults.mode == Mode::both);
  REQUIRE(defaults.rule == Rule::text);
  REQUIRE(defaults.combine == Combine::And);
}
