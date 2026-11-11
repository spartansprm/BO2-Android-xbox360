#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/xmemory.h>

#include "bo2zm_page_writes.h"
#if !defined(BO2_GAME_CAMPAIGN) && !defined(BO2_ANDROID_RUNTIME_PROBE)
#include "bo2zm_mod_hud_fit.h"
#endif

namespace {

inline uint8_t* Host(uint8_t* base, uint32_t guest) {
  return rex::memory::GuestPtr<uint8_t*>(base, guest);
}


inline void Move(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r5.u32;
  if (n != 0) [[likely]] {
#if !defined(BO2_GAME_CAMPAIGN) && !defined(BO2_ANDROID_RUNTIME_PROBE)
    if ((n == 124 && bo2zm::mod_hud_fit::IsCopySite(uint32_t(ctx.lr))) ||
        (n == 10760 && (bo2zm::mod_hud_fit::IsUnknownPsCopySite(uint32_t(ctx.lr)) ||
                        bo2zm::mod_hud_fit::IsBulkPsCopySite(uint32_t(ctx.lr))))) [[unlikely]]
      bo2zm::mod_hud_fit::BeforeCopy(ctx, base);
#endif
    std::memmove(Host(base, ctx.r3.u32), Host(base, ctx.r4.u32), n);
#if !defined(BO2_GAME_CAMPAIGN) && !defined(BO2_ANDROID_RUNTIME_PROBE)
    if (n == 124 && bo2zm::mod_hud_fit::IsCopySite(uint32_t(ctx.lr))) [[unlikely]]
      bo2zm::mod_hud_fit::AfterCopy(ctx, base);
#endif
    bo2zm::page_writes::MarkGuest(ctx.r3.u32, n);
  }
}


inline void Fill(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r5.u32;
  if (n != 0) [[likely]] {
    std::memset(Host(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), n);
    bo2zm::page_writes::MarkGuest(ctx.r3.u32, n);
  }
}

}

extern "C" REX_FUNC(rexcrt_bo2zm_memcpy) { Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_bo2zm_memmove) { Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_bo2zm_xmemcpy) { Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_bo2zm_bigcopy) { Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_bo2zm_bigcopy2) { Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_bo2zm_memset) { Fill(ctx, base); }
extern "C" REX_FUNC(rexcrt_bo2zm_xmemset) { Fill(ctx, base); }
