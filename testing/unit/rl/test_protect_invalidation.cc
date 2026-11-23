// Experimental Stage 7 reward correction (rl_reward_policy.h: RewardConfig::protect_invalidation_feedback,
// docs/rl/STAGE7.md). With the option, a protected (passed-over) line that leaves through a coherence or back
// invalidation before any hit gets the protect-wasted outcome +1 on its protection snapshot, exactly once. An upgrade
// carries the protection into the refill and is never rewarded. Off (the default), nothing changes.

#include <cmath>
#include <vector>

#include "rl_diagnostics.h"
#include "rl_reward_policy.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
constexpr Features S_bt{Mesi::S, false};
constexpr Features E_bt{Mesi::E, false};
constexpr Features M_bt{Mesi::M, false}, M_at{Mesi::M, true};

RewardConfig config(bool ifb, double alpha = 0.5)
{
  RewardConfig c;
  c.alpha = alpha;
  c.protect_invalidation_feedback = ifb;
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
    if (a.state_weight(s) != b.state_weight(s))
      return false;
  return a.share_weight(false) == b.share_weight(false) && a.share_weight(true) == b.share_weight(true);
}

std::vector<std::uint32_t> oldest_first(std::uint32_t n)
{
  std::vector<std::uint32_t> c(n);
  for (std::uint32_t i = 0; i < n; ++i)
    c[i] = i;
  return c;
}

auto table(std::vector<Features> per_way)
{
  return [per_way](std::uint32_t way) { return per_way.at(way); };
}

// AT scores +0.5: in the window {S_bt, E_bt, M_bt, M_at} way 3 is evicted and ways 0-2 are passed over (protected)
const WeightTable kAT = make_weights(0, 0, 0, 0, 0.5);

void protect_ways_0_to_2(RewardPolicy& p)
{
  const auto d = p.choose(0, oldest_first(4), table({S_bt, E_bt, M_bt, M_at}));
  REQUIRE(d.victim == 3);
  p.commit(0, d);
  REQUIRE(p.line(0, 0).is_protected);
  REQUIRE(p.line(0, 1).is_protected);
  REQUIRE(p.line(0, 2).is_protected);
}

// A pseudo-random 4-set, 8-way trace with hits, coherence and back invalidations and upgrades
template <typename Obs>
void drive(RewardPolicy& p, Obs* o, std::uint64_t seed, int n, std::vector<std::uint64_t>* victims = nullptr)
{
  rltest::SetModel<RewardPolicy> m{p, 4, 8, 4};
  m.observer = o;
  std::uint64_t x = seed;
  for (int i = 0; i < n; ++i) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto set = static_cast<std::uint32_t>((x >> 33) % 4);
    const std::uint64_t tag = (x >> 40) % 24;
    const auto op = (x >> 50) % 16;
    if (auto way = m.find(set, tag)) {
      if (op == 0)
        m.invalidate(set, *way, InvalidationReason::coherence);
      else if (op == 1)
        m.invalidate(set, *way, InvalidationReason::back_invalidation);
      else if (op == 2 && m.blocks[set][*way].state != Mesi::M)
        m.upgrade(set, tag);
      else
        m.hit(set, *way);
    } else if (auto r = m.fill(set, tag, static_cast<Mesi>(1 + (x >> 20) % 3), static_cast<std::uint32_t>((x >> 10) % 5)); r.evicted) {
      if (victims)
        victims->push_back(r.evicted_tag * 64 + set * 8 + r.way);
    }
  }
}
} // namespace

TEST_CASE("invalidation feedback: off by default; off, an invalidated protected line gets no outcome (as before)")
{
  REQUIRE_FALSE(RewardConfig{}.protect_invalidation_feedback);
  RewardPolicy p{config(false), 1, 8, kAT};
  protect_ways_0_to_2(p);
  const WeightTable before = p.weights();
  p.on_invalidate(0, 0, 10, InvalidationReason::coherence);
  p.on_invalidate(0, 1, 11, InvalidationReason::back_invalidation);
  REQUIRE(p.stats.protect_invalidated == 0);
  REQUIRE(p.stats.protect_wasted == 0);
  REQUIRE_FALSE(p.line(0, 0).is_protected);
  REQUIRE(same_weights(p.weights(), before));
}

TEST_CASE("invalidation feedback: a protected line invalidated by coherence gets +1 on its protection snapshot")
{
  RewardPolicy p{config(true), 1, 8, kAT};
  protect_ways_0_to_2(p);
  p.on_invalidate(0, 0, 10, InvalidationReason::coherence); // way 0: S/BT
  REQUIRE(p.stats.protect_invalidated == 1);
  REQUIRE(p.stats.protect_wasted == 0); // separate counter; the eviction outcome is unchanged
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), 0.5);  // 0 + 0.5 (1 - 0)
  REQUIRE_APPROX(p.weights().share_weight(false), 0.5);
  REQUIRE(p.weights().state_weight(Mesi::E) == 0.0);
  REQUIRE_FALSE(p.line(0, 0).is_protected);
}

