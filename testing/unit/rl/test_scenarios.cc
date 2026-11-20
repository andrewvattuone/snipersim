// Deterministic end-to-end scenarios driven through the Sniper-order cache model (rl_set_model.h). The numbers are
// worked out by hand in docs/rl/POLICY_CORES.md ("Worked scenario").

#include "rl_paper_policy.h"
#include "rl_reward_policy.h"
#include "rl_set_model.h"
#include "rl_test.h"

using namespace rlrepl;

namespace
{
constexpr Features S_bt{Mesi::S, false}, S_at{Mesi::S, true};
constexpr Features E_bt{Mesi::E, false}, E_at{Mesi::E, true};
constexpr Features M_bt{Mesi::M, false};

// Tags of the scenario blocks
enum : std::uint64_t { A = 1, B, C, D, P, Q, R, T, Y, Z };
} // namespace

TEST_CASE("scenario: rewards from decisions change later victim selections (modified RL, N=4, alpha=0.1)")
{
  // 4 cores (heavy iff >= 3 sharers), one 8-way set, history capacity 16, default configuration
  WeightTable w0;
  w0.set_state_weight(Mesi::S, -0.30);
  w0.set_state_weight(Mesi::E, 0.20);
  w0.set_state_weight(Mesi::M, -0.10);
  w0.set_share_weight(false, 0.10);
  w0.set_share_weight(true, -0.40);
  RewardPolicy p{RewardConfig{}, 1, 8, w0};
  rltest::SetModel<RewardPolicy> m{p, 1, 8, 4};

  // History holds 16 earlier evictions, oldest first: X = 900 {S, below}, W = 901 {E, below}, 902..915 {M, below}
  p.on_eviction(0, 900, S_bt);
  p.on_eviction(0, 901, E_bt);
  for (std::uint64_t t = 902; t < 916; ++t)
    p.on_eviction(0, t, M_bt);
  REQUIRE(p.history(0).size() == 16);
  REQUIRE(p.stats.evict_good == 0);

  // Fill the set: ways 0..7, way 0 oldest. A is M, B is S shared by 3 cores, C is E, D is S shared by 2, P..T are M
  m.fill(0, A, Mesi::M, 1);
  m.fill(0, B, Mesi::S, 3);
  m.fill(0, C, Mesi::E, 1);
  m.fill(0, D, Mesi::S, 2);
  for (auto t : {P, Q, R, T})
    m.fill(0, t, Mesi::M, 1);
  REQUIRE(m.lru_order(0) == (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7}));

  // Decision 1 (miss on Y). Window A 0.00, B -0.70, C 0.30, D -0.20: C is evicted; LRU would evict A.
  auto r1 = m.fill(0, Y, Mesi::E, 1);
  REQUIRE(r1.evicted_tag == C);
  REQUIRE(p.stats.chose_other == 1);
  REQUIRE(p.line(0, 0).is_protected); // A
  REQUIRE(p.line(0, 0).snapshot == M_bt);
  REQUIRE(p.line(0, 1).is_protected); // B
  REQUIRE(p.line(0, 1).snapshot == S_at);
  REQUIRE_FALSE(p.line(0, 3).is_protected); // D is younger than the victim
  // C enters the history; X ages out: +1 to {S, below}
  REQUIRE(p.history(0).contains(C));
  REQUIRE_FALSE(p.history(0).contains(900));
  REQUIRE(p.stats.evict_good == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), -0.17);
  REQUIRE_APPROX(p.weights().share_weight(false), 0.19);

  // B is hit while protected: -1 to {S, above}
  m.hit_tag(0, B);
  REQUIRE(p.stats.protect_good == 1);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), -0.253);
  REQUIRE_APPROX(p.weights().share_weight(true), -0.46);

  // Decision 2 (C misses again). The victim is chosen before C's fill is seen: A 0.09, D -0.063, P 0.09, Q 0.09,
  // tie to the oldest, A. A was protected and never hit: +1 to {M, below}. A enters the history and W ages out: +1 to
  // {E, below}. Then C's fill finds C in the history: -1 to {E, below}.
  REQUIRE(m.lru_order(0).front() == 0);
  auto r2 = m.fill(0, C, Mesi::E, 1);
  REQUIRE(r2.evicted_tag == A);
  REQUIRE(p.stats.chose_oldest == 1);
  REQUIRE(p.stats.protect_wasted == 1);
  REQUIRE(p.stats.evict_good == 2);
  REQUIRE(p.stats.evict_bad == 1);
  REQUIRE_FALSE(p.history(0).contains(C));
  REQUIRE(p.history(0).contains(A));
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), 0.01);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::E), 0.152);
  REQUIRE_APPROX(p.weights().share_weight(false), 0.20951);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::S), -0.253);
  REQUIRE_APPROX(p.weights().share_weight(true), -0.46);

  // Decision 3 (miss on Z). LRU would evict D; the learned weights now prefer M: D -0.04349, P/Q/R 0.21951.
  REQUIRE(m.blocks[0][m.lru_order(0).front()].tag == D);
  auto r3 = m.fill(0, Z, Mesi::E, 1);
  REQUIRE(r3.evicted_tag == P);
  REQUIRE(p.line(0, 3).is_protected); // D protected as {S, below}
  REQUIRE(p.line(0, 3).snapshot == S_bt);
  // P enters the history. C's return removed its entry, so the history had a free slot: nothing ages out and the
  // weights do not change at this decision.
  REQUIRE(p.history(0).size() == 16);
  REQUIRE(p.history(0).entries().back().tag == P);
  REQUIRE(p.stats.evict_good == 2);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), 0.01);
  REQUIRE_APPROX(p.weights().share_weight(false), 0.20951);
  REQUIRE(p.stats.decisions == 3);

  // Decision 4 (miss on 11): the next eviction does push out entry 902 {M, below}: +1
  auto r4 = m.fill(0, 11, Mesi::E, 1);
  REQUIRE(r4.evicted_tag == Q); // D -0.04349 is passed over again; Q (M) 0.21951
  REQUIRE(p.line(0, 3).passes == 2);
  REQUIRE(p.stats.evict_good == 3);
  REQUIRE_APPROX(p.weights().state_weight(Mesi::M), 0.109);
  REQUIRE_APPROX(p.weights().share_weight(false), 0.288559);
}

