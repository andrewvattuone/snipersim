// Experimental controls for the learning question (rl_reward_policy.h: Selection, learning, initial weights,
// random_draw). They must choose only among the same candidates as the algorithm, learn nothing when told not to,
// break ties toward the older candidate, draw reproducibly, and leave the default algorithm unchanged.

#include <map>
#include <set>

#include "rl_diagnostics.h"
#include "rl_lru_order.h"
#include "rl_reward_policy.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
constexpr Features S_bt{Mesi::S, false}, S_at{Mesi::S, true};
constexpr Features E_bt{Mesi::E, false}, E_at{Mesi::E, true};
constexpr Features M_bt{Mesi::M, false}, M_at{Mesi::M, true};
constexpr Features I_bt{Mesi::I, false};

RewardConfig control(Selection s, std::uint32_t window = 4, bool learning = false, double rate = 0.0, std::uint64_t seed = 0)
{
  RewardConfig c;
  c.window = window;
  c.alpha = 0.5;
  c.selection = s;
  c.learning = learning;
  c.random_rate = rate;
  c.seed = seed;
  return c;
}

auto table(std::vector<Features> per_way)
{
  return [per_way](std::uint32_t way) { return per_way.at(way); };
}

std::vector<std::uint32_t> oldest_first(std::uint32_t n)
{
  std::vector<std::uint32_t> c(n);
  for (std::uint32_t i = 0; i < n; ++i)
    c[i] = i;
  return c;
}

WeightTable make_weights(double s, double e, double m, double bt, double at)
{
  WeightTable w;
  w.set_state_weight(Mesi::S, s);
  w.set_state_weight(Mesi::E, e);
  w.set_state_weight(Mesi::M, m);
  w.set_share_weight(false, bt);
  w.set_share_weight(true, at);
  return w;
}

bool same_weights(const WeightTable& a, const WeightTable& b)
{
  for (auto s : {Mesi::S, Mesi::E, Mesi::M})
    if (a.state_weight(s) != b.state_weight(s)) // exact: frozen means bit-identical
      return false;
  return a.share_weight(false) == b.share_weight(false) && a.share_weight(true) == b.share_weight(true);
}

// The LU pilot's frozen vector (docs/rl/CAPBENCH.md)
const WeightTable kFrozen = make_weights(-0.09, -0.38, -0.76, -0.64, 0.0);

// Drive one set through a fixed sequence of misses, hits and evictions and return the victims (way per decision)
std::vector<std::uint32_t> victims_of(RewardPolicy& p, int decisions)
{
  rltest::SetModel<RewardPolicy> m{p, 1, 8, 4};
  std::vector<std::uint32_t> v;
  std::uint64_t x = 7;
  std::uint64_t tag = 0;
  while (static_cast<int>(v.size()) < decisions) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto state = static_cast<Mesi>(1 + (x >> 20) % 3);
    const auto hint = static_cast<std::uint32_t>((x >> 10) % 5);
    if ((x >> 50) % 4 == 0)
      if (auto way = m.find(0, tag > 3 ? tag - 3 : 0)) {
        m.hit(0, *way);
        continue;
      }
    if (auto r = m.fill(0, tag++, state, hint); r.evicted)
      v.push_back(r.way);
  }
  return v;
}
} // namespace

TEST_CASE("controls: defaults are the algorithm (learned selection, learning on, no random departures)")
{
  RewardConfig d;
  REQUIRE(d.selection == Selection::learned);
  REQUIRE(d.learning);
  REQUIRE(d.random_rate == 0.0);
  REQUIRE(d.seed == 0);

  for (auto s : {Selection::learned, Selection::priority, Selection::random})
    REQUIRE(parse_selection(name(s)) == s);
  REQUIRE_FALSE(parse_selection("fixed").has_value());
  REQUIRE_FALSE(parse_selection("").has_value());
}

TEST_CASE("controls: the learned selection's score is the weight score, so default decisions are unchanged")
{
  RewardPolicy p{RewardConfig{}, 1, 8, kFrozen};
  for (auto f : {S_bt, S_at, E_bt, E_at, M_bt, M_at, I_bt})
    REQUIRE(p.selection_score(0, f) == kFrozen.score(Mode::both, f));

  // Identical victim sequences for a policy built with the controls' fields at their defaults and one without them
  RewardConfig explicit_defaults;
  explicit_defaults.selection = Selection::learned;
  explicit_defaults.learning = true;
  RewardPolicy a{RewardConfig{}, 1, 8}, b{explicit_defaults, 1, 8};
  REQUIRE(victims_of(a, 3000) == victims_of(b, 3000));
  REQUIRE(a.stats.chose_other > 0); // the weights did learn and depart from LRU
}

