// Regression tests for the order of events on a miss (review of Stage 1). A returning evicted block must be looked up
// in, and removed from, the eviction history (with its -1) BEFORE the victim for its fill is chosen and recorded, as in
// ChampSim: CACHE::try_hit calls update_replacement_state on every tag check, misses included (src/cache.cc:398), and
// the victim is chosen later in handle_fill (src/cache.cc:241, 291). rl_mesi applies the -1 in on_access (miss);
// souza_freitas removes the entry in observe_access and again at the fill before inserting the victim.
//
// All scenarios go through rl_set_model.h, i.e. the event sequence the Stage 2 adapter will produce.

#include "rl_paper_policy.h"
#include "rl_reward_policy.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
constexpr Features S_bt{Mesi::S, false}, E_bt{Mesi::E, false}, M_bt{Mesi::M, false};

enum : std::uint64_t { A = 1, B, C, D, X = 50, Y, Z };

std::vector<std::uint64_t> tags_of(const EvictionHistory& h)
{
  std::vector<std::uint64_t> t;
  for (const auto& e : h.entries())
    t.push_back(e.tag);
  return t;
}

// Two ways (history capacity 4): history [A, B, C, D] (oldest first), resident X (oldest way) and Y.
// C and D default to {M, below}.
template <typename Policy>
void prepare(Policy& p, rltest::SetModel<Policy>& m, Features fa, Features fb, Mesi x_state = Mesi::M, Mesi y_state = Mesi::M,
             Features fc = M_bt, Features fd = M_bt)
{
  p.on_eviction(0, A, fa);
  p.on_eviction(0, B, fb);
  p.on_eviction(0, C, fc);
  p.on_eviction(0, D, fd);
  m.fill(0, X, x_state, 1);
  m.fill(0, Y, y_state, 1);
}
} // namespace

TEST_CASE("order (reward): a returning block is removed before the victim enters a full history")
{
  RewardPolicy p{RewardConfig{}, 1, 2};
  rltest::SetModel<RewardPolicy> m{p, 1, 2, 4};
  prepare(p, m, S_bt, E_bt);
  REQUIRE(tags_of(p.history(0)) == (std::vector<std::uint64_t>{A, B, C, D}));

  auto r = m.fill(0, B, Mesi::E, 1); // B misses; replacing it evicts X (oldest, all scores equal)
  REQUIRE(r.evicted_tag == X);
  REQUIRE(tags_of(p.history(0)) == (std::vector<std::uint64_t>{A, C, D, X}));
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE(p.stats.evict_good == 0); // no spurious age-out of A
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), -0.1);
  REQUIRE_APPROX(p.weights().share_weight(false), -0.1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), 0.0); // A's features untouched
}

TEST_CASE("order (reward): a returning block that is the oldest entry still gets its -1, never a +1")
{
  RewardPolicy p{RewardConfig{}, 1, 2};
  rltest::SetModel<RewardPolicy> m{p, 1, 2, 4};
  prepare(p, m, S_bt, E_bt);

  m.fill(0, A, Mesi::S, 1); // A, the oldest entry, misses
  REQUIRE(tags_of(p.history(0)) == (std::vector<std::uint64_t>{B, C, D, X}));
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE(p.stats.evict_good == 0);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), -0.1); // A's eviction was bad
}

TEST_CASE("order (reward): the -1 is applied before the victim for the returning block is chosen")
{
  // As in ChampSim, the weights used to pick the victim already include the returning block's -1.
  // Zero weights; B was evicted as {E, below}; X {E, below} is the oldest way, Y {M, below}.
  RewardPolicy p{RewardConfig{}, 1, 2};
  rltest::SetModel<RewardPolicy> m{p, 1, 2, 4};
  prepare(p, m, S_bt, E_bt, Mesi::E, Mesi::M);

  // After B's -1: X scores wE + wBT = -0.2, Y scores wM + wBT = -0.1, so Y is evicted and X is passed over.
  // (Choosing first would see all-zero scores and evict X.)
  auto r = m.fill(0, B, Mesi::E, 1);
  REQUIRE(r.evicted_tag == Y);
  REQUIRE(p.line(0, 0).is_protected);
}

