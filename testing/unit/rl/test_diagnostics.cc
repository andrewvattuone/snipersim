// Stage 4 diagnostics (rl_diagnostics.h): the observers count what the cores do without changing it.

#include "rl_diagnostics.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
RewardConfig rcfg(Mode m, std::uint32_t window = 4, double alpha = 0.5, bool protect = true,
                  WeightScope scope = WeightScope::global)
{
  RewardConfig c;
  c.mode = m;
  c.window = window;
  c.alpha = alpha;
  c.protect_feedback = protect;
  c.scope = scope;
  return c;
}

std::uint64_t sum(const std::array<std::uint64_t, 3>& a) { return a[0] + a[1] + a[2]; }

// A pseudo-random mix of misses, hits, coherence / back invalidations and upgrades, through the cache model.
// Returns the victim sequence (tag * sets + set) so runs with and without an observer can be compared.
template <typename Policy>
std::vector<std::uint64_t> random_trace(rltest::SetModel<Policy>& m, std::uint32_t sets, std::uint64_t seed, int n = 20000)
{
  std::vector<std::uint64_t> victims;
  std::uint64_t x = seed;
  for (int i = 0; i < n; ++i) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto set = static_cast<std::uint32_t>((x >> 33) % sets);
    const std::uint64_t tag = (x >> 40) % 14;
    const auto state = static_cast<Mesi>(1 + (x >> 20) % 3);
    const auto hint = static_cast<std::uint32_t>((x >> 10) % 5);
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
    } else if (auto r = m.fill(set, tag, state, hint); r.evicted) {
      victims.push_back(r.evicted_tag * sets + set);
    }
  }
  return victims;
}
} // namespace

TEST_CASE("diagnostics: run-length buckets are 1, 2, 3-4, 5-8, 9-16, 17-32, 33+")
{
  const std::vector<std::pair<std::uint32_t, std::size_t>> expect{
      {1, 0}, {2, 1}, {3, 2}, {4, 2}, {5, 3}, {8, 3}, {9, 4}, {16, 4}, {17, 5}, {32, 5}, {33, 6}, {1000000, 6}};
  for (const auto& [n, b] : expect)
    CHECK(run_bucket(n) == b);
}

TEST_CASE("diagnostics: the reward observer changes no decision and its totals match the core")
{
  const std::vector<RewardConfig> configs{rcfg(Mode::both, 4, 0.1), rcfg(Mode::both, 2, 0.5), rcfg(Mode::state, 4, 0.1, false),
                                          rcfg(Mode::bitset, 3, 0.1), rcfg(Mode::both, 4, 0.1, true, WeightScope::per_set),
                                          rcfg(Mode::off)};
  for (const auto& c : configs) {
    RewardPolicy plain{c, 4, 4};
    rltest::SetModel<RewardPolicy> m_plain{plain, 4, 4, 4};
    const auto expected = random_trace(m_plain, 4, 99);

    RewardPolicy p{c, 4, 4};
    RewardObserver o{4, 4, c.scope};
    rltest::SetModel<RewardPolicy> m{p, 4, 4, 4};
    m.observer = &o;
    REQUIRE(random_trace(m, 4, 99) == expected);
    REQUIRE(expected.size() > 1000);

    const auto& r = o.stats.rewards;
    const auto kind = [&](RewardKind k) { return r[static_cast<std::size_t>(k)]; };
    CHECK(kind(RewardKind::evict_bad).total() == p.stats.evict_bad);
    CHECK(kind(RewardKind::evict_good).total() == p.stats.evict_good);
    CHECK(kind(RewardKind::protect_good).total() == p.stats.protect_good);
    CHECK(kind(RewardKind::protect_wasted).total() == p.stats.protect_wasted);
    for (const auto& k : r)
      CHECK(sum(k.state) == k.total()); // no block without state information in this trace
    CHECK(o.stats.decisions == p.stats.decisions);
    std::uint64_t positions = 0;
    for (auto v : o.stats.victim_position)
      positions += v;
    CHECK(positions == p.stats.decisions);
    CHECK(o.stats.victim_position[0] == p.stats.chose_oldest);
    CHECK(o.runs().ended_passes + o.runs().open_passes() == p.stats.passes);
    CHECK(o.stats.double_agree + o.stats.double_contradict <= p.stats.protect_wasted);
    CHECK(o.stats.double_dropped == 0);
    CHECK(o.stats.tied_distinct <= o.stats.all_tied);
    CHECK(o.stats.tied_distinct <= o.stats.small_margin);
    if (c.mode == Mode::off) {
      CHECK(o.stats.decisions == 0);
      CHECK(p.stats.passes == 0);
    } else {
      CHECK(p.stats.passes > 0);
      CHECK(p.stats.evict_bad > 0);
    }
    if (c.protect_feedback && c.mode != Mode::off) {
      CHECK(p.stats.protect_good > 0);
      CHECK(p.stats.protect_wasted > 0);
    }
  }
}

