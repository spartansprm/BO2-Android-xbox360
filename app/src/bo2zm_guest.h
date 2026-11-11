#pragma once

#include <cstdint>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

namespace bo2zm::guest {


template <uint32_t Address>
inline void Call(PPCContext& ctx, uint8_t* base) {
  static PPCFunc* const function = rex::runtime::ResolveIndirectFunction(Address);
  function(ctx, base);
}

}