TEST_CASE("order (reward): a returning block filled into an invalid way gets its -1 without any decision")
{
  RewardPolicy p{RewardConfig{}, 1, 2};
  rltest::SetModel<RewardPolicy> m{p, 1, 2, 4};
  p.on_eviction(0, B, E_bt);
  auto r = m.fill(0, B, Mesi::E, 1);
  REQUIRE_FALSE(r.evicted);
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE(p.history(0).empty());
  REQUIRE(p.stats.decisions == 0);
}

TEST_CASE("order (paper): a returning block is removed before the victim enters a full history")
{
  // History [A:E, B:E, C:M, D:M]; without B, M is the highest state, so candidate #1 X (M) is accepted (text rule)
  PaperPolicy p{{Mode::state, Rule::text, Combine::And}, 1, 2};
  rltest::SetModel<PaperPolicy> m{p, 1, 2, 4};
  prepare(p, m, E_bt, E_bt);

  auto r = m.fill(0, B, Mesi::E, 1);
  REQUIRE(r.evicted_tag == X);
  REQUIRE(tags_of(p.history(0)) == (std::vector<std::uint64_t>{A, C, D, X}));
  REQUIRE(p.stats.negative_rewards == 1);
  REQUIRE(p.stats.history_aged_out == 0);
}

TEST_CASE("order (paper): a returning block that is the oldest entry is still counted as -1")
{
  PaperPolicy p{{Mode::state, Rule::text, Combine::And}, 1, 2};
  rltest::SetModel<PaperPolicy> m{p, 1, 2, 4};
  prepare(p, m, E_bt, E_bt);

  m.fill(0, A, Mesi::E, 1); // without A, M is the highest state: X (M) is evicted
  REQUIRE(tags_of(p.history(0)) == (std::vector<std::uint64_t>{B, C, D, X}));
  REQUIRE(p.stats.negative_rewards == 1);
  REQUIRE(p.stats.history_aged_out == 0);
}

TEST_CASE("order (paper): the ratio weights at decision time no longer contain the returning block")
{
  // History [A:E, B:M, C:M, D:E]. With B still counted, M and E tie (both "highest"); without it, E is highest.
  // Text rule, state mode: X (M) is candidate #1. ChampSim removes B at the tag check, so X is rejected and Y evicted.
  PaperPolicy p{{Mode::state, Rule::text, Combine::And}, 1, 2};
  rltest::SetModel<PaperPolicy> m{p, 1, 2, 4};
  prepare(p, m, E_bt, M_bt, Mesi::M, Mesi::E, M_bt, E_bt);
  REQUIRE_APPROX(compute_ratio_weights(p.history(0)).m, 0.5);

  auto r = m.fill(0, B, Mesi::M, 1);
  REQUIRE(r.evicted_tag == Y);
  REQUIRE(p.stats.second_chances == 1);
  REQUIRE(tags_of(p.history(0)) == (std::vector<std::uint64_t>{A, C, D, Y}));
}

TEST_CASE("order: a fill without a preceding on_miss removes a history entry silently and counts it")
{
  // Never expected in Sniper (every demand fill is reported with on_miss first); the counter makes an adapter that
  // forgets on_miss visible. The entry is removed (a resident block is never in the history) with no feedback,
  // as ChampSim's fill does.
  RewardPolicy r{RewardConfig{}, 1, 2};
  r.on_eviction(0, B, E_bt);
  r.on_fill(0, 0, B, FillKind::demand);
  REQUIRE(r.stats.refill_removals == 1);
  REQUIRE(r.stats.evict_bad == 0);
  REQUIRE(r.history(0).empty());
  REQUIRE_APPROX(r.weights().state_weight(Mesi::E), 0.0);

  PaperPolicy p{{Mode::both, Rule::text, Combine::And}, 1, 2};
  p.on_eviction(0, B, E_bt);
  p.on_fill(0, B);
  REQUIRE(p.stats.refill_removals == 1);
  REQUIRE(p.stats.negative_rewards == 0);
  REQUIRE(p.history(0).empty());
}

TEST_CASE("order: an upgrade refill is not a miss and never consults the history")
{
  RewardPolicy p{RewardConfig{}, 1, 2};
  rltest::SetModel<RewardPolicy> m{p, 1, 2, 4};
  m.fill(0, X, Mesi::S, 1);
  m.fill(0, Y, Mesi::S, 1);
  m.upgrade(0, X);
  REQUIRE(p.stats.evict_bad == 0);
  REQUIRE(p.stats.refill_removals == 0);
  REQUIRE(p.stats.upgrade_carryovers == 1);
}
