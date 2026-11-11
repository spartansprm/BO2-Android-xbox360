#include "bo2zm_ui.h"

#include <cstring>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "bo2zm_game.h"
#include "bo2zm_ui_lua.h"

REXCVAR_DEFINE_BOOL(bo2zm_ui_changes, true, "BO2",
                    "Applies mode-specific home menus, Extras and graphics settings. Online and System Link "
                    "entries are marked as work in progress.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824A0BA8);
REX_EXTERN(__imp__sub_824A0BC0);
REX_EXTERN(__imp__sub_82708D68);
REX_EXTERN(__imp__sub_826F2720);

namespace {

constexpr uint32_t kMenuLuaState = BO2_GAME_ADDR(0x845CD1F8, 0x83EF2F40);
constexpr uint32_t kGameLock = 38;
constexpr uint32_t kStateGlobal = 8;
constexpr uint32_t kGlobalCompilerSettings = 728;
constexpr uint32_t kGlobalSecurityMode = 284;
constexpr uint32_t kStateTop = 36;
constexpr uint32_t kStateStackBottom = 48;
constexpr uint32_t kTypeBoolean = 1;
constexpr uint32_t kTypeString = 4;
constexpr const char kChunkName[] = "=bo2zm menus";

constexpr const char kProbeLua[] =
    "local LUI = rawget(_G, \"LUI\") local s = LUI and rawget(LUI, \"bo2zm\") "
    "if not s then return false end if s.install then s.install() end return true";
constexpr const char kProbeName[] = "=bo2zm probe";
constexpr uint32_t kProbeEveryFrames = 60;

uint32_t g_installed_state = 0;
uint32_t g_wait_frames = 0;
uint32_t g_attempts = 0;
uint32_t g_text = 0;
uint32_t g_name = 0;
uint32_t g_probe_text = 0;
uint32_t g_probe_name = 0;
uint32_t g_probe_frames = 0;
uint32_t g_stack_relocations = 0;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}


std::string_view StringOf(const uint8_t* base, uint32_t object) {
  if (object < 0x1000) {
    return {};
  }
  const char* p = reinterpret_cast<const char*>(base + object);
  for (uint32_t start = 0; start < 32; ++start) {
    uint32_t n = 0;
    while (n < 300 && p[start + n] >= 0x20 && p[start + n] < 0x7F) {
      ++n;
    }
    if (n >= 6) {
      return {p + start, n};
    }
  }
  return {};
}

bool CopyToGuest(uint8_t* base) {
  if (g_text) {
    return true;
  }
  auto* memory = rex::Runtime::instance()->memory();
  const size_t length = std::strlen(bo2zm::ui::kMenusLua);
  g_text = memory->SystemHeapAlloc(uint32_t(length + 1));
  g_name = memory->SystemHeapAlloc(sizeof(kChunkName));
  g_probe_text = memory->SystemHeapAlloc(sizeof(kProbeLua));
  g_probe_name = memory->SystemHeapAlloc(sizeof(kProbeName));
  if (!g_text || !g_name || !g_probe_text || !g_probe_name) {
    REXLOG_ERROR("[ui] no guest memory for the menu Lua ({} bytes)", length);
    g_text = 0;
    return false;
  }
  std::memcpy(base + g_text, bo2zm::ui::kMenusLua, length + 1);
  std::memcpy(base + g_name, kChunkName, sizeof(kChunkName));
  std::memcpy(base + g_probe_text, kProbeLua, sizeof(kProbeLua));
  std::memcpy(base + g_probe_name, kProbeName, sizeof(kProbeName));
  return true;
}


bool RunLua(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t text, uint32_t length, uint32_t name) {
  const auto saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 512) & ~uint64_t(0xF);
  ctx.r3.u64 = kGameLock;
  __imp__sub_824A0BA8(ctx, base);

  const uint32_t top_before = Load32(base, L + kStateTop);
  const uint32_t bottom_before = Load32(base, L + kStateStackBottom);
  const uint32_t top_offset = top_before - bottom_before;
  const uint32_t global = Load32(base, L + kStateGlobal);
  const uint32_t security = Load32(base, global + kGlobalSecurityMode);
  Store32(base, global + kGlobalSecurityMode, 0);
  ctx.r3.u64 = L;
  ctx.r4.u64 = global + kGlobalCompilerSettings;
  ctx.r5.u64 = text;
  ctx.r6.u64 = length;
  ctx.r7.u64 = name;
  __imp__sub_82708D68(ctx, base);
  Store32(base, global + kGlobalSecurityMode, security);
  int status = int32_t(ctx.r3.u32);
  const char* stage = "compile";
  if (status == 0) {
    stage = "run";
    ctx.r3.u64 = L;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 1;
    ctx.r6.u64 = 0;
    __imp__sub_826F2720(ctx, base);
    status = int32_t(ctx.r3.u32);
  }
  bool installed = false;


  const uint32_t bottom_after = Load32(base, L + kStateStackBottom);
  const uint32_t restore_top = bottom_after + top_offset;
  if (bottom_after != bottom_before) {
    ++g_stack_relocations;
    REXLOG_INFO("[ui] menu Lua stack relocated {:08X} -> {:08X} (count {}, {} saved values)",
                bottom_before, bottom_after, g_stack_relocations, top_offset / 8);
  }
  const uint32_t top = Load32(base, L + kStateTop);
  if (top > restore_top) {
    const uint32_t value = Load32(base, top - 8);
    const uint32_t type = Load32(base, top - 4) & 0xF;
    if (status == 0) {
      installed = type == kTypeBoolean && value != 0;
    } else if (g_attempts <= 3) {
      const std::string_view error = type == kTypeString ? StringOf(base, value) : std::string_view();
      REXLOG_ERROR("[ui] menu Lua failed to {} (status {}): {}", stage, status, error);
    }
    Store32(base, L + kStateTop, restore_top);
  }

  ctx.r3.u64 = kGameLock;
  __imp__sub_824A0BC0(ctx, base);
  ctx = saved;
  return installed;
}