TEST_CASE("scenario: the paper's rules on one set (history M3 E9 S4, AT2 BT14)")
{
  // Candidate #1 is A {M, below}, candidate #2 is B {S, above}. Weights: wM 3/16, wE 9/16, wS 4/16, wAT 2/16.
  struct Variant {
    Mode mode;
    Rule rule;
    Combine combine;
    std::uint64_t expected_victim;
  };
  const Variant variants[] = {
      {Mode::state, Rule::text, Combine::And, B},     // M is not the highest: keep A, evict B unchecked
      {Mode::state, Rule::example, Combine::And, A},  // M is not the highest: evict A
      {Mode::both, Rule::text, Combine::And, B},      // state says keep
      {Mode::both, Rule::example, Combine::And, B},   // A's class (below) is the highest: keep (ChampSim AND)
      {Mode::both, Rule::example, Combine::Or, A},    // state differs from the highest: evict (figure read literally)
      {Mode::bitset, Rule::text, Combine::And, A},    // below is the highest: evict
      {Mode::bitset, Rule::example, Combine::And, B}, // below is the highest: keep
  };
  for (const auto& v : variants) {
    PaperPolicy p{{v.mode, v.rule, v.combine}, 1, 8};
    std::uint64_t t = 1000;
    for (int i = 0; i < 3; ++i)
      p.on_eviction(0, t++, M_bt);
    p.on_eviction(0, t++, E_at);
    p.on_eviction(0, t++, E_at);
    for (int i = 0; i < 7; ++i)
      p.on_eviction(0, t++, E_bt);
    for (int i = 0; i < 4; ++i)
      p.on_eviction(0, t++, S_bt);
    REQUIRE(p.history(0).size() == 16);

    rltest::SetModel<PaperPolicy> m{p, 1, 8, 4};
    m.fill(0, A, Mesi::M, 1);
    m.fill(0, B, Mesi::S, 3);
    for (auto tag : {C, D, P, Q, R, T})
      m.fill(0, tag, Mesi::E, 1);

    auto r = m.fill(0, Y, Mesi::E, 1);
    CHECK(r.evicted_tag == v.expected_victim);
    if (r.evicted_tag == B) {
      CHECK(p.stats.second_chances == 1);
      CHECK(m.lru_order(0).front() == 0); // A kept at the LRU position
    }
    // The evicted block's decision-time features are now the newest history entry; the oldest aged out
    CHECK(p.history(0).entries().back().tag == r.evicted_tag);
    CHECK(p.stats.history_aged_out == 1);
  }

  // The weights the check used
  PaperPolicy p{{Mode::both, Rule::text, Combine::And}, 1, 8};
  std::uint64_t t = 1;
  for (int i = 0; i < 3; ++i)
    p.on_eviction(0, t++, M_bt);
  p.on_eviction(0, t++, E_at);
  p.on_eviction(0, t++, E_at);
  for (int i = 0; i < 7; ++i)
    p.on_eviction(0, t++, E_bt);
  for (int i = 0; i < 4; ++i)
    p.on_eviction(0, t++, S_bt);
  auto w = compute_ratio_weights(p.history(0));
  REQUIRE_APPROX(w.m, 3.0 / 16);
  REQUIRE_APPROX(w.e, 9.0 / 16);
  REQUIRE_APPROX(w.s, 4.0 / 16);
  REQUIRE_APPROX(w.at, 2.0 / 16);
  REQUIRE_APPROX(w.bt, 14.0 / 16);
}

