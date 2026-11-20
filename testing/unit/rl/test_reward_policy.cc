// Modified reward-learning core (rl_reward_policy.h). Ports ChampSim test/cpp/src/447-rl-mesi.cc where the logic is
// shared (Iprev, parsing of removed options and lazy invalidation inference are not ported), plus Sniper-port tests.

#include "rl_reward_policy.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
constexpr Features S_bt{Mesi::S, false}, S_at{Mesi::S, true};
constexpr Features E_bt{Mesi::E, false};
constexpr Features M_bt{Mesi::M, false}, M_at{Mesi::M, true};
constexpr Features I_bt{Mesi::I, false}, I_at{Mesi::I, true};

RewardConfig cfg(Mode m, std::uint32_t window = 4, double alpha = 0.5, bool protect = true)
{
  RewardConfig c;
  c.mode = m;
  c.window = window;
  c.alpha = alpha;
  c.protect_feedback = protect;
  return c;
}

auto table(std::vector<Features> per_way)
{
  return [per_way](std::uint32_t way) { return per_way.at(way); };
}

// Candidates 0..n-1, way 0 oldest (the order the base policy would report)
std::vector<std::uint32_t> oldest_first(std::uint32_t n)
{
  std::vector<std::uint32_t> c(n);
  for (std::uint32_t i = 0; i < n; ++i)
    c[i] = i;
  return c;
}

WeightTable weights_after(Mode m, Features f, double r, double alpha)
{
  WeightTable w;
  w.reward(m, f, r, alpha);
  return w;
}
} // namespace

TEST_CASE("reward: all weights start at zero and every score is zero")
{
  RewardPolicy p{RewardConfig{}, 2, 8};
  for (auto s : {Mesi::S, Mesi::E, Mesi::M})
    REQUIRE(p.weights().state_weight(s) == 0.0);
  REQUIRE(p.weights().share_weight(false) == 0.0);
  REQUIRE(p.weights().share_weight(true) == 0.0);
  REQUIRE(p.weights().score(Mode::both, M_at) == 0.0);

  // Defaults approved for Stage 1 (docs/rl/ALGORITHM_AUDIT.md §6)
  RewardConfig d;
  REQUIRE(d.mode == Mode::both);
  REQUIRE(d.window == 4);
  REQUIRE(d.alpha == 0.1);
  REQUIRE(d.protect_feedback);
  REQUIRE(d.scope == WeightScope::global);
}

TEST_CASE("reward: weights move toward each reward, exactly, and stay in [-1, 1]")
{
  // Ported from ChampSim 447 (alpha 0.5)
  WeightTable w;
  w.reward(Mode::both, E_bt, +1.0, 0.5);
  REQUIRE_APPROX(w.state_weight(Mesi::E), 0.5);
  REQUIRE_APPROX(w.share_weight(false), 0.5);
  w.reward(Mode::both, E_bt, -1.0, 0.5);
  REQUIRE_APPROX(w.state_weight(Mesi::E), -0.25);
  REQUIRE_APPROX(w.score(Mode::both, E_bt), -0.5);
  REQUIRE_APPROX(w.score(Mode::state, E_bt), -0.25);
  REQUIRE_APPROX(w.score(Mode::bitset, E_bt), -0.25);

  // alpha 0.1, repeated +1: w_k = 1 - 0.9^k, never above 1
  WeightTable v;
  for (int k = 1; k <= 50; ++k) {
    v.reward(Mode::state, M_bt, +1.0, 0.1);
    REQUIRE_APPROX(v.state_weight(Mesi::M), 1.0 - std::pow(0.9, k));
    REQUIRE(v.state_weight(Mesi::M) <= 1.0);
  }
  for (int k = 0; k < 200; ++k)
    v.reward(Mode::state, M_bt, -1.0, 0.1);
  REQUIRE(v.state_weight(Mesi::M) >= -1.0);
  REQUIRE(v.state_weight(Mesi::M) < -0.99);
}

TEST_CASE("reward: positive rewards raise eviction priority, negative rewards lower it")
{
  WeightTable w;
  w.reward(Mode::both, S_bt, +1.0, 0.1);
  REQUIRE(w.score(Mode::both, S_bt) > w.score(Mode::both, E_bt));
  w.reward(Mode::both, M_bt, -1.0, 0.1);
  REQUIRE(w.score(Mode::both, M_bt) < w.score(Mode::both, E_bt));
}

