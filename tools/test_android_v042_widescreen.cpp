#include <sys/mman.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "bo2zm_widescreen.cpp"


static std::map<std::string, std::string> g_flags;
std::string rex::cvar::GetFlagByName(std::string_view name) {
  const auto it = g_flags.find(std::string(name));
  return it == g_flags.end() ? std::string() : it->second;
}

static int failures = 0;
#define CHECK(c)                                                                   \
  do {                                                                             \
    if (!(c)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);   \
      ++failures;                                                                  \
    }                                                                              \
  } while (0)

namespace {
uint8_t* base = nullptr;
void W32(uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
void WF(uint32_t a, float f) { W32(a, bo2zm::display::BitsOf(f)); }
float RF(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return bo2zm::display::Bits(__builtin_bswap32(v));
}
constexpr uint32_t kCg = 0x50000000;
constexpr uint32_t kTangents = kCg + 0x4D898;
const float kTanY = float(std::tan(65.0 * 3.14159265358979 / 360.0) * 0.75);

void SetupGuest() {
  base = static_cast<uint8_t*>(mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (base == MAP_FAILED) {
    std::perror("mmap");
    std::exit(2);
  }

  W32(0x82C153E0, 880);
  W32(0x82C153E4, 720);
  W32(0x82C153E8, 1280);
  W32(0x82C153EC, 720);
  W32(0x82C153F0, 60);
  W32(0x82C153F4, 1);
  WF(0x82C153F8, 16.0f / 9.0f);
  WF(0x82C153FC, 16.0f / 11.0f);
  WF(0x82C15400, 1.0f);

  const float full[31] = {1.5f, 1.5f, 1.5f, 1.5f, 1 / 1.5f, 1 / 1.5f, 0, 0, 853.333f, 480, 64, 36, 789.333f, 444,
                          0, 0, 1280, 720, 640, 360, 0, 0, 1280, 720, 96, 54, 1184, 666, 1, 1, 1};
  for (int i = 0; i < 31; ++i) WF(0x82CBC360 + 4 * i, full[i]);
  for (int i = 0; i < 31; ++i) WF(0x82CBC168 + 4 * i, full[i]);

  W32(0x82C70F44, 1);
  W32(0x82BBAE68, kCg);
  W32(kCg + 0x4D880, 0);
  W32(kCg + 0x4D884, 0);
  W32(kCg + 0x4D888, 1280);
  W32(kCg + 0x4D88C, 720);
  WF(kCg + 0x4D890, 0.0f);
  WF(kCg + 0x4D894, 512.0f);
  WF(kTangents, kTanY * 16.0f / 9.0f);
  WF(kTangents + 4, kTanY);
}

using U = bo2zm::display::Ultrawide;
U State() { return U(bo2zm::display::g_ultrawide.load()); }
double FullFactor() { return double(RF(0x82CBC364)) / double(RF(0x82CBC360)); }


void EngineFrame(bool follows) {
  const float window = RF(0x82C153F8);
  const float aspect = follows ? window : 16.0f / 9.0f;
  WF(kTangents, kTanY * aspect);
  WF(kTangents + 4, kTanY);
}
}

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "follow";
  g_flags["bo2zm_native_screen_mode"] = scenario == "off" ? "original" : "ultrawide";
  g_flags["bo2zm_native_output_resolution"] = "auto";
  g_flags["bo2zm_native_display_width"] = "1080";
  g_flags["bo2zm_native_display_height"] = "2340";
  SetupGuest();
  namespace ws = bo2zm::widescreen;
  const double factor = 2340.0 / 1080.0 / (16.0 / 9.0);


  for (int i = 0; i < 50; ++i) ws::PollBoot(base);
  if (scenario == "off") {
    CHECK(State() == U::kOff);
    CHECK(RF(0x82C153F8) == 16.0f / 9.0f && RF(0x82C15400) == 1.0f);
    for (int frame = 0; frame < 100; ++frame) ws::OnViewFov(base, 0);
    CHECK(std::fabs(FullFactor() - 1.0) < 1e-6);
    CHECK(ws::Placement(base, 0x82CBC360) == 0x82CBC360 && std::fabs(FullFactor() - 1.0) < 1e-6);

    CHECK(bo2zm::display::g_refdef_tangent_offset.load() == 0x4D898);
    CHECK(bo2zm::display::g_fov_samples.load() > 0);
    const auto fov = bo2zm::native::v042::FovFromTangents(bo2zm::display::Bits(bo2zm::display::g_tan_x_bits.load()),
                                                          bo2zm::display::Bits(bo2zm::display::g_tan_y_bits.load()));
    CHECK(std::fabs(fov.aspect - 16.0 / 9.0) < 1e-4);
  } else {
    CHECK(State() == U::kApplied);
    CHECK(bo2zm::display::g_vidconfig_address.load() == 0x82C153F8);
    CHECK(std::fabs(RF(0x82C153F8) - 2340.0f / 1080.0f) < 1e-5f);
    CHECK(std::fabs(RF(0x82C15400) - float(factor)) < 1e-6f);

    CHECK(ws::Placement(base, 0x82CBC360) == 0x82CBC360);
    CHECK(std::fabs(FullFactor() - factor) < 1e-5);

    WF(0x82CBC360, 1.5f);
    WF(0x82CBC364, 1.5f);
    ws::Placement(base, 0x82CBC360);
    CHECK(std::fabs(FullFactor() - factor) < 1e-5);
    if (scenario == "menus") {
      const float sixteen_nine[4] = {-592.0f, -333.0f, 592.0f, 333.0f};
      for (int i = 0; i < 3; ++i) ws::OnMenuSafeArea(base, sixteen_nine);
      CHECK(State() == U::kFallback);
      CHECK(std::string(bo2zm::display::g_ultrawide_reason.load()) == "menus_stay_16x9");
    } else {

      const bool follows = scenario == "follow";
      for (int frame = 0; frame < 200 && State() == U::kApplied; ++frame) {
        EngineFrame(follows);
        ws::OnViewFov(base, 0);
      }
      if (follows) {
        CHECK(State() == U::kConfirmed);
        CHECK(bo2zm::display::UltrawideConfirmed());

        WF(0x82C153F8, 16.0f / 9.0f);
        ws::PollBoot(base);
        CHECK(std::fabs(RF(0x82C153F8) - 2340.0f / 1080.0f) < 1e-5f);
        const auto fov = bo2zm::native::v042::FovFromTangents(
            bo2zm::display::Bits(bo2zm::display::g_tan_x_bits.load()),
            bo2zm::display::Bits(bo2zm::display::g_tan_y_bits.load()));
        CHECK(fov.horizontal_deg > 91.0 && fov.horizontal_deg < 93.0);

        const auto aspect = bo2zm::native::v042::PresentAspectFor(bo2zm::native::v042::Screen::kUltrawide,
                                                                   bo2zm::display::DisplayOf(), true);
        CHECK(aspect.x == 2340 && aspect.y == 1080);
      } else {
        CHECK(State() == U::kFallback);
        CHECK(std::string(bo2zm::display::g_ultrawide_reason.load()) == "view_fov_stays_16x9");
        CHECK(!bo2zm::display::UltrawideConfirmed());
      }
    }
    if (State() == U::kFallback) {

      ws::OnViewFov(base, 0);
      CHECK(RF(0x82C153F8) == 16.0f / 9.0f && RF(0x82C153FC) == 16.0f / 11.0f && RF(0x82C15400) == 1.0f);
      CHECK(std::fabs(FullFactor() - 1.0) < 1e-4);
      for (int i = 0; i < 20; ++i) ws::PollBoot(base);
      CHECK(RF(0x82C153F8) == 16.0f / 9.0f);
      ws::Placement(base, 0x82CBC360);
      CHECK(std::fabs(FullFactor() - 1.0) < 1e-4);
    }
  }
  std::printf("%s\n", bo2zm::display::Json().c_str());
  if (failures) {
    std::fprintf(stderr, "test_android_v042_widescreen %s: %d failures\n", scenario.c_str(), failures);
    return EXIT_FAILURE;
  }
  std::printf("test_android_v042_widescreen %s: all checks passed\n", scenario.c_str());
  return EXIT_SUCCESS;
}
