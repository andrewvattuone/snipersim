// Experimental Stage 7 variant (rl_reward_policy.h: Estimator::discounted_mean, docs/rl/STAGE7.md). It may change
// only how a weight estimates its feature's reward: the default must stay the algorithm's EMA bit for bit, every
// reward event and the selection must be unchanged, and the estimate must be the discounted mean of rewards already
// received, with the same time span for every feature.

#include <cmath>
#include <vector>

#include "rl_diagnostics.h"
#include "rl_reward_policy.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
constexpr Features S_bt{Mesi::S, false}, S_at{Mesi::S, true};
constexpr Features E_bt{Mesi::E, false};
constexpr Features M_bt{Mesi::M, false}, M_at{Mesi::M, true};

RewardConfig discounted(double horizon, bool learning = true)
{
  RewardConfig c;
  c.estimator = Estimator::discounted_mean;
  c.horizon = horizon;
  c.learning = learning;
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

const WeightTable kFrozen = make_weights(-0.09, -0.38, -0.76, -0.64, 0.0);

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

// Same driver as test_controls.cc: one set, pseudo-random states and hints, some hits on recent tags
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

// Brute force: the discounted mean of (t_k, r_k) at time `now`, with the prior (0, w0)
double brute_force(double w0, const std::vector<std::pair<std::uint64_t, double>>& events, std::uint64_t now, double horizon)
{
  double num = w0 * std::exp(-static_cast<double>(now) / horizon), den = std::exp(-static_cast<double>(now) / horizon);
  for (const auto& [t, r] : events) {
    const double g = std::exp(-static_cast<double>(now - t) / horizon);
    num += g * r;
    den += g;
  }
  return num / den;
}

struct Lcg {
  std::uint64_t x;
  double uniform()
  {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(x >> 11) * 0x1.0p-53;
  }
};
} // namespace

TEST_CASE("estimator: the default is the algorithm's EMA; names parse and invalid names are rejected")
{
  RewardConfig d;
  REQUIRE(d.estimator == Estimator::ema);
  REQUIRE(d.horizon == 65536.0);
  for (auto e : {Estimator::ema, Estimator::discounted_mean})
    REQUIRE(parse_estimator(name(e)) == e);
  REQUIRE_FALSE(parse_estimator("mean").has_value());
  REQUIRE_FALSE(parse_estimator("").has_value());
}

TEST_CASE("estimator: an explicit ema, with any horizon, gives the default's victims and weights bit for bit")
{
  RewardConfig ema;
  ema.estimator = Estimator::ema;
  ema.horizon = 7; // unused by the EMA
  RewardPolicy a{RewardConfig{}, 1, 8}, b{ema, 1, 8};
  REQUIRE(victims_of(a, 5000) == victims_of(b, 5000));
  REQUIRE(same_weights(a.weights(), b.weights()));
  REQUIRE(a.stats.chose_other > 0);
  REQUIRE(a.stats.evict_bad == b.stats.evict_bad);
  REQUIRE(a.stats.protect_wasted == b.stats.protect_wasted);
}

TEST_CASE("estimator: the discounted mean equals the brute-force discounted average, prior included")
{
  for (double w0 : {0.0, -0.76, 0.5})
    for (double horizon : {1.0, 100.0, 65536.0}) {
      WeightTable w = make_weights(0, 0, w0, 0, 0);
      std::vector<std::pair<std::uint64_t, double>> events;
      Lcg rng{static_cast<std::uint64_t>(horizon) + 3};
      std::uint64_t t = 0;
      for (int k = 0; k < 400; ++k) {
        t += static_cast<std::uint64_t>(rng.uniform() * 3 * horizon); // gaps 0 .. 3 horizons, several at the same time
        const double r = rng.uniform() < 0.4 ? 1.0 : -1.0;
        w.reward_discounted(Mode::both, M_bt, r, t, horizon);
        events.emplace_back(t, r);
        REQUIRE_APPROX(w.state_weight(Mesi::M), brute_force(w0, events, t, horizon));
        REQUIRE(w.state_weight(Mesi::M) >= -1.0);
        REQUIRE(w.state_weight(Mesi::M) <= 1.0);
      }
      // The sharing weight of the same snapshot got the same rewards, from its own prior (0)
      REQUIRE_APPROX(w.share_weight(false), brute_force(0.0, events, t, horizon));
      // Other features were never rewarded and keep their values
      REQUIRE(w.state_weight(Mesi::S) == 0.0);
      REQUIRE(w.state_weight(Mesi::E) == 0.0);
      REQUIRE(w.share_weight(true) == 0.0);
    }
}