TEST_CASE("reward: only the weights of the active features are updated")
{
  // Ported from ChampSim 447: the mode decides which groups train
  WeightTable w = weights_after(Mode::both, E_bt, -1.0, 0.5); // wE = -0.5, wBT = -0.5
  w.reward(Mode::state, M_bt, +1.0, 0.5);
  REQUIRE_APPROX(w.state_weight(Mesi::M), 0.5);
  REQUIRE_APPROX(w.share_weight(false), -0.5); // sharing group untouched in state mode
  w.reward(Mode::bitset, S_at, +1.0, 0.5);
  REQUIRE_APPROX(w.share_weight(true), 0.5);
  REQUIRE_APPROX(w.state_weight(Mesi::S), 0.0); // state group untouched in bitset mode
  // The other values of the same group are untouched
  REQUIRE_APPROX(w.state_weight(Mesi::E), -0.5);
  REQUIRE_APPROX(w.share_weight(false), -0.5);
}

TEST_CASE("reward: a block without state information contributes and trains no state weight")
{
  WeightTable w;
  w.set_state_weight(Mesi::M, 0.7);
  w.set_share_weight(true, 0.2);
  REQUIRE_APPROX(w.score(Mode::both, I_at), 0.2);
  REQUIRE_APPROX(w.score(Mode::state, I_at), 0.0);
  w.reward(Mode::both, I_at, -1.0, 0.5);
  REQUIRE_APPROX(w.share_weight(true), -0.4);
  REQUIRE_APPROX(w.state_weight(Mesi::M), 0.7);
  REQUIRE(w.state_weight(Mesi::I) == 0.0);
  w.set_state_weight(Mesi::I, 5.0); // ignored
  REQUIRE(w.state_weight(Mesi::I) == 0.0);
}

TEST_CASE("reward: score is the sum of the active state and sharing weights")
{
  WeightTable w;
  w.set_state_weight(Mesi::S, -0.3);
  w.set_state_weight(Mesi::E, 0.2);
  w.set_state_weight(Mesi::M, -0.1);
  w.set_share_weight(false, 0.1);
  w.set_share_weight(true, -0.4);
  REQUIRE_APPROX(w.score(Mode::both, M_bt), 0.0);
  REQUIRE_APPROX(w.score(Mode::both, S_at), -0.7);
  REQUIRE_APPROX(w.score(Mode::both, E_bt), 0.3);
  REQUIRE_APPROX(w.score(Mode::state, S_at), -0.3);
  REQUIRE_APPROX(w.score(Mode::bitset, S_at), -0.4);
  REQUIRE(w.score(Mode::off, S_at) == 0.0);
}

TEST_CASE("reward: mode off is the base policy's choice and learns nothing")
{
  // Ported from ChampSim 447 ("with mode off is exactly LRU")
  RewardPolicy p{cfg(Mode::off), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_bt, M_bt, M_bt, M_bt}));
  REQUIRE(d.victim == 0);
  REQUIRE_FALSE(d.learned);
  p.commit(0, d);
  p.on_eviction(0, 10, S_bt);
  p.on_fill(0, 0, 10, FillKind::demand);
  p.on_hit(0, 0);
  REQUIRE(p.stats.decisions == 0);
  REQUIRE(p.history(0).empty());
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), 0.5); // unchanged
}

TEST_CASE("reward: N=1 always takes the oldest candidate")
{
  RewardPolicy p{cfg(Mode::both, 1), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_bt, M_bt, M_bt, M_bt}));
  REQUIRE(d.victim == 0);
  REQUIRE(d.learned);
  REQUIRE(d.passed_over.empty());
}

TEST_CASE("reward: N=2 and N=4 take the highest score among the N oldest, ties to the older candidate")
{
  // Ported from ChampSim 447. wM = 0.5, wBT = 0.5: S_bt scores 0.5, M_bt 1.0
  const auto w = weights_after(Mode::both, M_bt, +1.0, 0.5);
  RewardPolicy n2{cfg(Mode::both, 2), 1, 4, w};
  RewardPolicy n3{cfg(Mode::both, 3), 1, 4, w};
  RewardPolicy n4{cfg(Mode::both, 4), 1, 4, w};

  auto d2 = n2.choose(0, oldest_first(4), table({S_bt, S_bt, M_bt, M_bt}));
  REQUIRE(d2.victim == 0); // M blocks are outside the window of 2; S_bt tie goes to the older way

  auto d4 = n4.choose(0, oldest_first(4), table({S_bt, S_bt, M_bt, M_bt}));
  REQUIRE(d4.victim == 2); // ties among the two M blocks go to the older one
  REQUIRE(d4.passed_over.size() == 2);
  REQUIRE(d4.passed_over[0].first == 0);
  REQUIRE(d4.passed_over[1].first == 1);
  REQUIRE(d4.passed_over[0].second == S_bt);

  auto d3 = n3.choose(0, oldest_first(4), table({S_bt, M_bt, M_bt, M_bt}));
  REQUIRE(d3.victim == 1);
  REQUIRE(d3.passed_over.size() == 1);

  // The candidate list is the base policy's order, not the way numbers
  auto dr = n4.choose(0, {3, 1, 2, 0}, table({M_bt, S_bt, S_bt, S_bt}));
  REQUIRE(dr.victim == 0);
  REQUIRE(dr.passed_over.size() == 3);
  REQUIRE(dr.passed_over[0].first == 3);
}

