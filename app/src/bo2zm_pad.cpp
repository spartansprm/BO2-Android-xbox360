#include "bo2zm_pad.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>

#include "bo2zm_cheats.h"
#include "bo2zm_controllers.h"
#include "bo2zm_preset.h"

namespace {
constexpr uint8_t kTriggerHeld = 30;
}

REX_EXTERN(__imp__sub_827D8A48);
REX_HOOK_RAW(sub_827D8A48) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_827D8A48(ctx, base);
  if (ctx.r3.u32 != 0 || !state || user >= 4) {
    return;
  }
  static std::atomic<uint16_t> earlier[4] = {};


  static std::atomic<uint16_t> stale[4] = {};
  static std::atomic<bool> stale_sticks[4] = {};
  static std::atomic<bool> was_blocked[4] = {};
  if (bo2zm::preset::InputBlocked()) {
    uint16_t held;
    std::memcpy(&held, base + state + 4, 2);
    earlier[user].store(__builtin_bswap16(held), std::memory_order_relaxed);
    std::memset(base + state + 4, 0, 12);
    was_blocked[user].store(true, std::memory_order_relaxed);
    return;
  }
  auto stick = [&](uint32_t offset) {
    uint16_t v;
    std::memcpy(&v, base + state + offset, 2);
    return int16_t(__builtin_bswap16(v));
  };
  const auto deflected = [&] {
    for (const uint32_t offset : {8u, 10u, 12u, 14u}) {
      if (std::abs(int(stick(offset))) > 9000) {
        return true;
      }
    }
    return base[state + 6] > kTriggerHeld || base[state + 7] > kTriggerHeld;
  };
  if (was_blocked[user].exchange(false, std::memory_order_relaxed)) {
    uint16_t held;
    std::memcpy(&held, base + state + 4, 2);
    stale[user].store(__builtin_bswap16(held), std::memory_order_relaxed);
    stale_sticks[user].store(deflected(), std::memory_order_relaxed);
  }
  if (stale_sticks[user].load(std::memory_order_relaxed)) {
    if (deflected()) {
      std::memset(base + state + 6, 0, 10);
    } else {
      stale_sticks[user].store(false, std::memory_order_relaxed);
    }
  }
  uint16_t raw;
  std::memcpy(&raw, base + state + 4, 2);
  bo2zm::pad::Pad pad;
  pad.buttons = __builtin_bswap16(raw);
  pad.before = earlier[user].exchange(pad.buttons, std::memory_order_relaxed);
  pad.aim = base[state + 6] > kTriggerHeld;
  pad.user = user;
  const int local = bo2zm::cheats::LocalPlayerForController(int(user));
  uint16_t hide = bo2zm::cheats::IsPrimaryController(int(user)) ? bo2zm::preset::OnPad(pad) : 0;
  if (local >= 0 && local < 4) hide |= bo2zm::cheats::OnPad(pad, local);

  const auto edit = bo2zm::controllers::OnPad(pad);

  const uint16_t held_since_block = stale[user].load(std::memory_order_relaxed) & pad.buttons;
  stale[user].store(held_since_block, std::memory_order_relaxed);
  hide = (hide | edit.hide | held_since_block) & pad.buttons;
  if (hide || edit.add) {
    const uint16_t visible = __builtin_bswap16(uint16_t((pad.buttons & ~hide) | edit.add));
    std::memcpy(base + state + 4, &visible, 2);
  }
}
