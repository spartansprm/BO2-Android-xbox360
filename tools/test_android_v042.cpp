#include "bo2zm_native_display.h"
#include "bo2zm_native_v040.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace v = bo2zm::native::v042;
namespace r = bo2zm::native::v041;
namespace o = bo2zm::native::v040;

static int failures = 0;
#define CHECK(c)                                                                   \
  do {                                                                             \
    if (!(c)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);   \
      ++failures;                                                                  \
    }                                                                              \
  } while (0)

static const v::Display kUltrawideDisplay = v::Landscape(1080, 2340);

static void Parsing() {
  v::Output out = v::Output::kAuto;
  CHECK(v::ParseOutput("1280x720", out) && out == v::Output::k720);
  CHECK(v::ParseOutput("1920x1080", out) && out == v::Output::k1080);
  CHECK(v::ParseOutput("auto", out) && out == v::Output::kAuto);
  CHECK(!v::ParseOutput("4k", out) && out == v::Output::kAuto);
  v::Screen s = v::Screen::kStretch;
  CHECK(v::ParseScreen("original", s) && s == v::Screen::kOriginal);
  CHECK(v::ParseScreen("stretch", s) && s == v::Screen::kStretch);
  CHECK(v::ParseScreen("ultrawide", s) && s == v::Screen::kUltrawide);
  CHECK(!v::ParseScreen("21:9", s) && s == v::Screen::kUltrawide);
  CHECK(kUltrawideDisplay.width == 2340 && kUltrawideDisplay.height == 1080);
  CHECK(!v::Landscape(0, 1080).Known());
}

static void SceneSizes() {

  const r::Resolution fhd{1920, 1080}, hd{1280, 720}, orig{880, 720};
  for (v::Screen s : {v::Screen::kOriginal, v::Screen::kStretch}) {
    for (const auto p : {fhd, hd, orig}) {
      const auto out = v::SceneSize(p, s, kUltrawideDisplay);
      CHECK(out.width == p.width && out.height == p.height);
    }
  }
  const auto wfhd = v::SceneSize(fhd, v::Screen::kUltrawide, kUltrawideDisplay);
  CHECK(wfhd.width == 2340 && wfhd.height == 1080);
  const auto whd = v::SceneSize(hd, v::Screen::kUltrawide, kUltrawideDisplay);
  CHECK(whd.width == 1560 && whd.height == 720);
  const auto worig = v::SceneSize(orig, v::Screen::kUltrawide, kUltrawideDisplay);
  CHECK(worig.width == 1074 && worig.height == 720);

  const auto comp = v::OutputSize(v::Output::k1080, v::Screen::kUltrawide, kUltrawideDisplay);
  CHECK(wfhd.width * comp.height == comp.width * wfhd.height);

  const auto sc = r::MakeSceneScale(wfhd, 100);
  CHECK(r::ScaledSide(880, sc.x) == 2340 && r::ScaledSide(720, sc.y) == 1080);
  CHECK(r::SceneHost(sc).width == 2340 && r::SceneHost(sc).height == 1080);

  CHECK(v::SceneSize(fhd, v::Screen::kUltrawide, v::Landscape(1920, 1080)).width == 1920);
  CHECK(v::SceneSize(fhd, v::Screen::kUltrawide, v::Display{}).width == 1920);
}