TEST_CASE("reward: with all weights equal the oldest candidate wins")
{
  WeightTable w;
  for (auto s : {Mesi::S, Mesi::E, Mesi::M})
    w.set_state_weight(s, 0.25);
  w.set_share_weight(false, 0.25);
  w.set_share_weight(true, 0.25);
  RewardPolicy p{cfg(Mode::both, 4), 1, 4, w};
  auto d = p.choose(0, oldest_first(4), table({S_at, E_bt, M_bt, M_at}));
  REQUIRE(d.victim == 0);
  REQUIRE(d.passed_over.empty());
}

TEST_CASE("reward: a window larger than the candidate list is clamped; a single candidate is not a decision")
{
  RewardPolicy p{cfg(Mode::both, 8), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(3), table({S_bt, S_bt, M_bt, M_bt}));
  REQUIRE(d.victim == 2);
  auto single = p.choose(0, {1}, table({M_bt, S_bt}));
  REQUIRE(single.victim == 1);
  REQUIRE_FALSE(single.learned);
  REQUIRE(single.victim_features == S_bt);
}

TEST_CASE("reward: choosing changes nothing until commit")
{
  // Ported from ChampSim 447
  RewardPolicy p{cfg(Mode::both), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_bt, M_bt, S_bt, S_bt}));
  REQUIRE(p.stats.decisions == 0);
  REQUIRE_FALSE(p.line(0, 0).is_protected);

  p.commit(0, d);
  REQUIRE(p.stats.decisions == 1);
  REQUIRE(p.stats.chose_other == 1);
  REQUIRE(p.line(0, 0).is_protected);
  REQUIRE(p.line(0, 0).snapshot == S_bt);
  REQUIRE(p.line(0, 0).passes == 1);
}

TEST_CASE("reward: a protected block that is hit gives -1 to its saved features")
{
  // Ported from ChampSim 447
  RewardPolicy p{cfg(Mode::both), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_at, M_bt, S_bt, S_bt}));
  p.commit(0, d);
  p.on_eviction(0, 11, d.victim_features);
  p.on_fill(0, d.victim, 20, FillKind::demand);

  p.on_hit(0, 0);
  REQUIRE(p.stats.protect_good == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), -0.5);
  REQUIRE_APPROX(p.weights().share_weight(true), -0.5);
  REQUIRE_APPROX(p.weights().share_weight(false), 0.5); // untouched
  REQUIRE_FALSE(p.line(0, 0).is_protected);

  p.on_hit(0, 0); // a second hit is not a second reward
  REQUIRE(p.stats.protect_good == 1);
}

TEST_CASE("reward: a protected block evicted without a hit gives +1 to its saved features")
{
  // Ported from ChampSim 447
  RewardPolicy p{cfg(Mode::state, 2), 1, 2, weights_after(Mode::state, M_bt, +1.0, 0.5)};
  auto d1 = p.choose(0, {0, 1}, table({E_bt, M_bt}));
  REQUIRE(d1.victim == 1);
  p.commit(0, d1);
  p.on_eviction(0, 11, d1.victim_features);
  p.on_fill(0, 1, 20, FillKind::demand);

  // The new block in way 1 scores the same as way 0, so the older way 0 is evicted while still protected
  auto d2 = p.choose(0, {0, 1}, table({E_bt, E_bt}));
  REQUIRE(d2.victim == 0);
  p.commit(0, d2);
  REQUIRE(p.stats.protect_wasted == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), 0.5);
}

TEST_CASE("reward: protection feedback disabled marks nothing and gives no protection rewards")
{
  RewardPolicy p{cfg(Mode::both, 4, 0.5, false), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_at, M_bt, S_bt, S_bt}));
  p.commit(0, d);
  REQUIRE_FALSE(p.line(0, 0).is_protected);
  REQUIRE(p.line(0, 0).passes == 1); // still counted as a diagnostic
  p.on_hit(0, 0);
  REQUIRE(p.stats.protect_good == 0);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), 0.0);
}