TEST_CASE("controls: initial weights are used from the first decision; with learning on they then move")
{
  // S_at scores -0.09 + 0, the best of these four; E_bt -1.02; M_bt -1.40; M_at -0.76
  RewardPolicy p{control(Selection::learned, 4, true), 1, 8, kFrozen};
  const auto d = p.choose(0, oldest_first(4), table({M_bt, E_bt, M_at, S_at}));
  REQUIRE(d.victim == 3);
  REQUIRE(d.passed_over.size() == 3);

  p.commit(0, d);
  p.on_eviction(0, 100, d.victim_features);
  p.on_miss(0, 100); // the victim returns while in the history: -1 to S and AT
  REQUIRE(p.weights().state_weight(Mesi::S) < -0.09);
  REQUIRE(p.weights().share_weight(true) < 0.0);
}

TEST_CASE("controls: learning off keeps the weights bit-identical while every reward is still counted")
{
  RewardPolicy p{control(Selection::learned, 4, false), 1, 8, kFrozen};

  // Ways 0-2 are passed over (protected) when way 3 is evicted
  auto d1 = p.choose(0, oldest_first(4), table({M_bt, E_bt, M_bt, S_at}));
  REQUIRE(d1.victim == 3);
  p.commit(0, d1);
  p.on_eviction(0, 10, d1.victim_features);
  // protect-good: protected way 1 is hit
  p.on_hit(0, 1);
  // evict-bad: tag 10 returns while in the history
  p.on_miss(0, 10);
  auto d2 = p.choose(0, oldest_first(4), table({M_bt, M_bt, M_bt, M_bt}));
  REQUIRE(d2.victim == 0); // all tied: the oldest
  p.commit(0, d2);         // protect-wasted: protected way 0 is evicted without a hit
  // evict-good: fill the 16-entry history until an entry ages out
  for (std::uint64_t t = 20; t < 40; ++t)
    p.on_eviction(0, t, M_bt);

  REQUIRE(p.stats.protect_wasted == 1);
  REQUIRE(p.stats.protect_good == 1);
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE(p.stats.evict_good > 0);
  REQUIRE(same_weights(p.weights(), kFrozen));
}

TEST_CASE("controls: frozen weights choose exactly as the learned selection does with the same table")
{
  RewardPolicy frozen{control(Selection::learned, 4, false), 1, 8, kFrozen};
  RewardPolicy learned{control(Selection::learned, 4, true), 1, 8, kFrozen};
  const std::vector<std::vector<Features>> windows{{M_bt, E_bt, M_at, S_at}, {S_bt, M_at, E_bt, M_bt}, {M_bt, M_bt, E_bt, E_bt},
                                                   {E_at, S_bt, S_at, M_bt}, {M_at, M_at, M_at, M_at}};
  for (const auto& w : windows)
    REQUIRE(frozen.choose(0, oldest_first(4), table(w)).victim == learned.choose(0, oldest_first(4), table(w)).victim);
  // Ties go to the older candidate: E_bt at positions 2 and 3
  REQUIRE(frozen.choose(0, oldest_first(4), table({M_bt, M_bt, E_bt, E_bt})).victim == 2);
}

TEST_CASE("controls: fixed priority ranks S > E > M, then above > below the threshold, per the mode")
{
  REQUIRE(priority_rank(Mode::both, S_at) == 5);
  REQUIRE(priority_rank(Mode::both, S_bt) == 4);
  REQUIRE(priority_rank(Mode::both, E_at) == 3);
  REQUIRE(priority_rank(Mode::both, E_bt) == 2);
  REQUIRE(priority_rank(Mode::both, M_at) == 1);
  REQUIRE(priority_rank(Mode::both, M_bt) == 0);
  REQUIRE(priority_rank(Mode::both, I_bt) == 2); // no state information: between S and M
  REQUIRE(priority_rank(Mode::state, S_at) == priority_rank(Mode::state, S_bt));
  REQUIRE(priority_rank(Mode::bitset, M_at) > priority_rank(Mode::bitset, S_bt));
  REQUIRE(priority_rank(Mode::bitset, S_bt) == priority_rank(Mode::bitset, M_bt));
}

