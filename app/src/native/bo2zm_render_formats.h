#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

namespace bo2zm_formats {

namespace xenos = rex::graphics::xenos;


inline bool ColorApprox(uint32_t format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (F(format)) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      return true;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_FLOAT:
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
    case F::k_2_10_10_10_AS_10_10_10_10:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
      static bool warned[16] = {};
      if (!warned[format & 0xF]) {
        warned[format & 0xF] = true;
        REXLOG_INFO("[native] BO2: color target with format {} stored as RGBA8 (approximate)", format);
      }
      return true;
    }
    default:
      return false;
  }
}


inline bool CopyApprox(uint32_t format) {
  using F = xenos::ColorFormat;
  switch (F(format)) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return true;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
    case F::k_10_11_11:
    case F::k_11_11_10:
    case F::k_10_11_11_AS_16_16_16_16:
    case F::k_11_11_10_AS_16_16_16_16:
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT: {
      static bool warned[64] = {};
      if (!warned[format & 0x3F]) {
        warned[format & 0x3F] = true;
        REXLOG_INFO("[native] BO2: copy to a texture with format {} stored as RGBA8 (approximate)", format);
      }
      return true;
    }
    default:
      return false;
  }
}


inline uint32_t Storage(uint32_t format) {
  return uint32_t(xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(format)));
}


inline uint32_t Log2BytesCopy(uint32_t format) {
  using F = xenos::ColorFormat;
  return (F(format) == F::k_16_16_16_16 || F(format) == F::k_16_16_16_16_FLOAT) ? 3 : 2;
}

}