TEST_CASE("reward: history feedback, -1 when the block returns, +1 when its entry ages out")
{
  // Ported from ChampSim 447. 2 ways: history capacity 4
  RewardPolicy p{cfg(Mode::state, 4, 0.5), 1, 2};
  p.on_eviction(0, 10, E_bt);
  p.on_fill(0, 0, 10, FillKind::demand); // block 10 returns
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), -0.5);
  REQUIRE_FALSE(p.history(0).contains(10));

  // Five more evictions of blocks that never return overflow the 4-entry history
  for (std::uint64_t tag = 30; tag < 35; ++tag)
    p.on_eviction(0, tag, M_bt);
  REQUIRE(p.stats.evict_good == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), 0.5);
  REQUIRE_FALSE(p.history(0).contains(30));
  REQUIRE(p.history(0).contains(34));
}

TEST_CASE("reward: feedback goes to the saved snapshot, not to the block's current features")
{
  WeightTable w;
  w.set_state_weight(Mesi::E, 0.5);
  RewardPolicy p{cfg(Mode::both, 4, 0.5), 1, 4, w};

  // Way 0 is passed over while S and heavily shared; by the time it is hit it would look like M, light
  auto d = p.choose(0, oldest_first(4), table({S_at, E_bt, M_bt, M_bt}));
  p.commit(0, d);
  p.on_hit(0, 0);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), -0.5);
  REQUIRE_APPROX(p.weights().share_weight(true), -0.5);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), 0.0);
  REQUIRE_APPROX(p.weights().share_weight(false), 0.0);

  // History entries keep the features recorded at eviction
  p.on_eviction(0, 77, d.victim_features); // E, below
  p.on_fill(0, 1, 77, FillKind::demand);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), 0.5 + 0.5 * (-1.0 - 0.5));
  REQUIRE_APPROX(p.weights().share_weight(false), -0.5);
}

TEST_CASE("reward: weights are global, shared by every set; histories are per set")
{
  RewardPolicy p{cfg(Mode::state, 4, 0.5), 3, 2};
  p.on_eviction(0, 10, E_bt);
  p.on_fill(1, 0, 10, FillKind::demand); // same tag in another set: not that set's history
  REQUIRE(p.stats.evict_bad == 0);
  REQUIRE(p.history(0).contains(10));
  REQUIRE(p.history(1).empty());

  p.on_fill(0, 0, 10, FillKind::demand);
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE_APPROX(p.weights(0).state_weight(Mesi::E), -0.5);
  REQUIRE_APPROX(p.weights(2).state_weight(Mesi::E), -0.5); // the same table
  REQUIRE(&p.weights(0) == &p.weights(2));
  REQUIRE(p.history(0).capacity() == 4);
}

TEST_CASE("reward: per-set weight scope keeps each set's weights separate")
{
  RewardConfig c = cfg(Mode::state, 4, 0.5);
  c.scope = WeightScope::per_set;
  RewardPolicy p{c, 2, 2};
  p.on_eviction(0, 10, E_bt);
  p.on_fill(0, 0, 10, FillKind::demand);
  REQUIRE_APPROX(p.weights(0).state_weight(Mesi::E), -0.5);
  REQUIRE_APPROX(p.weights(1).state_weight(Mesi::E), 0.0);
  REQUIRE(parse_weight_scope("per_set") == WeightScope::per_set);
  REQUIRE(parse_weight_scope("global") == WeightScope::global);
  REQUIRE_FALSE(parse_weight_scope("set").has_value());
}

TEST_CASE("reward: coherence and back invalidations are not evictions and give no feedback")
{
  RewardPolicy p{cfg(Mode::both, 4, 0.5), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_bt, M_bt, S_bt, S_bt}));
  p.commit(0, d); // way 0 protected

  p.on_invalidate(0, 0, 100, InvalidationReason::coherence);
  REQUIRE_FALSE(p.line(0, 0).is_protected); // protection dropped without a reward
  REQUIRE(p.history(0).empty());
  REQUIRE(p.stats.invalidations[0] == 1);

  // The invalidated block misses again: it was never in the history, so the policy is not blamed
  p.on_fill(0, 0, 100, FillKind::demand);
  REQUIRE(p.stats.evict_bad == 0);

  p.on_invalidate(0, 2, 102, InvalidationReason::back_invalidation);
  REQUIRE(p.stats.invalidations[1] == 1);
  REQUIRE(p.history(0).empty());
  REQUIRE(p.stats.protect_good + p.stats.protect_wasted == 0);
}