bool InstallIn(PPCContext& ctx, uint8_t* base, uint32_t L) {
  if (L == g_installed_state) {
    if (RunLua(ctx, base, L, g_probe_text, uint32_t(sizeof(kProbeLua) - 1), g_probe_name)) {
      return true;
    }
    REXLOG_INFO("[ui] the menu system was rebuilt in Lua state {:08X}: installing the menu changes again", L);
    g_installed_state = 0;
  }
  if (!CopyToGuest(base)) {
    return false;
  }
  ++g_attempts;
  if (!RunLua(ctx, base, L, g_text, uint32_t(std::strlen(bo2zm::ui::kMenusLua)), g_name)) {
    return false;
  }
  g_installed_state = L;
  g_attempts = 0;
  REXLOG_INFO("[ui] menu changes installed in the menu Lua state {:08X} (app/src/ui/menus.lua)", L);
  return true;
}

}


REX_EXTERN(__imp__sub_8270BDB0);
REX_HOOK_RAW(sub_8270BDB0) {
  const uint32_t name = ctx.r3.u32;
  __imp__sub_8270BDB0(ctx, base);
  if (!REXCVAR_GET(bo2zm_ui_changes) || name == 0) {
    return;
  }
  const std::string_view module(reinterpret_cast<const char*>(base + name));
  if (module != "T6.Zombie.ffotdzm" && module != "T6.ffotdmp" && module != "T6.ffotdsp") {
    return;
  }
  if (const uint32_t L = Load32(base, kMenuLuaState); L != 0) {
    const uint64_t result = ctx.r3.u64;
    InstallIn(ctx, base, L);
    ctx.r3.u64 = result;
  }
}


REX_EXTERN(__imp__sub_8270BBD0);
REX_HOOK_RAW(sub_8270BBD0) {
  const uint32_t L = ctx.r4.u32;
  const uint32_t top = L ? Load32(base, L + kStateTop) : 0;
  if (top >= 8 && (Load32(base, top - 4) & 0xF) == kTypeString) {
    REXLOG_ERROR("[ui] Lua error the menus did not catch: {}", StringOf(base, Load32(base, top - 8)));
  } else {
    REXLOG_ERROR("[ui] Lua error the menus did not catch (no message on the stack)");
  }
  __imp__sub_8270BBD0(ctx, base);
}

void bo2zm::ui::EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(bo2zm_ui_changes)) [[unlikely]] {
    return;
  }
  const uint32_t L = Load32(base, kMenuLuaState);
  if (L == 0) [[unlikely]] {
    return;
  }
  if (L == g_installed_state) [[likely]] {
    if (++g_probe_frames < kProbeEveryFrames) [[likely]] {
      return;
    }
    g_probe_frames = 0;
  } else if (g_wait_frames > 0) {
    --g_wait_frames;
    return;
  }
  if (!InstallIn(ctx, base, L)) {
    g_wait_frames = g_text ? 30 : 0xFFFFFFFF;
  }
}