TEST_CASE("estimator: one exact case by hand, and the estimate depends on time, not on the number of events")
{
  const double H = 100.0;
  WeightTable w;
  w.reward_discounted(Mode::both, M_bt, +1.0, 10, H);
  REQUIRE_APPROX(w.state_weight(Mesi::M), 1.0 / (1.0 + std::exp(-0.1)));
  w.reward_discounted(Mode::both, M_bt, -1.0, 30, H);
  REQUIRE_APPROX(w.state_weight(Mesi::M), (std::exp(-0.2) - 1.0) / (std::exp(-0.3) + std::exp(-0.2) + 1.0));

  // A long silence discounts old evidence: the next reward then dominates
  WeightTable a, b;
  a.reward_discounted(Mode::both, S_bt, -1.0, 0, H);
  b.reward_discounted(Mode::both, S_bt, -1.0, 0, H);
  a.reward_discounted(Mode::both, S_bt, +1.0, 1, H);
  b.reward_discounted(Mode::both, S_bt, +1.0, 1000, H);
  REQUIRE(a.state_weight(Mesi::S) < 0.5);
  REQUIRE(b.state_weight(Mesi::S) > 0.99);
}

TEST_CASE("estimator: a very long silence or a huge horizon gives finite weights")
{
  WeightTable w;
  w.reward_discounted(Mode::both, E_bt, -1.0, 0, 1.0);
  w.reward_discounted(Mode::both, E_bt, +1.0, 1ULL << 40, 1.0); // the discount underflows to 0
  REQUIRE(w.state_weight(Mesi::E) == 1.0);

  WeightTable m; // horizon far beyond any run: the plain mean of the prior and the rewards
  for (int k = 0; k < 9; ++k)
    m.reward_discounted(Mode::both, E_bt, k < 3 ? 1.0 : -1.0, static_cast<std::uint64_t>(k) * 1000, 1e300);
  REQUIRE_APPROX(m.state_weight(Mesi::E), (3.0 - 6.0) / 10.0);
  REQUIRE(std::isfinite(m.share_weight(false)));
}

TEST_CASE("estimator: features rewarded at different rates see the same time span (phase-structured rewards)")
{
  // Rewards common to all features flip between +1 and -1 phases of 2,000 decisions. M is rewarded at every
  // decision, S once per 50 decisions. With the per-event EMA, M forgets within ~10 decisions and S within ~500, so
  // the two weights describe different phases; with the discounted mean they describe the same stretch of time.
  const double H = 8192;
  WeightTable ema, dm;
  double max_gap_ema = 0, max_gap_dm = 0;
  for (std::uint64_t t = 0; t < 200000; ++t) {
    const double r = (t / 2000) % 2 == 0 ? 1.0 : -1.0;
    ema.reward(Mode::state, M_bt, r, 0.1);
    dm.reward_discounted(Mode::state, M_bt, r, t, H);
    if (t % 50 == 0) {
      ema.reward(Mode::state, S_bt, r, 0.1);
      dm.reward_discounted(Mode::state, S_bt, r, t, H);
    }
    if (t > 4 * static_cast<std::uint64_t>(H) && t % 97 == 0) {
      max_gap_ema = std::max(max_gap_ema, std::abs(ema.state_weight(Mesi::M) - ema.state_weight(Mesi::S)));
      max_gap_dm = std::max(max_gap_dm, std::abs(dm.state_weight(Mesi::M) - dm.state_weight(Mesi::S)));
    }
  }
  REQUIRE(max_gap_ema > 0.5); // same reward stream, yet the EMA weights disagree strongly
  REQUIRE(max_gap_dm < 0.1);
}

