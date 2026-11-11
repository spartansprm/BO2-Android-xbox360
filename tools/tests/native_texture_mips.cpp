#include "../../app/src/native/render/bo2zm_native_texture_mips.h"

#include <array>
#include <cassert>
#include <iostream>

using namespace bo2zm::native;

int main() {

  constexpr uint64_t ui = 0x58C4C9291CEDFCA7ull;
  constexpr uint64_t icon = 0xB60067497710DCF4ull;
  constexpr uint64_t unknown = 0x123456789ABCDEF0ull;
  constexpr uint32_t depth_off = 0xF0;


  for (uint64_t raw : {0x0479CB710D805FF3ull, 0x4A8255A17452248Dull}) {
    assert(!IsUiTextureDraw(raw, depth_off));
    assert(TextureMipFloor(raw, depth_off, 2, 0) == 2);
  }


  constexpr std::array presets{0u, 2u, 2u, 3u, 1u, 0u, 2u, 2u, 2u};
  constexpr std::array diagnostics{0u, 0u, 0u, 0u, 0u, 0u, 1u, 4u, 0u};
  for (size_t i = 0; i < presets.size(); ++i) {
    for (uint64_t shader : {ui, icon}) {
      assert(TextureMipFloor(shader, depth_off, presets[i], diagnostics[i]) == diagnostics[i]);
      assert(SamplerMipRange(0xA0, TextureMipFloor(shader, depth_off, presets[i], diagnostics[i])) ==
             (0xA0 | diagnostics[i]));
    }
    assert(TextureMipFloor(unknown, depth_off, presets[i], diagnostics[i]) ==
           std::max(presets[i], diagnostics[i]));
  }


  for (uint32_t preset = 0; preset <= 4; ++preset) {
    for (uint32_t diagnostic = 0; diagnostic <= 4; ++diagnostic) {
      for (uint32_t control = 0; control <= 0xFF; ++control) {
        for (uint64_t shader : {ui, icon, unknown, uint64_t(0)}) {
          const uint32_t floor = TextureMipFloor(shader, control, preset, diagnostic);
          if (shader == unknown || !shader || (control & 7)) {
            assert(floor == std::max(preset, diagnostic));
          } else {
            assert(floor == diagnostic);
          }
        }
      }
    }
  }


  for (uint32_t mips = 0; mips <= 0xFF; ++mips) {
    const uint32_t guest_min = mips & 15;
    const uint32_t guest_max = mips >> 4;
    assert(SamplerMipRange(mips, 0) == mips);
    for (uint32_t floor = 0; floor <= 4; ++floor) {
      const uint32_t result = SamplerMipRange(mips, floor);
      assert((result >> 4) == guest_max);
      const uint32_t expected_min = guest_max > guest_min ? std::min(guest_max, guest_min + floor) : guest_min;
      assert((result & 15) == expected_min);
    }
  }
  assert(TextureMipFloor(unknown, depth_off, 10, 0) == 4);
  assert(TextureMipFloor(ui, depth_off, 10, 0) == 0);
  assert(TextureMipFloor(ui, depth_off, 0, 10) == 4);
  std::cout << "Texture mip checks passed: UI guards, preset transitions, diagnostics and guest mip bounds\n";
}
