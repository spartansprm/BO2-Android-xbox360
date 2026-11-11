#include "bo2zm_native_v040.h"

#include <cstdio>
#include <cstdlib>

namespace v = bo2zm::native::v040;

static int failures = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)


constexpr uint32_t kRead = 0, kIgnore = 1, kClear = 2;

static void LoadModes() {

  for (uint32_t mode : {kRead, kIgnore, kClear}) {
    for (uint32_t i = 0; i < 5; ++i) {
      CHECK(v::AttachmentLoadMode(mode, i, kClear) == mode);
    }
  }

  for (uint32_t base : {kRead, kIgnore}) {
    for (uint32_t a = 0; a < 5; ++a) {
      const uint32_t mode = v::WithClear(base, a);
      CHECK(mode != base);
      for (uint32_t i = 0; i < 5; ++i) {
        CHECK(v::AttachmentLoadMode(mode, i, kClear) == (i == a ? kClear : base));
      }
    }
  }

  for (uint32_t mask = 0; mask < 32; ++mask) {
    uint32_t mode = kRead;
    for (uint32_t i = 0; i < 5; ++i) {
      if (mask & (1u << i)) mode = v::WithClear(mode, i);
    }
    for (uint32_t i = 0; i < 5; ++i) {
      CHECK(v::AttachmentLoadMode(mode, i, kClear) == ((mask >> i) & 1 ? kClear : kRead));
    }
  }
}

static void ClearPacking() {

  {
    const uint32_t formats[5] = {37, 0, 0, 0, 129};
    std::array<bool, 5> fused{false, false, false, false, true};
    std::array<int, 5> values{10, 11, 12, 13, 14};
    std::array<int, 5> out{};
    CHECK(v::PackClearValues(formats, fused, values, out) == 2);
    CHECK(out[1] == 14);
  }
  {
    const uint32_t formats[5] = {37, 0, 0, 0, 129};
    std::array<bool, 5> fused{true, false, false, false, false};
    std::array<int, 5> values{10, 11, 12, 13, 14};
    std::array<int, 5> out{};
    CHECK(v::PackClearValues(formats, fused, values, out) == 1);
    CHECK(out[0] == 10);
  }
  {
    const uint32_t formats[5] = {0, 0, 0, 0, 129};
    std::array<bool, 5> fused{false, false, false, false, true};
    std::array<int, 5> values{0, 0, 0, 0, 7};
    std::array<int, 5> out{};
    CHECK(v::PackClearValues(formats, fused, values, out) == 1);
    CHECK(out[0] == 7);
  }
  {
    const uint32_t formats[5] = {37, 37, 0, 0, 0};
    std::array<bool, 5> fused{};
    std::array<int, 5> values{};
    std::array<int, 5> out{};
    CHECK(v::PackClearValues(formats, fused, values, out) == 0);
  }
}

static void SceneScale() {

  for (uint32_t pct = 50; pct <= 100; ++pct) {
    const uint32_t w = v::ScaledSide(880, pct), h = v::ScaledSide(880, pct);
    CHECK(w >= 16 && w <= 880 && h <= 880);
    CHECK(w == h);

    CHECK(v::ScaledSide(880, pct) == w);

    const uint32_t tw = v::StoredSide(896, w, 880), th = v::StoredSide(720, h, 880);
    CHECK(tw >= 1 && th >= 1 && tw <= 896 && th <= 720);


    for (int32_t x0 = 0; x0 < 880; x0 += 8) {
      const int32_t hx0 = v::ToHost(x0, w, 880);
      const int32_t hx8 = v::ToHost(x0 + 8, w, 880);
      CHECK(hx0 >= 0 && hx8 >= hx0 && hx8 <= int32_t(w));
      CHECK(v::ToHost(x0, tw, 896) <= int32_t(tw));
    }
    CHECK(v::ToHost(880, w, 880) <= int32_t(w));
    CHECK(v::ToHost(720, h, 880) <= int32_t(h));
    CHECK(v::ToHost(720, th, 720) == int32_t(th));


    const int32_t src_w = v::ToHost(880, w, 880), dst_w = int32_t(tw);
    CHECK(src_w <= dst_w + 1);
    const int32_t src_h = v::ToHost(720, h, 880), dst_h = int32_t(th);
    CHECK(src_h <= dst_h + 1);


    const double content = double(v::ToHost(880, tw, 896)) / double(tw);
    CHECK(content > 880.0 / 896.0 - 1.0 / double(tw) - 1e-9 && content < 880.0 / 896.0 + 1.0 / double(tw) + 1e-9);
  }

  CHECK(v::ScaledSide(880, 100) == 880);
  CHECK(v::ToHost(123, 880, 880) == 123);
  CHECK(v::ToHost(123, 880, 0) == 123);
}

int main() {
  LoadModes();
  ClearPacking();
  SceneScale();
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::puts("test_android_v040: all checks passed");
  return 0;
}
