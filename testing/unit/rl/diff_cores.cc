// Differential check of the RL replacement cores against another source tree (make diff-cores OLD=<tree>).
// The same pseudo-random traces (MESI states, sharer hints, hits, coherence and back invalidations, upgrades) run
// through the cores of each tree via rl_set_model.h. Each line is one configuration: an order-sensitive hash of the
// victim sequence, the core statistics, the final weights to 17 digits and the diagnostics. Identical output means
// identical decisions, rewards and diagnostics for every configuration listed. The configurations use only options
// both trees have, so a tree that adds options can be compared with one that predates them.
#include <cstdio>
#include "rl_diagnostics.h"
#include "rl_set_model.h"
using namespace rlrepl;

static std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 0x100000001b3ULL + 0x9e3779b97f4a7c15ULL; }

template <typename Policy, typename Obs>
static std::uint64_t drive(Policy& p, Obs* o, std::uint32_t sets, std::uint64_t seed, int n)
{
  rltest::SetModel<Policy> m{p, sets, 8, 4};
  m.observer = o;
  std::uint64_t x = seed, h = 1469598103934665603ULL;
  for (int i = 0; i < n; ++i) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto set = static_cast<std::uint32_t>((x >> 33) % sets);
    const std::uint64_t tag = (x >> 40) % 20;
    const auto state = static_cast<Mesi>(1 + (x >> 20) % 3);
    const auto hint = static_cast<std::uint32_t>((x >> 10) % 5);
    const auto op = (x >> 50) % 16;
    if (auto way = m.find(set, tag)) {
      if (op == 0) m.invalidate(set, *way, InvalidationReason::coherence);
      else if (op == 1) m.invalidate(set, *way, InvalidationReason::back_invalidation);
      else if (op == 2 && m.blocks[set][*way].state != Mesi::M) m.upgrade(set, tag);
      else m.hit(set, *way);
    } else if (auto r = m.fill(set, tag, state, hint); r.evicted) {
      h = mix(h, r.evicted_tag * 64 + set * 8 + r.way);
    }
  }
  return h;
}

int main()
{
  const Mode modes[] = {Mode::both, Mode::state, Mode::bitset, Mode::off};
  for (Mode mode : modes)
    for (std::uint32_t window : {1u, 2u, 4u, 8u})
      for (double alpha : {0.0, 0.1, 0.5})
        for (bool prot : {true, false})
          for (WeightScope sc : {WeightScope::global, WeightScope::per_set})
            for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
              RewardConfig c; c.mode = mode; c.window = window; c.alpha = alpha; c.protect_feedback = prot; c.scope = sc;
              RewardPolicy p{c, 16, 8};
              RewardObserver o{16, 8, sc};
              const auto h = drive(p, &o, 16, seed, 40000);
              const auto& s = p.stats; const auto& d = o.stats;
              std::printf("rl mode=%d N=%u a=%.1f prot=%d scope=%d seed=%llu hash=%016llx dec=%llu other=%llu bad=%llu good=%llu pg=%llu pw=%llu passes=%llu maxp=%u w=%.17g,%.17g,%.17g,%.17g,%.17g | diag dec=%llu tied=%llu tdist=%llu small=%llu fs=%llu fh=%llu pos0=%llu\n",
                (int)mode, window, alpha, (int)prot, (int)sc, (unsigned long long)seed, (unsigned long long)h,
                (unsigned long long)s.decisions, (unsigned long long)s.chose_other, (unsigned long long)s.evict_bad, (unsigned long long)s.evict_good,
                (unsigned long long)s.protect_good, (unsigned long long)s.protect_wasted, (unsigned long long)s.passes, s.max_passes,
                p.weights().state_weight(Mesi::S), p.weights().state_weight(Mesi::E), p.weights().state_weight(Mesi::M), p.weights().share_weight(false), p.weights().share_weight(true),
                (unsigned long long)d.decisions, (unsigned long long)d.all_tied, (unsigned long long)d.tied_distinct, (unsigned long long)d.small_margin,
                (unsigned long long)d.flips_state, (unsigned long long)d.flips_sharing, (unsigned long long)d.victim_position[0]);
            }
  for (Rule rule : {Rule::text, Rule::example})
    for (Combine cb : {Combine::And, Combine::Or})
      for (Mode mode : modes)
        for (std::uint64_t seed : {1ULL, 2ULL}) {
          PaperConfig c; c.mode = mode; c.rule = rule; c.combine = cb;
          PaperPolicy p{c, 16, 8};
          PaperObserver o{16, 8};
          const auto h = drive(p, &o, 16, seed, 40000);
          std::printf("sf rule=%d combine=%d mode=%d seed=%llu hash=%016llx\n", (int)rule, (int)cb, (int)mode, (unsigned long long)seed, (unsigned long long)h);
        }
}