TEST_CASE("diagnostics: a dead block passed over at every miss forms one long run, ended by its eviction (I-1)")
{
  WeightTable w;
  w.set_state_weight(Mesi::M, -0.5); // M blocks are kept
  RewardPolicy p{rcfg(Mode::state, 4, 0.0), 1, 4, w}; // alpha 0: the weights stay fixed
  RewardObserver o{1, 4, WeightScope::global};
  rltest::SetModel<RewardPolicy> m{p, 1, 4, 4};
  m.observer = &o;
  m.fill(0, 100, Mesi::M, 1); // way 0, oldest, never used again
  m.fill(0, 101, Mesi::E, 1);
  m.fill(0, 102, Mesi::E, 1);
  m.fill(0, 103, Mesi::E, 1);
  for (std::uint64_t tag = 200; tag < 210; ++tag)
    m.fill(0, tag, Mesi::E, 1);
  REQUIRE(o.runs().run(0, 0) == 10);
  REQUIRE(p.line(0, 0).passes == 10);
  // Decision k sees way 0 with a run of k - 1: pinned (>= 2) from decision 3, long-pinned (>= 8) at decisions 9 and 10
  REQUIRE(o.stats.pinned_short == 8);
  REQUIRE(o.stats.pinned_long == 2);
  // Once the other three ways are M too, way 0 is the oldest of four equal scores and is finally evicted
  for (std::uint64_t tag = 300; tag < 303; ++tag)
    m.fill(0, tag, Mesi::M, 1);
  const auto r = m.fill(0, 400, Mesi::M, 1);
  REQUIRE(r.evicted_tag == 100);
  const auto& evicted = o.runs().histogram[static_cast<std::size_t>(RunEnd::evicted)];
  CHECK(evicted[run_bucket(13)] == 1);
  CHECK(o.runs().open_passes() == 0);
  CHECK(o.runs().ended_passes == p.stats.passes);
}

TEST_CASE("diagnostics: a protected block that is hit ends its run with a hit and is classed by its snapshot")
{
  WeightTable w;
  w.set_state_weight(Mesi::S, -0.5);
  RewardPolicy p{rcfg(Mode::both, 4, 0.5), 1, 4, w};
  RewardObserver o{1, 4, WeightScope::global};
  rltest::SetModel<RewardPolicy> m{p, 1, 4, 4};
  m.observer = &o;
  m.fill(0, 1, Mesi::S, 4); // way 0: S, above the threshold (4 of 4 cores)
  m.fill(0, 2, Mesi::E, 1);
  m.fill(0, 3, Mesi::E, 1);
  m.fill(0, 4, Mesi::E, 1);
  m.fill(0, 5, Mesi::E, 1); // decision: way 0 passed over, way 1 evicted
  REQUIRE(p.line(0, 0).is_protected);
  m.blocks[0][0].state = Mesi::M; // its current state no longer matters: the snapshot is rewarded
  m.hit(0, 0);
  REQUIRE(p.stats.protect_good == 1);
  const auto& good = o.stats.rewards[static_cast<std::size_t>(RewardKind::protect_good)];
  CHECK(good.state[0] == 1); // S
  CHECK(good.share[1] == 1); // AT
  CHECK(o.runs().histogram[static_cast<std::size_t>(RunEnd::hit)][0] == 1);
  CHECK(o.runs().run(0, 0) == 0);
}