TEST_CASE("invalidation feedback: a protected line removed by back invalidation gets the same +1")
{
  RewardPolicy p{config(true), 1, 8, kAT};
  protect_ways_0_to_2(p);
  p.on_invalidate(0, 1, 11, InvalidationReason::back_invalidation); // way 1: E/BT
  REQUIRE(p.stats.protect_invalidated == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), 0.5);
  REQUIRE(p.weights().state_weight(Mesi::S) == 0.0);
}

TEST_CASE("invalidation feedback: an unprotected line, or a hit after the protection, gets nothing at invalidation")
{
  RewardPolicy p{config(true), 1, 8, kAT};
  protect_ways_0_to_2(p);
  p.on_invalidate(0, 3, 13, InvalidationReason::coherence); // way 3 was the victim's way: not protected
  REQUIRE(p.stats.protect_invalidated == 0);
  p.on_hit(0, 2); // protect-good -1, protection cleared
  REQUIRE(p.stats.protect_good == 1);
  p.on_invalidate(0, 2, 12, InvalidationReason::coherence);
  REQUIRE(p.stats.protect_invalidated == 0);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), -0.5); // only the hit's -1
}

TEST_CASE("invalidation feedback: an upgrade carries the protection and is never rewarded; a later exit is, once")
{
  RewardPolicy p{config(true), 1, 8, kAT};
  protect_ways_0_to_2(p);
  p.on_invalidate(0, 0, 10, InvalidationReason::upgrade);
  REQUIRE(p.stats.protect_invalidated == 0);
  p.on_fill(0, 0, 10, FillKind::upgrade_refill);
  REQUIRE(p.stats.upgrade_carryovers == 1);
  REQUIRE(p.line(0, 0).is_protected); // carried into the refill
  REQUIRE(p.weights().state_weight(Mesi::S) == 0.0);
  // The refilled line is later invalidated by another core's write: one +1 for that protected residency
  p.on_invalidate(0, 0, 10, InvalidationReason::coherence);
  p.on_invalidate(0, 0, 10, InvalidationReason::coherence); // a duplicate event on the now-cleared way: nothing more
  REQUIRE(p.stats.protect_invalidated == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), 0.5);
}

TEST_CASE("invalidation feedback: repeated passes give one outcome per protected residency, on the latest snapshot")
{
  RewardPolicy p{config(true), 1, 8, make_weights(0, 0, 0, 0, 0.5)};
  // Way 0 is passed over in 50 decisions; its features at the passes change from M/BT to S/BT after the 25th
  for (int k = 0; k < 50; ++k) {
    const Features f0 = k < 25 ? M_bt : S_bt;
    const auto d = p.choose(0, {0, 4 + static_cast<std::uint32_t>(k % 4)}, table({f0, M_bt, M_bt, M_bt, M_at, M_at, M_at, M_at}));
    REQUIRE(d.victim != 0);
    p.commit(0, d);
  }
  REQUIRE(p.line(0, 0).passes == 50);
  REQUIRE(p.stats.passes == 50);
  REQUIRE(p.stats.protect_wasted == 0); // the victims (ways 4-7) were never protected
  p.on_invalidate(0, 0, 10, InvalidationReason::coherence);
  REQUIRE(p.stats.protect_invalidated == 1);       // one outcome, not one per pass
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), 0.5); // the snapshot of the last pass (S/BT)
  REQUIRE(p.weights().state_weight(Mesi::M) == 0.0);
}

TEST_CASE("invalidation feedback: eviction and history rewards are unchanged; each residency has one outcome")
{
  RewardPolicy p{config(true), 1, 8, kAT};
  protect_ways_0_to_2(p);
  p.on_eviction(0, 30, M_at);
  // Way 1 is evicted while protected: the existing protect-wasted outcome, then its later invalidation cannot happen
  const auto d = p.choose(0, {1, 2}, table({S_bt, E_bt, M_bt, M_at}));
  REQUIRE(d.victim == 1); // E/BT and M/BT tie at 0: the older (way 1)
  p.commit(0, d);
  REQUIRE(p.stats.protect_wasted == 1);
  p.on_eviction(0, 31, d.victim_features);
  p.on_miss(0, 31); // its block returns while in the history: evict-bad -1, as before
  REQUIRE(p.stats.evict_bad == 1);
  p.on_invalidate(0, 1, 31, InvalidationReason::coherence); // way 1 now holds an unprotected line: nothing
  REQUIRE(p.stats.protect_invalidated == 0);
  p.on_miss(0, 30); // the first victim returns: -1 to M/AT
  REQUIRE(p.stats.evict_bad == 2);
}

