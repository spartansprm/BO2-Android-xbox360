// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_native_edram.h"

#include <cstdio>
#include <cstdlib>

namespace e = bo2zm::native::edram;
static int g_failures = 0;
#define CHECK(c)                                                                  \
  do {                                                                            \
    if (!(c)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)


static bool Frame(e::Tracker& t, uint64_t depth, bool read_pass1_result = false) {
  t.Clear(depth);
  const bool d0 = t.ShouldDiscard(depth);
  t.PassEnded(depth, d0);
  CHECK(t.Read(depth));
  const bool d1 = t.ShouldDiscard(depth);
  t.PassEnded(depth, d1);
  CHECK(!d0);
  if (read_pass1_result) {
    t.Read(depth);
  }
  return d1;
}

int main() {
  e::g_enabled.store(true);
  e::g_off.store(false);
  {
    e::Tracker t;
    const uint64_t depth = 0x1000;
    for (uint32_t i = 0; i < e::kLearnDead; ++i) {
      CHECK(!Frame(t, depth));
    }
    CHECK(Frame(t, depth));
    CHECK(Frame(t, depth));

    e::g_enabled.store(false);
    CHECK(!Frame(t, depth));
    e::g_enabled.store(true);
    CHECK(Frame(t, depth));

    t.Clear(depth);
    t.PassEnded(depth, false);
    CHECK(t.Read(depth));
    CHECK(t.ShouldDiscard(depth));
    t.PassEnded(depth, true);
    CHECK(!t.Read(depth));
    CHECK(e::g_off.load());
    CHECK(!Frame(t, depth));
  }
  e::g_off.store(false);
  {

    e::Tracker t;
    const uint64_t color = 0x2000;
    for (uint32_t i = 0; i < e::kLearnDead - 1; ++i) CHECK(!Frame(t, color));
    CHECK(!Frame(t, color,                       true));
    for (uint32_t i = 0; i < e::kLearnDead; ++i) CHECK(!Frame(t, color));
    CHECK(Frame(t, color));

    t.Clear(color);
    t.PassEnded(color, false);
    t.Touch(color);
    CHECK(t.Read(color));

    t.PassEnded(color, true);
    t.Forget(color);
    CHECK(t.Read(color));
    CHECK(!e::g_off.load());
  }
  {

    e::Tracker t;
    CHECK(t.Read(0x3000));
    CHECK(!t.ShouldDiscard(0x3000));
  }
  CHECK(e::BytesPerTexel(37) == 4 && e::BytesPerTexel(129) == 4 && e::BytesPerTexel(97) == 8);
  if (g_failures) {
    std::fprintf(stderr, "test_android_v047: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("test_android_v047: all checks passed\n");
  return EXIT_SUCCESS;
}