TEST_CASE("estimator: under bursty rewards the discounted mean keeps the order of two features' true means")
{
  // E's mean reward +0.20, M's +0.10, plus a common phase of +/-0.6 alternating every 3,000 decisions;
  // M rewarded every decision, E once per 10. The EMA order follows the phases; the discounted mean's does not.
  const double H = 65536;
  WeightTable ema, dm;
  Lcg rng{42};
  int samples = 0, ema_right = 0, dm_right = 0;
  for (std::uint64_t t = 0; t < 1200000; ++t) {
    const double phase = (t / 3000) % 2 == 0 ? 0.6 : -0.6;
    auto draw = [&](double mu) { return rng.uniform() < std::clamp((1 + mu + phase) / 2, 0.0, 1.0) ? 1.0 : -1.0; };
    const double rm = draw(0.10);
    ema.reward(Mode::state, M_bt, rm, 0.1);
    dm.reward_discounted(Mode::state, M_bt, rm, t, H);
    if (t % 10 == 0) {
      const double re = draw(0.20);
      ema.reward(Mode::state, E_bt, re, 0.1);
      dm.reward_discounted(Mode::state, E_bt, re, t, H);
    }
    if (t > 4 * static_cast<std::uint64_t>(H) && t % 101 == 0) {
      ++samples;
      ema_right += ema.state_weight(Mesi::E) > ema.state_weight(Mesi::M);
      dm_right += dm.state_weight(Mesi::E) > dm.state_weight(Mesi::M);
    }
  }
  std::printf("    [order kept: EMA %.1f%%, discounted mean %.1f%% of %d samples]\n", 100.0 * ema_right / samples,
              100.0 * dm_right / samples, samples);
  REQUIRE(dm_right > samples * 99 / 100);
  REQUIRE(ema_right < samples * 80 / 100);
}

TEST_CASE("estimator: the initial weight is one pseudo-reward at decision 0; learning off keeps it bit-identical")
{
  // Learning on: the first reward at decision t averages with the prior discounted by t
  RewardPolicy p{discounted(100.0), 1, 8, kFrozen};
  for (int k = 0; k < 50; ++k) { // 50 decisions over four M ways: all tied, the oldest (way 0) is the victim
    const auto d = p.choose(0, oldest_first(4), table({M_bt, M_bt, M_bt, M_bt}));
    p.commit(0, d);
  }
  REQUIRE(p.stats.decisions == 50);
  REQUIRE(p.weights().state_weight(Mesi::S) == -0.09); // nothing rewarded yet: still the configured values
  p.on_eviction(0, 7, S_at);
  p.on_miss(0, 7); // S/AT returns while in the history at decision 50: -1
  const double g = std::exp(-50.0 / 100.0);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), (-0.09 * g - 1.0) / (g + 1.0));
  REQUIRE_APPROX(p.weights().share_weight(true), (0.0 * g - 1.0) / (g + 1.0));
  REQUIRE(p.weights().state_weight(Mesi::M) == -0.76);

  // Learning off: every reward counted, no weight changes
  RewardPolicy f{discounted(100.0, false), 1, 8, kFrozen};
  auto d1 = f.choose(0, oldest_first(4), table({M_bt, E_bt, M_bt, S_at}));
  f.commit(0, d1);
  f.on_eviction(0, 10, d1.victim_features);
  f.on_hit(0, 1);
  f.on_miss(0, 10);
  for (std::uint64_t t = 20; t < 40; ++t)
    f.on_eviction(0, t, M_bt);
  REQUIRE(f.stats.protect_good == 1);
  REQUIRE(f.stats.evict_bad == 1);
  REQUIRE(f.stats.evict_good > 0);
  REQUIRE(same_weights(f.weights(), kFrozen));
}