TEST_CASE("invalidation feedback: mode off, learning off and the priority control")
{
  RewardConfig off = config(true);
  off.mode = Mode::off;
  RewardPolicy po{off, 1, 8, kAT};
  po.on_invalidate(0, 0, 1, InvalidationReason::coherence);
  REQUIRE(po.stats.protect_invalidated == 0);

  RewardConfig frozen = config(true);
  frozen.learning = false;
  RewardPolicy pf{frozen, 1, 8, kAT};
  protect_ways_0_to_2(pf);
  pf.on_invalidate(0, 0, 10, InvalidationReason::coherence);
  REQUIRE(pf.stats.protect_invalidated == 1); // counted
  REQUIRE(same_weights(pf.weights(), kAT));   // but no weight changes

  // The priority control ignores the weights, so its decisions do not change with the option; the outcome is counted
  RewardConfig a;
  a.selection = Selection::priority;
  RewardConfig b = a;
  b.protect_invalidation_feedback = true;
  RewardPolicy pa{a, 4, 8}, pb{b, 4, 8};
  std::vector<std::uint64_t> va, vb;
  drive<RewardObserver>(pa, nullptr, 5, 30000, &va);
  drive<RewardObserver>(pb, nullptr, 5, 30000, &vb);
  REQUIRE(va == vb);
  REQUIRE(pa.stats.protect_invalidated == 0);
  REQUIRE(pb.stats.protect_invalidated > 0);
}

TEST_CASE("invalidation feedback: with the discounted-mean estimator the +1 is applied at the cache's decision count")
{
  RewardConfig c = config(true);
  c.estimator = Estimator::discounted_mean;
  c.horizon = 10.0;
  RewardPolicy p{c, 1, 8, kAT};
  protect_ways_0_to_2(p); // decision 1
  for (int k = 0; k < 4; ++k) // decisions 2-5 elsewhere in the set, all tied: way 4 each time, never protected
    p.commit(0, p.choose(0, {4, 5}, table({M_bt, M_bt, M_bt, M_bt, M_bt, M_bt})));
  p.on_invalidate(0, 0, 10, InvalidationReason::back_invalidation); // way 0 (S/BT) at decision 5
  const double g = std::exp(-5.0 / 10.0);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), 1.0 / (g + 1.0));
}

TEST_CASE("invalidation feedback: off, a full trace is identical to the default; on, it differs")
{
  RewardConfig explicit_off;
  explicit_off.protect_invalidation_feedback = false;
  RewardPolicy a{RewardConfig{}, 4, 8}, b{explicit_off, 4, 8}, c{config(true, 0.1), 4, 8};
  std::vector<std::uint64_t> va, vb, vc;
  drive<RewardObserver>(a, nullptr, 3, 40000, &va);
  drive<RewardObserver>(b, nullptr, 3, 40000, &vb);
  drive<RewardObserver>(c, nullptr, 3, 40000, &vc);
  REQUIRE(va == vb);
  REQUIRE(same_weights(a.weights(), b.weights()));
  REQUIRE(a.stats.protect_wasted == b.stats.protect_wasted);
  REQUIRE(c.stats.protect_invalidated > 0);
  REQUIRE(va != vc); // the extra outcomes changed the weights and so later decisions
}

TEST_CASE("invalidation feedback: the observer's classes and the runs ended by invalidation equal the core's count")
{
  for (std::uint64_t seed : {1ULL, 2ULL, 9ULL})
    for (Estimator est : {Estimator::ema, Estimator::discounted_mean}) {
      RewardConfig c = config(true, 0.1);
      c.estimator = est;
      c.horizon = 512;
      RewardPolicy p{c, 4, 8};
      RewardObserver o{4, 8, c.scope};
      drive(p, &o, seed, 40000);
      const auto& h = o.runs().histogram;
      std::uint64_t ended_by_invalidation = 0;
      for (auto n : h[static_cast<std::size_t>(RunEnd::invalidated)])
        ended_by_invalidation += n;
      REQUIRE(p.stats.protect_invalidated > 0);
      REQUIRE(o.stats.protect_invalidated.total() == p.stats.protect_invalidated);
      REQUIRE(o.stats.protect_invalidated.state[0] + o.stats.protect_invalidated.state[1] + o.stats.protect_invalidated.state[2]
              == p.stats.protect_invalidated);
      REQUIRE(ended_by_invalidation == p.stats.protect_invalidated);
      // The existing identities still hold
      const auto total = [&](RewardKind k) { return o.stats.rewards[static_cast<std::size_t>(k)].total(); };
      REQUIRE(total(RewardKind::protect_wasted) == p.stats.protect_wasted);
      REQUIRE(total(RewardKind::protect_good) == p.stats.protect_good);
      REQUIRE(total(RewardKind::evict_bad) == p.stats.evict_bad);
      REQUIRE(total(RewardKind::evict_good) == p.stats.evict_good);
    }
}