TEST_CASE("controls: fixed priority evicts the highest rank among the N oldest, ties to the older, ignoring weights")
{
  // Weights that would prefer M: the priority selection must not look at them
  RewardPolicy p{control(Selection::priority), 1, 8, make_weights(-1, -1, +1, -1, -1)};
  REQUIRE(p.choose(0, oldest_first(4), table({M_bt, E_bt, S_bt, M_at})).victim == 2);
  REQUIRE(p.choose(0, oldest_first(4), table({M_bt, S_bt, E_at, S_bt})).victim == 1); // tie S_bt: the older
  REQUIRE(p.choose(0, oldest_first(4), table({M_bt, M_bt, M_bt, M_bt})).victim == 0); // all tied: the oldest (LRU)
  REQUIRE(p.choose(0, oldest_first(4), table({M_at, M_bt, M_bt, M_bt})).victim == 0);
  REQUIRE(p.choose(0, oldest_first(4), table({M_bt, M_at, M_bt, M_bt})).victim == 1);
  // Only the first N count: an S block at position 4 (the fifth oldest) is out of the window
  REQUIRE(p.choose(0, oldest_first(8), table({M_bt, M_bt, M_bt, M_at, S_at, S_at, S_at, S_at})).victim == 3);
  // N = 2
  RewardPolicy p2{control(Selection::priority, 2), 1, 8};
  REQUIRE(p2.choose(0, oldest_first(4), table({M_bt, E_bt, S_at, S_at})).victim == 1);
}

TEST_CASE("controls: candidate eligibility comes from the base policy's list, unchanged for every selection")
{
  // 8 ways, way 7 least recent, way 6 next; way 7 is not eligible (Sniper: SHARED_UPGRADING)
  LruOrder lru{8};
  const auto eligible = [](std::uint32_t way) { return way != 7; };
  const auto candidates = lru.candidates(8, eligible);
  REQUIRE(candidates.front() == 6);
  REQUIRE(std::find(candidates.begin(), candidates.end(), 7u) == candidates.end());

  // Every way is Shared except the ineligible one... the window is the 4 oldest ELIGIBLE ways 6, 5, 4, 3
  std::vector<Features> f(8, M_bt);
  f[7] = S_at;
  f[3] = S_bt;
  for (auto s : {Selection::learned, Selection::priority, Selection::random}) {
    RewardPolicy p{control(s, 4, false, s == Selection::random ? 1.0 : 0.0), 1, 8, make_weights(+1, 0, -1, 0, +1)};
    for (int i = 0; i < 50; ++i) {
      const auto d = p.choose(0, candidates, table(f));
      REQUIRE(d.victim != 7);
      REQUIRE(std::find(candidates.begin(), candidates.begin() + 4, d.victim) != candidates.begin() + 4);
      if (s != Selection::random)
        REQUIRE(d.victim == 3);
      p.commit(0, d);
    }
  }
}

TEST_CASE("controls: the random draw is a function of seed and decision number, and departs at the given rate")
{
  // Reproducible
  for (std::uint64_t k = 0; k < 1000; ++k)
    REQUIRE(random_draw(42, k, 0.3, 4) == random_draw(42, k, 0.3, 4));
  // Rate 0: always the oldest (LRU); rate 1: never; window 1: nothing to draw
  for (std::uint64_t k = 0; k < 10000; ++k) {
    REQUIRE(random_draw(1, k, 0.0, 4) == 0);
    REQUIRE(random_draw(1, k, 1.0, 4) != 0);
    REQUIRE(random_draw(1, k, 1.0, 1) == 0);
  }
  // Departure rate and uniformity over positions 1..3, 200,000 draws at the LU pilot's rate (8.3%)
  std::map<std::size_t, int> count;
  const int n = 200000;
  for (std::uint64_t k = 0; k < static_cast<std::uint64_t>(n); ++k)
    ++count[random_draw(12345, k, 0.083, 4)];
  const double rate = 1.0 - count[0] / static_cast<double>(n);
  REQUIRE(std::fabs(rate - 0.083) < 4 * std::sqrt(0.083 * 0.917 / n)); // within 4 standard errors
  for (std::size_t i = 1; i <= 3; ++i)
    REQUIRE(std::fabs(count[i] / (0.083 * n) - 1.0 / 3) < 0.02);
  REQUIRE(count.size() == 4); // nothing outside the window
  // Different seeds give different sequences
  int differ = 0;
  for (std::uint64_t k = 0; k < 1000; ++k)
    differ += random_draw(1, k, 0.5, 4) != random_draw(2, k, 0.5, 4);
  REQUIRE(differ > 300);
}

