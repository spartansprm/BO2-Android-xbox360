#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bo2zm::native::translate {


inline constexpr uint32_t kHeapSizes[4] = {16384, 64, 256, 512};

struct Result {
  std::vector<uint32_t> spirv;
  std::string error;
  bool pixel = false;
  double ms_hlsl = 0;
  double ms_compile = 0;
};


Result Translate(std::span<const uint8_t> container);


bool ApplyCompat(std::string& hlsl, std::string& error);


uint64_t Identity();

}