TEST_CASE("diagnostics: double rewards of protected blocks agree when the entry ages out, contradict when it returns (I-6)")
{
  auto setup = [](RewardPolicy& p, RewardObserver& o, rltest::SetModel<RewardPolicy>& m) {
    m.observer = &o;
    // Way 0 (S) is passed over once, then evicted while protected: protect-wasted (+1)
    m.fill(0, 1, Mesi::S, 1);
    m.fill(0, 2, Mesi::E, 1);
    m.fill(0, 3, Mesi::E, 1);
    m.fill(0, 4, Mesi::E, 1);
    m.fill(0, 5, Mesi::E, 1); // way 0 passed over (S scores lower), way 1 evicted
    for (std::uint32_t way = 0; way < 4; ++way)
      if (way != 0)
        m.blocks[0][way].state = Mesi::S; // now all S: ties go to the oldest, way 0
    m.fill(0, 6, Mesi::E, 1);
    REQUIRE(p.stats.protect_wasted == 1);
    REQUIRE(p.history(0).contains(1));
  };
  {
    WeightTable w;
    w.set_state_weight(Mesi::S, -0.5);
    RewardPolicy p{rcfg(Mode::state, 4, 0.0), 1, 4, w};
    RewardObserver o{1, 4, WeightScope::global};
    rltest::SetModel<RewardPolicy> m{p, 1, 4, 4};
    setup(p, o, m);
    m.fill(0, 1, Mesi::E, 1); // returns while in the history: -1
    CHECK(o.stats.double_contradict == 1);
    CHECK(o.stats.double_agree == 0);
  }
  {
    WeightTable w;
    w.set_state_weight(Mesi::S, -0.5);
    RewardPolicy p{rcfg(Mode::state, 4, 0.0), 1, 4, w};
    RewardObserver o{1, 4, WeightScope::global};
    rltest::SetModel<RewardPolicy> m{p, 1, 4, 4};
    setup(p, o, m);
    for (std::uint64_t tag = 10; tag < 30 && p.history(0).contains(1); ++tag)
      m.fill(0, tag, Mesi::E, 1); // eight more evictions push entry 1 out: +1
    CHECK(!p.history(0).contains(1));
    CHECK(o.stats.double_agree == 1);
    CHECK(o.stats.double_contradict == 0);
  }
}

TEST_CASE("diagnostics: ties among different features are counted, and weight order changes are flips (I-4)")
{
  RewardPolicy p{rcfg(Mode::state, 4, 0.5), 1, 4};
  RewardObserver o{1, 4, WeightScope::global};
  rltest::SetModel<RewardPolicy> m{p, 1, 4, 4};
  m.observer = &o;
  m.fill(0, 1, Mesi::S, 1);
  m.fill(0, 2, Mesi::E, 1);
  m.fill(0, 3, Mesi::M, 1);
  m.fill(0, 4, Mesi::E, 1);
  m.fill(0, 5, Mesi::E, 1); // all weights 0: a tie between S, E, M
  CHECK(o.stats.all_tied == 1);
  CHECK(o.stats.tied_distinct == 1);
  CHECK(o.stats.small_margin == 1);
  CHECK(o.stats.flips_state == 0); // first observation of the table
  m.fill(0, 1, Mesi::S, 1);        // S was evicted and returns: wS = -0.5, the order changes
  CHECK(p.weights().state_weight(Mesi::S) < 0);
  CHECK(o.stats.flips_state == 1);
  CHECK(o.stats.flips_sharing == 0); // sharing group not used in mode state
}

TEST_CASE("diagnostics: the paper observer changes no decision; rejection runs and class counts match the core")
{
  for (Rule rule : {Rule::text, Rule::example})
    for (Combine combine : {Combine::And, Combine::Or}) {
      PaperConfig c;
      c.rule = rule;
      c.combine = combine;
      PaperPolicy plain{c, 4, 4};
      rltest::SetModel<PaperPolicy> m_plain{plain, 4, 4, 4};
      const auto expected = random_trace(m_plain, 4, 7);

      PaperPolicy p{c, 4, 4};
      PaperObserver o{4, 4};
      rltest::SetModel<PaperPolicy> m{p, 4, 4, 4};
      m.observer = &o;
      REQUIRE(random_trace(m, 4, 7) == expected);
      CHECK(o.stats.history_hits.total() == p.stats.negative_rewards);
      CHECK(o.stats.aged_out.total() == p.stats.history_aged_out);
      CHECK(sum(o.stats.history_hits.state) == o.stats.history_hits.total());
      CHECK(o.stats.decisions == p.stats.decisions);
      CHECK(o.runs().ended_passes + o.runs().open_passes() == p.stats.second_chances);
      CHECK(p.stats.second_chances > 0);
    }
}

TEST_CASE("diagnostics: a candidate #1 rejected at every miss forms one run (paper, I-1)")
{
  PaperConfig c;
  c.mode = Mode::bitset; // empty-history bias and then the ratios keep the AT block
  c.rule = Rule::text;
  PaperPolicy p{c, 1, 4};
  PaperObserver o{1, 4};
  rltest::SetModel<PaperPolicy> m{p, 1, 4, 4};
  m.observer = &o;
  m.fill(0, 1, Mesi::E, 4); // way 0, AT: rejected while BT dominates the history
  m.fill(0, 2, Mesi::E, 1);
  m.fill(0, 3, Mesi::E, 1);
  m.fill(0, 4, Mesi::E, 1);
  for (std::uint64_t tag = 10; tag < 16; ++tag)
    m.fill(0, tag, Mesi::E, 1);
  REQUIRE(p.stats.second_chances == 6);
  CHECK(o.runs().run(0, 0) == 6);
  m.hit(0, 0);
  CHECK(o.runs().histogram[static_cast<std::size_t>(RunEnd::hit)][run_bucket(6)] == 1);
}
