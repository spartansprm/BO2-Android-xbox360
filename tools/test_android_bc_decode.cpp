// SPDX-License-Identifier: GPL-3.0-only
#include "texture_tests/bc_fixtures.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <limits>

void Require(bool value) { if (!value) throw std::runtime_error("BC golden fixture failed"); }
int main() {
  using namespace bo2zm::texture;
  unsigned count = 0;
  for (auto format : {BcFormat::BC1, BcFormat::BC2, BcFormat::BC3, BcFormat::BC4, BcFormat::BC5}) {
    for (auto shape : {std::array<uint32_t, 2>{16, 16}, {7, 5}, {1, 1}, {2, 3}, {5, 1}}) {
      const auto fixture = bo2test::MakeBcFixture(format, shape[0], shape[1]);
      std::vector<uint8_t> guarded(fixture.rgba.size() + 32, 0xcd);
      auto output = std::span<uint8_t>(guarded).subspan(16, fixture.rgba.size());
      Require(DecodeBcRgba8(format, shape[0], shape[1], fixture.blocks, output));
      Require(std::equal(output.begin(), output.end(), fixture.rgba.begin()));
      Require(std::all_of(guarded.begin(), guarded.begin() + 16, [](uint8_t v) { return v == 0xcd; }));
      Require(std::all_of(guarded.end() - 16, guarded.end(), [](uint8_t v) { return v == 0xcd; }));
      std::fill(output.begin(), output.end(), 0xcd);
      Require(!DecodeBcRgba8(format, shape[0], shape[1], std::span(fixture.blocks).first(fixture.blocks.size() - 1), output));
      Require(!DecodeBcRgba8(format, shape[0], shape[1], fixture.blocks, output.first(output.size() - 1)));
      Require(std::all_of(guarded.begin(), guarded.end(), [](uint8_t v) { return v == 0xcd; }));
      ++count;
    }
  }
  uint8_t sentinel = 123;
  Require(!DecodeBcRgba8(BcFormat::BC1, 0, 1, {}, {&sentinel, 1}));
  Require(!DecodeBcRgba8(BcFormat::BC5, UINT32_MAX, UINT32_MAX, {}, {&sentinel, 1}));
  Require(!DecodeBcRgba8(static_cast<BcFormat>(99), 1, 1, {}, {&sentinel, 1}));
  Require(sentinel == 123);


  std::array<uint8_t, 8> endpoint{0x10, 0x84, 0xe0, 0x07, 0, 0, 0, 0};
  std::array<uint8_t, 4> pixel{};
  Require(DecodeBcRgba8(BcFormat::BC1, 1, 1, endpoint, pixel));
  Require(pixel == std::array<uint8_t, 4>{132, 130, 132, 255});
  endpoint[4] = 1;
  Require(DecodeBcRgba8(BcFormat::BC1, 1, 1, endpoint, pixel));
  Require(pixel == std::array<uint8_t, 4>{0, 255, 0, 255});
  endpoint = {0, 0, 0, 0, 3, 0, 0, 0};
  Require(DecodeBcRgba8(BcFormat::BC1, 1, 1, endpoint, pixel));
  Require(pixel == std::array<uint8_t, 4>{0, 0, 0, 0});
  endpoint = {128, 128, 7, 0, 0, 0, 0, 0};
  Require(DecodeBcRgba8(BcFormat::BC4, 1, 1, endpoint, pixel));
  Require(pixel == std::array<uint8_t, 4>{255, 0, 0, 255});
  std::cout << count << " golden images passed (BC1-5, alpha branches, partial blocks, bounds); invalid inputs rejected\n";
}