TEST_CASE("scenario: an upgrade through the cache model keeps protection and creates no history entry")
{
  WeightTable w;
  w.set_state_weight(Mesi::E, 0.5);
  RewardPolicy p{RewardConfig{}, 1, 4, w};
  rltest::SetModel<RewardPolicy> m{p, 1, 4, 4};
  m.fill(0, 100, Mesi::S, 1);
  m.fill(0, 101, Mesi::E, 1);
  m.fill(0, 102, Mesi::S, 1);
  m.fill(0, 103, Mesi::S, 1);
  m.fill(0, 104, Mesi::S, 1); // evicts 101 (E); 100 passed over and protected
  REQUIRE(p.line(0, 0).is_protected);
  const auto history_before = p.history(0).size();

  // The core writes block 100 (S): Sniper invalidates its copy and refills it as M
  auto r = m.upgrade(0, 100);
  REQUIRE_FALSE(r.evicted);
  REQUIRE(r.way == 0);
  REQUIRE(m.blocks[0][0].state == Mesi::M);
  REQUIRE(p.line(0, 0).is_protected);
  REQUIRE(p.line(0, 0).snapshot == S_bt);
  REQUIRE(p.history(0).size() == history_before);
  REQUIRE(p.stats.upgrade_carryovers == 1);
  REQUIRE(m.lru_order(0).back() == 0); // the refill makes it MRU, as Sniper's re-insert does

  // The next hit on it is the protection's success
  m.hit_tag(0, 100);
  REQUIRE(p.stats.protect_good == 1);
}

TEST_CASE("scenario: N=1 and zero weights reproduce plain LRU victims on a long access sequence")
{
  // The same pseudo-random sequence through LRU (mode off), N=1 and N=4 with weights frozen at 0 (alpha 0):
  // all must evict exactly the same blocks.
  auto run = [](RewardConfig c) {
    RewardPolicy p{c, 4, 4};
    rltest::SetModel<RewardPolicy> m{p, 4, 4, 4};
    std::vector<std::uint64_t> evicted;
    std::uint64_t x = 12345;
    for (int i = 0; i < 5000; ++i) {
      x = x * 6364136223846793005ULL + 1442695040888963407ULL;
      const std::uint32_t set = static_cast<std::uint32_t>((x >> 33) % 4);
      const std::uint64_t tag = (x >> 40) % 12;
      const auto st = static_cast<Mesi>(1 + (x >> 20) % 3);
      if (auto way = m.find(set, tag))
        m.hit(set, *way);
      else if (auto r = m.fill(set, tag, st, static_cast<std::uint32_t>((x >> 10) % 5)); r.evicted)
        evicted.push_back(r.evicted_tag * 4 + set);
    }
    return evicted;
  };
  RewardConfig off;
  off.mode = Mode::off;
  RewardConfig n1;
  n1.window = 1;
  RewardConfig frozen;
  frozen.alpha = 0.0;
  const auto lru = run(off);
  REQUIRE(lru.size() > 1000);
  REQUIRE(run(n1) == lru);
  REQUIRE(run(frozen) == lru);
}
