#pragma once
#include <cstdint>
#include <rex/ppc.h>

namespace bo2zm::mod_hud_fit {
void BeginEpoch();


void EndMapLoad();
constexpr bool IsCopySite(uint32_t lr) {
  return lr == 0x8232CDB0 || lr == 0x8232CE8C ||
         lr == 0x8232CF68 || lr == 0x8232D044;
}

constexpr bool IsUnknownPsCopySite(uint32_t lr) { return lr == 0x82439010; }
constexpr bool IsBulkPsCopySite(uint32_t lr) { return lr == 0x8230369C; }


void BeforeCopy(const PPCContext& ctx, uint8_t* base);


void AfterCopy(const PPCContext& ctx, uint8_t* base);
}