static void OutputSizes() {
  for (v::Screen s : {v::Screen::kOriginal, v::Screen::kStretch}) {

    const auto o720 = v::OutputSize(v::Output::k720, s, kUltrawideDisplay);
    CHECK(o720.width == 1280 && o720.height == 720);
    CHECK(v::OutputScale(o720).Identity());

    const auto o1080 = v::OutputSize(v::Output::k1080, s, kUltrawideDisplay);
    CHECK(o1080.width == 1920 && o1080.height == 1080);
    const auto sc = v::OutputScale(o1080);
    CHECK(sc.x.num == 3 && sc.x.den == 2 && sc.y.num == 3 && sc.y.den == 2);
  }

  {
    const auto u720 = v::OutputSize(v::Output::k720, v::Screen::kUltrawide, kUltrawideDisplay);
    CHECK(u720.width == 1560 && u720.height == 720);
    const auto u1080 = v::OutputSize(v::Output::k1080, v::Screen::kUltrawide, kUltrawideDisplay);
    CHECK(u1080.width == 2340 && u1080.height == 1080);

    const auto flat = v::OutputSize(v::Output::k1080, v::Screen::kUltrawide, v::Landscape(1920, 1080));
    CHECK(flat.width == 1920 && flat.height == 1080);
    const auto none = v::OutputSize(v::Output::k720, v::Screen::kUltrawide, v::Display{});
    CHECK(none.width == 1280 && none.height == 720);
  }

  const auto a_orig = v::OutputSize(v::Output::kAuto, v::Screen::kOriginal, kUltrawideDisplay);
  CHECK(a_orig.width == 1920 && a_orig.height == 1080);
  const auto a_wide = v::OutputSize(v::Output::kAuto, v::Screen::kUltrawide, kUltrawideDisplay);
  CHECK(a_wide.width == 2340 && a_wide.height == 1080);
  const auto a_str = v::OutputSize(v::Output::kAuto, v::Screen::kStretch, kUltrawideDisplay);
  CHECK(a_str.width == 2340 && a_str.height == 1080);

  const auto a720 = v::OutputSize(v::Output::kAuto, v::Screen::kOriginal, v::Landscape(1600, 720));
  CHECK(a720.width == 1280 && a720.height == 720);
  const auto a1440 = v::OutputSize(v::Output::kAuto, v::Screen::kUltrawide, v::Landscape(3200, 1440));
  CHECK(a1440.height == 1440 && a1440.width == 3200);
  const auto a4k = v::OutputSize(v::Output::kAuto, v::Screen::kOriginal, v::Landscape(3840, 2160));
  CHECK(a4k.height == 1440 && a4k.width == 2560);
  const auto unknown = v::OutputSize(v::Output::kAuto, v::Screen::kUltrawide, v::Display{});
  CHECK(unknown.width == 1920 && unknown.height == 1080);

  for (const auto out : {a_orig, a_wide, v::OutputSize(v::Output::k1080, v::Screen::kOriginal, kUltrawideDisplay)}) {
    const auto s = v::OutputScale(out);
    const uint32_t tw = r::ScaledSide(1280, s.x), th = r::ScaledSide(1280, s.y);
    CHECK(tw == out.width);
    CHECK(th == out.height * 1280 / 720);

    const uint32_t fw = o::StoredSide(1280, tw, 1280), fh = o::StoredSide(720, th, 1280);
    CHECK(fw == out.width && fh == out.height);

    CHECK(uint32_t(o::ToHost(1280, fw, 1280)) == fw && uint32_t(o::ToHost(720, fh, 720)) == fh);

    for (int32_t g = 0; g < 1280; g += 8) {
      CHECK(o::ToHost(g + 8, tw, 1280) >= o::ToHost(g, tw, 1280));
      CHECK(o::ToHost(g + 8, tw, 1280) <= int32_t(tw));
    }


    const auto scene = r::MakeSceneScale({1920, 1080}, 100);
    const double scene_cols = double(r::ScaledSide(880, scene.x));
    const double composite_cols = double(tw);
    CHECK(composite_cols / scene_cols >= 0.999);
  }
}

static void Presenter() {

  const auto orig = v::PresentAspectFor(v::Screen::kOriginal, kUltrawideDisplay, true);
  CHECK(orig.x == 1280 && orig.y == 720);
  const auto str = v::PresentAspectFor(v::Screen::kStretch, kUltrawideDisplay, false);
  CHECK(str.x == 2340 && str.y == 1080);
  const auto uw_no = v::PresentAspectFor(v::Screen::kUltrawide, kUltrawideDisplay, false);
  CHECK(uw_no.x == 1280 && uw_no.y == 720);
  const auto uw_yes = v::PresentAspectFor(v::Screen::kUltrawide, kUltrawideDisplay, true);
  CHECK(uw_yes.x == 2340 && uw_yes.y == 1080);
  CHECK(v::PresentAspectFor(v::Screen::kStretch, v::Display{}, true).x == 1280);

  CHECK(std::fabs(v::UltrawideAspect(kUltrawideDisplay) - 2340.0 / 1080.0) < 1e-9);
  CHECK(std::fabs(v::UltrawideFactor(kUltrawideDisplay) - 1.21875) < 1e-9);
  CHECK(v::UltrawideFactor(v::Landscape(1920, 1080)) == 1.0);
  CHECK(v::UltrawideFactor(v::Landscape(1920, 1200)) == 1.0);
  CHECK(v::UltrawideAspect(v::Landscape(10000, 1080)) <= 32.0 / 9.0 + 1e-9);
}

static void VidConfig() {

  std::array<float, 32> w{};
  for (size_t i = 0; i < w.size(); ++i) w[i] = float(i) * 3.25f + 100.0f;
  w[10] = 16.0f / 9.0f;
  w[11] = 16.0f / 11.0f;
  w[12] = 1.0f;
  CHECK(v::FindAspectTriple(w.data(), int(w.size())) == 10);

  auto two = w;
  two[20] = 16.0f / 9.0f;
  two[21] = 16.0f / 11.0f;
  two[22] = 1.0f;
  CHECK(v::FindAspectTriple(two.data(), int(two.size())) == -1);

  auto none = w;
  none[11] = 1.7727f;
  CHECK(v::FindAspectTriple(none.data(), int(none.size())) == -1);

  const auto a = v::AspectFor(1.21875);
  CHECK(std::fabs(a.window - 2340.0f / 1080.0f) < 1e-5f);
  CHECK(std::fabs(a.scene_pixel - float(16.0 / 11.0 * 1.21875)) < 1e-5f);
  CHECK(std::fabs(a.display_pixel - 1.21875f) < 1e-6f);
  const auto b = v::AspectFor(1.0);
  CHECK(b.window == float(16.0 / 9.0) && b.display_pixel == 1.0f);

  auto patched = w;
  patched[10] = a.window;
  patched[11] = a.scene_pixel;
  patched[12] = a.display_pixel;
  CHECK(v::FindAspectTriple(patched.data(), int(patched.size())) == -1);
}