TEST_CASE("estimator: eviction-history hard cases give the same reward events as the EMA, with decision times")
{
  // Drive the same core calls through an EMA policy and a discounted-mean policy. The windows hold identical features
  // (all tied: the oldest is always the victim), so both make the same decisions and must count the same events.
  RewardPolicy e{RewardConfig{}, 1, 8}, dm{discounted(1000.0), 1, 8};
  auto step = [&](RewardPolicy& p, std::uint64_t victim_tag) {
    const auto d = p.choose(0, oldest_first(8), table({M_bt, M_bt, M_bt, M_bt, M_bt, M_bt, M_bt, M_bt}));
    p.commit(0, d);
    p.on_eviction(0, victim_tag, d.victim_features);
  };
  for (RewardPolicy* p : {&e, &dm}) {
    for (std::uint64_t t = 100; t < 116; ++t) // fill the 16-entry history: no age-out yet
      step(*p, t);
    REQUIRE(p->stats.evict_good == 0);
    // (1) The oldest entry's block returns: -1 before the victim's insertion, which therefore ages nothing out
    p->on_miss(0, 100);
    step(*p, 200);
    REQUIRE(p->stats.evict_bad == 1);
    REQUIRE(p->stats.evict_good == 0);
    // (2) The next insertion ages out tag 101 (+1); its block's later return is not in the history: no reward
    step(*p, 201);
    REQUIRE(p->stats.evict_good == 1);
    p->on_miss(0, 101);
    REQUIRE(p->stats.evict_bad == 1);
    REQUIRE(p->history(0).size() == 16);
  }
  REQUIRE(e.stats.decisions == dm.stats.decisions);
  // Weights: EMA from 0: -0.1, then -0.1 + 0.1 * (1 + 0.1) = 0.01; the discounted mean of {prior 0 at 0, -1 at 16,
  // +1 at 18}
  REQUIRE_APPROX(e.weights().state_weight(Mesi::M), -0.1 + 0.1 * (1.0 + 0.1));
  const double H = 1000.0;
  const double num = -std::exp(-2.0 / H) + 1.0, den = std::exp(-18.0 / H) + std::exp(-2.0 / H) + 1.0;
  REQUIRE_APPROX(dm.weights().state_weight(Mesi::M), num / den);
}

TEST_CASE("estimator: a protected victim's +1 and its block's return -1 are applied at their own decision times")
{
  const double H = 50.0;
  RewardPolicy p{discounted(H), 1, 8, make_weights(0, 0, 0, 0, 0.5)};
  // AT scores +0.5: way 3 (M/AT) is evicted, ways 0-2 (M/BT) are passed over and protected at decision 1
  const auto d1 = p.choose(0, oldest_first(4), table({M_bt, M_bt, M_bt, M_at}));
  REQUIRE(d1.victim == 3);
  p.commit(0, d1);
  p.on_eviction(0, 30, d1.victim_features);
  for (int k = 0; k < 9; ++k) { // decisions 2-10 evict the protected way 0 at decision 2 (protect-wasted +1)
    const auto d = p.choose(0, oldest_first(4), table({M_bt, M_bt, M_bt, M_bt}));
    p.commit(0, d);
    if (k == 0) {
      REQUIRE(p.stats.protect_wasted == 1);
      p.on_eviction(0, 40, d.victim_features);
    }
  }
  p.on_miss(0, 40); // the protected block returns while in the history at decision 10: -1, contradicting the +1
  REQUIRE(p.stats.evict_bad == 1);
  const double num = 1.0 * std::exp(-8.0 / H) - 1.0;
  const double den = std::exp(-10.0 / H) + std::exp(-8.0 / H) + 1.0;
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), num / den);
}

