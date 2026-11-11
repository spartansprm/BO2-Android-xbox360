// SPDX-License-Identifier: GPL-3.0-only


#include "android_thread_placement.h"

#include <cstdio>
#include <cstdlib>

namespace p = rex::thread::placement;
static int g_failures = 0;
#define CHECK(c)                                                                  \
  do {                                                                            \
    if (!(c)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)

int main() {

  CHECK(p::TruncateThreadName("GPU native recorder") == "GPU native reco");
  CHECK(p::TruncateThreadName("BO2 reports") == "BO2 reports");
  CHECK(p::TruncateThreadName("Presenter Paint") == "Presenter Paint");
  CHECK(p::TruncateThreadName("").empty());

  {
    const std::string t = p::TruncateThreadName("12345678901234\xC3\xB1xyz");
    CHECK(t == "12345678901234");
  }

  const auto L = p::kLittleCoreThreads;
  CHECK(p::MatchesList("Sound Mix (F8000034)", L));
  CHECK(p::MatchesList("XMA Decoder (F8000010)", L));
  CHECK(p::MatchesList("Audio Worker (F8000014)", L));
  CHECK(p::MatchesList("BO2 pipeline compiles 2", L));
  CHECK(p::MatchesList("BO2 reports", L));
  CHECK(!p::MatchesList("Stream (F8000058)", L));
  CHECK(!p::MatchesList("BO2 pipeline precompile", L));
  CHECK(!p::MatchesList("BO2 pipeline prewarm 2", L));
  CHECK(!p::MatchesList("BO2 vertex copies", L));
  CHECK(!p::MatchesList("BO2 texture binds", L));
  CHECK(!p::MatchesList("GPU native recorder", L));
  CHECK(!p::MatchesList("GPU native ring", L));
  CHECK(!p::MatchesList("Presenter Paint", L));
  CHECK(!p::MatchesList("Main XThread (F8000004)", L));
  CHECK(!p::MatchesList("Server (F8000190)", L));
  CHECK(!p::MatchesList("JQ Worker 1 (Pri 0)", L));
  CHECK(!p::MatchesList("Sound", L));
  CHECK(p::MatchesList("abc", " x , abc "));
  CHECK(!p::MatchesList("abc", ",,"));
  CHECK(!p::MatchesList("abc", ""));

  std::vector<p::Cpu> cluster;
  for (uint32_t i = 0; i < 8; ++i) cluster.push_back({i, i < 4 ? 427u : 1024u});
  CHECK(p::LittleMask(cluster, 0xFF) == 0x0F);
  CHECK(p::LittleMask(cluster, 0xF0) == 0);
  CHECK(p::LittleMask(cluster, 0x3C) == 0x0C);

  std::vector<p::Cpu> tri;
  for (uint32_t i = 0; i < 8; ++i) tri.push_back({i, i < 4 ? 400u : (i < 7 ? 800u : 1024u)});
  CHECK(p::LittleMask(tri, 0xFF) == 0x0F);

  std::vector<p::Cpu> same(8);
  for (uint32_t i = 0; i < 8; ++i) same[i] = {i, 1024};
  CHECK(p::LittleMask(same, 0xFF) == 0);
  cluster[5].capacity = 0;
  CHECK(p::LittleMask(cluster, 0xFF) == 0);
  CHECK(p::LittleMask({}, 0xFF) == 0);

  CHECK(p::Decide("Sound Mix (F8000034)", L, 0x0F, 0xFF) == p::Action::kLittle);
  CHECK(p::Decide("Sound Mix (F8000034)", L, 0x0F, 0x0F) == p::Action::kNone);
  CHECK(p::Decide("Server (F8000190)", L, 0x0F, 0xFF) == p::Action::kNone);
  CHECK(p::Decide("Server (F8000190)", L, 0x0F, 0x0F) == p::Action::kRestore);
  CHECK(p::Decide("Sound Mix (F8000034)", L, 0, 0xFF) == p::Action::kNone);
  if (g_failures) {
    std::fprintf(stderr, "test_android_v044: %d failures\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("test_android_v044: all checks passed\n");
  return EXIT_SUCCESS;
}