static std::array<float, 31> FullRecord() {
  std::array<float, 31> p{};
  p[0] = p[1] = 1.5f;
  p[2] = p[3] = 1.5f;
  p[4] = p[5] = 1.0f / 1.5f;
  p[6] = 0.0f;   p[7] = 0.0f;
  p[8] = 853.333f; p[9] = 480.0f;
  p[10] = 64.0f; p[11] = 36.0f;
  p[12] = 789.333f; p[13] = 444.0f;
  p[14] = 0; p[15] = 0;
  p[16] = 1280; p[17] = 720;
  p[18] = 640; p[19] = 360;
  p[20] = 0; p[21] = 0; p[22] = 1280; p[23] = 720;
  p[24] = 96; p[25] = 54; p[26] = 1184; p[27] = 666;
  p[28] = 1; p[29] = 1; p[30] = 1;
  return p;
}

static void Placements() {
  const double f = v::UltrawideFactor(kUltrawideDisplay);
  auto p = FullRecord();
  const auto original = p;
  CHECK(std::fabs(v::PlacementFactor(p.data()) - 1.0) < 1e-6);
  CHECK(v::AdjustPlacement(p.data(), f, f));
  CHECK(std::fabs(v::PlacementFactor(p.data()) - f) < 1e-5);

  for (int i = 14; i < 31; ++i) CHECK(p[i] == original[i]);

  for (int i : {1, 3, 5, 7, 9, 11, 13}) CHECK(p[i] == original[i]);

  CHECK(std::fabs(double(p[0]) * f - double(p[1])) < 1e-5);

  CHECK(std::fabs(double(p[8]) / double(original[8]) - f) < 1e-4);

  const double W = 100.0;
  CHECK(std::fabs(W * p[0] * f - W * original[0]) < 1e-3);

  auto again = p;
  CHECK(v::AdjustPlacement(again.data(), f, f));
  for (int i = 0; i < 31; ++i) CHECK(std::fabs(again[i] - p[i]) <= 1e-6f * std::max(1.0f, std::fabs(p[i])));
  CHECK(v::AdjustPlacement(again.data(), 1.0, f));
  for (int i = 0; i < 31; ++i) CHECK(std::fabs(again[i] - original[i]) <= 2e-4f * std::max(1.0f, std::fabs(original[i])));

  std::array<float, 31> zero{};
  CHECK(!v::AdjustPlacement(zero.data(), f, f));
  for (float x : zero) CHECK(x == 0.0f);

  auto odd = FullRecord();
  odd[0] = 1.2f;
  const auto odd_before = odd;
  CHECK(!v::AdjustPlacement(odd.data(), f, f));
  CHECK(!v::AdjustPlacement(odd.data(), 1.0, f));
  for (int i = 0; i < 31; ++i) CHECK(odd[i] == odd_before[i]);
}

static void FieldOfView() {

  const float ty = float(std::tan(65.0 * 3.14159265358979 / 360.0) * 0.75);
  const float tx16 = float(double(ty) * 16.0 / 9.0);
  const auto f16 = v::FovFromTangents(tx16, ty);
  CHECK(std::fabs(f16.aspect - 16.0 / 9.0) < 1e-5);
  CHECK(f16.horizontal_deg > 80.0 && f16.horizontal_deg < 82.0);

  const float txw = float(double(ty) * 2340.0 / 1080.0);
  const auto fw = v::FovFromTangents(txw, ty);
  CHECK(v::AspectMatches(fw.aspect, 2340.0 / 1080.0));
  CHECK(!v::AspectMatches(fw.aspect, 16.0 / 9.0));
  CHECK(fw.horizontal_deg > 91.0 && fw.horizontal_deg < 93.0);
  CHECK(std::fabs(fw.vertical_deg - f16.vertical_deg) < 1e-6);

  std::array<float, 10> f{0.0f, 1.0f, 0.5f, tx16, ty, 512.0f, -3.0f, 0.0f, 0.0f, 0.0f};
  CHECK(v::FindTangentPair(f.data(), 10, 16.0 / 9.0, 2340.0 / 1080.0) == 3);
  f[3] = txw;
  CHECK(v::FindTangentPair(f.data(), 10, 16.0 / 9.0, 2340.0 / 1080.0) == 3);
  std::array<float, 10> nothing{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  CHECK(v::FindTangentPair(nothing.data(), 10, 16.0 / 9.0, 2.1667) == -1);
  CHECK(!v::AspectMatches(0.0, 1.0) && v::FovFromTangents(0.0f, 1.0f).aspect == 0.0);
}

int main() {
  Parsing();
  OutputSizes();
  SceneSizes();
  Presenter();
  VidConfig();
  Placements();
  FieldOfView();
  if (failures) {
    std::fprintf(stderr, "test_android_v042: %d failures\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("test_android_v042: all checks passed\n");
  return EXIT_SUCCESS;
}
