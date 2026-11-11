// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_native_v055.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <vector>

namespace v = bo2zm::native::v055;
static int g_failures = 0;
#define CHECK(c)                                                                  \
  do {                                                                            \
    if (!(c)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)


struct Model {
  std::unordered_map<uint64_t, v::ReadOnlyLearn> learn;
  bool active = false, read_only = false, opened_nonwriting = false, written = false, blocked = false;
  uint64_t key = 0, learn_key = 0;
  uint64_t passes = 0, read_only_passes = 0, misses = 0, writes_in_read_only = 0;

  void Finish() {
    if (!active) return;
    if (opened_nonwriting) {
      v::ReadOnlyLearn& l = learn[learn_key];
      if (read_only) {
        if (blocked) {
          ++misses;
          v::Missed(l);
        } else {
          v::PassClosed(l, false);
          ++read_only_passes;
        }
      } else {
        v::PassClosed(l, written);
      }
    }
    active = read_only = opened_nonwriting = false;
  }


  void Draw(uint64_t pass_key, uint32_t vs, uint32_t ps, uint32_t control, bool cleared = false) {
    const bool writes = v::WritesDepthStencil(control);
    const uint64_t lk = v::LearnKey(pass_key, vs, ps);
    if (active && read_only && writes && pass_key == key) {
      blocked = true;
      Finish();
    }
    if (!active || pass_key != key) {
      Finish();
      active = true;
      ++passes;
      key = pass_key;
      learn_key = lk;

      opened_nonwriting = (control & 0x3u) != 0 && !writes && !cleared;
      written = cleared;
      blocked = false;
      read_only = false;
      if (opened_nonwriting) {
        const auto it = learn.find(lk);
        read_only = it != learn.end() && v::OpenReadOnly(it->second);
      }
    }
    if (writes) {
      written = true;
      if (read_only) ++writes_in_read_only;
    }
  }
};

int main() {

  CHECK(v::UsefulRows(720, true) == 720);
  CHECK(v::UsefulRows(720, false) == 768);
  CHECK(v::UsefulRows(1080, true) == 1088);
  CHECK(v::UsefulRows(1080, false) == 1088);
  CHECK(v::UsefulRows(0, true) == 0 && v::UsefulRows(1, true) == 16 && v::UsefulRows(16, true) == 16);
  CHECK(v::UsefulRows(1024, true) == 1024 && v::UsefulRows(1024, false) == 1024);
  for (uint32_t b = 0; b <= 4096; ++b) {
    CHECK(v::UsefulRows(b, true) >= b && v::UsefulRows(b, true) < b + 16 && v::UsefulRows(b, true) % 16 == 0);
    CHECK(v::UsefulRows(b, true) <= v::UsefulRows(b, false));
  }


  CHECK(v::ClearsWholeAttachment(v::kAspectColor, true, false));
  CHECK(!v::ClearsWholeAttachment(v::kAspectDepth, true, false));
  CHECK(v::ClearsWholeAttachment(v::kAspectDepth | v::kAspectStencil, false, true));
  CHECK(!v::ClearsWholeAttachment(v::kAspectDepth, false, true));
  CHECK(!v::ClearsWholeAttachment(v::kAspectStencil, false, true));
  CHECK(v::ClearsWholeAttachment(v::kAspectDepth, false, false));
  CHECK(!v::ClearsWholeAttachment(v::kAspectDepth | v::kAspectStencil, false, false));
  CHECK(!v::ClearsWholeAttachment(0, false, true) && !v::ClearsWholeAttachment(0, true, true));


  CHECK(!v::WritesDepthStencil(0x0));
  CHECK(!v::WritesDepthStencil(0x2));
  CHECK(!v::WritesDepthStencil(0x2 | 0x70));
  CHECK(v::WritesDepthStencil(0x6));
  CHECK(!v::WritesDepthStencil(0x4));
  CHECK(v::WritesDepthStencil(0x1));
  CHECK(v::WritesDepthStencil(0x3) && v::WritesDepthStencil(0x7));

  CHECK((v::kLoadDepthStoreNone & (0x3u | (0x1Fu << 4) | (0x3u << 12) | (0x1Fu << 16))) == 0);
  CHECK(v::kStoreOpNone == 1000301000);


  {
    v::ReadOnlyLearn l;
    for (uint32_t i = 0; i + 1 < v::kLearnClean; ++i) {
      v::PassClosed(l, false);
      CHECK(!v::OpenReadOnly(l));
    }
    v::PassClosed(l, false);
    CHECK(v::OpenReadOnly(l));
    v::PassClosed(l, true);
    CHECK(!v::OpenReadOnly(l) && l.clean == 0);
    for (uint32_t m = 0; m < v::kMaxMisses; ++m) {
      for (uint32_t i = 0; i < v::kLearnClean; ++i) v::PassClosed(l, false);
      CHECK(v::OpenReadOnly(l));
      const bool banned = v::Missed(l);
      CHECK(banned == (m + 1 == v::kMaxMisses));
      CHECK(!v::OpenReadOnly(l));
    }
    for (uint32_t i = 0; i < 10 * v::kLearnClean; ++i) v::PassClosed(l, false);
    CHECK(!v::OpenReadOnly(l));
  }
  CHECK(v::LearnKey(1, 2, 3) != v::LearnKey(1, 3, 2) && v::LearnKey(1, 2, 3) != v::LearnKey(2, 2, 3));
  CHECK(v::LearnKey(1, 2, 0) != v::LearnKey(1, 2, 1));


  {
    Model m;
    const uint64_t scene = 0x1111, composite = 0x2222;
    for (uint32_t frame = 0; frame < 2 * v::kLearnClean; ++frame) {
      m.Draw(scene, 10, 20, 0x6, true);
      m.Draw(scene, 11, 21, 0x6);
      m.Finish();
      m.Draw(scene, 30, 40, 0x2);
      m.Draw(scene, 31, 41, 0x2);
      m.Draw(composite, 50, 60, 0x0);
    }
    m.Finish();
    CHECK(m.writes_in_read_only == 0 && m.misses == 0);
    CHECK(m.read_only_passes == v::kLearnClean);
  }

  {
    Model m;
    for (uint32_t frame = 0; frame < 4 * v::kLearnClean; ++frame) {
      m.Draw(0x1111, 30, 40, 0x2);
      m.Draw(0x1111, 70, 80, 0x6);
      m.Finish();
    }
    CHECK(m.read_only_passes == 0 && m.misses == 0 && m.writes_in_read_only == 0);
  }

  {
    Model m;
    for (uint32_t frame = 0; frame < v::kLearnClean; ++frame) {
      m.Draw(0x1111, 30, 40, 0x2);
      m.Finish();
    }
    const uint64_t before = m.passes;
    for (uint32_t frame = 0; frame < 5000; ++frame) {
      m.Draw(0x1111, 30, 40, 0x2);
      m.Draw(0x1111, 70, 80, 0x7);
      m.Finish();
    }
    CHECK(m.writes_in_read_only == 0);
    CHECK(m.misses <= v::kMaxMisses);
    CHECK(m.passes - before == 5000 + m.misses);
  }

  {
    std::mt19937 rng(55);
    Model m;
    for (uint32_t i = 0; i < 400000; ++i) {
      const uint32_t x = rng();
      const uint64_t key = 1 + (x & 3);
      const uint32_t shader = (x >> 2) & 7;

      const uint32_t control = ((x >> 8) % 97 == 0) ? 0x6 : ((x >> 8) % 211 == 0) ? 0x1 : 0x2;
      if (((x >> 20) & 31) == 0) m.Finish();
      m.Draw(key, shader, shader + 1, control, ((x >> 26) & 63) == 0);
    }
    m.Finish();
    CHECK(m.writes_in_read_only == 0);
    CHECK(m.read_only_passes > 0);
    for (const auto& [k, l] : m.learn) {
      CHECK(l.misses <= v::kMaxMisses);
    }
  }

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::puts("test_android_v055: ok");
  return EXIT_SUCCESS;
}
