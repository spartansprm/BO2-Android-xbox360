#pragma once

#include <algorithm>
#include <cstdint>

namespace bo2zm::native {

constexpr bool IsUiTextureDraw(uint64_t vertex_hash, uint32_t depth_control) {


  return !(depth_control & 0x7u) &&
         (vertex_hash == 0x58C4C9291CEDFCA7ull || vertex_hash == 0xB60067497710DCF4ull);
}

constexpr uint32_t TextureMipFloor(uint64_t vertex_hash, uint32_t depth_control,
                                   uint32_t preset, uint32_t diagnostic) {
  return std::min(4u, std::max(diagnostic, IsUiTextureDraw(vertex_hash, depth_control) ? 0u : preset));
}


constexpr uint32_t SamplerMipRange(uint32_t guest_mips, uint32_t floor) {
  const uint32_t min = guest_mips & 0xFu;
  const uint32_t max = (guest_mips >> 4) & 0xFu;
  return max > min ? (guest_mips & 0xF0u) | std::min(max, min + floor) : guest_mips;
}

}