TEST_CASE("controls: random selection is reproducible through the policy and advances only on commit")
{
  auto run = [](std::uint64_t seed) {
    RewardPolicy p{control(Selection::random, 4, false, 0.25, seed), 1, 8};
    return std::make_pair(victims_of(p, 4000), p.stats);
  };
  const auto [a, sa] = run(99);
  const auto [b, sb] = run(99);
  const auto [c, sc] = run(100);
  REQUIRE(a == b);
  REQUIRE(sa.chose_other == sb.chose_other);
  REQUIRE(a != c);
  // Measured departure rate close to the configured one
  const double rate = static_cast<double>(sa.chose_other) / static_cast<double>(sa.decisions);
  REQUIRE(std::fabs(rate - 0.25) < 4 * std::sqrt(0.25 * 0.75 / static_cast<double>(sa.decisions)));

  // choose() alone changes nothing: repeated calls without commit give the same victim
  RewardPolicy p{control(Selection::random, 4, false, 0.5, 7), 1, 8};
  const auto w = table({M_bt, M_bt, M_bt, M_bt});
  std::set<std::uint32_t> seen;
  for (int i = 0; i < 20; ++i) {
    const auto v = p.choose(0, oldest_first(4), w).victim;
    REQUIRE(v == p.choose(0, oldest_first(4), w).victim);
    seen.insert(v);
    p.commit(0, p.choose(0, oldest_first(4), w));
  }
  REQUIRE(seen.size() > 1);
  REQUIRE(p.stats.decisions == 20);
  REQUIRE(same_weights(p.weights(), WeightTable{}));
}

TEST_CASE("controls: mode off and single candidates are the base policy's choice under every selection")
{
  for (auto s : {Selection::learned, Selection::priority, Selection::random}) {
    RewardConfig c = control(s, 4, false, s == Selection::random ? 1.0 : 0.0);
    c.mode = Mode::off;
    RewardPolicy off{c, 1, 8};
    REQUIRE(off.choose(0, oldest_first(4), table({M_bt, S_at, S_at, S_at})).victim == 0);
    RewardPolicy one{control(s, 4, false, s == Selection::random ? 1.0 : 0.0), 1, 8};
    const auto d = one.choose(0, {5}, table({M_bt, M_bt, M_bt, M_bt, M_bt, M_bt}));
    REQUIRE(d.victim == 5);
    REQUIRE_FALSE(d.learned);
  }
}

TEST_CASE("controls: the diagnostics count ties on the value the selection maximises")
{
  // Priority: M_bt, M_bt, M_bt, M_bt is all tied; M_bt, S_bt, M_bt, M_bt is not, even with all weights 0
  RewardPolicy p{control(Selection::priority), 1, 8};
  RewardObserver o{1, 8, WeightScope::global};
  const auto same = table({M_bt, M_bt, M_bt, M_bt});
  const auto mixed = table({M_bt, S_bt, M_bt, M_bt});
  auto d = p.choose(0, oldest_first(4), same);
  o.observe_decision(p, 0, oldest_first(4), same, d);
  p.commit(0, d);
  d = p.choose(0, oldest_first(4), mixed);
  REQUIRE(d.victim == 1);
  o.observe_decision(p, 0, oldest_first(4), mixed, d);
  p.commit(0, d);
  REQUIRE(o.stats.decisions == 2);
  REQUIRE(o.stats.all_tied == 1);
  REQUIRE(o.stats.tied_distinct == 0);
  REQUIRE(o.stats.victim_position[1] == 1);

  // Random: features are ignored, so every decision is "all tied"; distinct features count as tied-distinct
  RewardPolicy r{control(Selection::random, 4, false, 1.0, 3), 1, 8};
  RewardObserver ro{1, 8, WeightScope::global};
  d = r.choose(0, oldest_first(4), mixed);
  ro.observe_decision(r, 0, oldest_first(4), mixed, d);
  REQUIRE(ro.stats.all_tied == 1);
  REQUIRE(ro.stats.tied_distinct == 1);
}