TEST_CASE("estimator: protection survives an upgrade; the later hit rewards at the decision count of the hit")
{
  const double H = 10.0;
  RewardPolicy p{discounted(H), 1, 8, make_weights(0, 0, 0, 0, 0.5)};
  const auto d = p.choose(0, oldest_first(4), table({E_bt, M_bt, M_bt, M_at}));
  p.commit(0, d); // decision 1: way 0 (E/BT) protected
  REQUIRE(p.line(0, 0).is_protected);
  p.on_invalidate(0, 0, 5, InvalidationReason::upgrade);
  p.on_fill(0, 0, 5, FillKind::upgrade_refill);
  REQUIRE(p.line(0, 0).is_protected);
  for (int k = 0; k < 4; ++k) // decisions 2-5 among other ways
    p.commit(0, p.choose(0, {1, 2, 3, 4}, table({M_bt, M_bt, M_bt, M_bt, M_bt, M_bt, M_bt, M_bt})));
  p.on_hit(0, 0); // protect-good at decision 5
  REQUIRE(p.stats.protect_good == 1);
  const double g = std::exp(-5.0 / H);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), -1.0 / (g + 1.0));
}

TEST_CASE("estimator: the priority and random controls ignore the estimator")
{
  for (Selection s : {Selection::priority, Selection::random}) {
    RewardConfig a;
    a.selection = s;
    a.learning = true;
    a.random_rate = s == Selection::random ? 0.3 : 0.0;
    a.seed = 9;
    RewardConfig b = a;
    b.estimator = Estimator::discounted_mean;
    b.horizon = 500;
    RewardPolicy pa{a, 1, 8}, pb{b, 1, 8};
    REQUIRE(victims_of(pa, 4000) == victims_of(pb, 4000));
    REQUIRE(pa.stats.evict_bad == pb.stats.evict_bad);
    REQUIRE(pa.stats.protect_wasted == pb.stats.protect_wasted);
  }
}

TEST_CASE("estimator: per-set scope keeps one accumulator per set and one clock per cache")
{
  RewardConfig c = discounted(100.0);
  c.scope = WeightScope::per_set;
  RewardPolicy p{c, 2, 8};
  for (int k = 0; k < 20; ++k)
    p.commit(1, p.choose(1, oldest_first(4), table({M_bt, M_bt, M_bt, M_bt}))); // 20 decisions, all in set 1
  p.on_eviction(0, 3, E_bt);
  p.on_miss(0, 3); // set 0, at decision 20 of the cache
  const double g = std::exp(-20.0 / 100.0);
  REQUIRE_APPROX(p.weights(0).state_weight(Mesi::E), -1.0 / (g + 1.0));
  REQUIRE(p.weights(1).state_weight(Mesi::E) == 0.0);
}

TEST_CASE("estimator: the diagnostics' totals still equal the core's counters")
{
  RewardConfig c = discounted(256.0);
  RewardPolicy p{c, 4, 8};
  RewardObserver o{4, 8, c.scope};
  rltest::SetModel<RewardPolicy> m{p, 4, 8, 4};
  m.observer = &o;
  std::uint64_t x = 11;
  for (int i = 0; i < 30000; ++i) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto set = static_cast<std::uint32_t>((x >> 33) % 4);
    const std::uint64_t tag = (x >> 40) % 24;
    const auto op = (x >> 50) % 16;
    if (auto way = m.find(set, tag)) {
      if (op == 0)
        m.invalidate(set, *way, InvalidationReason::coherence);
      else if (op == 2 && m.blocks[set][*way].state != Mesi::M)
        m.upgrade(set, tag);
      else
        m.hit(set, *way);
    } else {
      m.fill(set, tag, static_cast<Mesi>(1 + (x >> 20) % 3), static_cast<std::uint32_t>((x >> 10) % 5));
    }
  }
  const auto total = [&](RewardKind k) { return o.stats.rewards[static_cast<std::size_t>(k)].total(); };
  REQUIRE(o.stats.decisions == p.stats.decisions);
  REQUIRE(total(RewardKind::evict_bad) == p.stats.evict_bad);
  REQUIRE(total(RewardKind::evict_good) == p.stats.evict_good);
  REQUIRE(total(RewardKind::protect_good) == p.stats.protect_good);
  REQUIRE(total(RewardKind::protect_wasted) == p.stats.protect_wasted);
  REQUIRE(p.stats.chose_other > 0);
  for (auto s : {Mesi::S, Mesi::E, Mesi::M}) {
    REQUIRE(p.weights().state_weight(s) >= -1.0);
    REQUIRE(p.weights().state_weight(s) <= 1.0);
  }
}
