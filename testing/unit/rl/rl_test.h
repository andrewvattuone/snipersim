#ifndef RL_TEST_H
#define RL_TEST_H

/*
 * Minimal unit-test harness for the RL replacement cores (no external dependency; Sniper has no test framework).
 * The macro names follow Catch2 so the ported ChampSim tests read the same:
 *   TEST_CASE("name") { ... }
 *   REQUIRE(cond)            stop this test case on failure
 *   CHECK(cond)              record the failure and continue
 *   REQUIRE_FALSE / CHECK_FALSE
 *   REQUIRE_APPROX(a, b)     |a - b| <= 1e-9 * max(1, |b|)
 * Exactly one translation unit defines RL_TEST_MAIN to get main(). Run with a substring argument to filter tests.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rltest
{
struct Case {
  const char* name;
  const char* file;
  int line;
  std::function<void()> fn;
};

inline std::vector<Case>& registry()
{
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* name, const char* file, int line, std::function<void()> fn) { registry().push_back({name, file, line, std::move(fn)}); }
};

struct Abort {}; // thrown by a failed REQUIRE

inline int& failures_in_case()
{
  static int n = 0;
  return n;
}
inline long& assertions()
{
  static long n = 0;
  return n;
}

inline void fail(const char* file, int line, const std::string& what)
{
  ++failures_in_case();
  std::printf("    FAILED %s:%d: %s\n", file, line, what.c_str());
}

inline bool approx(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); }

inline std::string approx_message(const char* expr_a, const char* expr_b, double a, double b)
{
  char buf[512];
  std::snprintf(buf, sizeof buf, "%s == Approx(%s): %.12g vs %.12g", expr_a, expr_b, a, b);
  return buf;
}
} // namespace rltest

#define RLTEST_CAT2(a, b) a##b
#define RLTEST_CAT(a, b) RLTEST_CAT2(a, b)
#define TEST_CASE(name)                                                                                                     \
  static void RLTEST_CAT(rltest_fn_, __LINE__)();                                                                           \
  static ::rltest::Registrar RLTEST_CAT(rltest_reg_, __LINE__){name, __FILE__, __LINE__, &RLTEST_CAT(rltest_fn_, __LINE__)}; \
  static void RLTEST_CAT(rltest_fn_, __LINE__)()

#define RLTEST_ASSERT(cond, text, fatal)                                                                                   \
  do {                                                                                                                     \
    ++::rltest::assertions();                                                                                              \
    if (!(cond)) {                                                                                                         \
      ::rltest::fail(__FILE__, __LINE__, text);                                                                            \
      if (fatal)                                                                                                           \
        throw ::rltest::Abort{};                                                                                           \
    }                                                                                                                      \
  } while (0)

#define REQUIRE(cond) RLTEST_ASSERT((cond), #cond, true)
#define CHECK(cond) RLTEST_ASSERT((cond), #cond, false)
#define REQUIRE_FALSE(cond) RLTEST_ASSERT(!(cond), "!(" #cond ")", true)
#define CHECK_FALSE(cond) RLTEST_ASSERT(!(cond), "!(" #cond ")", false)
#define REQUIRE_APPROX(a, b)                                                                                               \
  do {                                                                                                                     \
    const double rltest_a_ = (a), rltest_b_ = (b);                                                                         \
    RLTEST_ASSERT(::rltest::approx(rltest_a_, rltest_b_), ::rltest::approx_message(#a, #b, rltest_a_, rltest_b_), true);   \
  } while (0)

#ifdef RL_TEST_MAIN
int main(int argc, char** argv)
{
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  for (const auto& c : ::rltest::registry()) {
    if (filter && !std::strstr(c.name, filter))
      continue;
    ++run;
    ::rltest::failures_in_case() = 0;
    try {
      c.fn();
    } catch (const ::rltest::Abort&) {
    } catch (const std::exception& e) {
      ::rltest::fail(c.file, c.line, std::string("unexpected exception: ") + e.what());
    }
    const bool ok = ::rltest::failures_in_case() == 0;
    failed += !ok;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", c.name);
  }
  std::printf("\n%d test cases, %d failed, %ld assertions\n", run, failed, ::rltest::assertions());
  return failed == 0 && run > 0 ? 0 : 1;
}
#endif

#endif
