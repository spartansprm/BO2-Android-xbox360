#include "bo2zm_volume_writes.h"

#include <algorithm>
#include <array>
#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "bo2zm_benchmark.h"

REXCVAR_DEFINE_BOOL(bo2zm_native_volume_by_writes, true, "BO2",
                    "Native renderer: the lighting volume's changed pages come from a hook on the game "
                    "function that writes it, instead of hashing all 2 MB every frame (checked against the hash). "
                    "false = hashing, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_volume_by_writes_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_volume_by_writes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828E5570);

namespace bo2zm::volume_writes {
namespace {

constexpr uint32_t kPhysicalPages = 0x20000000u >> 12;
constexpr uint32_t kBrickBytes = 5408;

std::array<std::atomic<uint64_t>, kPhysicalPages / 64> g_marks{};
std::atomic<bool> g_missed{false};

bool Hooked() {
  static const bool on = REXCVAR_GET(bo2zm_native_volume_by_writes) || REXCVAR_GET(bo2zm_native_volume_by_writes_ab);
  return on;
}

void Mark(uint32_t guest_address) {
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    return;
  }
  const uint32_t physical = memory->GetPhysicalAddress(guest_address);
  if (physical == UINT32_MAX) {
    return;
  }
  const uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
  const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + kBrickBytes - 1) >> 12, kPhysicalPages - 1);
  for (uint32_t p = first; p <= last; ++p) {
    g_marks[p >> 6].fetch_or(uint64_t(1) << (p & 63), std::memory_order_release);
  }
}

}

bool UseMarks() {
  if (g_missed.load(std::memory_order_relaxed)) {
    return false;
  }
  static const bool on = REXCVAR_GET(bo2zm_native_volume_by_writes);
  static const bool ab = REXCVAR_GET(bo2zm_native_volume_by_writes_ab);
  if (ab) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & 1) != 0;
    }
  }
  return on;
}

void TakeMarks(uint32_t physical_first, uint32_t count, std::vector<uint32_t>& pages) {
  const uint32_t first = (physical_first & 0x1FFFFFFFu) >> 12;
  const uint32_t end = std::min<uint32_t>(first + count, kPhysicalPages);
  for (uint32_t w = first >> 6; w <= (end - 1) >> 6 && first < end; ++w) {

    uint64_t mask = ~uint64_t(0);
    if (w == first >> 6) {
      mask &= ~uint64_t(0) << (first & 63);
    }
    if (w == (end - 1) >> 6 && (end & 63) != 0) {
      mask &= ~(~uint64_t(0) << (end & 63));
    }
    uint64_t bits = g_marks[w].fetch_and(~mask, std::memory_order_acquire) & mask;
    while (bits) {
      const uint32_t b = uint32_t(__builtin_ctzll(bits));
      bits &= bits - 1;
      pages.push_back(w * 64 + b - first);
    }
  }
}

void Missed(uint32_t physical_page) {
  if (!g_missed.exchange(true)) {
    REXLOG_ERROR("[native] C3 lighting volume: page {:08X} changed without a mark from sub_828E5570 (another writer); "
                 "its pages are found by hashing for the rest of the session",
                 physical_page);
  }
}

}


REX_HOOK_RAW(sub_828E5570) {
  const uint32_t destination = ctx.r4.u32;
  __imp__sub_828E5570(ctx, base);
  if (bo2zm::volume_writes::Hooked()) {
    bo2zm::volume_writes::Mark(destination);
  }
}
