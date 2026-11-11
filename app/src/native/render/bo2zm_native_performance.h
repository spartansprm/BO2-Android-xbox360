// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <unistd.h>
#endif

namespace bo2zm::native::performance {


struct Window {
  uint64_t sampled_ns = 0, swaps = 0, jobs = 0, category_jobs = 0, report_thread_tid = 0;
  double interval_seconds = 0, gpu_ms_per_swap = 0, gpu_wait_ms_per_swap = 0, gpu_gap_ms_per_swap = 0;
};
inline std::mutex mutex;
inline Window latest;
inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void Publish(Window value) {
  value.sampled_ns = NowNs();
#if defined(__ANDROID__)
  value.report_thread_tid = uint64_t(gettid());
#endif
  std::lock_guard guard(mutex);
  latest = value;
}
inline Window Read() { std::lock_guard guard(mutex); return latest; }


inline std::atomic<uint64_t> g_passes{0};
inline std::atomic<uint64_t> g_pass_texels{0};
inline std::atomic<uint64_t> g_copies{0};
inline std::atomic<uint64_t> g_copies_depth{0};
inline std::atomic<uint64_t> g_clears{0};
inline std::atomic<uint64_t> g_clears_depth{0};
inline std::atomic<uint64_t> g_clears_skipped{0};
inline std::atomic<uint64_t> g_clears_skipped_depth{0};
inline std::atomic<uint64_t> g_clears_useless{0};


inline std::atomic<uint64_t> g_clears_deferred{0};
inline std::atomic<uint64_t> g_clears_fused{0};
inline std::atomic<uint64_t> g_clears_flushed{0};
inline std::atomic<uint64_t> g_clears_dropped{0};


inline std::atomic<uint64_t> g_scene_scale_pct{100};
inline std::atomic<uint64_t> g_scaled_targets{0};
inline std::atomic<uint64_t> g_scaled_readbacks_skipped{0};


inline std::atomic<uint64_t> g_scene_preset_width{880};
inline std::atomic<uint64_t> g_scene_preset_height{720};
inline std::atomic<uint64_t> g_scene_width{880};
inline std::atomic<uint64_t> g_scene_height{720};


inline std::atomic<uint64_t> g_guest_clears_in_open_pass{0};
inline std::atomic<uint64_t> g_guest_clears_deferred{0};
inline std::atomic<uint64_t> g_guest_clears_rect_fused{0};
inline std::atomic<uint64_t> g_guest_clears_rect_at_begin{0};
inline std::atomic<uint64_t> g_guest_clears_separate{0};
inline std::atomic<uint64_t> g_guest_clears_pass_closed{0};


inline std::atomic<uint64_t> g_pass_splits_copy{0};
inline std::atomic<uint64_t> g_pass_splits_volume{0};
inline std::atomic<uint64_t> g_volumes_eager{0};
inline std::string CountersJson() {
  const auto v = [](const std::atomic<uint64_t>& a) { return std::to_string(a.load(std::memory_order_relaxed)); };
  return "{\"passes\":" + v(g_passes) + ",\"pass_texels\":" + v(g_pass_texels) + ",\"copies\":" + v(g_copies) +
         ",\"copies_depth\":" + v(g_copies_depth) + ",\"clears\":" + v(g_clears) +
         ",\"clears_depth\":" + v(g_clears_depth) + ",\"clears_skipped\":" + v(g_clears_skipped) +
         ",\"clears_skipped_depth\":" + v(g_clears_skipped_depth) + ",\"clears_useless\":" + v(g_clears_useless) +
         ",\"clears_deferred\":" + v(g_clears_deferred) + ",\"clears_fused\":" + v(g_clears_fused) +
         ",\"clears_flushed\":" + v(g_clears_flushed) + ",\"clears_dropped\":" + v(g_clears_dropped) +
         ",\"scene_scale_pct\":" + v(g_scene_scale_pct) + ",\"scaled_targets\":" + v(g_scaled_targets) +
         ",\"scaled_readbacks_skipped\":" + v(g_scaled_readbacks_skipped) +
         ",\"scene_resolution\":\"" + v(g_scene_preset_width) + "x" + v(g_scene_preset_height) + "\"" +
         ",\"scene_host_width\":" + v(g_scene_width) + ",\"scene_host_height\":" + v(g_scene_height) +
         ",\"guest_clears_in_open_pass\":" + v(g_guest_clears_in_open_pass) +
         ",\"guest_clears_deferred\":" + v(g_guest_clears_deferred) +
         ",\"guest_clears_rect_fused\":" + v(g_guest_clears_rect_fused) +
         ",\"guest_clears_rect_at_begin\":" + v(g_guest_clears_rect_at_begin) +
         ",\"guest_clears_separate\":" + v(g_guest_clears_separate) +
         ",\"guest_clears_pass_closed\":" + v(g_guest_clears_pass_closed) +
         ",\"pass_splits_copy\":" + v(g_pass_splits_copy) + ",\"pass_splits_volume\":" + v(g_pass_splits_volume) +
         ",\"volumes_eager\":" + v(g_volumes_eager) + "}";
}
}
