#include "bo2zm_native_resolution.h"
#include "bo2zm_native_v040.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace r = bo2zm::native::v041;
namespace v = bo2zm::native::v040;

static int failures = 0;
#define CHECK(c)                                                                   \
  do {                                                                             \
    if (!(c)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);   \
      ++failures;                                                                  \
    }                                                                              \
  } while (0)


constexpr uint32_t kPitch = 880, kRows = 880, kTexW = 896, kTexH = 720;

static void Presets() {
  r::Resolution res;
  CHECK(r::ParsePreset("880x720", res) && res.width == 880 && res.height == 720);
  CHECK(r::ParsePreset("1280x720", res) && res.width == 1280 && res.height == 720);
  CHECK(r::ParsePreset("1920x1080", res) && res.width == 1920 && res.height == 1080);
  CHECK(r::ParsePreset("hd", res) && res.width == 1280);
  CHECK(r::ParsePreset("fullhd", res) && res.width == 1920);
  CHECK(r::ParsePreset("original", res) && res.width == 880);

  r::Resolution keep{123, 456};
  CHECK(!r::ParsePreset("2560x1440", keep) && keep.width == 123 && keep.height == 456);
  CHECK(!r::ParsePreset("", keep));
  CHECK(!r::ParsePreset("1280X720", keep));

  CHECK(r::MakeSceneScale({880, 720}, 100).Identity());
}

static void Sizes() {

  {
    const auto s = r::MakeSceneScale({1280, 720}, 100);
    CHECK(s.x.num == 16 && s.x.den == 11 && s.y.Identity());
    CHECK(r::ScaledSide(kPitch, s.x) == 1280 && r::ScaledSide(kRows, s.y) == 880);
    const auto host = r::SceneHost(s);
    CHECK(host.width == 1280 && host.height == 720);
  }

  {
    const auto s = r::MakeSceneScale({1920, 1080}, 100);
    CHECK(s.x.num == 24 && s.x.den == 11 && s.y.num == 3 && s.y.den == 2);
    CHECK(r::ScaledSide(kPitch, s.x) == 1920 && r::ScaledSide(kRows, s.y) == 1320);
    const auto host = r::SceneHost(s);
    CHECK(host.width == 1920 && host.height == 1080);
  }

  {
    const auto host = r::SceneHost(r::MakeSceneScale({1280, 720}, 75));
    CHECK(host.width == 960 && host.height == 540);
  }

  for (uint32_t pct = 50; pct <= 100; ++pct) {
    const auto s = r::MakeSceneScale({880, 720}, pct);
    for (uint32_t side : {kPitch, kRows, 720u, 800u, 960u}) {
      CHECK(r::ScaledSide(side, s.x) == v::ScaledSide(side, pct));
      CHECK(r::ScaledSide(side, s.y) == v::ScaledSide(side, pct));
    }
  }

  CHECK(r::SceneHost(r::MakeSceneScale({1280, 720}, 10)).width == 640);
  CHECK(r::SceneHost(r::MakeSceneScale({1280, 720}, 400)).width == 1280);


  for (const auto& p : r::kPresets) {
    for (uint32_t pct : {50u, 75u, 100u}) {
      const auto s = r::MakeSceneScale(p.resolution, pct);
      for (uint32_t pitch = 800; pitch <= 960; pitch += 16) {
        const uint32_t w = r::ScaledSide(pitch, s.x), h = r::ScaledSide(pitch, s.y);
        CHECK(w >= 16 && h >= 16 && w <= 8192 && h <= 8192);
      }
    }
  }
}


static void Mapping() {
  for (const auto& p : r::kPresets) {
    for (uint32_t pct = 50; pct <= 100; pct += 5) {
      const auto s = r::MakeSceneScale(p.resolution, pct);
      const uint32_t tw = r::ScaledSide(kPitch, s.x), th = r::ScaledSide(kRows, s.y);
      const uint32_t xw = v::StoredSide(kTexW, tw, kPitch), xh = v::StoredSide(kTexH, th, kRows);
      for (int32_t g = 0; g < 880; g += 8) {
        const int32_t a = v::ToHost(g, tw, kPitch), b = v::ToHost(g + 8, tw, kPitch);
        CHECK(a >= 0 && b >= a && b <= int32_t(tw));
        CHECK(v::ToHost(g, xw, kTexW) <= int32_t(xw));
      }
      for (int32_t g = 0; g < 720; g += 8) {
        const int32_t a = v::ToHost(g, th, kRows), b = v::ToHost(g + 8, th, kRows);
        CHECK(a >= 0 && b >= a && b <= int32_t(th));
        CHECK(v::ToHost(g, xh, kTexH) <= int32_t(xh));
      }
      const int32_t src_w = v::ToHost(880, tw, kPitch), src_h = v::ToHost(720, th, kRows);
      CHECK(src_w <= int32_t(xw) + 1 && src_h <= int32_t(xh) + 1);

      const double content_u = double(src_w) / double(xw), sampled_u = 880.0 / 896.0;
      const double content_v = double(std::min<int32_t>(src_h, int32_t(xh))) / double(xh);
      CHECK(std::fabs(content_u - sampled_u) * xw <= 1.0);
      CHECK(std::fabs(content_v - 1.0) * xh <= 1.0);

      CHECK(v::StoredSide(kTexW, tw, kPitch) == xw && v::StoredSide(kTexH, th, kRows) == xh);
    }
  }
}


static void Framing() {
  for (const auto& p : r::kPresets) {
    const auto s = r::MakeSceneScale(p.resolution, 100);
    const uint32_t tw = r::ScaledSide(kPitch, s.x), th = r::ScaledSide(kRows, s.y);
    const float sx = r::PassScale(tw, kPitch), sy = r::PassScale(th, kRows);

    const float vx = (440.0f - 440.0f) * sx, vw = 2.0f * 440.0f * sx;
    const float vy = (360.0f - 360.0f) * sy, vh = 2.0f * 360.0f * sy;
    CHECK(vx == 0.0f && vy == 0.0f);
    CHECK(std::fabs(vw - float(p.resolution.width)) < 0.01f);
    CHECK(std::fabs(vh - float(p.resolution.height)) < 0.01f);

    CHECK(int32_t(std::ceil(880.0f * sx)) == int32_t(p.resolution.width));
    CHECK(int32_t(std::ceil(720.0f * sy)) == int32_t(p.resolution.height));

    const float gx = (float(p.resolution.width) - 0.5f) / sx, gy = (float(p.resolution.height) - 0.5f) / sy;
    CHECK(gx > 879.0f && gx < 880.0f && gy > 719.0f && gy < 720.0f);

    CHECK(r::PassScale(1280, 0) == 1.0f && r::PassScale(1280, 1280) == 1.0f);
  }

  {
    const auto s = r::MakeSceneScale({1280, 720}, 100);
    CHECK(r::PassScale(r::ScaledSide(kRows, s.y), kRows) == 1.0f);
  }
}

int main() {
  Presets();
  Sizes();
  Mapping();
  Framing();
  if (failures) {
    std::fprintf(stderr, "test_android_v041: %d failures\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("test_android_v041: all checks passed\n");
  return EXIT_SUCCESS;
}