TEST_CASE("reward: an upgrade (invalidate + refill) keeps the block's metadata and gives no feedback")
{
  RewardPolicy p{cfg(Mode::both, 4, 0.5), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_bt, M_bt, S_bt, S_bt}));
  p.commit(0, d); // way 0 (tag 100) protected with snapshot S_bt
  const auto before = p.weights();

  p.on_invalidate(0, 0, 100, InvalidationReason::upgrade);
  p.on_fill(0, 3, 100, FillKind::upgrade_refill); // may come back in another way
  REQUIRE(p.line(0, 3).is_protected);
  REQUIRE(p.line(0, 3).snapshot == S_bt);
  REQUIRE(p.line(0, 3).passes == 1);
  REQUIRE_FALSE(p.line(0, 0).is_protected);
  REQUIRE(p.stats.upgrade_carryovers == 1);
  REQUIRE(p.stats.invalidations[2] == 1);
  REQUIRE(p.history(0).empty());
  REQUIRE(p.weights().state_weight(Mesi::S) == before.state_weight(Mesi::S));
  REQUIRE(p.stats.evict_bad + p.stats.evict_good + p.stats.protect_good + p.stats.protect_wasted == 0);

  // A refill that does not match a pending upgrade starts fresh and is counted
  p.on_fill(0, 1, 555, FillKind::upgrade_refill);
  REQUIRE(p.stats.upgrade_refills_unmatched == 1);
  REQUIRE_FALSE(p.line(0, 1).is_protected);
}

TEST_CASE("reward: a demand fill resets the way's metadata")
{
  RewardPolicy p{cfg(Mode::both, 4, 0.5), 1, 4, weights_after(Mode::both, M_bt, +1.0, 0.5)};
  auto d = p.choose(0, oldest_first(4), table({S_bt, M_bt, S_bt, S_bt}));
  p.commit(0, d);
  REQUIRE(p.line(0, 0).is_protected);
  p.on_fill(0, 0, 999, FillKind::demand);
  REQUIRE_FALSE(p.line(0, 0).is_protected);
  REQUIRE(p.line(0, 0).passes == 0);
}

TEST_CASE("reward: a low-scoring dead block is passed over at every miss and keeps its LRU position (audit I-1)")
{
  // Intentional existing behavior, documented rather than changed: the block is never hit and never evicted, so it
  // produces no reward; only its pass counter grows.
  WeightTable w;
  w.set_state_weight(Mesi::M, -0.5); // M blocks are kept
  RewardPolicy p{cfg(Mode::state, 4, 0.1), 1, 4, w};
  rltest::SetModel<RewardPolicy> m{p, 1, 4, 4};
  m.fill(0, 100, Mesi::M, 1); // way 0, oldest, never used again
  m.fill(0, 101, Mesi::E, 1);
  m.fill(0, 102, Mesi::E, 1);
  m.fill(0, 103, Mesi::E, 1);

  for (std::uint64_t tag = 200; tag < 210; ++tag) {
    auto r = m.fill(0, tag, Mesi::E, 1);
    REQUIRE(r.evicted_tag != 100);
    REQUIRE(m.lru_order(0).front() == 0);
    REQUIRE(m.lru.at(0).rank(0) == 3);
  }
  REQUIRE(p.line(0, 0).passes == 10);
  REQUIRE(p.line(0, 0).is_protected);
  REQUIRE(p.stats.max_passes == 10);
  REQUIRE(p.stats.protect_good == 0);
  REQUIRE(p.stats.protect_wasted == 0);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), -0.5); // never corrected by this block
}

TEST_CASE("reward: protected blocks receive a protection reward and later a history reward (audit I-6)")
{
  // Intentional existing behavior: +1 for the wasted protection, then -1 when the evicted block returns
  RewardPolicy p{cfg(Mode::state, 2, 0.5), 1, 2, weights_after(Mode::state, M_bt, +1.0, 0.5)};
  auto d1 = p.choose(0, {0, 1}, table({E_bt, M_bt}));
  p.commit(0, d1); // way 0 (E) protected
  p.on_eviction(0, 11, d1.victim_features);
  p.on_fill(0, 1, 20, FillKind::demand);

  auto d2 = p.choose(0, {0, 1}, table({E_bt, E_bt}));
  p.commit(0, d2); // way 0 evicted while protected: +1
  p.on_eviction(0, 10, d2.victim_features);
  p.on_fill(0, 0, 21, FillKind::demand);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), 0.5);

  p.on_fill(0, 1, 10, FillKind::demand); // block 10 returns: -1 for the same features
  REQUIRE(p.stats.protect_wasted == 1);
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), 0.5 + 0.5 * (-1.0 - 0.5));
}
