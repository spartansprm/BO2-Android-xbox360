// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_native_v043.h"

#include <cstdio>
#include <cstdlib>
#include <random>

namespace v = bo2zm::native::v043;

static int g_failures = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

static v::RectRequest Req(uint32_t aspects, int32_t x, int32_t y, uint32_t w, uint32_t h) {
  v::RectRequest r;
  r.active = true;
  r.aspects = aspects;
  r.area = {x, y, w, h};
  return r;
}

int main() {
  constexpr uint32_t kC = v::kAspectColor, kD = v::kAspectDepth, kS = v::kAspectStencil;

  CHECK(v::AspectsFitSlot(kC, 0) && v::AspectsFitSlot(kC, 3));
  CHECK(!v::AspectsFitSlot(kD, 0) && !v::AspectsFitSlot(kC | kD, 0) && !v::AspectsFitSlot(0, 0));
  CHECK(v::AspectsFitSlot(kD, 4) && v::AspectsFitSlot(kS, 4) && v::AspectsFitSlot(kD | kS, 4));
  CHECK(!v::AspectsFitSlot(kC, 4) && !v::AspectsFitSlot(kC | kD, 4));
  CHECK(v::FullAspects(0) == kC && v::FullAspects(4) == (kD | kS));

  CHECK(v::RectInside({0, 0, 880, 720}, 880, 768));
  CHECK(!v::RectInside({0, 0, 880, 769}, 880, 768));
  CHECK(!v::RectInside({-1, 0, 10, 10}, 880, 768));
  CHECK(!v::RectInside({0, 0, 0, 10}, 880, 768));
  CHECK(!v::RectInside({0x7FFFFFFF, 0, 0xFFFFFFFFu, 1}, 880, 768));
  CHECK(v::RectWhole({0, 0, 880, 768}, 880, 768) && !v::RectWhole({0, 0, 880, 720}, 880, 768));
  CHECK(v::WholeImageClear({0, 0, 64, 64}, kD | kS, 4, 64, 64));
  CHECK(!v::WholeImageClear({0, 0, 64, 64}, kD, 4, 64, 64));
  CHECK(!v::WholeImageClear({0, 0, 64, 32}, kC, 0, 64, 64));

  {
    const uint32_t formats[5] = {0, 37, 0, 44, 129};
    CHECK(v::ColorAttachmentIndex(formats, 1) == 0);
    CHECK(v::ColorAttachmentIndex(formats, 3) == 1);
    const uint32_t all[5] = {1, 1, 1, 1, 1};
    CHECK(v::ColorAttachmentIndex(all, 2) == 2);
  }

  {
    std::array<v::RectRequest, 5> r{};
    r[0] = Req(kC, 0, 0, 880, 720);
    r[4] = Req(kD | kS, 0, 0, 880, 720);
    const v::PassPlan p = v::PlanRectClears(880, 768, 720, false, r);
    CHECK(p.area_height == 720);
    CHECK(p.fuse[0] && p.fuse[4] && !p.in_pass[0] && !p.in_pass[4]);
  }

  {
    std::array<v::RectRequest, 5> r{};
    r[4] = Req(kD, 0, 0, 880, 720);
    const v::PassPlan p = v::PlanRectClears(880, 768, 720, false, r);
    CHECK(p.area_height == 720 && !p.fuse[4] && p.in_pass[4]);
  }

  {
    std::array<v::RectRequest, 5> r{};
    r[0] = Req(kC, 0, 0, 880, 720);
    const v::PassPlan p = v::PlanRectClears(880, 768, 768, false, r);
    CHECK(p.area_height == 768 && !p.fuse[0] && p.in_pass[0]);
  }

  {
    std::array<v::RectRequest, 5> r{};
    r[0] = Req(kC, 0, 0, 880, 768);
    r[4] = Req(kD | kS, 0, 0, 880, 600);
    const v::PassPlan p = v::PlanRectClears(880, 768, 720, false, r);
    CHECK(p.area_height == 768 && p.fuse[0] && !p.fuse[4] && p.in_pass[4]);
  }

  {
    std::array<v::RectRequest, 5> r{};
    r[1] = Req(kC, 0, 0, 880, 720);
    const v::PassPlan p = v::PlanRectClears(880, 768, 720, true, r);
    CHECK(p.area_height == 768 && !p.fuse[1] && p.in_pass[1]);
  }

  {
    std::array<v::RectRequest, 5> r{};
    r[0] = Req(kC, 0, 360, 640, 360);
    const v::PassPlan p = v::PlanRectClears(1280, 720, 720, false, r);
    CHECK(p.area_height == 720 && !p.fuse[0] && p.in_pass[0]);
  }

  {
    std::array<v::RectRequest, 5> r{};
    CHECK(v::PlanRectClears(880, 768, 720, false, r).area_height == 720);
    CHECK(v::PlanRectClears(880, 768, 900, false, r).area_height == 768);
    CHECK(v::PlanRectClears(880, 768, 0, false, r).area_height == 1);
  }


  {
    std::mt19937 rng(43);
    for (int iter = 0; iter < 200000; ++iter) {
      const uint32_t width = 1 + rng() % 2048, height = 1 + rng() % 2048;
      const uint32_t useful = 1 + rng() % height;
      const bool whole = (rng() & 7) == 0;
      std::array<v::RectRequest, 5> r{};
      for (uint32_t i = 0; i < 5; ++i) {
        if (rng() & 1) {
          continue;
        }
        const uint32_t x = (rng() & 3) ? 0 : rng() % width, y = (rng() & 3) ? 0 : rng() % height;
        const uint32_t w = (rng() & 1) ? width - x : 1 + rng() % (width - x);
        uint32_t h = 1 + rng() % (height - y);
        if ((rng() & 3) == 0) h = std::min(height - y, useful);
        const uint32_t aspects = i == 4 ? ((rng() & 1) ? (kD | kS) : (1 + rng() % 2) * 2) : kC;
        r[i] = Req(aspects, int32_t(x), int32_t(y), w, h);
      }
      const v::PassPlan p = v::PlanRectClears(width, height, useful, whole, r);
      CHECK(p.area_height >= (whole ? height : useful) && p.area_height <= height);
      for (uint32_t i = 0; i < 5; ++i) {
        CHECK(!(p.fuse[i] && p.in_pass[i]));
        if (!r[i].active) {
          CHECK(!p.fuse[i] && !p.in_pass[i]);
          continue;
        }
        CHECK(p.fuse[i] || p.in_pass[i]);
        if (p.fuse[i]) {
          CHECK(r[i].area.x == 0 && r[i].area.y == 0 && r[i].area.width == width &&
                r[i].area.height == p.area_height && r[i].aspects == v::FullAspects(i));
        }
        if (p.in_pass[i]) {
          CHECK(v::RectInside(r[i].area, width, p.area_height));
        }
      }
      if (g_failures > 20) break;
    }
  }
  if (g_failures) {
    std::fprintf(stderr, "test_android_v043: %d failures\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("test_android_v043: all checks passed\n");
  return EXIT_SUCCESS;
}
