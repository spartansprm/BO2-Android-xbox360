// SPDX-License-Identifier: GPL-3.0-only

#include "touch_look.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <rex/ppc.h>
#include <rex/hook.h>

namespace {
uint32_t Read(uint8_t* base, uint32_t address) {
  uint32_t value; std::memcpy(&value, base + address, 4); return __builtin_bswap32(value);
}
void Write(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value); std::memcpy(base + address, &value, 4);
}
void Add(uint8_t* base, uint32_t address, int32_t delta) {
  const int64_t sum = int64_t(int32_t(Read(base,address))) + delta;
  Write(base,address,uint32_t(std::clamp(sum,int64_t(INT32_MIN),int64_t(INT32_MAX))));
}
void Inject(uint8_t* base, uint32_t active, uint32_t active_pointer, uint32_t mouse) {

  if (!active || Read(base,active_pointer) != active) return;
  const uint32_t index = Read(base,mouse+16);
  if (index > 1) { bo2zm::android::RelativeTouch().Cancel(); return; }
  const auto counts = bo2zm::android::RelativeTouch().Consume();
  Add(base,mouse+index*4,counts.x); Add(base,mouse+8+index*4,counts.y);
}
}

#if BO2_ANDROID_CAMPAIGN

REX_EXTERN(__imp__sub_821EBCF8);
REX_HOOK_RAW(sub_821EBCF8) {
  Inject(base,ctx.r3.u32,0x82929120,0x828D80E4);
  __imp__sub_821EBCF8(ctx,base);
}
#else

REX_EXTERN(__imp__sub_8225E798);
REX_HOOK_RAW(sub_8225E798) {
  Inject(base,ctx.r3.u32,0x82C70F4C,0x82BE943C);
  __imp__sub_8225E798(ctx,base);
}
#endif
