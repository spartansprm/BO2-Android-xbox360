


#include "bo2zm_native_render.h"
#if defined(__ANDROID__)
#include <rex/android.h>
#endif
#include "../bo2zm_native_vulkan_profile.h"
#include "bo2zm_native_output_sync.h"
#include "../bo2zm_native_system.h"

#include "bo2zm_hitch_waits.h"
#include "bo2zm_native_performance.h"
#include "bo2zm_android_perf_hint.h"
#include "bo2zm_native_targets.h"
#include "bo2zm_native_hooks.h"
#include "bo2zm_native_shaders.h"
#include "bo2zm_native_progress.h"
#include "bo2zm_native_precompile.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>


#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>


#ifndef BO2ZM_SHADER_LIBRARY_FILE
#define BO2ZM_SHADER_LIBRARY_FILE "bo2zm_shaders.spvpack"
#endif


namespace bo2zm::scenery_lod {
std::string Summary();
}

#if REX_PLATFORM_SWITCH

#include "core/threading_switch.h"
#endif


REXCVAR_DEFINE_STRING(bo2zm_native_dump_memory, "", "BO2",
                      "Diagnostics: ADDRESS:BYTES@SECONDS (hex:hex@s) dumps guest memory once at that time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_bo2zm_dump_texture;


REXCVAR_DEFINE_BOOL(bo2zm_native_diag_lut, false, "BO2",
                    "Diagnostics: log the colour-grading strip draw's constants and base table every 5 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_memexport, true, "BO2",
                    "Native renderer: run the vertex shaders that write guest memory (memexport) on the CPU, as the "
                    "GPU does. BO2 copies streamed texture data (mip levels) this way. false = drop them, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_memexport_fast, true, "BO2",
                    "Native renderer: BO2's two memexport copy shaders (texture streaming) are done directly in C++ "
                    "instead of through the shader interpreter, with the same writes. false = interpreter");
REXCVAR_DEFINE_BOOL(bo2zm_native_memexport_verify, false, "BO2",
                    "Diagnostic: after each direct memexport copy, run the interpreter too and compare the bytes (log "
                    "'direct memexport'). Slow");


REXCVAR_DEFINE_BOOL(bo2zm_native_fences_wait_copies, true, "BO2",
                    "Native renderer: fences, memory writes and interrupts from the ring wait for the "
                    "vertex copy thread first, so the game cannot reuse vertices that are not copied yet. false = as "
                    "before (sprites blinked on battery)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::memexport {


class Sink final : public rex::graphics::ShaderInterpreter::ExportSink {
 public:
  explicit Sink(uint8_t* physical) : physical_(physical) {}

  void AllocExport(rex::graphics::ucode::AllocType, uint32_t) override { Dump(); }

  void Export(rex::graphics::ucode::ExportRegister reg_index, const float* value, uint32_t mask) override {
    const uint32_t r = uint32_t(reg_index);
    float* target = r == 32 ? ea_ : (r >= 33 && r <= 37) ? em_[r - 33] : nullptr;
    if (!target) {
      return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (mask & (1u << i)) {
        target[i] = value[i];
      }
    }
    if (r != 32) {
      written_ |= 1u << (r - 33);
    }
  }

  void End() { Dump(); }


  bool Integers16(const uint32_t a[4], const uint16_t v[4][4]) {
    if ((a[0] >> 30) != 0x1 || (a[1] >> 23) != 0x96 || (a[2] >> 23) != 0x96 || (a[3] >> 23) != 0x96) {
      return false;
    }
    const uint32_t z = a[2];
    if (((z >> 8) & 0x3F) != 26 || ((z >> 16) & 0x1) || !((z >> 17) & 0x1)) {
      return false;
    }
    if (tracing && trace.size() < 1500) {
      trace += fmt::format(" [eA {:08X} {:08X} {:08X} {:08X} eMF i{}]", a[0], a[1], a[2], a[3], a[1] & 0x7FFFFF);
    }
    const bool red_blue = (z >> 19) & 0x1;
    for (uint32_t e = 0; e < 4; ++e) {
      const uint32_t x = v[e][red_blue ? 2 : 0], y = v[e][1], zz = v[e][red_blue ? 0 : 2], w = v[e][3];
      uint32_t d[4] = {x | (y << 16), zz | (w << 16), 0, 0};
      Write(a, e, d, 3);
    }
    return true;
  }

  uint64_t elements = 0;
  uint64_t outside = 0;
  uint64_t rejected = 0;
  uint32_t format_odd = 0;
  uint32_t dir_min = UINT32_MAX, dir_max = 0;
  std::string trace;


  bool tracing = false;

 private:

  static uint32_t Whole(float v, uint32_t bits, bool with_sign, bool whole) {
    if (v != v) {
      v = 0.0f;
    }
    if (with_sign) {
      const float max = float((1u << (bits - 1)) - 1);
      v = whole ? std::clamp(v, -max - 1.0f, max) : std::clamp(v, -1.0f, 1.0f) * max;
      return uint32_t(int32_t(v + (v < 0.0f ? -0.5f : 0.5f))) & ((bits == 32) ? ~0u : ((1u << bits) - 1));
    }
    const float max = float(bits == 32 ? 4294967295.0 : double((1u << bits) - 1));
    v = whole ? std::clamp(v, 0.0f, max) : std::clamp(v, 0.0f, 1.0f) * max;
    return uint32_t(v + 0.5f);
  }


  static int32_t Pack(uint32_t format, bool with_sign, bool whole, const float* v, uint32_t* d) {
    auto bits = [](float f) {
      uint32_t u;
      std::memcpy(&u, &f, sizeof(u));
      return u;
    };
    switch (format) {
      case 6:
        d[0] = Whole(v[0], 8, with_sign, whole) | (Whole(v[1], 8, with_sign, whole) << 8) |
               (Whole(v[2], 8, with_sign, whole) << 16) | (Whole(v[3], 8, with_sign, whole) << 24);
        return 2;
      case 24:
        d[0] = Whole(v[0], 16, with_sign, whole);
        return 1;
      case 25:
        d[0] = Whole(v[0], 16, with_sign, whole) | (Whole(v[1], 16, with_sign, whole) << 16);
        return 2;
      case 26:
        d[0] = Whole(v[0], 16, with_sign, whole) | (Whole(v[1], 16, with_sign, whole) << 16);
        d[1] = Whole(v[2], 16, with_sign, whole) | (Whole(v[3], 16, with_sign, whole) << 16);
        return 3;
      case 36:
        d[0] = bits(v[0]);
        return 2;
      case 37:
        d[0] = bits(v[0]);
        d[1] = bits(v[1]);
        return 3;
      case 38:
        for (uint32_t i = 0; i < 4; ++i) {
          d[i] = bits(v[i]);
        }
        return 4;
      default:
        return -1;
    }
  }

  void Dump() {
    if (!written_) {
      return;
    }
    const uint32_t written = written_;
    written_ = 0;
    uint32_t a[4];
    std::memcpy(a, ea_, sizeof(a));
    if ((a[0] >> 30) != 0x1 || (a[1] >> 23) != 0x96 || (a[2] >> 23) != 0x96 || (a[3] >> 23) != 0x96) {
      ++rejected;
      return;
    }
    const uint32_t z = a[2];
    const uint32_t format = (z >> 8) & 0x3F;
    const bool with_sign = (z >> 16) & 0x1;
    const bool whole = (z >> 17) & 0x1;
    const bool red_blue = (z >> 19) & 0x1;
    const uint32_t index = a[1] & 0x7FFFFF;
    if (tracing && trace.size() < 1500) {
      trace += fmt::format(" [eA {:08X} {:08X} {:08X} {:08X} eM{:X} i{}]", a[0], a[1], a[2], a[3], written, index);
    }
    for (uint32_t e = 0; e < 5; ++e) {
      if (!(written & (1u << e))) {
        continue;
      }
      float v[4] = {em_[e][0], em_[e][1], em_[e][2], em_[e][3]};
      if (red_blue) {
        std::swap(v[0], v[2]);
      }
      uint32_t d[4] = {};
      const int32_t log2_sz = Pack(format, with_sign, whole, v, d);
      if (log2_sz < 0) {
        format_odd = format;
        continue;
      }
      Write(a, e, d, uint32_t(log2_sz));
    }
  }


  void Write(const uint32_t a[4], uint32_t e, uint32_t (&d)[4], uint32_t log2_sz) {
    const uint32_t endian = a[2] & 0x7;
    const uint32_t index = a[1] & 0x7FFFFF;
    const uint32_t count = a[3] & 0x7FFFFF;
    const uint32_t base = a[0] << 2;

    uint32_t endian_e = endian;
    if (endian_e == 4) {
      std::swap(d[0], d[1]);
      std::swap(d[2], d[3]);
      endian_e = 2;
    } else if (endian_e == 5) {
      std::swap(d[0], d[3]);
      std::swap(d[1], d[2]);
      endian_e = 2;
    }
    for (uint32_t& w : d) {
      if (endian_e == 1 || endian_e == 2) {
        w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
      }
      if (endian_e == 2 || endian_e == 3) {
        w = (w << 16) | (w >> 16);
      }
    }
    if (index + e >= count) {
      ++outside;
      return;
    }
    const uint32_t sz = 1u << log2_sz;
    const uint32_t address = (base + ((index + e) << log2_sz)) & 0x1FFFFFFF;
    if (address + sz > 0x20000000u) {
      ++outside;
      return;
    }

    std::memcpy(physical_ + address, d, sz);
    ++elements;
    dir_min = std::min(dir_min, address);
    dir_max = std::max(dir_max, address + sz);
  }

  uint8_t* physical_;
  float ea_[4] = {};
  float em_[5][4] = {};
  uint32_t written_ = 0;
};

}

REXCVAR_DEFINE_STRING(bo2zm_c1_renderer, "xenos", "BO2",
                      "xenos = GPU emulation; native = native renderer in development (piece C1: the game runs without "
                      "emulation and the screen shows a test color)")
    .allowed({"xenos", "native"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_vector_register_writes, true, "BO2",
                    "Native renderer: the ring's register blocks are copied 4 at a time with NEON. "
                    "The first 200,000 blocks are checked against the usual path and, if one differs, it turns itself "
                    "off. false = the usual path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_block_register_writes, true, "BO2",
                    "Native renderer: register blocks without side effects (almost always VS and PS constants) are "
                    "written in one go instead of register by register. false: as before");


REXCVAR_DEFINE_INT32(bo2zm_native_diag_report_s, 20, "BO2",
                     "Native renderer: seconds between dumps of the diagnostic report. Each dump is written by the "
                     "thread that feeds the GPU and causes stutters: 10 gives twice the detail and twice the stutters "
                     "from this cause")
    .range(5, 120);


REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_reports, true, "BO2",
                    "Native renderer: the ring thread's periodic report and [hitch] lines are "
                    "written to the SD from a separate thread; the ring only formats and queues them. false = as "
                    "before, the ring writes them itself (29-32 ms every 20 s in 176)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


#include "bo2zm_microcode_versions.h"
#include "../../bo2zm_preset.h"
#include "../../bo2zm_benchmark.h"
#include "../../bo2zm_memcpy_callers.h"
#include "../../bo2zm_page_writes.h"
#include "../../bo2zm_pc_profiler.h"

REXCVAR_DEFINE_BOOL(bo2zm_native_im_load_without_memcmp, true, "BO2",
                    "Native renderer: an IM_LOAD whose microcode nobody has written since the last "
                    "check (the constructors and the D3D fetch patcher report it) is taken from the cache without "
                    "comparing it whole with memcmp. Starts by checking each load against the memcmp and turns itself "
                    "off at the first disagreement. false = memcmp on every IM_LOAD, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_native_im_immediate_cache, true, "BO2",
                    "Native renderer: an IM_LOAD_IMMEDIATE (the VS the D3D copies into the ring with "
                    "the patched fetches) byte-for-byte identical to one already seen reuses its flipped copy, its "
                    "identification, its hash and its generation, without flipping or identifying again. Starts by "
                    "checking each hit against the usual path and turns itself off at the first disagreement. false = "
                    "no cache, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


extern std::atomic<uint64_t> g_bo2zm_frames_game;

namespace bo2zm::native {
namespace {


class ReportQueue {
 public:
  ~ReportQueue() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    warning_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
  }


  bool Enqueue(std::string&& line) {
    bool warn = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || no_thread_) {
        return false;
      }
      if (!thread_.joinable()) {
        try {
          thread_ = std::thread([this] { Loop(); });
        } catch (...) {
          no_thread_ = true;
          return false;
        }
      }
      if (queue_.size() >= kMaxLines) {
        ++lost_;
        return true;
      }
      warn = queue_.empty();
      queue_.push_back(std::move(line));
    }
    if (warn) {
      warning_.notify_one();
    }
    return true;
  }

 private:
  static constexpr size_t kMaxLines = 4096;

  void Loop() {
    rex::thread::set_current_thread_name("BO2 reports");
#if REX_PLATFORM_SWITCH
    const bool priority = RexSwitchSetCurrentThreadPriorityOk(REX_SWITCH_PRIO_GUEST);
#else
    const bool priority = true;
#endif
    REXLOG_INFO("[native] deferred reports: thread running{}",
                priority ? "" : " (the kernel did NOT accept priority 0x3B)");
    std::deque<std::string> batch;
    for (;;) {
      uint64_t lost = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        warning_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;
        }
        batch.swap(queue_);
        lost = lost_;
        lost_ = 0;
      }
      for (const std::string& line : batch) {
        REXLOG_INFO("{}", line);
      }
      batch.clear();
      if (lost) {
        REXLOG_WARN("[native] deferred reports: {} lines lost (the queue of {} was full)", lost,
                    kMaxLines);
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable warning_;
  std::deque<std::string> queue_;
  std::thread thread_;
  bool stop_ = false;
  bool no_thread_ = false;
  uint64_t lost_ = 0;
};

}

void ReportDeferred(std::string line) {
  static const bool defer = REXCVAR_GET(bo2zm_native_deferred_reports);
  if (!defer) {
    REXLOG_INFO("{}", line);
    return;
  }
  static ReportQueue queue;
  if (!queue.Enqueue(std::move(line))) {
    REXLOG_INFO("{}", line);
  }
}

}


REXCVAR_DEFINE_INT32(bo2zm_native_ring_priority, 0x2D, "BO2",
                     "Switch: Horizon priority of the ring thread (0x1C-0x3B). 0x2D by default, one step below the "
                     "presenter (0x2C) and two below audio (0x2B). Raising it to 0x2C brings back an older split, in "
                     "which only half the frames reached the screen");


REXCVAR_DEFINE_INT32(bo2zm_native_ring_core, 1, "BO2",
                     "Switch: preferred core of the ring thread (-1 = the default, which is the same as the game "
                     "thread's; 0-2 = a specific one). Not exclusive: the mask is not touched, so the kernel can still "
                     "move it");


REXCVAR_DEFINE_BOOL(bo2zm_native_ring_core_exclusive, false, "BO2",
                    "Switch: besides preferring bo2zm_native_ring_core's core, FORBIDS the others (exclusive mask). It "
                    "is the real pin. Careful: if that core saturates, the ring does not get to run; check the "
                    "migrations/s in the log, which must drop to 0");
REXCVAR_DEFINE_INT32(bo2zm_native_regmem_wait_us, 1000, "BO2",
                     "Native renderer: pause between WAIT_REG_MEM polls in microseconds (exact on the Switch; on "
                     "Windows, below 1000 it only yields)");


REXCVAR_DEFINE_INT32(bo2zm_native_ring_spin_ns_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the ring thread spins this many ns per draw in the arms of A/B "
                     "factor 1 (0 = off)");


#if defined(__SWITCH__)
constexpr int32_t kMenuBlurEveryDefault = 0;
#else
constexpr int32_t kMenuBlurEveryDefault = 1;
#endif
constexpr uint32_t kMenuBlurSecond = 15;
REXCVAR_DEFINE_INT32(bo2zm_native_menu_blur_every, kMenuBlurEveryDefault, "BO2",
                     "Native renderer: after a popup's first frames, the blur behind it runs on one frame in N and the "
                     "others reuse it. 0 = it is kept until the popup closes (the Switch's default); 1 = every frame, "
                     "as the game does (the PC's)")
    .range(0, 1000);
REXCVAR_DEFINE_STRING(bo2zm_native_menu_blur_ps, "", "BO2",
                      "Native renderer: the menu blur's pixel shaders, as microcode hashes (hex, comma-separated). "
                      "Empty = the three the game uses");
REXCVAR_DEFINE_INT32(bo2zm_native_menu_blur_every_alternate_s, 0, "BO2",
                     "Test helper: bo2zm_native_menu_blur_every and 1 (every frame) take turns every N seconds, "
                     "logged as '[native] menu blur'. 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


#if defined(__SWITCH__)
constexpr bool kFrontEndNoPrepassDefault = true;
#else
constexpr bool kFrontEndNoPrepassDefault = false;
#endif
REXCVAR_DEFINE_BOOL(bo2zm_native_front_end_no_prepass, kFrontEndNoPrepassDefault, "BO2",
                    "Native renderer: in the campaign's front-end menus the 3D scene's depth prepass is skipped (on "
                    "by default on the Switch)");


#if defined(__SWITCH__)
constexpr bool kFrontEndReuseDefault = true;
#else
constexpr bool kFrontEndReuseDefault = false;
#endif
constexpr uint32_t kSceneNoisy = 64;
REXCVAR_DEFINE_BOOL(bo2zm_native_front_end_reuse, kFrontEndReuseDefault, "BO2",
                    "Native renderer: in the campaign's front-end menus the 3D scene is drawn again only when its "
                    "view changes, and reused in between (on by default on the Switch)");
REXCVAR_DEFINE_DOUBLE(bo2zm_native_front_end_reuse_move, 0.05, "BO2",
                      "Native renderer: with bo2zm_native_front_end_reuse, a view constant that changes by more than "
                      "this (relative to its size, at least 1) redraws the scene at once")
    .range(0.0, 10.0);
REXCVAR_DEFINE_DOUBLE(bo2zm_native_front_end_reuse_drift, 0.05, "BO2",
                      "Native renderer: with bo2zm_native_front_end_reuse, smaller changes of the view constants than "
                      "this are ignored; changes between this and bo2zm_native_front_end_reuse_move redraw at most "
                      "once in bo2zm_native_front_end_reuse_drift_ms")
    .range(0.0, 10.0);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_drift_ms, 3000, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, a slow drift of the view redraws the scene "
                     "at most this often (ms)")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_settle, 30, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, when the front end opens (or its scene "
                     "changes entirely) the scene is drawn in full for this many still frames before it is reused, so "
                     "the effects that build up over frames on the GPU (exposure, glow) settle first")
    .range(0, 600);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_settle_ms, 8000, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, a new scene (the front end opening or "
                     "loading again) is not kept before this long, while its textures stream in and the landing's "
                     "blur and boot-up run")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_still_ms, 2000, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, the scene is kept only once its view has held "
                     "still for this long after a move")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_settle_move, 10, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, the still frames drawn in full after a camera "
                     "move (the exposure changes less than when the front end opens)")
    .range(0, 600);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_refresh_ms, 0, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, the scene is drawn again after this many "
                     "ms even if its view did not change (0 = only when it changes)")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_diag_frame_s, 0, "BO2",
                     "Native renderer: after these seconds, logs every draw and every copy of one whole frame (0 = no; "
                     "tests only)");
REXCVAR_DEFINE_STRING(bo2zm_native_diag_vertices_ps, "", "BO2",
                      "Native renderer: in the traced frame, for draws with these PS (comma-separated numbers) logs the "
                      "VS fetches and the bytes of their first vertices and texels (tests only)");


REXCVAR_DEFINE_INT32(bo2zm_native_occlusion, 1, "BO2",
                     "Native renderer: the game's occlusion queries (the sun flare). 1 = measured on the GPU, like the "
                     "Xbox 360; 0 = a fake count of 1000 samples (what was there before: the flare is never "
                     "hidden); 2 = a fake count of 0 samples (tests only: never a flare)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_native_diag_constants_ps, "", "BO2",
                      "Native renderer (tests only): PS numbers (comma-separated) whose first 12 constants are logged, "
                      "at most every bo2zm_native_diag_constants_ms");
REXCVAR_DEFINE_INT32(bo2zm_native_diag_constants_ms, 250, "BO2",
                     "Native renderer (tests only): minimum interval between constant logs per PS")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_occlusion_alternate_s, 0, "BO2",
                     "Native renderer (tests only): with bo2zm_native_occlusion = 1 and N > 0, alternates every N "
                     "seconds between measured queries (even spans) and the fake count of 1000 (odd spans), and logs "
                     "each switch, to compare the sun flare in the same session")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_occlusion_scale, 0, "BO2",
                     "Native renderer: samples per pixel used to count the occlusion queries (0 = those of the "
                     "antialiasing mode the game has chosen, as on the Xbox 360; 1-16 = fixed, tests only)")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


namespace bo2zm::guard30 {
void Heartbeat(double ms);
void Report();
}

namespace bo2zm::render_targets {
uint32_t SamplesOriginalModeCurrent(const uint8_t* base);
}

namespace bo2zm::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
using RampOutputContext = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

std::atomic<uint64_t> g_swaps_native{0};


template <typename T>
inline void AddOnlyRing(std::atomic<T>& counter, T how_much = 1) {
  counter.store(counter.load(std::memory_order_relaxed) + how_much, std::memory_order_relaxed);
}

constexpr uint32_t kBaseMmio = 0x7FC80000;


constexpr uint64_t kStopwatchPacketsEvery = 128;


constexpr uint64_t kStopwatchDrawsEvery = 64;
constexpr uint32_t kMaskMmio = 0xFFFF0000;
constexpr uint32_t kSizeMmio = 0x0000FFFF;
constexpr uint32_t kNumRegisters = 0x5003;


constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1GrphPrimarySurface = 0x1844;


constexpr uint32_t kRegRampFirst = 0x1921;
constexpr uint32_t kRegRampIndex = 0x1922;
constexpr uint32_t kRegRampSequential = 0x1923;
constexpr uint32_t kRegRamp30 = 0x1925;
constexpr uint32_t kRegRampMask = 0x1927;
constexpr uint32_t kRegRampLast = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;


constexpr uint32_t kRegFetchFirst = 0x4800;
constexpr uint32_t kRegFetchLast = 0x48BF;


constexpr uint32_t kRegFramingFirst = 0x2080;
constexpr uint32_t kRegFramingLast = 0x2302;
inline bool IsFramingRegister(uint32_t index) {

  if (index - kRegFramingFirst > kRegFramingLast - kRegFramingFirst) {
    return false;
  }
  return index <= 0x2082 ||
         (index >= 0x210F && index <= 0x2114) ||
         (index >= 0x2204 && index <= 0x2206) ||
         index == 0x2302;
}


inline void SpanScalar(uint32_t index, uint32_t* target, const uint8_t* source, uint32_t n, bool& vs,
                         bool& ps, bool& fetch, bool& framing) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t new_value = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
    if (target[i] != new_value) {
      const uint32_t abs = index + i;
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsFramingRegister(abs)) {
        framing = true;
      }
      target[i] = new_value;
    }
  }
}


inline bool CopyWithChange(uint32_t* target, const uint8_t* source, uint32_t n) {
  uint32_t i = 0;
  bool change = false;
#if defined(__aarch64__)
  uint32x4_t mismatch = vdupq_n_u32(0);
  for (; i + 4 <= n; i += 4) {
    const uint32x4_t new_value = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(source + size_t(i) * 4)));
    mismatch = vorrq_u32(mismatch, veorq_u32(new_value, vld1q_u32(target + i)));
    vst1q_u32(target + i, new_value);
  }
  change = vmaxvq_u32(mismatch) != 0;
#endif
  for (; i < n; ++i) {
    const uint32_t new_value = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
    change |= target[i] != new_value;
    target[i] = new_value;
  }
  return change;
}

inline void SpanVector(uint32_t index, uint32_t* target, const uint8_t* source, uint32_t n, bool& vs,
                           bool& ps, bool& fetch, bool& framing) {


  if (n != 0) {
    const uint32_t last = index + n - 1;
    if (index >= 0x4000) {
      if (last < 0x4400) {
        if (CopyWithChange(target, source, n)) {
          vs = true;
        }
        return;
      }
      if (index >= 0x4400 && last < 0x4800) {
        if (CopyWithChange(target, source, n)) {
          ps = true;
        }
        return;
      }
      if (index >= 0x4800 && last <= kRegFetchLast) {
        if (CopyWithChange(target, source, n)) {
          fetch = true;
        }
        return;
      }
      if (index > kRegFetchLast) {
        CopyWithChange(target, source, n);
        return;
      }
    } else if (last < kRegFramingFirst || (index > kRegFramingLast && last < 0x4000)) {
      CopyWithChange(target, source, n);
      return;
    }
  }

  static constexpr uint32_t kSlices[] = {kRegFramingFirst, 0x2083, 0x210F, 0x2115, 0x2204, 0x2207,
                                         0x2302, 0x2303, 0x4000, 0x4400, 0x4800, kRegFetchLast + 1};
  uint32_t i = 0;
  while (i < n) {
    const uint32_t abs = index + i;
    uint32_t end = n;
    for (const uint32_t slice : kSlices) {
      if (slice > abs && slice - index < end) {
        end = slice - index;
      }
    }
    const bool change = CopyWithChange(target + i, source + size_t(i) * 4, end - i);
    if (change) {
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsFramingRegister(abs)) {
        framing = true;
      }
    }
    i = end;
  }
}

constexpr uint32_t kSamplesOcclusion = 1000;
constexpr uint16_t kExtensionMax = 2048 >> 3;
constexpr int kMaxDepthIndirect = 4;
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

uint64_t NanosecondsSince(std::chrono::steady_clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count());
}


struct RingReader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Look() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Look();
    Advance(1);
    return value;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compares(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0:
      return false;
    case 0x1:
      return value < reference;
    case 0x2:
      return value <= reference;
    case 0x3:
      return value == reference;
    case 0x4:
      return value != reference;
    case 0x5:
      return value >= reference;
    case 0x6:
      return value > reference;
    default:
      return true;
  }
}

class NativeRenderSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeRenderSystem() : registers_(kNumRegisters, 0), seen_(kNumRegisters) {}
  ~NativeRenderSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {


      provider_ = CreateNativeVulkanProvider(true);
      if (!provider_) {
        REXLOG_ERROR("[native] Could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] Could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] Native graphics system (C1): the SDK's presenter, without Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kBaseMmio, kMaskMmio, kSizeMmio, this,
                                                        &ReadMmio, &WriteMmio);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] Could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }


    std::unique_ptr<ProgressScreen> progress;
    const auto show = [&](uint64_t done, uint64_t total) {
      if (!progress) {
        progress = std::make_unique<ProgressScreen>(presenter_.get(),
                                                    provider_ ? provider_->vulkan_device() : nullptr);
      }
      progress->Update(done, total);
    };
    auto library_path = rex::filesystem::GetExecutableFolder() / BO2ZM_SHADER_LIBRARY_FILE;
#if REX_PLATFORM_ANDROID
    library_path = rex::GetAndroidPrivateFilesDir() / "game-shaders" / BO2ZM_SHADER_LIBRARY_FILE;
#endif
    const bool library = shaders_.Load(library_path,
                                       presenter_ ? NativeShaders::IndexProgress(show) : nullptr);
    if (progress) {
      progress->Finish();
      progress.reset();
    }
    if (library) {
      ActivateHooks(&shaders_);
    }
    last_report_ = Clock::now();
    last_time_report_ = last_report_;
    start_system_ = last_report_;
    thread_vblank_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return LoopVblank(); }));
    thread_vblank_->set_name("GPU native VSync");
    thread_vblank_->Create();
    thread_ring_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return LoopRing(); }));
    thread_ring_->set_name("GPU native ring");
    thread_ring_->Create();

    ActivateConsumerMarkers(true);
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] Interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {

    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    generation_ring_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] Ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_returned_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    ActivateHooks(nullptr);
    ActivateConsumerMarkers(false);
    if (active_.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_all();
      if (thread_ring_) {
        thread_ring_->Wait(0, 0, 0, nullptr);
        thread_ring_.reset();
      }
      if (thread_vblank_) {
        thread_vblank_->Wait(0, 0, 0, nullptr);
        thread_vblank_.reset();
      }
      Report(true);
    }

    DestroyVulkan();
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:


  static uint32_t ReadMmio(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<NativeRenderSystem*>(context)->ReadRegisterMmio(address);
  }

  static void WriteMmio(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<NativeRenderSystem*>(context)->WriteRegisterMmio(address, value);
  }

  uint32_t ReadRegisterMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        break;
    }
    AnnotateFirstTime("read", r, 0);
    return DvarRecord(r);
  }

  void WriteRegisterMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      g_publications_ring.fetch_add(1, std::memory_order_relaxed);
      pointer_write_.store(value, std::memory_order_release);
      writes_wptr_.fetch_add(1, std::memory_order_relaxed);


      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    } else if (r != kRegD1GrphPrimarySurface) {
      AnnotateFirstTime("write", r, value);
    }
    SaveRegister(r, value);
  }


  void AnnotateRampGamma(uint32_t index, uint32_t value) {
    ++writes_ramp_[index - kRegRampFirst];
    if (index == kRegRampIndex) {
      component_ramp_ = 0;
    } else if (index == kRegRampMask) {
      mask_ramp_ = value & 0b111;
    } else if (index == kRegRampSequential) {
      const uint32_t i = registers_[kRegRampIndex] & 0xFF;
      if (mask_ramp_ & (UINT32_C(1) << (2 - component_ramp_))) {
        ramp_gamma_[i][component_ramp_] = uint16_t((value >> 6) & 0x3FF);
        ++version_ramp_;
      }
      if (++component_ramp_ >= 3) {
        component_ramp_ = 0;
        registers_[kRegRampIndex] = (registers_[kRegRampIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      }
    } else if (index == kRegRamp30) {
      const uint32_t i = registers_[kRegRampIndex] & 0xFF;
      if (mask_ramp_ & 0b001) {
        ramp_gamma_[i][2] = uint16_t(value & 0x3FF);
      }
      if (mask_ramp_ & 0b010) {
        ramp_gamma_[i][1] = uint16_t((value >> 10) & 0x3FF);
      }
      if (mask_ramp_ & 0b100) {
        ramp_gamma_[i][0] = uint16_t((value >> 20) & 0x3FF);
      }
      if (mask_ramp_) {
        ++version_ramp_;
      }
      component_ramp_ = 0;
      registers_[kRegRampIndex] = (registers_[kRegRampIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
    }
  }


  void QueryOcclusion(uint32_t address) {
    using CountSet = xenos::xe_gpu_depth_sample_counts;
    constexpr uint32_t kMark = 0xEDFEFFFF;
    const auto marked = [](const CountSet* c) {
      return uint32_t(c->ZPass_A) == kMark || uint32_t(c->ZPass_B) == kMark || uint32_t(c->ZFail_A) == kMark ||
             uint32_t(c->ZFail_B) == kMark;
    };
    auto* counts = memory_->TranslatePhysical<CountSet*>(address);
    const bool marks_here = marked(counts);
    const bool marks_before = address >= 32 && marked(memory_->TranslatePhysical<CountSet*>(address - 32));
    int32_t mode = REXCVAR_GET(bo2zm_native_occlusion);

    if (const int32_t alternate_s = REXCVAR_GET(bo2zm_native_occlusion_alternate_s); mode == 1 && alternate_s > 0) {
      const auto now = Clock::now();
      if (!occlusion_test_started_) {
        occlusion_test_started_ = true;
        occlusion_test_start_ = now;
      }
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(now - occlusion_test_start_).count();
      const bool fake = (seconds / alternate_s) % 2 == 1;
      if (fake != occlusion_test_fake_) {
        occlusion_test_fake_ = fake;
        if (fake && occlusion_base_ != 0 && targets_) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(occlusion_base_, discarded);
          occlusion_base_ = 0;
        }
        REXLOG_INFO("[native] occlusion test: {}", fake ? "fake (1000)" : "measured");
      }
      if (fake) {
        mode = 0;
      }
    }
    const char* what = nullptr;
    uint32_t written = UINT32_MAX;
    uint64_t measures = 0;
    bool measure = false;
    uint32_t scale = 0;
    if (mode == 1 && targets_) {
      if (occlusion_base_ != 0 && address == occlusion_base_) {
        measure = targets_->FinishOcclusion(address, measures);
        occlusion_base_ = 0;
        scale = ScaleOcclusion();


        written = measure ? uint32_t(std::min<uint64_t>(measures * scale, 0xFFFFFF)) : 0;
        std::memset(counts, 0, sizeof(*counts));
        counts->ZPass_A = written;
        counts->Total_A = written;
        what = measure ? "final" : "end without a measurement";
        if (measure) {
          ++occlusion_counters_[1];
          occlusion_counters_[5] += written;
          occlusion_counters_[6] = std::max<uint64_t>(occlusion_counters_[6], written);
        } else {
          ++occlusion_counters_[2];
        }

        const size_t group = scale > 1 ? 1 : 0;
        ++occlusion_per_target_[group];
        if (measure) {
          occlusion_max_per_target_[group] = std::max(occlusion_max_per_target_[group], written);
        }
        if (group == 1 && warnings_occlusion_scene_ < 12) {
          ++warnings_occlusion_scene_;
          const uint32_t info = DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          REXLOG_INFO("[native] C2 occlusion on scene target {}: structure {:08X}, pitch {}, MSAA {}, scale {}, host "
                      "samples {} ({}), written {}",
                      warnings_occlusion_scene_, address, info & 0x3FFF, (info >> 16) & 0x3, scale, measures,
                      measure ? "measured" : "without a measurement", written);
        }
      } else if (address >= 32 && (marks_before || bases_occlusion_.count(address - 32))) {
        const uint32_t base = address - 32;
        if (occlusion_base_ != 0) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(occlusion_base_, discarded);
          ++occlusion_counters_[3];
        }
        if (bases_occlusion_.size() < 64) {
          bases_occlusion_.insert(base);
        }
        targets_->BeginOcclusion(base);
        occlusion_base_ = base;
        std::memset(counts, 0, sizeof(*counts));
        what = "start";
        ++occlusion_counters_[0];
      }
    }
    if (!what) {

      std::memset(counts, 0, sizeof(*counts));
      if (marks_here) {
        written = mode == 2 ? 0 : kSamplesOcclusion;
        counts->ZPass_A = written;
        counts->Total_A = written;
        ++occlusion_counters_[4];
      }
      what = mode == 1 ? "without a pair" : "fake";
    }
    if (warnings_occlusion_ < 8) {
      ++warnings_occlusion_;
      const uint8_t* base_virtual = memory_->virtual_membase();
      const auto read_be = [base_virtual](uint32_t d) {
        uint32_t v = 0;
        std::memcpy(&v, base_virtual + d, sizeof(v));
        return __builtin_bswap32(v);
      };
      const uint32_t renderer = 0;
      const uint32_t info = DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
      REXLOG_INFO("[native] C2 occlusion {}: ZPD at {:08X} ({}), marks here {} and 32 before {}; host samples {} ({}), "
                  "scale {}, written {}; bins {:016X}/{:016X}; RB_SURFACE_INFO {:08X} (pitch {}, MSAA {}), "
                  "RB_DEPTH_INFO {:08X}; game AA mode {}, quality {}",
                  warnings_occlusion_, address, what, marks_here, marks_before, measures, measure ? "measured" : "without a measurement",
                  scale, written == UINT32_MAX ? -1 : int64_t(written), bin_select_, bin_mask_, info, info & 0x3FFF,
                  (info >> 16) & 0x3, DvarRecord(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                  renderer ? int64_t(read_be(renderer)) : -1, 0u);
    }
  }


  uint32_t ScaleOcclusion() const {
    const int32_t fixed = REXCVAR_GET(bo2zm_native_occlusion_scale);
    if (fixed > 0) {
      return uint32_t(fixed);
    }
    const uint32_t info = DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t samples_target = UINT32_C(1) << std::min<uint32_t>((info >> 16) & 0x3, 2);
    const uint32_t samples_scene =
        (info & 0x3FFF) >= 640 ? bo2zm::render_targets::SamplesOriginalModeCurrent(memory_->virtual_membase()) : 1;
    return std::max(samples_target, samples_scene);
  }

  void AnnotateFirstTime(const char* what, uint32_t index, uint32_t value) {
    if (index >= kNumRegisters || seen_[index].exchange(1, std::memory_order_acq_rel)) {
      return;
    }
    REXLOG_INFO("[native] MMIO register {:04X}: first {} ({:08X})", index, what, value);
  }


  void SaveRegister(uint32_t index, uint32_t value) {
    if (index < kNumRegisters) {


      if (registers_[index] != value) {
        if (index >= kRegFetchFirst && index <= kRegFetchLast) {
          ++generation_fetch_;
        } else if (IsFramingRegister(index)) {
          ++generation_framing_;
        }
      }
      registers_[index] = value;
    }
  }


  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kNumRegisters) {
      return;
    }


    if (index >= 0x4000) {
      if (index < 0x4800) {
        if (registers_[index] != value) {
          ++(index < 0x4400 ? generation_constants_vs_ : generation_constants_ps_);
        } else {
          ++constants_no_change_;
        }
        ++constants_written_;
      } else if (index <= kRegFetchLast) {
        if (registers_[index] != value) {
          ++generation_fetch_;
        } else {
          ++fetch_no_change_;
        }
        ++fetch_written_;
      }
    } else if (IsFramingRegister(index)) {
      if (registers_[index] != value) {
        ++generation_framing_;
      } else {
        ++framing_no_change_;
      }
      ++framing_written_;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
    if (index >= kRegRampFirst && index <= kRegRampLast) {
      AnnotateRampGamma(index, value);
    }
    registers_[index] = value;
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
        index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        BeforeFence();
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
        AddOnlyRing(writes_memory_);
      }
    }
  }


  bool WriteRegistersInBlock(uint32_t index, uint32_t count, RingReader& data) {
    if (registers_in_block_ < 0) {
      registers_in_block_ = REXCVAR_GET(bo2zm_native_block_register_writes) ? 1 : 0;
      REXLOG_INFO("[native] block register writes (bo2zm_native_block_register_writes) = {}",
                  registers_in_block_ ? "SI" : "no");
    }
    if (!registers_in_block_ || count < 2 || index >= kNumRegisters || count > kNumRegisters - index) {
      return false;
    }
    const uint32_t end = index + count;
    const auto due = [index, end](uint32_t since, uint32_t until) { return index < until && since < end; };
    if (due(rex::graphics::XE_GPU_REG_COHER_STATUS_HOST, rex::graphics::XE_GPU_REG_COHER_STATUS_HOST + 1) ||
        due(rex::graphics::XE_GPU_REG_SCRATCH_REG0, rex::graphics::XE_GPU_REG_SCRATCH_REG7 + 1) ||
        due(kRegRampFirst, kRegRampLast + 1)) {
      return false;
    }


    const bool looks_vs = due(0x4000, 0x4400);
    const bool looks_ps = due(0x4400, 0x4800);


    const bool looks_fetch = due(kRegFetchFirst, kRegFetchLast + 1);
    const bool looks_framing = due(kRegFramingFirst, kRegFramingLast + 1);
    const bool looks_values = looks_vs || looks_ps || looks_fetch || looks_framing;
    bool change_vs = false;
    bool change_ps = false;
    bool change_fetch = false;
    bool change_framing = false;

    if (registers_vector_ < 0) {
      registers_vector_ = REXCVAR_GET(bo2zm_native_vector_register_writes) ? 1 : 0;
      REXLOG_INFO("[native] vector register writes (bo2zm_native_vector_register_writes) = {}",
                  registers_vector_ ? "YES, checking the first blocks against the usual path"
                                         : "no");
    }
    const bool vectorial = registers_vector_ > 0;
    const bool verify = vectorial && blocks_verified_ < kBlocksAVerify;
    RingReader data_verification;
    if (verify) {
      data_verification = data;
      copy_verification_.assign(registers_.begin() + index, registers_.begin() + index + count);
    }
    uint32_t index_current = index;
    uint32_t* target = registers_.data() + index;
    uint32_t remaining = count;
    while (remaining) {

      const uint32_t span = data.mask ? std::min(remaining, data.mask + 1 - data.pos) : remaining;
      const uint8_t* source = data.base + size_t(data.pos) * 4;
      if (looks_values) {
        if (vectorial) {
          SpanVector(index_current, target, source, span, change_vs, change_ps, change_fetch,
                         change_framing);
        } else {
          SpanScalar(index_current, target, source, span, change_vs, change_ps, change_fetch,
                       change_framing);
        }
        if (looks_vs || looks_ps) {
          constants_written_ += span;
        }
        if (looks_fetch) {
          fetch_written_ += span;
        }
        if (looks_framing) {
          framing_written_ += span;
        }
      } else if (vectorial) {
        CopyWithChange(target, source, span);
      } else {
        for (uint32_t i = 0; i < span; ++i) {
          target[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
      }
      index_current += span;
      target += span;
      data.Advance(span);
      remaining -= span;
    }
    if (verify) {
      VerifyBlock(index, count, data_verification, looks_values, change_vs, change_ps, change_fetch,
                      change_framing);
    }
    if (change_vs) {
      ++generation_constants_vs_;
    }
    if (change_ps) {
      ++generation_constants_ps_;
    }
    if (change_fetch) {
      ++generation_fetch_;
    }
    if (change_framing) {
      ++generation_framing_;
    }
    if ((looks_vs && !change_vs) || (looks_ps && !change_ps)) {
      ++blocks_constants_no_change_;
    }
    if (looks_vs || looks_ps) {
      ++blocks_constants_;
    }
    return true;
  }


  void VerifyBlock(uint32_t index, uint32_t count, RingReader data, bool looks_values, bool& vs, bool& ps,
                       bool& fetch, bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_framing = false;
    uint32_t done = 0;
    while (done < count) {
      const uint32_t remaining = count - done;
      const uint32_t span = data.mask ? std::min(remaining, data.mask + 1 - data.pos) : remaining;
      SpanScalar(index + done, copy_verification_.data() + done, data.base + size_t(data.pos) * 4, span,
                   ref_vs, ref_ps, ref_fetch, ref_framing);
      data.Advance(span);
      done += span;
    }
    ++blocks_verified_;
    const bool equal_values =
        std::equal(copy_verification_.begin(), copy_verification_.end(), registers_.begin() + index);
    const bool equal_classes =
        !looks_values || (ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_framing == framing);
    if (!equal_values || !equal_classes) {
      REXLOG_ERROR("[native] vector register writes: block {:04X}+{} does NOT match the usual path (values {}, classes "
                   "VS {}/{} PS {}/{} fetch {}/{} framing {}/{}). The vector path is switched OFF.",
                   index, count, equal_values ? "equal" : "DIFFERENT", vs, ref_vs, ps, ref_ps, fetch,
                   ref_fetch, framing, ref_framing);
      std::copy(copy_verification_.begin(), copy_verification_.end(), registers_.begin() + index);
      if (looks_values) {
        vs = ref_vs;
        ps = ref_ps;
        fetch = ref_fetch;
        framing = ref_framing;
      }
      registers_vector_ = 0;
      return;
    }
    if (blocks_verified_ == kBlocksAVerify) {
      REXLOG_INFO("[native] vector register writes: {} blocks checked against the usual path, all equal. The vector "
                  "path stays.",
                  blocks_verified_);
    }
  }


  void ProcessMarker(RingReader& data, uint32_t words) {
    if (words < 2 || (data.Look() & 0xFFFFFF00u) != kMarkerMagic) {
      return;
    }
    const uint32_t header = data.Read();
    const uint32_t mode = header & 0x0Fu;
    const uint32_t mode_draw = (header >> 4) & 0x0Fu;
    const uint32_t sequence = data.Read();
    uint32_t n = words - 2;


    const uint8_t* p = data.base + size_t(data.pos) * 4;
    if (data.mask && data.pos + n > data.mask + 1) {
      marker_plane_.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&marker_plane_[i], data.base + size_t((data.pos + i) & data.mask) * 4, 4);
      }
      p = reinterpret_cast<const uint8_t*>(marker_plane_.data());
    }


    DrawRecord draw;
    if (mode_draw != 0) {
      if ((mode_draw != kDrawApply && mode_draw != kDrawCheck) || n < kWordsDraw ||
          !ReadMarkerDraw(p, draw)) {
        MarkerBad(mode, sequence, header);
        return;
      }
      p += size_t(kWordsDraw) * 4;
      n -= kWordsDraw;
    }

    uint32_t spans = 0;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t count = t >> 16;
      if (count >= n - i || !DumpSpan(t & 0xFFFFu, count)) {
        MarkerBad(mode, sequence, t);
        return;
      }
      i += 1 + count;
      ++spans;
    }
    if (mode > kMarkerCheck || (mode == 0 && (n != 0 || mode_draw == 0))) {
      MarkerBad(mode, sequence, header);
      return;
    }


    if (mode_draw != 0) {
      draw_marker_ = draw;
      draw_marker_mode_ = mode_draw;
    }
    if (mode == kMarkerCheck) {
      CheckMarker(p, n, sequence);
      return;
    }
    if (mode == 0) {
      return;
    }


    const bool fast =
        registers_in_block_ > 0 && registers_vector_ > 0 && blocks_verified_ >= kBlocksAVerify;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t count = t >> 16;
      if (fast && count >= 2 && marker_fast_ > 0) {
        ApplySpanFast(index, count, p + size_t(i + 1) * 4);
      } else {

        ++marker_spans_slow_;
        RingReader linear;
        linear.base = p;
        linear.pos = i + 1;
        linear.end = n;
        if (count < 2 || !WriteRegistersInBlock(index, count, linear)) {
          for (uint32_t k = 0; k < count; ++k) {
            WriteRegister(index + k, rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4));
          }
        }
      }
      i += 1 + count;
    }
    ++markers_applied_;
    marker_spans_ += spans;
    marker_words_ += n - spans;
  }

  void MarkerBad(uint32_t mode, uint32_t sequence, uint32_t word) {
    ++markers_bad_;
    if (warnings_marker_ < 8) {
      ++warnings_marker_;
      REXLOG_ERROR("[native] D3D marker {} MALFORMED (mode {}, word {:08X}): not applied, and the game thread switches "
                   "the marker path off",
                   sequence, mode, word);
    }
    AnnotateCheckMarker(false, sequence, 0, 0, word);
  }

  void CheckMarker(const uint8_t* p, uint32_t n, uint32_t sequence) {
    ++markers_checked_;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t count = t >> 16;
      for (uint32_t k = 0; k < count; ++k) {
        const uint32_t value = rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4);
        if (registers_[index + k] != value) {
          ++markers_different_;
          if (warnings_marker_ < 8) {
            ++warnings_marker_;
            REXLOG_ERROR("[native] D3D marker {}: register {:04X} is {:08X} after its packets and {:08X} in the marker. "
                         "The game thread switches the marker path off",
                         sequence, index + k, registers_[index + k], value);
          }
          AnnotateCheckMarker(false, sequence, index + k, registers_[index + k], value);
          return;
        }
      }
      i += 1 + count;
    }
    AnnotateCheckMarker(true, sequence, 0, 0, 0);
  }


  void ApplySpanFast(uint32_t index, uint32_t count, const uint8_t* source) {
    uint32_t* target = registers_.data() + index;
    const bool verify = spans_fast_verified_ < kSpansFastAVerify;
    if (verify) {
      copy_marker_.assign(target, target + count);
    }
    bool vs = false;
    bool ps = false;
    bool fetch = false;
    bool framing = false;
    const uint32_t end = index + count;
    if (index >= 0x4000) {

      const bool change = CopyWithChange(target, source, count);
      if (index < 0x4400) {
        vs = change;
      } else if (index < 0x4800) {
        ps = change;
      } else if (index <= kRegFetchLast) {
        fetch = change;
      }
    } else {


      uint32_t e0 = end;
      uint32_t e1 = end;
      if (index >= 0x2100 && index < 0x2180) {
        e0 = 0x210F;
        e1 = 0x2115;
      } else if (index >= 0x2200 && index < 0x2280) {
        e0 = 0x2204;
        e1 = 0x2207;
      } else if (index >= 0x2300 && index < 0x2380) {
        e0 = 0x2302;
        e1 = 0x2303;
      }
      e0 = std::min(std::max(e0, index), end);
      e1 = std::min(std::max(e1, e0), end);
      CopyWithChange(target, source, e0 - index);
      framing = CopyWithChange(target + (e0 - index), source + size_t(e0 - index) * 4, e1 - e0);
      CopyWithChange(target + (e1 - index), source + size_t(e1 - index) * 4, end - e1);
    }
    if (verify) {
      VerifySpanMarker(index, count, source, vs, ps, fetch, framing);
    }

    if (index >= 0x4000 && index < 0x4800) {
      constants_written_ += count;
      ++blocks_constants_;
      if (!(index < 0x4400 ? vs : ps)) {
        ++blocks_constants_no_change_;
      }
    } else if (index >= kRegFetchFirst && index <= kRegFetchLast) {
      fetch_written_ += count;
    } else if (index <= kRegFramingLast && end > kRegFramingFirst) {
      framing_written_ += count;
    }
    if (vs) {
      ++generation_constants_vs_;
    }
    if (ps) {
      ++generation_constants_ps_;
    }
    if (fetch) {
      ++generation_fetch_;
    }
    if (framing) {
      ++generation_framing_;
    }
  }


  void VerifySpanMarker(uint32_t index, uint32_t count, const uint8_t* source, bool& vs, bool& ps, bool& fetch,
                              bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_framing = false;
    SpanScalar(index, copy_marker_.data(), source, count, ref_vs, ref_ps, ref_fetch, ref_framing);
    ++spans_fast_verified_;
    const bool equal = std::equal(copy_marker_.begin(), copy_marker_.end(), registers_.begin() + index) &&
                         ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_framing == framing;
    if (!equal) {
      REXLOG_ERROR("[native] D3D marker: the fast path of span {:04X}+{} does NOT match the usual one (classes VS {}/{} "
                   "PS {}/{} fetch {}/{} framing {}/{}). The fast path is switched OFF.",
                   index, count, vs, ref_vs, ps, ref_ps, fetch, ref_fetch, framing, ref_framing);
      std::copy(copy_marker_.begin(), copy_marker_.end(), registers_.begin() + index);
      vs = ref_vs;
      ps = ref_ps;
      fetch = ref_fetch;
      framing = ref_framing;
      marker_fast_ = 0;
      return;
    }
    if (spans_fast_verified_ == kSpansFastAVerify) {
      REXLOG_INFO("[native] D3D marker: {} fast-path spans checked against the usual one, all equal. The fast path "
                  "stays.",
                  spans_fast_verified_);
    }
  }

  uint32_t DvarRecord(uint32_t index) const { return index < kNumRegisters ? registers_[index] : 0; }


  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t sorted = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &sorted, sizeof(sorted));
    AddOnlyRing(writes_memory_);
  }


  void Interrupt(uint32_t source_kind, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source_kind, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);
    WarnProgressRing();
  }

  int LoopVblank() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();

      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int LoopRing() {
    bo2zm::memcpy_callers::MarkThread();
    bo2zm::android_perf::RegisterCurrentThread();


#if REX_PLATFORM_SWITCH
    {
      const int32_t priority = REXCVAR_GET(bo2zm_native_ring_priority);
      if (priority >= 0x1C && priority <= 0x3B) {
        RexSwitchSetCurrentThreadPriority(int(priority));
        REXLOG_INFO("[native] ring thread at priority {:#x} (the presenter runs at 0x2C and has to be able to preempt "
                    "it)", uint32_t(priority));
      }


      const int core_before = RexSwitchCurrentCore();
      const int32_t core = REXCVAR_GET(bo2zm_native_ring_core);
      const bool exclusive = core >= 0 && REXCVAR_GET(bo2zm_native_ring_core_exclusive);
      if (core >= -1 && core <= 2) {
        const bool set = exclusive ? RexSwitchPinCurrentThreadToCore(int(core))
                                      : RexSwitchSetCurrentThreadCore(int(core));
        REXLOG_INFO("[native] ring thread: core {} {} ({}); it was on {} and now runs on {}",
                    core, exclusive ? "EXCLUSIVE (reduced mask)" : "preferred",
                    set ? "accepted" : "REJECTED by the kernel", core_before,
                    RexSwitchCurrentCore());
      }
      core_ring_ = RexSwitchCurrentCore();
    }
#endif


    if (bo2zm::native::precompile::Decide(provider_ ? provider_->vulkan_device() : nullptr,
                                          shaders_.loaded() ? &shaders_ : nullptr)) {
      if (EnsureTargets()) {
        targets_->Precompile();
      }
      bo2zm::native::precompile::End();
    }
    uint32_t read = 0;
    uint32_t generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        const auto before_no_job = Clock::now();
        const bool with_signal = ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 pointer_write_.load(std::memory_order_acquire) != read ||
                 generation_ring_.load(std::memory_order_acquire) != generation;
        });
        bo2zm::waits::Add(bo2zm::waits::kRingNoJob, NanosecondsSince(before_no_job));

        ++loops_ring_;
        if (!with_signal) {
          ++waits_ring_exhausted_;
        }
#if REX_PLATFORM_SWITCH


        {
          const int now = RexSwitchCurrentCore();
          if (now != core_ring_) {
            core_ring_ = now;
            ++migrations_ring_;
          }
        }
#endif
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report(false);
      const uint32_t gen = generation_ring_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = pointer_write_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        continue;
      }
      ++loops_ring_with_data_;
      RingReader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      const auto start_ring = Clock::now();
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {

          break;
        }
      }
      {
        const uint64_t ns_ring = NanosecondsSince(start_ring);
        time_ring_ns_ += ns_ring;
        AddOnlyRing(bo2zm::waits::g_ns_ring_working, ns_ring);
      }
      read = reader.pos;


      if (targets_) {
        targets_->WaitUploads();
      }
      const uint32_t to_return = read_returned_.load(std::memory_order_acquire);
      if (to_return) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(to_return), read);
      }

      WarnProgressRing();
    }
    return 0;
  }


  bool Packet(RingReader& reader, int depth) {
    const uint32_t packet = reader.Look();
    uint32_t load = 0;
    switch (packet >> 30) {
      case 0:
        load = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0;
        break;
      case 1:
        load = 2;
        break;
      case 2:
        load = 0;
        break;
      default:
        load = ((packet >> 16) & 0x3FFF) + 1;
        break;
    }
    if (reader.Pending() < load + 1) {
      return false;
    }
    reader.Advance(1);
    AddOnlyRing(packets_);
    RingReader data = reader;
    reader.Advance(load);
    if (!packet) {
      return true;
    }


    const bool time = (++stopwatch_packets_ & (kStopwatchPacketsEvery - 1)) == 0;
    const auto start = time ? Clock::now() : Clock::time_point{};
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool a_register = (packet >> 15) & 0x1;
        if (a_register || !WriteRegistersInBlock(index, load, data)) {
          for (uint32_t i = 0; i < load; ++i) {
            WriteRegister(a_register ? index : index + i, data.Read());
          }
          words_loose_ += load;
        } else {
          words_block_ += load;
        }
        if (time) {
          time_registers_ns_ += NanosecondsSince(start) * kStopwatchPacketsEvery;
        }
        ++count_registers_;
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        if (time) {
          time_registers_ns_ += NanosecondsSince(start) * kStopwatchPacketsEvery;
        }
        ++count_registers_;
        break;
      }
      case 2:
        break;
      default: {
        const uint32_t opcode = (packet >> 8) & 0x7F;
        const bool wait = opcode == xenos::PM4_WAIT_REG_MEM;
        const auto start_op = wait && !time ? Clock::now() : start;
        PacketType3(data, packet, load, depth);

        if (opcode != xenos::PM4_INDIRECT_BUFFER && opcode != xenos::PM4_INDIRECT_BUFFER_PFD) {
          if (wait) {
            time_opcode_ns_[opcode] += NanosecondsSince(start_op);
          } else if (time) {
            time_opcode_ns_[opcode] += NanosecondsSince(start) * kStopwatchPacketsEvery;
          }
          ++count_opcode_[opcode];
        }
        break;
      }
    }
    return true;
  }


  void SpinRing() {
    const int arm = bo2zm::benchmark::AbArmCached();
    if (arm < 0 || !(arm & 1)) {
      return;
    }
    const auto end = Clock::now() + std::chrono::nanoseconds(ring_spin_ns_);
    while (Clock::now() < end) {
    }
  }

  void PacketType3(RingReader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    AddOnlyRing(opcodes_[opcode]);
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        BeforeFence();
        const uint32_t cpus = data.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP: {

        if (words >= 4) {
          data.Read();
          swap_frontbuffer_.store(data.Read(), std::memory_order_relaxed);
          swap_width_.store(data.Read(), std::memory_order_relaxed);
          swap_height_.store(data.Read(), std::memory_order_relaxed);
        }
        TraceSwap();
        Present();
        AnnotateGameAhead();
        break;
      }
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxDepthIndirect) {
          BufferIndirect(address, length, depth + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto start_regmem = Clock::now();
        const auto limit = start_regmem + kWaitRegMemMax;
        uint32_t loops = 0;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (DvarRecord(poll) & UINT32_C(0x80000000))) {
            SaveRegister(poll, 0);
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : DvarRecord(poll);
          if (Compares(info, value & mask, reference)) {
            break;
          }


          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_exhausted_.fetch_add(1, std::memory_order_relaxed) == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not satisfied ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          const int32_t pause_us = REXCVAR_GET(bo2zm_native_regmem_wait_us);
          rex::thread::Sleep(std::chrono::microseconds(pause_us > 50 ? pause_us : 50));
          ++loops;
        }


        if ((info & 0x10) || poll != rex::graphics::XE_GPU_REG_COHER_STATUS_HOST ||
            g_syncs_on_flush_waits.load(std::memory_order_relaxed)) {
          g_syncs_ring.fetch_add(1, std::memory_order_relaxed);
        }
        bo2zm::waits::Add(bo2zm::waits::kRingRegMem, NanosecondsSince(start_regmem));
        const uint64_t key_wait = (uint64_t(info & 0x10) << 32) | poll;
        if (waits_.size() < 64 || waits_.count(key_wait)) {
          Wait& wait = waits_[key_wait];
          ++wait.times;
          wait.loops += loops;
          wait.reference = reference;
          wait.mask = mask;
          wait.info = info;
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read();
        const uint32_t y = data.Read();
        const uint32_t o = data.Read();
        uint32_t value = DvarRecord(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? DvarRecord(y & 0x1FFF) : y;
        value |= ((info >> 30) & 0x1) ? DvarRecord(o & 0x1FFF) : o;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t reg_index = data.Read();
        const uint32_t address = data.Read();
        BeforeFence();
        WriteMemory(address, DvarRecord(reg_index));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        BeforeFence();
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t target = data.Read();
        const uint32_t datum = data.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : DvarRecord(poll);
        if (Compares(info, value & mask, reference)) {
          if (info & 0x100) {
            BeforeFence();
            WriteMemory(target, datum);
          } else {
            WriteRegister(target, datum);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        BeforeFence();
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1)
                                       ? counter_.load(std::memory_order_relaxed)
                                       : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

        const uint16_t extension[] = {0, kExtensionMax, 0, kExtensionMax, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extension); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extension[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        const uint32_t address = DvarRecord(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (!address) break;
        QueryOcclusion(address);
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;
          case 1: base = 0x4800; break;
          case 2: base = 0x4900; break;
          case 3: base = 0x4908; break;
          case 4: base = 0x2000; break;
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) {
            WriteRegister(index + i - 1, data.Read());
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (words < 2) break;
        const uint64_t height = data.Read();
        const uint64_t low = data.Read();
        bin_mask_ = (height << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (words < 2) break;
        const uint64_t height = data.Read();
        const uint64_t low = data.Read();
        bin_select_ = (height << 32) | low;
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {

        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        for (uint32_t i = 1; i < words; ++i) {
          WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {

        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t amount = data.Read() & 0xFFF;
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;
          case 1: base = 0x4800; break;
          case 2: base = 0x4900; break;
          case 3: base = 0x4908; break;
          case 4: base = 0x2000; break;
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < amount; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {


        if (words < 1) break;
        const uint32_t datum = data.Read();
        const uint32_t id = datum & 0x3F;
        if (!(datum & 0x100)) {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg_index = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                            : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          const uint32_t bit = UINT32_C(1) << (id < 32 ? id : id - 32);
          SaveRegister(reg_index, DvarRecord(reg_index) | bit);
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        if (ring_spin_ns_ > 0) [[unlikely]] {
          SpinRing();
        }

        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) {
          data.Read();
        }
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);


        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (words < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        AddOnlyRing(draws_);


        AddOnlyRing(bo2zm::waits::g_draws);


        if ((DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) ==
            uint32_t(xenos::EdramMode::kCopy)) {
          AddOnlyRing(copies_);
          last_copy_target_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE),
                                      std::memory_order_relaxed);
          last_copy_control_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_CONTROL),
                                      std::memory_order_relaxed);
          last_copy_info_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_DEST_INFO),
                                   std::memory_order_relaxed);
          last_copy_pitch_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_DEST_PITCH),
                                    std::memory_order_relaxed);
          bool skip_copy = MenuBlurSkipCopy();
          if (!skip_copy) {
            skip_copy = SceneReuseCopy();
          }
          if (!skip_copy) {
            DoCopy();
          }
        } else {
          CountDrawShaders();
          PairDraw();
          TraceDraw();
          if (SceneReuseDraw() || MenuBlurSkipDraw() || SkipPrepassDraw()) {
            break;
          }


          if (vs_draw_ &&
              (ps_draw_ || (DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5)) {
            DrawNative();
          } else if (!ClearD3D() && !ExecuteMemexportBo2zm()) {
            DiagDrawNoShader();
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {

        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        const uint8_t* source = memory_->TranslatePhysical(address_type & ~uint32_t(0x3));
        if ((address_type & 0x3) == 1) ps_microcode_bo2zm_ = size;
        if (size && (address_type & 0x3) <= 1) {
          LoadShaderCached(address_type, size, source);
          break;
        }
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
        IdentifyShader(address_type & 0x3);
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {

        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        if (size > words - 2) break;
        if ((type & 0x3) == 1) {
          ps_microcode_bo2zm_ = size;
          if (size <= ps_immediate_bo2zm_.size()) {
            RingReader copy = data;
            for (uint32_t i = 0; i < size; ++i) {
              ps_immediate_bo2zm_[i] = copy.Read();
            }
          }
        }
        LoadImmediate(data, type, size);
        break;
      }
      case xenos::PM4_NOP:


        ProcessMarker(data, words);
        break;
      default:

        break;
    }
  }


  void DiagDrawNoShader() {
    if (!bo2zm::native::LogVertexShaderMisses() || diag_no_shader_ >= 3000) {
      return;
    }
    namespace g = rex::graphics;

    const uint32_t control_z = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL);
    if (DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) == 0 && !(control_z & 0x4) && !(control_z & 0x1)) {
      return;
    }
    {
      const uint64_t key = (uint64_t(DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO)) << 32) ^
                             (uint64_t(control_z) << 8) ^ DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) ^
                             (uint64_t(DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0xFF) << 56);
      if (!diag_no_shader_seen_.insert(key).second) {
        return;
      }
    }
    ++diag_no_shader_;
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    std::string constants;
    for (uint32_t k = 0; k < 4; ++k) {
      constants += fmt::format(" vc{}=({:.4g},{:.4g},{:.4g},{:.4g})", k, float_value(DvarRecord(0x4000 + k * 4)),
                                float_value(DvarRecord(0x4001 + k * 4)), float_value(DvarRecord(0x4002 + k * 4)),
                                float_value(DvarRecord(0x4003 + k * 4)));
    }
    for (uint32_t k = 0; k < 2; ++k) {
      constants += fmt::format(" pc{}=({:.4g},{:.4g},{:.4g},{:.4g})", k, float_value(DvarRecord(0x4400 + k * 4)),
                                float_value(DvarRecord(0x4401 + k * 4)), float_value(DvarRecord(0x4402 + k * 4)),
                                float_value(DvarRecord(0x4403 + k * 4)));
    }
    const uint32_t f0 = DvarRecord(0x4800), f1 = DvarRecord(0x4801);
    std::string vertices;
    if ((f0 & 0x3) == 3) {
      const uint8_t* data = memory_->TranslatePhysical(f0 & 0x1FFFFFFC);
      for (uint32_t i = 0; i < 12; ++i) {
        vertices += fmt::format(" {:.4g}", float_value(rex::memory::load_and_swap<uint32_t>(data + i * 4)));
      }
    }
    REXLOG_INFO("[native] BO2 draw without a shader: vs {} ({} words) ps {} ({} words) start {:08X} mode {} z {:08X} "
                "mask {:08X} color {:08X} depth {:08X} surface {:08X} vte {:08X} stencil {:08X} clear z {:08X} color "
                "{:08X} fetch0 {:08X} {:08X};{} | vertices:{}",
                vs_draw_ ? int(vs_draw_->number) : -1, vs_microcode_.size(),
                ps_draw_ ? int(ps_draw_->number) : -1, ps_microcode_bo2zm_,
                DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR), DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7,
                DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL), DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO), DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO),
                DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO), DvarRecord(g::XE_GPU_REG_PA_CL_VTE_CNTL),
                DvarRecord(g::XE_GPU_REG_RB_STENCILREFMASK), DvarRecord(g::XE_GPU_REG_RB_DEPTH_CLEAR),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_CLEAR), f0, f1, constants, vertices);
  }
  uint32_t diag_no_shader_ = 0;
  std::set<uint64_t> diag_no_shader_seen_;


  bool VsExportsMemory() const {
    bool exports = false;
    for (size_t i = 0; i + 2 < vs_microcode_.size() && i < 3 * 64 && !exports; i += 3) {
      const uint32_t w0 = vs_microcode_[i], w1 = vs_microcode_[i + 1], w2 = vs_microcode_[i + 2];
      (void)w0;
      const uint32_t high[2] = {w1 & 0xFFFF, w2 >> 16};
      bool end = false;
      for (uint32_t height : high) {
        const uint32_t opcode = (height >> 12) & 0xF;
        if (opcode == 12 && ((height >> 9) & 0x3) == 3) {
          exports = true;
        }
        end |= opcode == 2 || opcode == 4;
      }
      if (end) {
        break;
      }
    }
    return exports;
  }


  bool ExecuteMemexportBo2zm() {
    if (vs_microcode_.empty() || !VsExportsMemory()) {
      return false;
    }
    DiagMemexportBo2zm();
    static const bool active = REXCVAR_GET(bo2zm_native_memexport);
    if (!active) {
      return true;
    }
    namespace g = rex::graphics;

    static const bool direct_active = REXCVAR_GET(bo2zm_native_memexport_fast);
    static const bool verify = REXCVAR_GET(bo2zm_native_memexport_verify);
    const uint64_t hash = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    const uint32_t copy = !direct_active                     ? 0
                           : hash == kMemexportCopy64Bytes  ? 1
                           : hash == kMemexportCopy128Bytes ? 2
                                                               : 0;
    const auto start = std::chrono::steady_clock::now();
    if (!copy || verify) {
      if (!interpreter_bo2zm_) {
        registers_interpreter_bo2zm_ = std::make_unique<g::RegisterFile>();
        interpreter_bo2zm_ = std::make_unique<g::ShaderInterpreter>(*registers_interpreter_bo2zm_, *memory_);
      }
      std::memcpy(registers_interpreter_bo2zm_->values, registers_.data(),
                  std::min(registers_.size(), g::RegisterFile::kRegisterCount) * sizeof(uint32_t));
      interpreter_bo2zm_->SetShader(g::xenos::ShaderType::kVertex, vs_microcode_.data());
    }
    bo2zm::memexport::Sink sink(memory_->TranslatePhysical(0));
    sink.tracing = memexport_done_ < 2;
    const uint32_t initiator = DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    const uint32_t num_indices = initiator >> 16;
    const uint32_t source_kind = (initiator >> 6) & 0x3;
    const uint32_t displacement = DvarRecord(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
    const uint32_t min = DvarRecord(g::XE_GPU_REG_VGT_MIN_VTX_INDX) & 0xFFFFFF;
    const uint32_t max = DvarRecord(g::XE_GPU_REG_VGT_MAX_VTX_INDX) & 0xFFFFFF;

    const uint32_t sz_dma = DvarRecord(g::XE_GPU_REG_VGT_DMA_SIZE);
    const bool indices32 = (initiator >> 11) & 0x1;
    auto endian_indices = g::xenos::Endian(sz_dma >> 30);
    if (!indices32) {
      endian_indices = endian_indices == g::xenos::Endian::k8in32    ? g::xenos::Endian::k8in16
                       : endian_indices == g::xenos::Endian::k16in32 ? g::xenos::Endian::kNone
                                                                      : endian_indices;
    }
    const uint32_t base_dma = DvarRecord(g::XE_GPU_REG_VGT_DMA_BASE) & ~uint32_t(indices32 ? 3 : 1);
    const uint8_t* indices = source_kind == 0 ? memory_->TranslatePhysical(base_dma) : nullptr;

    const auto index_of = [&](uint32_t i, uint32_t& index) {
      index = i;
      if (source_kind == 0) {
        if (i >= (sz_dma & 0xFFFFFF)) {
          index = 0;
        } else if (indices32) {
          uint32_t v;
          std::memcpy(&v, indices + size_t(i) * 4, 4);
          index = g::xenos::GpuSwap(v, endian_indices) & 0xFFFFFF;
        } else {
          uint16_t v;
          std::memcpy(&v, indices + size_t(i) * 2, 2);
          index = g::xenos::GpuSwap(uint32_t(v), endian_indices) & 0xFFFF;
        }
      } else if (source_kind != 2) {
        return false;
      }
      index = std::min(max, std::max(min, (index + displacement) & 0xFFFFFF));
      return true;
    };
    const auto interpret = [&](bo2zm::memexport::Sink& target) {
      interpreter_bo2zm_->SetExportSink(&target);
      uint32_t index;
      for (uint32_t i = 0; i < num_indices && index_of(i, index); ++i) {
        interpreter_bo2zm_->temp_registers()[0] = float(index);
        interpreter_bo2zm_->Execute();
        target.End();
      }
      interpreter_bo2zm_->SetExportSink(nullptr);
    };
    if (!copy) {
      interpret(sink);
    } else {
      uint32_t index;
      for (uint32_t i = 0; i < num_indices && index_of(i, index); ++i) {
        CopyMemexportBo2zm(copy == 2, index, sink);
      }
      ++memexport_direct_;
      if (verify) {
        VerifyMemexportBo2zm(sink, interpret);
      }
    }
    ++memexport_done_;

    g_syncs_ring.fetch_add(1, std::memory_order_relaxed);

    if (sink.dir_min < sink.dir_max && bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed)) {
      bo2zm::page_writes::MarkPhysical(sink.dir_min, sink.dir_max - sink.dir_min);
    }
    memexport_elements_ += sink.elements;
    memexport_ns_ += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    if (memexport_done_ <= 2) {
      REXLOG_INFO("[native] BO2 memexport trace: c255 {:08X} {:08X} {:08X} {:08X} c254 {:08X} {:08X};{}",
                  DvarRecord(0x4000 + 255 * 4), DvarRecord(0x4001 + 255 * 4), DvarRecord(0x4002 + 255 * 4),
                  DvarRecord(0x4003 + 255 * 4), DvarRecord(0x4000 + 254 * 4), DvarRecord(0x4001 + 254 * 4),
                  sink.trace);
    }
    if (memexport_done_ <= 3 || memexport_done_ % 2000 == 0 || sink.rejected || sink.format_odd) {
      REXLOG_INFO("[native] BO2 memexport done on the CPU: draw {} ({} vertices, source {}): {} elements at "
                  "{:08X}-{:08X}, {} outside the count, {} eA rejected, format not handled {}; in total {} elements, "
                  "{:.1f} ms ({} without the interpreter)",
                  memexport_done_, num_indices, source_kind, sink.elements,
                  sink.dir_min == UINT32_MAX ? 0 : sink.dir_min, sink.dir_max, sink.outside,
                  sink.rejected, sink.format_odd, memexport_elements_, double(memexport_ns_) / 1e6,
                  memexport_direct_);
    }
    return true;
  }
  std::unique_ptr<rex::graphics::RegisterFile> registers_interpreter_bo2zm_;
  std::unique_ptr<rex::graphics::ShaderInterpreter> interpreter_bo2zm_;
  uint64_t memexport_done_ = 0;
  uint64_t memexport_direct_ = 0;
  uint64_t memexport_verified_ = 0;
  uint64_t memexport_different_ = 0;


  static constexpr uint64_t kMemexportCopy64Bytes = 0x682419C7A651D9A2ull;
  static constexpr uint64_t kMemexportCopy128Bytes = 0x77B7DDF40EBDA6CBull;


  void CopyMemexportBo2zm(bool of_128, uint32_t index, bo2zm::memexport::Sink& sink) {
    namespace g = rex::graphics;


    g::xenos::xe_gpu_vertex_fetch_t vf;
    vf.dword_0 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2 * 95);
    vf.dword_1 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2 * 95 + 1);
    const auto constant = [this](uint32_t c, uint32_t k) {
      const uint32_t bits = DvarRecord(0x4000 + c * 4 + k);
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    const uint32_t* guest_memory = reinterpret_cast<const uint32_t*>(memory_->physical_membase());
    const uint32_t blocks = of_128 ? 16 : 8;
    const uint32_t first = (of_128 ? 64 : 16) * index + vf.address;
    const uint32_t end = vf.address + vf.size;
    uint16_t r[16][4];
    for (uint32_t k = 0; k < blocks; ++k) {
      uint32_t d[2];
      for (uint32_t j = 0; j < 2; ++j) {
        const uint32_t a = first + 2 * k + j;
        d[j] = a >= vf.address && a < end ? g::xenos::GpuSwap(guest_memory[a], vf.endian) : 0;
      }
      r[k][0] = uint16_t(d[0] >> 16);
      r[k][1] = uint16_t(d[0]);
      r[k][2] = uint16_t(d[1] >> 16);
      r[k][3] = uint16_t(d[1]);
    }
    const float vertex = float(index);
    float groups[4];
    uint32_t n = 2;
    if (of_128) {
      const float base = vertex * constant(254, 0);
      groups[0] = base;
      groups[1] = constant(255, 2) + base;
      groups[2] = constant(255, 3) + base;
      groups[3] = constant(254, 1) + base;
      n = 4;
    } else {
      groups[0] = constant(255, 3) * vertex;
      groups[1] = constant(255, 2) + groups[0];
    }
    const float scale[4] = {constant(255, 0), constant(255, 1), constant(255, 0), constant(255, 0)};
    const float c0[4] = {constant(0, 0), constant(0, 1), constant(0, 2), constant(0, 3)};
    for (uint32_t group = 0; group < n; ++group) {
      sink.AllocExport(g::ucode::AllocType::kMemory, 2);
      float ea[4];
      for (uint32_t i = 0; i < 4; ++i) {
        ea[i] = groups[group] * scale[i] + c0[i];
      }
      uint32_t a[4];
      std::memcpy(a, ea, sizeof(a));
      if (sink.Integers16(a, &r[group * 4])) {
        continue;
      }
      sink.Export(g::ucode::ExportRegister(32), ea, 0xF);
      for (uint32_t e = 0; e < 4; ++e) {
        const uint16_t* b = r[group * 4 + e];
        const float values[4] = {float(b[0]), float(b[1]), float(b[2]), float(b[3])};
        sink.Export(g::ucode::ExportRegister(33 + e), values, 0xF);
      }
    }
    sink.End();
  }


  template <typename Interpret>
  void VerifyMemexportBo2zm(const bo2zm::memexport::Sink& direct, Interpret&& interpret) {
    const uint8_t* physical = memory_->TranslatePhysical(0);
    std::vector<uint8_t> copy;
    if (direct.dir_min < direct.dir_max) {
      copy.assign(physical + direct.dir_min, physical + direct.dir_max);
    }
    bo2zm::memexport::Sink reference(memory_->TranslatePhysical(0));
    interpret(reference);
    ++memexport_verified_;
    const bool same_range = reference.dir_min == direct.dir_min && reference.dir_max == direct.dir_max &&
                             reference.elements == direct.elements && reference.outside == direct.outside;
    const bool same_bytes = copy.empty() || std::memcmp(copy.data(), physical + direct.dir_min, copy.size()) == 0;
    if ((!same_range || !same_bytes) && ++memexport_different_ <= 8) {
      REXLOG_ERROR("[native] BO2 direct memexport: MISMATCH with the interpreter in draw {}: range {:08X}-{:08X} and {} "
                   "elements ({} outside) versus {:08X}-{:08X} and {} ({} outside); bytes {}",
                   memexport_done_ + 1, direct.dir_min, direct.dir_max, direct.elements, direct.outside,
                   reference.dir_min, reference.dir_max, reference.elements, reference.outside,
                   same_bytes ? "equal" : "different");
    }
    if (memexport_verified_ % 500 == 0) {
      REXLOG_INFO("[native] BO2 direct memexport: {} draws compared with the interpreter, {} with differences",
                  memexport_verified_, memexport_different_);
    }
  }
  uint64_t memexport_elements_ = 0;
  uint64_t memexport_ns_ = 0;


  void DiagMemexportBo2zm() {
    if (!bo2zm::native::LogVertexShaderMisses()) {
      return;
    }
    ++memexport_draws_;
    const uint64_t hash = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    if (!memexport_seen_.insert(hash).second || memexport_seen_.size() > 64) {
      if (memexport_draws_ % 2000 == 0) {
        REXLOG_INFO("[native] BO2 memexport: {} draws not handled ({} different VS)", memexport_draws_,
                    memexport_seen_.size());
      }
      return;
    }
    namespace g = rex::graphics;
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    std::string constants;
    for (uint32_t k = 0; k < 4; ++k) {
      constants += fmt::format(" c{}=({:08X} {:08X} {:08X} {:08X} | {:.4g})", k, DvarRecord(0x4000 + k * 4),
                                DvarRecord(0x4001 + k * 4), DvarRecord(0x4002 + k * 4), DvarRecord(0x4003 + k * 4),
                                float_value(DvarRecord(0x4000 + k * 4)));
    }
    REXLOG_INFO("[native] BO2 memexport: VS {:016X} ({} words, in the library: {}) ps {} start {:08X} indices {} mode "
                "{} mask {:08X} fetch0 {:08X} {:08X};{}",
                hash, vs_microcode_.size(), vs_draw_ ? "si" : "no", ps_draw_ ? int(ps_draw_->number) : -1,
                DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR), DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16,
                DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7, DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK),
                DvarRecord(0x4800), DvarRecord(0x4801), constants);
  }
  uint64_t memexport_draws_ = 0;
  std::set<uint64_t> memexport_seen_;
  size_t ps_microcode_bo2zm_ = 0;
  std::array<uint32_t, 16> ps_immediate_bo2zm_{};


  bool ClearD3D() {
    namespace g = rex::graphics;
    if (!bo2zm::native::ClearQuads()) {
      return false;
    }
    const uint32_t initiator = DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    if ((initiator & 0x3F) != uint32_t(xenos::PrimitiveType::kRectangleList) ||
        ((initiator >> 6) & 0x3) != uint32_t(xenos::SourceSelect::kAutoIndex) || (initiator >> 16) != 3 ||
        DvarRecord(g::XE_GPU_REG_PA_CL_VTE_CNTL) != 0x300 || vs_microcode_.size() != 27 ||
        ps_microcode_bo2zm_ != 9) {
      return false;
    }
    if (!EnsureTargets()) {
      return true;
    }
    const auto no_is_clear = [this](const char* reason) {
      if (clears_d3d_rejected_++ < 16) {
        REXLOG_WARN("[native] BO2 D3D clear not applied ({}): z {:08X} stencil {:08X} mask {:08X} mode {}",
                    reason, DvarRecord(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL),
                    DvarRecord(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK),
                    DvarRecord(rex::graphics::XE_GPU_REG_RB_COLOR_MASK),
                    DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7);
      }
      return true;
    };
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    bo2zm::native::DrawClear b;
    const uint32_t mode = DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7;
    const uint32_t control_z = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL);
    const uint32_t stencil = DvarRecord(g::XE_GPU_REG_RB_STENCILREFMASK);
    if (control_z & 0x2) {
      if (((control_z >> 4) & 0x7) != 7) {
        return no_is_clear("z test other than ALWAYS");
      }
      b.depth = (control_z & 0x4) != 0;
    }
    if ((control_z & 0x1) && ((stencil >> 16) & 0xFF)) {
      if (((control_z >> 8) & 0x7) != 7 || ((control_z >> 14) & 0x7) != 2 || ((stencil >> 16) & 0xFF) != 0xFF) {
        return no_is_clear("stencil that is not a full replace");
      }
      b.stencil = true;
      b.value_stencil = stencil & 0xFF;
    }
    const uint32_t mask = DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) & 0xF;
    if (mode == 4 && mask) {
      if (mask != 0xF) {
        return no_is_clear("partial color mask");
      }
      b.color = true;
    }
    if (!b.depth && !b.stencil && !b.color) {
      return true;
    }

    const uint32_t f0 = DvarRecord(0x4800), f1 = DvarRecord(0x4801);
    const uint32_t words = (f1 >> 2) & 0xFFFFFF;
    if ((f0 & 0x3) != 3 || (f1 & 0x3) != 2 || words < 9 || words % 3 || words > 48) {
      return no_is_clear("vertices that are not understood");
    }
    const uint32_t step = words / 3;
    const uint8_t* data = memory_->TranslatePhysical(f0 & 0x1FFFFFFC);
    float v[3][3];
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        v[i][c] = float_value(rex::memory::load_and_swap<uint32_t>(data + (size_t(i) * step + c) * 4));
      }
    }
    if (b.color) {

      if (step < 7) {
        return no_is_clear("color clear without a color in the vertices");
      }
      for (uint32_t i = 0; i < 4; ++i) {
        b.rgba[i] = float_value(rex::memory::load_and_swap<uint32_t>(data + (3 + size_t(i)) * 4));
      }
    }

    const auto edge = [](float x) { return int32_t(std::ceil(x - 1.0f / 1024.0f)); };
    b.x0 = std::max(0, edge(std::min({v[0][0], v[1][0], v[2][0]})));
    b.x1 = std::max(0, edge(std::max({v[0][0], v[1][0], v[2][0]})));
    b.y0 = std::max(0, edge(std::min({v[0][1], v[1][1], v[2][1]})));
    b.y1 = std::max(0, edge(std::max({v[0][1], v[1][1], v[2][1]})));
    b.z = v[0][2];
    const uint32_t surface = DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t info_z = DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t info_color = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO);
    b.rb_depth_info = info_z;
    b.rb_color_info = info_color;

    std::string detail;
    for (uint32_t part = 0; part < 2; ++part) {
      bo2zm::native::DrawClear p = b;
      if (part == 0) {
        p.color = false;
        if (!p.depth && !p.stencil) continue;
      } else {
        p.depth = p.stencil = false;
        if (!p.color) continue;
      }
      const uint32_t key = part == 0 ? (info_z & 0xFFF) : (0x1000 | (info_color & 0xFFF));
      p.rb_surface_info = surface;
      const auto it = surfaces_bo2zm_.find(key);
      if (it != surfaces_bo2zm_.end() && (it->second & 0x33FFF) != (surface & 0x33FFF)) {
        FromAlias(surface, it->second, p);
      }
      const bool applied = targets_->ClearAsDraw(p);
      detail += fmt::format(" {} sup {:08X} [{},{})x[{},{}){}", part == 0 ? "z/stencil" : "color", p.rb_surface_info,
                             p.x0, p.x1, p.y0, p.y1, applied ? "" : " (no image)");
    }
    ++clears_d3d_;

    const uint64_t type = (uint64_t(surface) << 32) ^ (uint64_t(info_z) << 12) ^ info_color ^
                          (uint64_t(control_z) << 20) ^ (uint64_t(b.x1) << 48) ^ (uint64_t(b.color) << 63);
    if (clears_d3d_seen_.size() < 64 && clears_d3d_seen_.insert(type).second) {
      std::string raw;
      for (uint32_t i = 0; i < words; ++i) {
        raw += fmt::format(" {:.4g}", float_value(rex::memory::load_and_swap<uint32_t>(data + size_t(i) * 4)));
      }
      REXLOG_INFO("[native] BO2 D3D clear: surface {:08X} z {:08X} stencil {:08X} depth {:08X} color {:08X} mode {} -> "
                  "z {} ({:.6g}) stencil {} ({}) color {} ({:.3g},{:.3g},{:.3g},{:.3g});{} | vertices:{}",
                  surface, control_z, stencil, info_z, info_color, mode, b.depth, b.z, b.stencil,
                  b.value_stencil, b.color, b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3], detail, raw);
      if (clears_d3d_seen_.size() == 1) {
        std::string vs, ps;
        for (uint32_t w : vs_microcode_) vs += fmt::format(" {:08X}", w);
        for (uint32_t i = 0; i < 9; ++i) ps += fmt::format(" {:08X}", ps_immediate_bo2zm_[i]);
        REXLOG_INFO("[native] BO2 D3D clear: VS{} | PS{}", vs, ps);
      }
    }
    return true;
  }


  static bool SameSamplesPerRow(uint32_t a, uint32_t b) {
    const auto in_x = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 2 ? 2u : 1u; };
    return (a & 0x3FFF) * in_x(a) == (b & 0x3FFF) * in_x(b);
  }


  static void FromAlias(uint32_t alias, uint32_t real, bo2zm::native::DrawClear& b) {
    const auto in_x = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 2 ? 2 : 1; };
    const auto in_y = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 1 ? 2 : 1; };
    if (!SameSamplesPerRow(alias, real)) {
      return;
    }
    b.x0 = b.x0 * in_x(alias) / in_x(real);
    b.x1 = b.x1 * in_x(alias) / in_x(real);
    b.y0 = b.y0 * in_y(alias) / in_y(real);
    b.y1 = b.y1 * in_y(alias) / in_y(real);
    b.rb_surface_info = real;
  }


  void AnnotateSurfaceBo2zm() {
    namespace g = rex::graphics;
    const uint32_t surface = DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t info_z = DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t info_color = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO);
    const uint32_t control_z = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x7;
    const uint32_t color = (DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 4 &&
                           (DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) & 0xF);
    if (surface == annotated_sup_ && info_z == annotated_z_ && info_color == annotated_color_ &&
        control_z == annotated_control_z_ && color == annotated_writes_color_) [[likely]] {
      return;
    }
    annotated_sup_ = surface;
    annotated_z_ = info_z;
    annotated_color_ = info_color;
    annotated_control_z_ = control_z;
    annotated_writes_color_ = color;
    if (control_z) {
      surfaces_bo2zm_[info_z & 0xFFF] = surface;
    }
    if (color) {
      surfaces_bo2zm_[0x1000 | (info_color & 0xFFF)] = surface;
    }
  }
  std::unordered_map<uint32_t, uint32_t> surfaces_bo2zm_;
  uint32_t annotated_sup_ = 0, annotated_z_ = 0, annotated_color_ = 0, annotated_control_z_ = 0, annotated_writes_color_ = 0;
  uint64_t clears_d3d_ = 0;
  uint32_t clears_d3d_rejected_ = 0;
  std::set<uint64_t> clears_d3d_seen_;


  bool EnsureTargets() {
    if (!targets_ && !targets_failed_) {
      targets_ = NativeTargets::Create(provider_ ? provider_->vulkan_device() : nullptr, memory_);
      if (!targets_) {
        targets_failed_ = true;
        REXLOG_ERROR("[native] Could not create the native targets (C2)");
      }
    }
    return targets_ != nullptr;
  }


  void AnnotateConstantsPs() {
    if (!ps_draw_) {
      return;
    }
    const std::string& list = REXCVAR_GET(bo2zm_native_diag_constants_ps);
    if (list.empty()) {
      return;
    }
    if (list != constants_list_text_) {
      constants_list_text_ = list;
      constants_list_.clear();
      size_t start = 0;
      while (start <= list.size()) {
        const size_t end = std::min(list.find(',', start), list.size());
        if (end > start) {
          constants_list_.insert(uint32_t(std::strtoul(list.substr(start, end - start).c_str(), nullptr, 10)));
        }
        start = end + 1;
      }
    }
    if (!constants_list_.count(ps_draw_->number)) {
      return;
    }
    const auto now = Clock::now();
    auto& last = constants_last_[ps_draw_->number];
    if (now - last < std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_diag_constants_ms))) {
      return;
    }
    last = now;
    std::string values;


    values += fmt::format(" SQ_VS_CONST={:08X} SQ_PS_CONST={:08X}", DvarRecord(rex::graphics::XE_GPU_REG_SQ_VS_CONST),
                           DvarRecord(rex::graphics::XE_GPU_REG_SQ_PS_CONST));
    for (uint32_t k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 56u, 57u, 58u, 59u}) {
      values += fmt::format(" c{}=(", k);
      for (uint32_t c = 0; c < 4; ++c) {
        float f;
        const uint32_t bits = registers_[0x4400 + k * 4 + c];
        std::memcpy(&f, &bits, sizeof(f));
        values += fmt::format("{}{:.4g}", c ? "," : "", f);
      }
      values += ")";
    }
    REXLOG_INFO("[native] PS n{} constants (Swap {}):{}", ps_draw_->number, swaps_.load(), values);

    std::string vs_values;
    for (uint32_t k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 64u, 65u, 66u, 67u, 68u}) {
      vs_values += fmt::format(" c{}=(", k);
      for (uint32_t c = 0; c < 4; ++c) {
        float f;
        const uint32_t bits = registers_[0x4000 + k * 4 + c];
        std::memcpy(&f, &bits, sizeof(f));
        vs_values += fmt::format("{}{:.4g}", c ? "," : "", f);
      }
      vs_values += ")";
    }
    REXLOG_INFO("[native] VS n{} constants with PS n{}:{}", vs_draw_ ? int(vs_draw_->number) : -1,
                ps_draw_->number, vs_values);
  }


  void AnnotateGameAhead() {
    const int64_t mismatch = int64_t(::g_bo2zm_frames_game.load(std::memory_order_relaxed)) -
                               int64_t(swaps_.load(std::memory_order_relaxed));
    if (!game_ahead_first_valid_) {
      game_ahead_first_valid_ = true;
      game_ahead_first_ = mismatch;
    }
    ++game_ahead_[mismatch <= 0 ? 0 : mismatch == 1 ? 1 : 2];
    game_ahead_max_ = std::max(game_ahead_max_, mismatch);
  }

  void AnnotateFenceCopies() {
    ++fences_total_;
    if (targets_) {
      const size_t pending = targets_->CopiesPending();
      if (pending) {
        ++fences_with_copies_;
        fences_copies_max_ = std::max(fences_copies_max_, pending);
      }
    }
  }


  void BeforeFence() {
    AnnotateFenceCopies();
    static const bool wait = REXCVAR_GET(bo2zm_native_fences_wait_copies);
    if (wait && targets_) {
      targets_->WaitUploads();
    }
  }


  void DiagDropsLutBo2zm() {
    static const bool active = REXCVAR_GET(bo2zm_native_diag_lut);
    if (!active) [[likely]] {
      return;
    }
    namespace g = rex::graphics;
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    const float scale_x = float_value(DvarRecord(g::XE_GPU_REG_PA_CL_VPORT_XSCALE));
    const float scale_y = float_value(DvarRecord(g::XE_GPU_REG_PA_CL_VPORT_YSCALE));
    if (std::abs(scale_x) != 16.0f || std::abs(scale_y) != 512.0f) {
      return;
    }
    const auto now = Clock::now();
    if (now - diag_lut_last_ < std::chrono::seconds(5)) {
      return;
    }
    diag_lut_last_ = now;
    std::string constants;
    for (uint32_t c : {72u, 73u, 74u, 236u, 237u, 238u, 240u, 245u, 247u, 248u, 249u, 250u}) {
      constants += fmt::format(" c{}=({:.3g},{:.3g},{:.3g},{:.3g})", c, float_value(DvarRecord(0x4400 + c * 4)),
                                float_value(DvarRecord(0x4401 + c * 4)), float_value(DvarRecord(0x4402 + c * 4)),
                                float_value(DvarRecord(0x4403 + c * 4)));
    }
    const uint32_t f0 = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    const uint32_t base = DvarRecord(f0 + 1) & 0x1FFFF000;
    uint64_t sum = 0;
    if (base && base + 4096 <= 0x20000000u) {
      const uint8_t* data = memory_->TranslatePhysical(base);
      for (uint32_t i = 0; i < 4096; ++i) {
        sum += data[i];
      }
    }
    REXLOG_INFO("[native] BO2 LUT strip: PS n{} viewport {}x{}; fetch 0 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}, "
                "average of the table's 4 KB {:.1f};{}",
                ps_draw_ ? int(ps_draw_->number) : -1, std::abs(scale_x) * 2, std::abs(scale_y) * 2,
                DvarRecord(f0), DvarRecord(f0 + 1), DvarRecord(f0 + 2), DvarRecord(f0 + 3), DvarRecord(f0 + 4),
                DvarRecord(f0 + 5), double(sum) / 4096.0, constants);
  }
  Clock::time_point diag_lut_last_{};

  void DrawNative() {
    if (!EnsureTargets()) {
      return;
    }
    AnnotateSurfaceBo2zm();
    DiagDropsLutBo2zm();


    if (diag_constants_active_) {
      AnnotateConstantsPs();
    }
    DrawRequest request;
    request.registers = registers_.data();
    request.vs = vs_draw_;
    request.ps = ps_draw_;
    request.vs_microcode = vs_microcode_;
    request.generation_vs = generation_vs_;
    request.generation_constants_vs = generation_constants_vs_;
    request.generation_constants_ps = generation_constants_ps_;
    request.generation_fetch = generation_fetch_;
    request.generation_framing = generation_framing_;
    request.vegetation_game = vegetation_game_;


    if ((phase_stopwatch_draws_++ & (kStopwatchDrawsEvery - 1)) == kStopwatchDrawsEvery / 2) {
      const auto start = Clock::now();
      targets_->Draw(request);
      time_draws_ns_ += NanosecondsSince(start) * kStopwatchDrawsEvery;
    } else {
      targets_->Draw(request);
    }
    ++draws_measured_;
  }

  void DoCopy() {
    if (!EnsureTargets()) {
      return;
    }
    namespace g = rex::graphics;
    CopyRegisters r;
    r.rb_surface_info = DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = DvarRecord(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = DvarRecord(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = DvarRecord(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = DvarRecord(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = DvarRecord(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = DvarRecord(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = DvarRecord(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = DvarRecord(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = DvarRecord(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = DvarRecord(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = DvarRecord(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = DvarRecord(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);


    if (const uint32_t source = r.rb_copy_control & 0x7; source < 4) {
      const auto it = surfaces_bo2zm_.find(0x1000 | (r.rb_color_info[source] & 0xFFF));
      if (it != surfaces_bo2zm_.end() && (it->second & 0x33FFF) != (r.rb_surface_info & 0x33FFF) &&
          SameSamplesPerRow(r.rb_surface_info, it->second)) {
        r.surface_real = it->second;
      }
    }
    TraceCopy(r);
    const auto start = Clock::now();
    targets_->Copy(r);
    time_copies_ns_ += NanosecondsSince(start);
    ++copies_measures_;
  }


  void DumpMemoryBo2zm() {
    if (memory_dumped_bo2zm_) [[likely]] {
      return;
    }
    const std::string& text = REXCVAR_GET(bo2zm_native_dump_memory);
    const size_t two_points = text.find(':'), at = text.find('@');
    if (text.empty() || two_points == std::string::npos || at == std::string::npos) {
      memory_dumped_bo2zm_ = true;
      return;
    }
    const uint32_t seconds = uint32_t(std::strtoul(text.c_str() + at + 1, nullptr, 10));
    if (Clock::now() - start_system_ < std::chrono::seconds(seconds)) {
      return;
    }
    uint32_t address = uint32_t(std::strtoul(text.c_str(), nullptr, 16)) & 0x1FFFFFFF;
    uint32_t bytes = uint32_t(std::strtoul(text.c_str() + two_points + 1, nullptr, 16));
    if (text.rfind("auto", 0) == 0) {
      const uint64_t dump = g_bo2zm_dump_texture.load(std::memory_order_relaxed);
      if (!dump) {
        return;
      }
      address = uint32_t(dump >> 32);
      bytes = uint32_t(dump);
    }
    memory_dumped_bo2zm_ = true;
    if (!bytes || uint64_t(address) + bytes > 0x20000000) {
      return;
    }
    const std::filesystem::path path =
        rex::filesystem::GetExecutableFolder() / fmt::format("dump_mem_{:08X}_{}s.bin", address, seconds);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(memory_->TranslatePhysical(address)), std::streamsize(bytes));
    REXLOG_INFO("[native] BO2: memory {:08X}+{:X} dumped to {}", address, bytes, path.string());
  }
  bool memory_dumped_bo2zm_ = false;


  static uint32_t SurfacePitch(uint32_t surface_info) { return surface_info & 0x3FFF; }

  static constexpr uint32_t kSceneSamplesEach = 24;
  static constexpr uint32_t kSceneSamples = kSceneSamplesEach * 2;
  static constexpr uint32_t kSceneRegisters = 16;
  static constexpr uint32_t kSceneSlots = kSceneRegisters * 2;
  struct SceneSample {
    uint64_t key = 0;
    uint32_t bank = 0;
    std::array<uint32_t, kSceneRegisters * 4> constants{};
  };

  bool SceneReuseDraw() {
    if (!scene_reuse_frame_) {
      return false;
    }
    namespace g = rex::graphics;
    const uint32_t pitch = SurfacePitch(DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO));
    if (pitch == 0 || pitch == 1280) {
      return false;
    }


    const bool depth = (DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x6) == 0x6;
    if (scene_samples_count_ < kSceneSamples && (depth ? vs_draw_ != nullptr : ps_draw_ != nullptr) &&
        (depth ? scene_mesh_samples_ : scene_post_samples_) < kSceneSamplesEach) {
      ++(depth ? scene_mesh_samples_ : scene_post_samples_);
      SceneSample& sample = scene_samples_[scene_samples_count_++];
      sample.bank = depth ? 0 : 1;
      sample.key = depth ? (uint64_t(DvarRecord(g::XE_GPU_REG_VGT_DMA_BASE)) << 32) ^
                               (uint64_t(DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16) << 8) ^ vs_draw_->hash
                         : (uint64_t(pitch) << 48) ^ (uint64_t(DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO)) << 24) ^
                               ps_draw_->hash ^ 1;
      const uint32_t first = g::XE_GPU_REG_SHADER_CONSTANT_000_X + (depth ? 0 : 256 * 4);
      for (uint32_t i = 0; i < kSceneRegisters * 4; ++i) {
        sample.constants[i] = DvarRecord(first + i);
      }
    }
    if (scene_skip_) {
      ++scene_skipped_draws_;
    }
    return scene_skip_;
  }

  bool SceneReuseCopy() {
    if (!scene_reuse_frame_ || !scene_skip_) {
      return false;
    }
    const uint32_t pitch = SurfacePitch(DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO));
    if (pitch == 0 || pitch == 1280) {
      return false;
    }
    ++scene_skipped_copies_;
    return true;
  }

  static float AsFloat(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }


  bool CheckSceneView(bool full) {
    std::array<float, kSceneSlots> change{};
    uint32_t matched = 0;
    for (uint32_t s = 0; s < scene_samples_count_; ++s) {
      const SceneSample& sample = scene_samples_[s];
      for (uint32_t t = 0; t < scene_reference_count_; ++t) {
        const SceneSample& reference = scene_reference_[t];
        if (reference.key != sample.key) {
          continue;
        }
        ++matched;
        for (uint32_t i = 0; i < kSceneRegisters * 4; ++i) {
          if (sample.constants[i] == reference.constants[i]) {
            continue;
          }
          const float before = AsFloat(reference.constants[i]);
          float c = std::abs(AsFloat(sample.constants[i]) - before) / std::max(1.0f, std::abs(before));
          if (!(c == c)) {
            c = 1e9f;
          }
          change[sample.bank * kSceneRegisters + i / 4] = std::max(change[sample.bank * kSceneRegisters + i / 4], c);
        }
        break;
      }
    }

    const float drift_limit = float(REXCVAR_GET(bo2zm_native_front_end_reuse_drift));
    float largest = 0.0f;
    uint32_t largest_register = 0;
    uint32_t noisy = 0;
    for (uint32_t r = 0; r < kSceneSlots; ++r) {
      scene_streak_[r] = change[r] > drift_limit ? std::min(scene_streak_[r] + 1, kSceneNoisy) : 0;
      if (scene_streak_[r] >= kSceneNoisy) {
        ++noisy;
        continue;
      }
      if (change[r] > largest) {
        largest = change[r];
        largest_register = r;
      }
    }
    scene_noisy_ = noisy;
    scene_largest_ = std::max(scene_largest_, largest);
    if (!matched) {
      ++scene_unmatched_;
    }
    const auto now = Clock::now();
    const int32_t refresh_ms = REXCVAR_GET(bo2zm_native_front_end_reuse_refresh_ms);
    const bool refresh = refresh_ms > 0 && now - scene_drawn_ >= std::chrono::milliseconds(refresh_ms);
    const bool moved = !matched || largest > float(REXCVAR_GET(bo2zm_native_front_end_reuse_move));
    const bool drifted = largest > drift_limit &&
                         now - scene_drawn_ >=
                             std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_front_end_reuse_drift_ms));
    const bool had_reference = scene_reference_count_ > 0;
    if (had_reference && (moved || drifted)) {
      if (matched) {
        ++scene_redraws_by_register_[largest_register];
      }
      ++(moved ? scene_redraws_moved_ : scene_redraws_drifted_);
    }


    if (!had_reference || !matched || moved) {
      scene_last_move_ = now;
    }
    if ((!had_reference || !matched) && !full) {
      scene_force_full_ = true;
    } else if (!had_reference || !matched) {

      scene_kept_ = false;
      scene_settle_ = uint32_t(REXCVAR_GET(bo2zm_native_front_end_reuse_settle));
      scene_settle_until_ = now + std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_front_end_reuse_settle_ms));
    } else if (moved) {
      scene_settle_ = std::max(scene_settle_, uint32_t(REXCVAR_GET(bo2zm_native_front_end_reuse_settle_move)));
    } else if (full && scene_settle_) {
      --scene_settle_;
    }
    if (full) {
      scene_force_full_ = false;
      scene_reference_ = scene_samples_;
      scene_reference_count_ = scene_samples_count_;
      scene_drawn_ = now;
    }

    const bool still =
        now - scene_last_move_ >= std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_front_end_reuse_still_ms));
    return had_reference && !moved && !drifted && !refresh && !scene_settle_ && now >= scene_settle_until_ && still;
  }

  void SceneReuseSwap() {
    const bool on = REXCVAR_GET(bo2zm_native_front_end_reuse) && bo2zm::preset::FrontEnd();
    bool next = false;
    if (scene_reuse_frame_ && scene_samples_count_) {
      next = CheckSceneView(!scene_skip_);
      if (scene_skip_) {
        ++scene_reused_frames_;
      } else {
        ++scene_drawn_frames_;
      }
    }
    if (!on && scene_reuse_frame_) {
      scene_reference_count_ = 0;
      scene_streak_.fill(0);
    }
    if (!on) {
      scene_kept_ = false;
    }


    if (on && next) {
      scene_kept_ = true;
    }
    bo2zm::preset::SetFrontEndSceneKept(on && scene_kept_);

    scene_reuse_frame_ = on;
    scene_skip_ = on && next;
    scene_samples_count_ = 0;
    scene_mesh_samples_ = scene_post_samples_ = 0;
    if (Clock::now() - scene_report_ >= std::chrono::seconds(10)) {
      scene_report_ = Clock::now();
      if (scene_reused_frames_ || scene_drawn_frames_) {

        std::string top;
        for (int k = 0; k < 4; ++k) {
          uint32_t best = 0, count = 0;
          for (uint32_t r = 0; r < kSceneSlots; ++r) {
            if (scene_redraws_by_register_[r] > count) {
              count = scene_redraws_by_register_[r];
              best = r;
            }
          }
          if (!count) {
            break;
          }
          top += fmt::format("{}{}c{} x{}", top.empty() ? "" : ", ", best < kSceneRegisters ? "VS " : "PS ",
                             best % kSceneRegisters, count);
          scene_redraws_by_register_[best] = 0;
        }
        REXLOG_INFO("[native] front end, last 10 s: the scene drawn in {} frames ({} for a move, {} for a drift, {} "
                    "with no mesh in common) and reused in {} ({} draws and {} copies not recorded); largest change "
                    "{:.4f}; redraws by constant: {}; {} constants left out as always changing",
                    scene_drawn_frames_, scene_redraws_moved_, scene_redraws_drifted_, scene_unmatched_,
                    scene_reused_frames_, scene_skipped_draws_, scene_skipped_copies_, double(scene_largest_),
                    top.empty() ? "none" : top, scene_noisy_);
      }
      scene_drawn_frames_ = scene_reused_frames_ = scene_skipped_draws_ = scene_skipped_copies_ = 0;
      scene_redraws_moved_ = scene_redraws_drifted_ = scene_unmatched_ = 0;
      scene_redraws_by_register_.fill(0);
      scene_largest_ = 0.0f;
    }
  }
  std::array<SceneSample, kSceneSamples> scene_samples_{};
  std::array<SceneSample, kSceneSamples> scene_reference_{};
  uint32_t scene_samples_count_ = 0;
  uint32_t scene_reference_count_ = 0;
  uint32_t scene_settle_ = 0;
  bool scene_kept_ = false;
  bool scene_force_full_ = false;
  Clock::time_point scene_last_move_{};
  Clock::time_point scene_settle_until_{};
  std::array<uint32_t, kSceneSlots> scene_streak_{};
  uint32_t scene_mesh_samples_ = 0;
  uint32_t scene_post_samples_ = 0;
  bool scene_reuse_frame_ = false;
  bool scene_skip_ = false;
  uint32_t scene_noisy_ = 0;
  float scene_largest_ = 0.0f;
  uint32_t scene_redraws_moved_ = 0;
  uint32_t scene_redraws_drifted_ = 0;
  uint32_t scene_unmatched_ = 0;
  std::array<uint32_t, kSceneSlots> scene_redraws_by_register_{};
  uint32_t scene_reused_frames_ = 0;
  uint32_t scene_drawn_frames_ = 0;
  uint32_t scene_skipped_draws_ = 0;
  uint32_t scene_skipped_copies_ = 0;
  Clock::time_point scene_drawn_{};
  Clock::time_point scene_report_{};


  bool SkipPrepassDraw() {
    if (!prepass_skip_frame_) {
      return false;
    }
    namespace g = rex::graphics;
    if (DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) != 0) {
      return false;
    }
    const uint32_t depth = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL);

    if ((depth & 0x6) != 0x6 || ((depth >> 4) & 0x7) == 0x7 || !vs_draw_) {
      return false;
    }
    if ((depth & 0x1) && ((depth >> 11) & 0x1FF) != 0) {
      return false;
    }
    if ((depth & 0x81) == 0x81 && ((depth >> 23) & 0x1FF) != 0) {
      return false;
    }
    ++prepass_skipped_;
    return true;
  }
  bool prepass_skip_frame_ = false;
  uint32_t prepass_skipped_ = 0;
  Clock::time_point prepass_report_{};


  bool IsMenuBlurPass() const {
    if (!ps_draw_) {
      return false;
    }
    for (const uint64_t hash : menu_blur_ps_) {
      if (ps_draw_->hash == hash) {
        return true;
      }
    }
    return false;
  }


  bool MenuBlurSkipDraw() {
    if (menu_blur_ps_.empty() || !IsMenuBlurPass()) {
      return false;
    }
    namespace g = rex::graphics;
    menu_blur_seen_ = true;
    menu_blur_surface_ = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF;

    const uint32_t texture = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1) & 0xFFFFF000;
    AddMenuBlurTexture(texture);
    menu_blur_pending_copy_ = true;
    if (menu_blur_reuse_) {
      ++menu_blur_skipped_draws_;
      return true;
    }
    return false;
  }

  void AddMenuBlurTexture(uint32_t texture) {
    if (!texture) {
      return;
    }
    for (uint32_t& t : menu_blur_textures_) {
      if (t == texture) {
        return;
      }
      if (t == 0) {
        t = texture;
        return;
      }
    }
  }


  bool MenuBlurSkipCopy() {
    namespace g = rex::graphics;
    const uint32_t target = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_BASE) & 0xFFFFF000;
    const uint32_t source = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF;
    if (menu_blur_pending_copy_) {

      menu_blur_pending_copy_ = false;
      AddMenuBlurTexture(target);
    }
    if (!menu_blur_reuse_ || source != menu_blur_surface_) {
      return false;
    }
    for (const uint32_t t : menu_blur_textures_) {
      if (t && t == target) {
        ++menu_blur_skipped_copies_;
        return true;
      }
    }
    return false;
  }

  void MenuBlurSwap() {
    static bool parsed = false;
    if (!parsed) {
      parsed = true;
      std::string text = REXCVAR_GET(bo2zm_native_menu_blur_ps);
      if (text.empty()) {
        text = "96DABEACA2BE010F,8BEE95B21E191C1E,ED9222BFCE1FD6D9";
      }
      for (size_t at = 0; at < text.size();) {
        const size_t comma = std::min(text.find(',', at), text.size());
        const std::string word = text.substr(at, comma - at);
        if (!word.empty()) {
          menu_blur_ps_.push_back(std::strtoull(word.c_str(), nullptr, 16));
        }
        at = comma + 1;
      }
    }
    int32_t every = REXCVAR_GET(bo2zm_native_menu_blur_every);
    if (const int32_t alternate = REXCVAR_GET(bo2zm_native_menu_blur_every_alternate_s); alternate > 0) {
      const int64_t period =
          std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start_system_).count() / alternate;
      if (period != menu_blur_period_) {
        menu_blur_period_ = period;
        REXLOG_INFO("[native] menu blur: every {} for the next {} s (0 = kept)", period % 2 == 1 ? 1 : every,
                    alternate);
      }
      if (period % 2 == 1) {
        every = 1;
      }
    }

    const bool had_chain = menu_blur_seen_;
    menu_blur_seen_ = false;
    menu_blur_pending_copy_ = false;
    if (had_chain) {
      ++menu_blur_frames_;
    } else {
      menu_blur_textures_.fill(0);
      menu_blur_frames_ = 0;
    }

    menu_blur_reuse_ = had_chain && every != 1 && menu_blur_frames_ != kMenuBlurSecond &&
                       (every == 0 || menu_blur_frames_ % uint32_t(every) != 0) && !bo2zm::preset::ClientInGame();
    if (Clock::now() - menu_blur_report_ >= std::chrono::seconds(10)) {
      menu_blur_report_ = Clock::now();
      if (menu_blur_skipped_draws_ || menu_blur_skipped_copies_) {
        REXLOG_INFO("[native] menu blur, last 10 s: {} passes and {} copies reused (every {}); textures "
                    "{:08X} {:08X} {:08X}, surface {:03X}", menu_blur_skipped_draws_, menu_blur_skipped_copies_,
                    every, menu_blur_textures_[0], menu_blur_textures_[1], menu_blur_textures_[2],
                    menu_blur_surface_);
      }
      menu_blur_skipped_draws_ = menu_blur_skipped_copies_ = 0;
    }
  }
  std::vector<uint64_t> menu_blur_ps_;
  std::array<uint32_t, 4> menu_blur_textures_{};
  uint32_t menu_blur_surface_ = 0xFFFFFFFF;
  uint32_t menu_blur_frames_ = 0;
  uint32_t menu_blur_skipped_draws_ = 0;
  uint32_t menu_blur_skipped_copies_ = 0;
  int64_t menu_blur_period_ = -1;
  bool menu_blur_seen_ = false;
  bool menu_blur_pending_copy_ = false;
  bool menu_blur_reuse_ = false;
  Clock::time_point menu_blur_report_{};

  void TraceSwap() {
    MenuBlurSwap();
    SceneReuseSwap();
    prepass_skip_frame_ = REXCVAR_GET(bo2zm_native_front_end_no_prepass) && bo2zm::preset::FrontEnd();
    if (Clock::now() - prepass_report_ >= std::chrono::seconds(10)) {
      prepass_report_ = Clock::now();
      if (prepass_skipped_) {
        REXLOG_INFO("[native] front end, last 10 s: {} depth prepass draws skipped", prepass_skipped_);
      }
      prepass_skipped_ = 0;
    }
    DumpMemoryBo2zm();

    diag_constants_active_ = !REXCVAR_GET(bo2zm_native_diag_constants_ps).empty();
    if (tracing_) {
      tracing_ = false;
      trace_done_ = true;
      if (targets_) targets_->TraceFrontFrame(false);
      REXLOG_INFO("[trace] end of frame: {} lines", traces_);
      return;
    }
    const int32_t seconds = REXCVAR_GET(bo2zm_native_diag_frame_s);
    if (!trace_done_ && seconds > 0 &&
        Clock::now() - start_system_ >= std::chrono::seconds(seconds)) {
      tracing_ = true;
      traces_ = 0;
      if (targets_) targets_->TraceFrontFrame(true);
      REXLOG_INFO("[trace] full frame after Swap {}", swaps_.load());
    }
  }


  void TraceDraw() {
    if (!tracing_ || traces_ >= 20000) {
      return;
    }
    ++traces_;
    namespace g = rex::graphics;
    std::string textures;
    if (ps_draw_) {
      for (const SamplerShader& s : ps_draw_->samplers) {
        if (s.reg_index >= 16) {
          continue;
        }
        const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_index) * 6;
        const uint32_t d0 = DvarRecord(base);
        const uint32_t d1 = DvarRecord(base + 1);
        const uint32_t d3 = DvarRecord(base + 3);
        const uint32_t d5 = DvarRecord(base + 5);

        textures += fmt::format(" t{}={:08X}/f{}/d{}/s{:03X}/e{}/z{:08X}/k{}/w0{:08X}/w3{:08X}{}", s.reg_index,
                                d1 & 0xFFFFF000, d1 & 0x3F, (d5 >> 9) & 0x3, (d3 >> 1) & 0xFFF, (d1 >> 6) & 0x3,
                                DvarRecord(base + 2), s.type, d0, d3, (d0 & 0x3) == 2 ? "" : "!");
      }
    }
    if (!vs_draw_ || !ps_draw_) {
      static constexpr const char* kReasons[] = {"record with unknown shaders",
                                                 "no record",
                                                 "record with shaders different from the IM_LOAD"};
      textures += fmt::format(" | {}: VS objects {:08X} PS {:08X}; IM_LOAD PS n{} VS n{}; pending:",
                              kReasons[reason_paired_ % 3], object_vs_, object_ps_,
                              ps_current_ ? int(ps_current_->number) : -1,
                              vs_current_ ? int(vs_current_->number) : -1);
      for (size_t i = 0; i < pending_.size() && i < 4; ++i) {
        const DrawRecord& pending = pending_[i];
        const ShaderInput* vs = CachedObjectShader(pending.vs);
        const ShaderInput* ps = CachedObjectShader(pending.ps);
        textures += fmt::format(" [function {} type {} count {} VS n{} PS n{}]",
                                int(pending.function), pending.args[0],
                                RegisterCount(pending), vs ? int(vs->number) : -1,
                                ps ? int(ps->number) : -1);
      }
    }
    const uint32_t initiator = DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    if (ps_draw_) {
      textures += fmt::format(" ps_hash {:016X}", ps_draw_->hash);
    }
    REXLOG_INFO("[trace] draw VS n{} PS n{} type {} count {} surface {:08X} rt0 {:08X} rt1 {:08X} mask {:08X} blend "
                "{:08X} colorctl {:08X} depth {:08X} stencil {:08X} mode {:08X}{}",
                vs_draw_ ? int(vs_draw_->number) : -1, ps_draw_ ? int(ps_draw_->number) : -1,
                initiator & 0x3F, initiator >> 16, DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO), DvarRecord(g::XE_GPU_REG_RB_COLOR1_INFO),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK), DvarRecord(g::XE_GPU_REG_RB_BLENDCONTROL0),
                DvarRecord(g::XE_GPU_REG_RB_COLORCONTROL), DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL),
                DvarRecord(g::XE_GPU_REG_RB_STENCILREFMASK), DvarRecord(g::XE_GPU_REG_RB_MODECONTROL),
                textures);
    TraceVertices(initiator);
  }


  void TraceVertices(uint32_t initiator) {
    namespace g = rex::graphics;
    if (!vs_draw_ || !ps_draw_ || traces_ >= 20000 ||
        vs_microcode_.size() != vs_draw_->microcode.size()) {
      return;
    }
    const std::string list = REXCVAR_GET(bo2zm_native_diag_vertices_ps);
    bool in_list = false;
    uint32_t value = 0;
    bool has = false;
    for (char c : list + ",") {
      if (c >= '0' && c <= '9') {
        value = value * 10 + uint32_t(c - '0');
        has = true;
      } else if (has) {
        in_list = in_list || value == ps_draw_->number;
        value = 0;
        has = false;
      }
    }
    if (!in_list) {
      return;
    }
    ++traces_;
    std::string detail;
    uint32_t slot = UINT32_MAX;
    uint32_t stride = 0;
    for (const VertexElement& element : vs_draw_->elements) {
      const size_t i = size_t(element.instruction) * 3;
      const uint32_t o0 = vs_draw_->microcode[i], o1 = vs_draw_->microcode[i + 1];
      const uint32_t p0 = vs_microcode_[i], p1 = vs_microcode_[i + 1], p2 = vs_microcode_[i + 2];
      detail += fmt::format(" {}{}@{}: original r{} s{:03X} fmt{}; patched r{} op{} s{:03X} fmt{} f{} z{} o{} ({:08X} "
                             "{:08X} {:08X});",
                             NameUse(element.use), element.index_use, element.instruction,
                             (o0 >> 12) & 0x3F, o1 & 0xFFF, (o1 >> 16) & 0x3F, (p0 >> 12) & 0x3F,
                             p0 & 0x1F, p1 & 0xFFF, (p1 >> 16) & 0x3F,
                             ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3), p2 & 0xFF,
                             int32_t(p2 << 1) >> 9, p0, p1, p2);
      if (slot == UINT32_MAX && (p0 & 0x1F) == 0 && !((p1 >> 30) & 0x1)) {
        slot = ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3);
        stride = (p2 & 0xFF) * 4;
      }
    }
    if (slot < 96 && stride && stride <= 64) {
      const uint32_t f0 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 2);
      const uint32_t f1 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 2 + 1);
      const uint32_t source_kind = (initiator >> 6) & 0x3;
      uint32_t first = DvarRecord(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
      if (source_kind == uint32_t(xenos::SourceSelect::kDMA)) {
        const bool of_32 = (initiator >> 11) & 0x1;
        const uint32_t base_indices =
            DvarRecord(g::XE_GPU_REG_VGT_DMA_BASE) & (of_32 ? 0x1FFFFFFC : 0x1FFFFFFE);
        const auto order_indices =
            static_cast<xenos::Endian>(DvarRecord(g::XE_GPU_REG_VGT_DMA_SIZE) >> 30);
        if (of_32) {
          uint32_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 4);
          first = (first + (xenos::GpuSwap(raw, order_indices) & 0xFFFFFF)) & 0xFFFFFF;
        } else {
          uint16_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 2);
          first = (first + xenos::GpuSwap(raw, order_indices)) & 0xFFFFFF;
        }
      }
      const uint64_t address = uint64_t(f0 & 0x1FFFFFFC) + uint64_t(first) * stride;
      std::string bytes;
      if (address + uint64_t(4) * stride <= 0x20000000) {
        const uint8_t* data = memory_->TranslatePhysical(uint32_t(address));
        for (uint32_t b = 0; b < 4 * stride; ++b) {
          bytes += fmt::format("{}{:02X}", b % stride == 0 ? " | " : (b % 4 == 0 ? " " : ""),
                               data[b]);
        }
      }
      detail += fmt::format(" fetch f{} {:08X} {:08X} (order {}) source {} first {} stride {}:{}",
                             slot, f0, f1, f1 & 0x3, source_kind, first, stride, bytes);
    }
    for (const SamplerShader& s : ps_draw_->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_index) * 6;
      const uint32_t t0 = DvarRecord(base), t1 = DvarRecord(base + 1), t2 = DvarRecord(base + 2);
      if ((t0 & 0x3) != 2 || (t1 & 0x3F) != 6) {
        continue;
      }
      const uint32_t width = (t2 & 0x1FFF) + 1, height = ((t2 >> 13) & 0x1FFF) + 1;
      const uint32_t pitch = std::max<uint32_t>(((t0 >> 22) & 0x1FF) << 5, 1);
      const bool tiled = (t0 >> 31) & 0x1;
      const uint32_t y = height / 2;
      std::string texels;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t x = std::min(width - 1, (width > 8 ? width / 2 - 4 : 0) + k);
        const int64_t displacement = tiled ? OffsetTiledDiag(int32_t(x), int32_t(y), pitch, 2)
                                               : int64_t(y) * pitch * 4 + int64_t(x) * 4;
        const uint64_t d = uint64_t(t1 & 0x1FFFF000) + uint64_t(displacement);
        if (displacement < 0 || d + 4 > 0x20000000) {
          break;
        }
        const uint8_t* texel = memory_->TranslatePhysical(uint32_t(d));
        texels += fmt::format(" {:02X}{:02X}{:02X}{:02X}", texel[0], texel[1], texel[2], texel[3]);
      }
      detail += fmt::format("; t{} {}x{} {} pitch {} swizzle {:03X} order {} signs {:02X} row {}:{}",
                             s.reg_index, width, height, tiled ? "tiled" : "linear", pitch,
                             (DvarRecord(base + 3) >> 1) & 0xFFF, (t1 >> 6) & 0x3, (t0 >> 2) & 0xFF,
                             y, texels);
      const uint32_t address_texture = t1 & 0x1FFFF000;
      if (textures_dumped_.size() < 8 && textures_dumped_.insert(address_texture).second) {
        std::vector<uint8_t> dump(size_t(width) * height * 4);
        bool full = true;
        for (uint32_t yy = 0; yy < height && full; ++yy) {
          for (uint32_t xx = 0; xx < width; ++xx) {
            const int64_t dt = tiled ? OffsetTiledDiag(int32_t(xx), int32_t(yy), pitch, 2)
                                       : int64_t(yy) * pitch * 4 + int64_t(xx) * 4;
            const uint64_t texel = uint64_t(address_texture) + uint64_t(dt);
            if (dt < 0 || texel + 4 > 0x20000000) {
              full = false;
              break;
            }
            std::memcpy(dump.data() + (size_t(yy) * width + xx) * 4,
                        memory_->TranslatePhysical(uint32_t(texel)), 4);
          }
        }
        const auto path = rex::filesystem::GetExecutableFolder() /
                          fmt::format("diag_texture_{:08X}_{}x{}.bin", address_texture, width, height);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(dump.data()), std::streamsize(dump.size()));
        detail += fmt::format(" (dumped to {}{})", path.filename().string(),
                               full ? "" : ", incomplete");
      }
    }
    detail += "; VS constants";
    for (uint32_t c = 0; c < 16; ++c) {
      const uint32_t b = 0x4000 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(DvarRecord(b)),
                             std::bit_cast<float>(DvarRecord(b + 1)), std::bit_cast<float>(DvarRecord(b + 2)),
                             std::bit_cast<float>(DvarRecord(b + 3)));
    }
    detail += "; PS constants";
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t b = 0x4400 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(DvarRecord(b)),
                             std::bit_cast<float>(DvarRecord(b + 1)), std::bit_cast<float>(DvarRecord(b + 2)),
                             std::bit_cast<float>(DvarRecord(b + 3)));
    }
    REXLOG_INFO("[trace] vertices PS n{} VS n{}:{}", ps_draw_->number, vs_draw_->number,
                detail);
  }


  static int32_t OffsetTiledDiag(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
    pitch = (pitch + 31) & ~uint32_t(31);
    const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
    const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
    const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  }

  void TraceCopy(const CopyRegisters& r) {
    if (!tracing_ || traces_ >= 20000) {
      return;
    }
    ++traces_;
    REXLOG_INFO("[trace] copy control {:08X} target {:08X} info {:08X} pitch {:08X} surface {:08X} rt0 {:08X} depth "
                "{:08X} clear {:08X}/{:08X} depth_clear {:08X}",
                r.rb_copy_control, r.rb_copy_dest_base, r.rb_copy_dest_info, r.rb_copy_dest_pitch,
                r.rb_surface_info, r.rb_color_info[0], r.rb_depth_info, r.rb_color_clear,
                r.rb_color_clear_lo, r.rb_depth_clear);
  }


  void LoadShaderCached(uint32_t address_type, uint32_t size, const uint8_t* source) {
    const uint32_t type = address_type & 0x3;
    const size_t bytes = size_t(size) * sizeof(uint32_t);
    auto& ways = loads_cache_[type][size_t((uint64_t(address_type) * 0x9E3779B97F4A7C15ull) >> 57)];
    if (im_shortcut_phase_ == kImShortcutNoBegin) {
      BeginImShortcut();
    }
    const uint32_t slot = microcode::SlotOf(address_type & ~uint32_t(0x3));

    ShaderLoad* candidate = nullptr;
    if (im_shortcut_phase_ != kImShortcutOff) {
      const uint32_t global = microcode::GlobalRelaxed();
      for (ShaderLoad& via : ways) {
        if (!via.validated || via.address_type != address_type || via.size != size) {
          continue;
        }
        if (via.version_global != global) {

          if (via.version_slot != microcode::StartAfterGlobal(slot)) {
            via.validated = false;
            ++im_i_version_changed_;
            continue;
          }
          if (microcode::g_patches_others.load(std::memory_order_relaxed) != 0) {
            TurnOffImShortcutPerOthers();
            break;
          }
          via.version_global = global;
        }
        candidate = &via;
        break;
      }
    }
    ShaderLoad* load = nullptr;
    if (candidate && im_shortcut_phase_ == kImShortcutApplying &&
        (++im_shortcut_turn_ & (kImShortcutCheckEvery - 1)) != 0) {

      load = candidate;
      ++loads_cached_;
      ++im_i_no_memcmp_;
    } else {

      const bool validate = im_shortcut_phase_ != kImShortcutOff;
      microcode::Readback read;
      if (validate) {
        read = microcode::BeforeRead(slot);
      }
      for (ShaderLoad& via : ways) {
        if (via.address_type == address_type && via.size == size &&
            std::memcmp(via.raw.data(), source, bytes) == 0) {
          load = &via;
          break;
        }
      }
      if (validate && candidate) {
        if (load == candidate) {
          ++im_shortcut_checked_;
          ++im_i_checked_;
          if (im_shortcut_phase_ == kImShortcutObserving && im_shortcut_checked_ >= kImShortcutALook &&
              microcode::g_patches_in_room.load(std::memory_order_relaxed) != 0) {
            im_shortcut_phase_ = kImShortcutApplying;
            BO2ZM_REPORT_RING("[native] C5a IM_LOAD without memcmp: {} loads checked against the memcmp, 0 "
                                 "disagreements and {} in-place patches seen: APPLYING, the memcmp is skipped if the "
                                 "version has not changed (1 in {} is still checked)",
                                 im_shortcut_checked_,
                                 microcode::g_patches_in_room.load(std::memory_order_relaxed),
                                 kImShortcutCheckEvery);
          }
        } else {
          TurnOffImShortcut(address_type, size, candidate->raw, candidate->version_slot,
                        candidate->version_global, load != nullptr, source, read);
        }
      }
      if (load) {
        ++loads_cached_;
      } else {
        load = ways[0].use <= ways[1].use ? &ways[0] : &ways[1];
        load->address_type = address_type;
        load->size = size;
        load->raw.resize(size);
        std::memcpy(load->raw.data(), source, bytes);
        load->host.resize(size);


        const uint8_t* copy = reinterpret_cast<const uint8_t*>(load->raw.data());
        for (uint32_t i = 0; i < size; ++i) {
          load->host[i] = rex::memory::load_and_swap<uint32_t>(copy + size_t(i) * 4);
        }
        load->input = shaders_.loaded() ? shaders_.Identify(type == 0, load->host) : nullptr;
        load->hash = XXH3_64bits(load->host.data(), bytes);
        load->generation = ++generations_microcode_;
        load->validated = false;
      }
      if (validate && im_shortcut_phase_ != kImShortcutOff) {
        ++im_i_with_memcmp_;
        if (microcode::AfterRead(slot, read)) {
          load->validated = true;
          load->version_slot = read.start;
          load->version_global = read.global;
        } else {
          load->validated = false;
          ++im_i_cancelled_;
        }
      }
    }
    load->use = ++loads_tic_;
    (type == 0 ? vs_current_ : ps_current_) = load->input;
    if (tracing_ && traces_ < 20000) {
      ++traces_;
      REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, hash {:016X})", type == 0 ? "VS" : "PS",
                  load->input ? int(load->input->number) : -1, load->host.size(), load->hash);
    }
    if (type == 0) {
      vs_microcode_ = load->host;
      generation_vs_ = load->generation;
      hash_vs_generation_ = load->generation;
      hash_vs_ = load->hash;
    }
  }


  void BeginImShortcut() {
    const bool requested = REXCVAR_GET(bo2zm_native_im_load_without_memcmp);
    im_shortcut_phase_ = requested && microcode::kCreationsWatched ? kImShortcutObserving : kImShortcutOff;
    BO2ZM_REPORT_RING("[native] C5a IM_LOAD without memcmp (bo2zm_native_im_load_without_memcmp) = {}",
                         !requested ? "no (off by the cvar): memcmp on every IM_LOAD"
                         : !microcode::kCreationsWatched
                             ? "no: with BO2ZM_NATIVE_SHADER_LIBRARY the constructors do not report"
                             : "YES, observing: each load is also compared with memcmp until there is proof");
  }

  const char* NamePhaseImShortcut() const {
    switch (im_shortcut_phase_) {
      case kImShortcutObserving:
        return "observing";
      case kImShortcutApplying:
        return "applying";
      case kImShortcutOff:
        return "OFF";
      default:
        return "not started";
    }
  }


  void TurnOffImShortcut(uint32_t address_type, uint32_t size, const std::vector<uint32_t>& raw,
                     uint32_t version_slot, uint32_t version_global, bool other_via, const uint8_t* source,
                     const microcode::Readback& read) {
    uint32_t first = 0;
    while (first < size && std::memcmp(raw.data() + first, source + size_t(first) * 4, 4) == 0) {
      ++first;
    }
    REXLOG_ERROR("[native] C5a IM_LOAD without memcmp: MISMATCH. The shortcut gave the copy of {} {:08X} ({} words) "
                 "checked with slot version {} and global {}, and the guest memory is already different (first "
                 "different word: {}; the memcmp {}). Current versions: start {} end {} global {}; phase {} with {} "
                 "loads checked. Reported writes: {} in-place patches, {} in the ring copy, {} others, {} creations. "
                 "Someone writes microcode without reporting it: shortcut OFF for the rest of the session (every load "
                 "with memcmp, as before 184)",
                 (address_type & 0x3) == 0 ? "VS" : "PS", address_type & ~uint32_t(0x3), size,
                 version_slot, version_global, first,
                 other_via ? "finds another way" : "has to copy it again", read.start, read.end,
                 read.global, NamePhaseImShortcut(), im_shortcut_checked_,
                 microcode::g_patches_in_room.load(std::memory_order_relaxed),
                 microcode::g_patches_in_copy.load(std::memory_order_relaxed),
                 microcode::g_patches_others.load(std::memory_order_relaxed),
                 microcode::g_creations.load(std::memory_order_relaxed));
    im_shortcut_phase_ = kImShortcutOff;
  }


  void TurnOffImShortcutPerOthers() {
    REXLOG_ERROR("[native] C5a IM_LOAD without memcmp: MISMATCH. The fetch patcher (sub_825A2FB8) has been called {} "
                 "times from outside its two known calls (return {:08X}, target {:08X}): it may write microcode some "
                 "other way. Shortcut OFF for the rest of the session (every load with memcmp, as before 184)",
                 microcode::g_patches_others.load(std::memory_order_relaxed),
                 microcode::g_other_return.load(std::memory_order_relaxed),
                 microcode::g_other_target.load(std::memory_order_relaxed));
    im_shortcut_phase_ = kImShortcutOff;
  }


  void IdentifyShader(uint32_t type) {
    if (type > 1) {
      return;
    }
    const ShaderInput* input =
        shaders_.loaded() ? shaders_.Identify(type == 0, microcode_) : nullptr;
    (type == 0 ? vs_current_ : ps_current_) = input;
    if (tracing_ && traces_ < 20000) {
      ++traces_;
      REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, hash {:016X})", type == 0 ? "VS" : "PS",
                  input ? int(input->number) : -1, microcode_.size(),
                  XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t)));
    }
    if (type == 0) {
      vs_immediate_ = microcode_;
      vs_microcode_ = vs_immediate_;
      generation_vs_ = ++generations_microcode_;
    }
  }


  struct ImmediateLoad {
    uint64_t signature = 0;
    uint32_t size = 0;
    std::vector<uint32_t> raw;
    std::vector<uint32_t> host;
    const ShaderInput* input = nullptr;
    uint64_t hash = 0;
    uint64_t generation = 0;
    uint64_t use = 0;
  };

  void LoadImmediate(RingReader data, uint32_t type, uint32_t size) {
    if (imm_phase_ == kImmNoBegin) {
      BeginImmediate();
    }
    const bool back = data.mask != 0 && size_t(data.pos) + size > size_t(data.mask) + 1;
    if (imm_phase_ == kImmOff || type > 1 || size == 0 || back) {
      if (back) {
        ++imm_i_back_;
      } else {
        ++imm_i_no_cache_;
      }
      LoadImmediateNoCache(data, type, size);
      return;
    }
    const uint8_t* raw = data.base + size_t(data.pos) * 4;
    const size_t bytes = size_t(size) * sizeof(uint32_t);
    const uint64_t signature = SignatureImmediate(raw, size);
    const size_t index = size_t(signature >> (64 - kImmBitsSet));
    auto& set = imm_cache_[type][index];
    uint8_t& last_way = imm_last_way_[type][index];
    ImmediateLoad* via = nullptr;
    for (uint32_t k = 0; k < kImmWays; ++k) {
      const uint32_t v = (last_way + k) % kImmWays;
      ImmediateLoad& candidate = set[v];
      if (candidate.signature == signature && candidate.size == size &&
          std::memcmp(candidate.raw.data(), raw, bytes) == 0) {
        via = &candidate;
        last_way = uint8_t(v);
        break;
      }
    }
    if (via && imm_phase_ == kImmApplying && (++imm_turn_ & (kImmCheckEvery - 1)) != 0) {

      via->use = ++imm_tic_;
      UseImmediate(type, *via);
      ++imm_i_hits_;
      ++loads_cached_;
      if (tracing_ && traces_ < 20000) {
        ++traces_;
        REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, hash {:016X}; IM_LOAD_IMMEDIATE from the cache)",
                    type == 0 ? "VS" : "PS", via->input ? int(via->input->number) : -1, via->host.size(),
                    via->hash);
      }
      return;
    }


    LoadImmediateNoCache(data, type, size);
    const ShaderInput* input = type == 0 ? vs_current_ : ps_current_;
    if (via) {

      uint32_t first = 0;
      while (first < size && first < via->host.size() && via->host[first] == microcode_[first]) {
        ++first;
      }
      if (first != size || via->host.size() != size || via->input != input) {
        TurnOffImmediate("a cache hit does not give the same as the usual path", type, size, signature, first,
                        via->input, input);
        return;
      }
      ++imm_checked_;
      ++imm_i_checked_;
      via->use = ++imm_tic_;
      if (imm_phase_ == kImmObserving && imm_checked_ >= kImmALook) {
        imm_phase_ = kImmApplying;
        BO2ZM_REPORT_RING("[native] C5a IM_LOAD_IMMEDIATE with a cache: {} hits checked against the usual path and 0 "
                             "disagreements: APPLYING, hits are no longer flipped or identified (1 in {} is still "
                             "checked)",
                             imm_checked_, kImmCheckEvery);
      }
      if (imm_phase_ == kImmApplying) {
        UseImmediate(type, *via);
      }
      return;
    }

    ++imm_i_misses_;
    uint32_t room = 0;
    for (uint32_t k = 1; k < kImmWays; ++k) {
      if (set[k].use < set[room].use) {
        room = k;
      }
    }
    ImmediateLoad& fresh = set[room];
    fresh.signature = signature;
    fresh.size = size;
    fresh.raw.resize(size);
    std::memcpy(fresh.raw.data(), raw, bytes);
    fresh.host.assign(microcode_.begin(), microcode_.end());
    fresh.input = input;
    fresh.hash = XXH3_64bits(fresh.host.data(), bytes);
    fresh.generation = ++generations_microcode_;
    fresh.use = ++imm_tic_;
    last_way = uint8_t(room);


    const uint8_t* copy = reinterpret_cast<const uint8_t*>(fresh.raw.data());
    for (uint32_t i = 0; i < size; ++i) {
      if (rex::memory::load_and_swap<uint32_t>(copy + size_t(i) * 4) != fresh.host[i]) {
        fresh.signature = 0;
        fresh.use = 0;
        TurnOffImmediate("the raw copy of a new way is not what the usual path read", type, size, signature,
                        i, input, input);
        return;
      }
    }
    if (imm_phase_ == kImmApplying) {
      UseImmediate(type, fresh);
    }
  }


  void LoadImmediateNoCache(RingReader data, uint32_t type, uint32_t size) {
    microcode_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
      microcode_[i] = data.Read();
    }
    IdentifyShader(type);
  }


  void UseImmediate(uint32_t type, const ImmediateLoad& via) {
    (type == 0 ? vs_current_ : ps_current_) = via.input;
    if (type == 0) {
      vs_microcode_ = via.host;
      generation_vs_ = via.generation;
      hash_vs_generation_ = via.generation;
      hash_vs_ = via.hash;
    }
  }


  static uint64_t SignatureImmediate(const uint8_t* raw, uint32_t size) {
    const size_t positions[4] = {0, size_t(size) / 3, size_t(size) * 2 / 3, size_t(size) - 1};
    uint64_t h = uint64_t(size) * 0x9E3779B97F4A7C15ull;
    for (const size_t p : positions) {
      uint32_t word;
      std::memcpy(&word, raw + p * 4, sizeof(word));
      h = (h ^ word) * 0xBF58476D1CE4E5B9ull;
      h ^= h >> 29;
    }
    return h | 1;
  }

  void BeginImmediate() {
    const bool requested = REXCVAR_GET(bo2zm_native_im_immediate_cache);
    imm_phase_ = requested ? kImmObserving : kImmOff;
    BO2ZM_REPORT_RING("[native] C5a IM_LOAD_IMMEDIATE with a cache (bo2zm_native_im_immediate_cache) = {}",
                         requested ? "YES, observing: each hit is compared with the usual path until there is proof"
                                : "no (off by the cvar): each IM_LOAD_IMMEDIATE is flipped and identified, as before");
  }

  const char* NamePhaseImmediate() const {
    switch (imm_phase_) {
      case kImmObserving:
        return "observing";
      case kImmApplying:
        return "applying";
      case kImmOff:
        return "OFF";
      default:
        return "not started";
    }
  }


  void TurnOffImmediate(const char* reason, uint32_t type, uint32_t size, uint64_t signature, uint32_t first,
                       const ShaderInput* of_cache, const ShaderInput* of_always) {
    REXLOG_ERROR("[native] C5a IM_LOAD_IMMEDIATE with a cache: MISMATCH, {}: {} of {} words (signature {:016X}), first "
                 "different word {}, shader from the cache n{} and from the usual path n{}; phase {} with {} hits "
                 "checked. Cache OFF for the rest of the session (everything by the usual path, as before 184); this "
                 "load already goes by the usual path",
                 reason, type == 0 ? "VS" : "PS", size, signature, first,
                 of_cache ? int(of_cache->number) : -1, of_always ? int(of_always->number) : -1,
                 NamePhaseImmediate(), imm_checked_);
    imm_phase_ = kImmOff;
  }

  void CountDrawShaders() {
    if (!shaders_.loaded()) {
      return;
    }
    if (!vs_current_) {
      ++draws_no_vs_;
    } else if (!ps_current_) {
      ++draws_no_ps_;
    } else {
      ++draws_identified_;
    }
    const uint64_t pair = (uint64_t(vs_current_ ? vs_current_->number + 1 : 0) << 32) |
                         (ps_current_ ? ps_current_->number + 1 : 0);


    if (pair != last_pair_shaders_) {
      last_pair_shaders_ = pair;

      if (pairs_count_ < kPairSlots / 2) {
        for (uint32_t i = uint32_t((pair * 0x9E3779B97F4A7C15ull) >> 52);; i = (i + 1) & (kPairSlots - 1)) {
          if (pairs_[i] == pair) {
            break;
          }
          if (pairs_[i] == 0) {
            pairs_[i] = pair;
            ++pairs_count_;
            break;
          }
        }
      }
    }
  }


  void PairDraw() {
    vs_draw_ = nullptr;
    ps_draw_ = nullptr;
    reason_paired_ = 0;
    object_vs_ = object_ps_ = 0;
    vegetation_game_ = 0;
    if (!shaders_.loaded()) {
      return;
    }
    RefreshObjects();
    const uint32_t initiator = DvarRecord(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
    const uint32_t type = initiator & 0x3F;
    const uint32_t count = initiator >> 16;
    const uint32_t mode_marker = draw_marker_mode_;
    bool marker_fine = false;
    if (mode_marker != 0) {
      marker_fine = draw_marker_.args[0] == type && RegisterCount(draw_marker_) == count &&
                      ShadersCoherent(draw_marker_);
      if (!marker_fine) {
        ++marker_rejected_registers_;
      } else {
        draw_marker_mode_ = 0;
        if (mode_marker == kDrawApply) {
          ++draws_with_marker_register_;
          UseDrawRegister(draw_marker_);
          return;
        }
      }
    }
    DrawRecord reg_index;
    while (pending_.size() < 64 && ExtractDraw(reg_index)) {
      pending_.push_back(reg_index);
    }
    size_t found = SIZE_MAX;
    bool incoherent = false;


    const size_t candidates = std::min<size_t>(pending_.size(), 8);
    for (size_t i = 0; i < candidates; ++i) {
      if (pending_[i].args[0] != type || RegisterCount(pending_[i]) != count) {
        continue;
      }


      if (!ShadersCoherent(pending_[i])) {
        ++candidates_incoherent_;
        incoherent = true;
        continue;
      }
      found = i;
      break;
    }
    if (mode_marker == kDrawCheck && marker_fine) {
      CompareDrawMarker(found == SIZE_MAX ? nullptr : &pending_[found], type, count);
    }
    if (found == SIZE_MAX) {
      ++draws_no_register_;
      draws_incoherent_ += incoherent;
      reason_paired_ = incoherent ? 2 : 1;
      if (warnings_c5b_ < 16) {
        ++warnings_c5b_;
        std::string first;
        if (!pending_.empty()) {
          const DrawRecord& r = pending_.front();
          first = fmt::format("; the first pending one is function {} type {} count {}",
                                int(r.function), r.args[0], RegisterCount(r));
        }
        REXLOG_WARN("[native] C5b: ring draw without a Draw* record: type {} count {} ({} pending{})",
                    type, count, pending_.size(), first);
      }
      UseRingIdentity();
      return;
    }
    registers_skipped_ += found;
    const DrawRecord r = pending_[found];
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(found + 1));
    UseDrawRegister(r);
  }


  void UseDrawRegister(const DrawRecord& r) {
    if (r.shadow) {
      CompareShadow(r.shadow, r);
    }
    object_vs_ = r.vs;
    object_ps_ = r.ps;
    ++draws_paired_;
    vs_draw_ = CachedObjectShader(r.vs);
    ps_draw_ = CachedObjectShader(r.ps);


    vegetation_game_ = IdentityForVegetation(r.vegetation, vs_draw_ != nullptr && ps_draw_ != nullptr);
    if (vs_draw_ && ps_draw_) {

      if ((vs_draw_ != mapped_vs_ || generation_vs_ != mapped_generation_) &&
          vs_per_microcode_.size() < 4096) {
        vs_per_microcode_[HashVs()] = vs_draw_;
        mapped_vs_ = vs_draw_;
        mapped_generation_ = generation_vs_;
      }
    } else {
      UseRingIdentity();
    }
    draws_with_vs_ += vs_draw_ != nullptr;
    draws_with_ps_ += ps_draw_ != nullptr;
    if (vs_draw_ && (vs_draw_ != annotated_vs_ || generation_vs_ != annotated_generation_)) {
      annotated_vs_ = vs_draw_;
      annotated_generation_ = generation_vs_;
      AnnotateInputVertices(*vs_draw_);
    }
  }


  std::pair<const ShaderInput*, const ShaderInput*> ShadersWithRegister(const DrawRecord* r) {
    const ShaderInput* vs = r ? CachedObjectShader(r->vs) : nullptr;
    const ShaderInput* ps = r ? CachedObjectShader(r->ps) : nullptr;


    const bool only_depth = (DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5;
    if ((vs && ps) || (!ps_current_ && !only_depth)) {
      return {vs, ps};
    }
    const ShaderInput* identity = vs_current_;
    if (!identity) {
      const auto it = vs_per_microcode_.find(HashVs());
      if (it == vs_per_microcode_.end()) {
        return {vs, ps};
      }
      identity = it->second;
    }
    return {identity, ps_current_};
  }


  void CompareDrawMarker(const DrawRecord* lookup, uint32_t type, uint32_t count) {
    const auto with_lookup = ShadersWithRegister(lookup);
    const auto with_marker = ShadersWithRegister(&draw_marker_);
    const DrawRecord& m = draw_marker_;
    uint32_t kind = 0;
    if (with_lookup != with_marker) {
      kind = lookup ? 1u : 3u;
    } else if (!lookup || lookup->function != m.function || lookup->vs != m.vs || lookup->ps != m.ps ||
               lookup->args[1] != m.args[1] || lookup->args[2] != m.args[2] || lookup->args[3] != m.args[3]) {
      ++checks_draw_other_register_;
    }
    ++checks_draw_;
    if (kind != 0) {
      ++checks_draw_different_;
      if (warnings_draw_marker_ < 8) {
        ++warnings_draw_marker_;
        const auto number = [](const ShaderInput* e) { return e ? int(e->number) : -1; };
        REXLOG_ERROR("[native] draw record: the lookup and the marker do not agree (case {}) in a draw of "
                     "type {} count {}: lookup {} (VS {:08X} PS {:08X}) -> VS n{} PS n{}; marker (VS {:08X} PS {:08X}) "
                     "-> VS n{} PS n{}. The game thread switches the draw records off",
                     kind, type, count, lookup ? "with a record" : "without a record", lookup ? lookup->vs : 0u,
                     lookup ? lookup->ps : 0u, number(with_lookup.first), number(with_lookup.second), m.vs,
                     m.ps, number(with_marker.first), number(with_marker.second));
      }
    }
    AnnotateCheckDraw(kind == 0, kind, lookup, &m);
  }


  static bool ReadMarkerDraw(const uint8_t* p, DrawRecord& r) {


    const uint32_t word = rex::memory::load_and_swap<uint32_t>(p);
    const uint32_t function = word & 0xFFu;
    if (function > uint32_t(DrawFunction::kIndexedUP) || (word >> 24) != 0) {
      return false;
    }
    r.function = DrawFunction(function);
    r.vegetation = uint16_t(word >> 8);
    r.vs = rex::memory::load_and_swap<uint32_t>(p + 4);
    r.ps = rex::memory::load_and_swap<uint32_t>(p + 8);
    for (uint32_t i = 0; i < 4; ++i) {
      r.args[i] = rex::memory::load_and_swap<uint32_t>(p + 12 + 4 * i);
    }
    r.shadow = (uint64_t(rex::memory::load_and_swap<uint32_t>(p + 28)) << 32) |
               rex::memory::load_and_swap<uint32_t>(p + 32);
    return true;
  }


  void CompareShadow(uint64_t sequence, const DrawRecord& record) {
    if (!ReadSnapshot(sequence, shadow_shot_)) {
      ++shadow_lost_;
      return;
    }
    ++shadow_draws_;
    const MirrorSnapshot& f = shadow_shot_;
    for (uint32_t g = 0; g < kGroupsMirror; ++g) {
      const uint64_t m = f.mask[g];
      for (uint32_t i = 0; m && i < 64; ++i) {
        if (!((m >> (63 - i)) & 1)) {
          continue;
        }
        const uint32_t reg = f.base_register[g] + i;
        ++shadow_state_compared_;
        if (DvarRecord(reg) != f.state[g][i]) {
          ++shadow_state_different_;
          ++shadow_per_register_[reg];
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      ++shadow_fetch_compared_;
      if (DvarRecord(0x4800 + i) != f.fetch[i]) {
        ++shadow_fetch_different_;
        ++shadow_per_register_[0x4800 + i];
      }
    }


    uint64_t read[8] = {};
    const auto mark = [&read](const ShaderInput* s, uint32_t offset) {
      if (!s) {
        return;
      }
      if (s->constants_runs_valid) {
        for (const auto& [first, n] : s->constants_runs) {
          for (uint32_t r = first; r < uint32_t(first) + n && r < 256; ++r) {
            read[(offset + r) >> 6] |= uint64_t(1) << ((offset + r) & 63);
          }
        }
      } else {
        for (uint32_t r = 0; r < 256 && r * 16 < s->constants_bytes; ++r) {
          read[(offset + r) >> 6] |= uint64_t(1) << ((offset + r) & 63);
        }
      }
    };
    mark(CachedObjectShader(record.vs), 0);
    mark(CachedObjectShader(record.ps), 256);
    for (uint32_t i = 0; i < 2048; ++i) {
      ++shadow_constants_compared_;
      const uint32_t ring = DvarRecord(0x4000 + i);
      const bool reads = (read[(i >> 2) >> 6] >> ((i >> 2) & 63)) & 1;
      shadow_constants_read_compared_ += reads;
      if (ring != f.constants[i]) {
        ++shadow_constants_different_;
        shadow_constants_read_different_ += reads;
        shadow_constants_read_unknown_ += reads && f.constants_from[i] == kFromUnknown;
        ++shadow_per_register_[0x4000 + i];
        if (reads) {
          ++shadow_per_register_read_[0x4000 + i];
        }


        const uint32_t from = f.constants_from[i] < 5 ? f.constants_from[i] : 4;
        ++shadow_from_[from][ring == f.constants_mirror[i] ? 1 : 0];
      }
    }
  }


  bool ShadersCoherent(const DrawRecord& reg_index) {
    const ShaderInput* ps = CachedObjectShader(reg_index.ps);
    if (ps && ps_current_ && ps->hash != ps_current_->hash) {
      return false;
    }
    const ShaderInput* vs = CachedObjectShader(reg_index.vs);
    if (!vs) {
      return true;
    }
    if (vs != coherence_vs_ || generation_vs_ != coherence_generation_) {
      coherence_vs_ = vs;
      coherence_generation_ = generation_vs_;
      coherence_ = FetchCoherent(*vs, vs_microcode_);
    }
    return coherence_;
  }


  void UseRingIdentity() {


    const bool only_depth = (DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5;
    if (!ps_current_ && !only_depth) {
      return;
    }
    const ShaderInput* vs = vs_current_;
    if (!vs) {
      const auto it = vs_per_microcode_.find(HashVs());
      if (it == vs_per_microcode_.end()) {
        return;
      }
      vs = it->second;
    }
    vs_draw_ = vs;
    ps_draw_ = ps_current_;
    ++draws_per_im_load_;
  }

  uint64_t HashVs() {
    if (hash_vs_generation_ != generation_vs_) {
      hash_vs_generation_ = generation_vs_;
      hash_vs_ = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    }
    return hash_vs_;
  }

  static uint32_t RegisterCount(const DrawRecord& r) {
    switch (r.function) {
      case DrawFunction::kVertices:
        return r.args[2];
      case DrawFunction::kIndexed:
        return r.args[3];
      case DrawFunction::kVerticesUP:
        return r.args[1];
      case DrawFunction::kIndexedUP:
      default:
        return r.args[3];
    }
  }


  void RefreshObjects() {
    const uint64_t generation = GenerationObjects();
    if (generation != generation_objects_) {
      objects_.clear();
      memo_objects_.fill(ObjectMemo{});
      generation_objects_ = generation;
    }
  }

  const ShaderInput* CachedObjectShader(uint32_t object) {


    ObjectMemo& memo = memo_objects_[size_t((object * UINT32_C(2654435761)) >> 28)];
    if (memo.object == object) {
      return memo.input;
    }
    const ShaderInput* input;
    if (const auto it = objects_.find(object); it != objects_.end()) {
      input = it->second;
    } else {
      input = ObjectShader(object);
      objects_.emplace(object, input);
    }
    memo.object = object;
    memo.input = input;
    return input;
  }


  void AnnotateInputVertices(const ShaderInput& vs) {
    if (vs_microcode_.size() != vs.microcode.size()) {
      ++variants_length_different_;
      return;
    }
    if (variants_views_.size() >= 256) {
      return;
    }
    const uint64_t key = (uint64_t(vs.number) << 48) ^ HashVs();
    if (!variants_views_.insert(key).second) {
      return;
    }
    std::string detail;
    for (const VertexElement& element : vs.elements) {
      const uint32_t reg_index = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
      size_t p = SIZE_MAX;
      for (const VertexElement& other : vs.elements) {
        const size_t q = size_t(other.instruction) * 3;
        if (((vs_microcode_[q] >> 12) & 0x3F) == reg_index && (vs_microcode_[q] & 0x1F) == 0) {
          p = q;
          break;
        }
      }
      if (p == SIZE_MAX) {
        detail += fmt::format(" {}{}:?", NameUse(element.use), element.index_use);
        ++elements_no_fetch_;
        continue;
      }
      const uint32_t d0 = vs_microcode_[p], d1 = vs_microcode_[p + 1], d2 = vs_microcode_[p + 2];


      const int32_t exponent = int32_t(d1 << 2) >> 26;
      detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}/s{:03X}>{:03X}{}{}{}{}", NameUse(element.use),
                             element.index_use, ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3),
                             (d1 >> 16) & 0x3F, d2 & 0xFF, int32_t(d2 << 1) >> 9,
                             vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF,
                             d1 & 0xFFF, ((d1 >> 30) & 0x1) ? "/mini" : "",
                             ((d1 >> 12) & 0x1) ? "/sign" : "",
                             exponent ? fmt::format("/exp{}", exponent) : std::string(),
                             ((d1 >> 14) & 0x1) ? "/rf1" : "");
    }
    if (variants_views_.size() <= 24) {
      REXLOG_INFO("[native] C5b: VS n{} (variant {:016X}): vertex input{}", vs.number,
                  key, detail);
    }
  }


  SwapTexture GetSwapTexture() const {
    SwapTexture texture;
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = DvarRecord(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    return texture;
  }

  void BufferIndirect(uint32_t address, uint32_t words, int depth) {
    AddOnlyRing(indirect_);
    RingReader reader;
    reader.base = memory_->TranslatePhysical(address);
    reader.pos = 0;
    reader.end = words;
    while (reader.Pending()) {
      if (!Packet(reader, depth)) {
        break;
      }
    }
  }


  void Present() {
    swaps_.fetch_add(1, std::memory_order_relaxed);
    bo2zm::benchmark::AnnotateFrame();
    bo2zm::pc_profiler::NoteFrame();
    g_swaps_native.fetch_add(1, std::memory_order_relaxed);
    counter_.fetch_add(1, std::memory_order_relaxed);
    if (!presenter_) {
      return;
    }

    if (targets_) {

      if (version_ramp_submitted_ != version_ramp_) {
        version_ramp_submitted_ = version_ramp_;
        targets_->RampGamma(ramp_gamma_);
      }
      const auto start = Clock::now();
      const bool presented = targets_->Present(presenter_.get(), GetSwapTexture(),
                                                   swap_width_.load(std::memory_order_relaxed),
                                                   swap_height_.load(std::memory_order_relaxed));
      time_present_ns_ += NanosecondsSince(start);
      ++presents_measures_;
      if (presented) {
        return;
      }
    }
    presenter_->RefreshGuestOutput(
        kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
        [this](rex::ui::Presenter::GuestOutputRefreshContext& context) {
          return CleanOutput(static_cast<RampOutputContext&>(context));
        });
  }

  bool CleanOutput(RampOutputContext& context) {
    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
    const auto& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      const auto output_dependencies = bo2zm::native::output::ImageDependencies();
      info.dependencyCount = uint32_t(output_dependencies.size());
      info.pDependencies = output_dependencies.data();
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      reserve.commandPool = pool_;
      reserve.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      reserve.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &reserve, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo info_fence{};
      info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &info_fence, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }


    if (fence_pending_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      fence_pending_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &start) != VK_SUCCESS) {
      return false;
    }

    VkClearValue color{};
    color.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    {
      const auto queue = vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) != VK_SUCCESS) {
        return false;
      }
    }
    fence_pending_ = true;
    context.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    targets_.reset();
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
    const auto& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pending_) {
        dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        fence_pending_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }


#define BO2ZM_REPORT_BY(defer, ...) do { if (defer) { BO2ZM_REPORT_RING(__VA_ARGS__); } else { REXLOG_INFO(__VA_ARGS__); } } while (0)
  void Report(bool force) {
    bo2zm::memcpy_callers::Report();
    const auto now = Clock::now();
    if (!force &&
        now - last_report_ < std::chrono::seconds(REXCVAR_GET(bo2zm_native_diag_report_s))) {
      return;
    }
    const bool defer_report = !force;
    last_report_ = now;
    BO2ZM_REPORT_BY(defer_report,
        "[native] swaps={} vblanks={} packets={} interrupts={} indirect={} memory_writes={} timed_out_waits={} draws={} "
        "copies={}",
        swaps_.load(), vblanks_.load(), packets_.load(), interrupts_.load(),
        indirect_.load(), writes_memory_.load(), waits_exhausted_.load(), draws_.load(),
        copies_.load());


    const double seconds = std::chrono::duration<double>(now - last_time_report_).count();
    last_time_report_ = now;
    if (seconds > 0.0) {
      BO2ZM_REPORT_BY(defer_report,
          "[native] times: ring {:.1f} ms/s; draws {:.1f} us/draw ({}); copies {:.1f} us/copy; present {:.2f} ms/Swap "
          "with the wait for the GPU",
          double(time_ring_ns_) / 1e6 / seconds,
          draws_measured_ ? double(time_draws_ns_) / 1e3 / double(draws_measured_) : 0.0,
          draws_measured_,
          copies_measures_ ? double(time_copies_ns_) / 1e3 / double(copies_measures_) : 0.0,
          presents_measures_
              ? double(time_present_ns_) / 1e6 / double(presents_measures_)
              : 0.0);


      {
        const uint64_t writes = writes_wptr_.load(std::memory_order_relaxed);
        const uint64_t d_writes = writes - writes_wptr_previous_;
        const uint64_t d_loops = loops_ring_ - loops_ring_previous_;
        const uint64_t d_with_data = loops_ring_with_data_ - loops_ring_with_data_previous_;
        const uint64_t d_exhausted = waits_ring_exhausted_ - waits_ring_exhausted_previous_;


        const uint64_t d_migrations = migrations_ring_ - migrations_ring_previous_;
        BO2ZM_REPORT_BY(defer_report, "[native] ring wake-ups: {:.0f} CP_RB_WPTR writes/s, {:.0f} loops/s, {:.0f} with "
                    "data/s ({:.2f} writes per loop with data), {:.0f} 4 ms waits timed out/s; core {} with {:.0f} "
                    "migrations/s ({:.2f} per loop)",
                    double(d_writes) / seconds, double(d_loops) / seconds, double(d_with_data) / seconds,
                    d_with_data ? double(d_writes) / double(d_with_data) : 0.0, double(d_exhausted) / seconds,
                    core_ring_, double(d_migrations) / seconds,
                    d_loops ? double(d_migrations) / double(d_loops) : 0.0);
        migrations_ring_previous_ = migrations_ring_;
        writes_wptr_previous_ = writes;
        loops_ring_previous_ = loops_ring_;
        loops_ring_with_data_previous_ = loops_ring_with_data_;
        waits_ring_exhausted_previous_ = waits_ring_exhausted_;
      }

      std::vector<std::tuple<uint64_t, int, uint64_t>> op_costs;
      if (time_registers_ns_) {
        op_costs.emplace_back(time_registers_ns_, -1, count_registers_);
      }
      for (int op = 0; op < int(time_opcode_ns_.size()); ++op) {
        if (time_opcode_ns_[op]) {
          op_costs.emplace_back(time_opcode_ns_[op], op, count_opcode_[op]);
        }
      }
      std::sort(op_costs.begin(), op_costs.end(), std::greater<>());
      std::string split;
      for (size_t i = 0; i < op_costs.size() && i < 8; ++i) {
        const auto& [ns, op, n] = op_costs[i];
        split += op < 0 ? fmt::format(" registers {:.1f} ms/s ({})", double(ns) / 1e6 / seconds, n)
                          : fmt::format(" op {:02X} {:.1f} ms/s ({})", op,
                                        double(ns) / 1e6 / seconds, n);
      }
      BO2ZM_REPORT_BY(defer_report, "[native] times per packet:{}", split);

      BO2ZM_REPORT_BY(defer_report, "[native] registers by path: {} words in blocks, {} one at a time ({:.1f} words per "
                  "type 0/1 packet)",
                  words_block_, words_loose_,
                  count_registers_ ? double(words_block_ + words_loose_) / double(count_registers_) : 0.0);
      words_block_ = words_loose_ = 0;

      if (markers_applied_ || markers_checked_ || markers_bad_) {
        BO2ZM_REPORT_BY(defer_report, "[native] D3D markers: {} applied ({:.1f} spans and {:.1f} words each; "
                    "{} spans by the usual path), {} checked against their packets ({} different), {} malformed; fast "
                    "path {} ({} spans checked)",
                    markers_applied_,
                    markers_applied_ ? double(marker_spans_) / double(markers_applied_) : 0.0,
                    markers_applied_ ? double(marker_words_) / double(markers_applied_) : 0.0,
                    marker_spans_slow_, markers_checked_, markers_different_, markers_bad_,
                    marker_fast_ > 0 ? "on" : "OFF", spans_fast_verified_);
        markers_applied_ = markers_checked_ = markers_different_ = markers_bad_ = 0;
        marker_spans_ = marker_spans_slow_ = marker_words_ = 0;
      }

      if (draws_with_marker_register_ || marker_rejected_registers_ || checks_draw_) {
        BO2ZM_REPORT_BY(defer_report, "[native] draw record in the marker: {} draws without a lookup, {} "
                    "marker records not accepted (type, count or shaders: looked up as always), {} checked against the "
                    "lookup ({} different, {} with another record and the same shaders)",
                    draws_with_marker_register_, marker_rejected_registers_, checks_draw_,
                    checks_draw_different_, checks_draw_other_register_);
        draws_with_marker_register_ = marker_rejected_registers_ = 0;
        checks_draw_ = checks_draw_different_ = checks_draw_other_register_ = 0;
      }

      if (shadow_draws_ || shadow_lost_) {


        std::vector<std::pair<uint32_t, uint64_t>> sorted(shadow_per_register_.begin(), shadow_per_register_.end());
        std::sort(sorted.begin(), sorted.end());
        struct Range {
          uint64_t n;
          uint32_t first, last;
        };
        std::vector<Range> ranges;
        for (const auto& [reg, n] : sorted) {
          if (!ranges.empty() && ranges.back().last + 1 == reg && ranges.back().n == n) {
            ranges.back().last = reg;
          } else {
            ranges.push_back({n, reg, reg});
          }
        }
        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
          return a.n * (a.last - a.first + 1) > b.n * (b.last - b.first + 1);
        });
        std::string list, list_state;
        size_t in_list = 0, in_state = 0;
        for (const Range& r : ranges) {
          const bool constant = r.first >= 0x4000 && r.first < 0x4800;
          std::string& out = constant ? list : list_state;
          size_t& count = constant ? in_list : in_state;
          if (count++ >= 8) {
            continue;
          }
          out += r.first == r.last ? fmt::format(" 0x{:04X}x{}", r.first, r.n)
                                   : fmt::format(" 0x{:04X}-0x{:04X}x{}", r.first, r.last, r.n);
        }
        if (!list_state.empty()) {
          list += " | state:" + list_state;
        }
        const size_t worst_size = sorted.size();
        size_t groups_seen = 0;
        for (uint32_t g = 0; g < kGroupsMirror; ++g) {
          groups_seen += shadow_shot_.mask[g] != 0;
        }
        BO2ZM_REPORT_BY(defer_report, "[native] D3D shadow: {} draws compared ({} snapshots lost; {} of {} mirror "
                    "groups learned) | state {} different of {} | fetch {} of {} | constants {} of {} | registers with "
                    "differences: {}{}",
                    shadow_draws_, shadow_lost_, groups_seen, kGroupsMirror, shadow_state_different_,
                    shadow_state_compared_, shadow_fetch_different_, shadow_fetch_compared_,
                    shadow_constants_different_, shadow_constants_compared_, worst_size,
                    list.empty() ? "" : " (the most:" + list + ")");
        shadow_draws_ = shadow_lost_ = 0;
        shadow_state_compared_ = shadow_state_different_ = 0;
        shadow_fetch_compared_ = shadow_fetch_different_ = 0;
        shadow_constants_compared_ = shadow_constants_different_ = 0;
        shadow_per_register_.clear();
        std::string list_read;
        {
          std::vector<std::pair<uint64_t, uint32_t>> worst;
          for (const auto& [reg, n] : shadow_per_register_read_) {
            worst.emplace_back(n, reg);
          }
          std::sort(worst.begin(), worst.end(), std::greater<>());
          for (size_t i = 0; i < worst.size() && i < 8; ++i) {
            list_read += fmt::format(" 0x{:04X}x{}", worst[i].second, worst[i].first);
          }
        }
        BO2ZM_REPORT_BY(defer_report, "[native] D3D shadow constants in registers the draw's shaders read: {} "
                    "different of {} ({} of them not given by the device's stream since its buffer began){}",
                    shadow_constants_read_different_, shadow_constants_read_compared_, shadow_constants_read_unknown_,
                    list_read.empty() ? "" : " (the most:" + list_read + ")");
        shadow_constants_read_compared_ = shadow_constants_read_different_ = shadow_constants_read_unknown_ = 0;
        shadow_per_register_read_.clear();
        BO2ZM_REPORT_BY(defer_report, "[native] D3D shadow constants by source (different; of them the ring has the "
                    "mirror's own value): mirror {} ({}), shader literal {} ({}), game-written {} ({}), command "
                    "buffer {} ({}), not given by the device's stream since its buffer began {} ({}); {}",
                    shadow_from_[0][0] + shadow_from_[0][1], shadow_from_[0][1],
                    shadow_from_[1][0] + shadow_from_[1][1], shadow_from_[1][1],
                    shadow_from_[2][0] + shadow_from_[2][1], shadow_from_[2][1],
                    shadow_from_[3][0] + shadow_from_[3][1], shadow_from_[3][1],
                    shadow_from_[4][0] + shadow_from_[4][1], shadow_from_[4][1], StreamConstantsReport());
        std::memset(shadow_from_, 0, sizeof(shadow_from_));
      }

      std::vector<std::pair<uint64_t, Wait>> waits(waits_.begin(), waits_.end());
      std::sort(waits.begin(), waits.end(), [](const auto& a, const auto& b) {
        return a.second.loops > b.second.loops;
      });
      std::string detail_waits;
      for (size_t i = 0; i < waits.size() && i < 4; ++i) {
        const auto& [key, e] = waits[i];
        detail_waits += fmt::format(
            " {} {:08X} (function {} ref {:08X} mask {:08X}): {} waits, {} loops;",
            (key >> 32) ? "memory" : "register", uint32_t(key), e.info & 0x7, e.reference,
            e.mask, e.times, e.loops);
      }
      if (!detail_waits.empty()) {
        BO2ZM_REPORT_BY(defer_report, "[native] WAIT_REG_MEM:{}", detail_waits);
      }
    }
    time_ring_ns_ = time_draws_ns_ = time_copies_ns_ = time_present_ns_ = 0;
    draws_measured_ = copies_measures_ = presents_measures_ = 0;
    time_registers_ns_ = count_registers_ = 0;
    time_opcode_ns_.fill(0);
    count_opcode_.fill(0);
    waits_.clear();
    const uint32_t frontbuffer = swap_frontbuffer_.load();
    const uint32_t target = last_copy_target_.load();
    BO2ZM_REPORT_BY(defer_report,
        "[native] last swap: frontbuffer={:08X} {}x{}; last copy: target={:08X} control={:08X} info={:08X} "
        "pitch={:08X}; match={}",
        frontbuffer, swap_width_.load(), swap_height_.load(), target, last_copy_control_.load(),
        last_copy_info_.load(), last_copy_pitch_.load(),
        (frontbuffer & 0x1FFFFFFF) == (target & 0x1FFFFFFF));
    if (targets_) {
      uint64_t copies = 0, clears = 0, presented = 0, rejections = 0;
      targets_->Stats(copies, clears, presented, rejections);
      uint64_t gpu_ns = 0, gpu_jobs = 0;
      targets_->TimeGpu(gpu_ns, gpu_jobs);
      const uint64_t category_jobs = targets_->JobsMarksCategories();
      const uint64_t category_jobs_interval = category_jobs - gpu_category_jobs_previous_;
      std::array<uint64_t, kGpuCategories> categories{};
      targets_->TimeGpuPerCategory(categories);
      const uint64_t presented_interval = presented - presented_previous_gpu_;
      {
        uint64_t wait_count = 0, wait_ns = 0;
        targets_->WaitsGpu(wait_count, wait_ns);
        bo2zm::native::performance::Window sample;
        sample.interval_seconds = seconds;
        sample.swaps = presented_interval;
        sample.jobs = gpu_jobs - gpu_jobs_previous_;
        sample.category_jobs = category_jobs_interval;
        if (presented_interval) {
          sample.gpu_ms_per_swap = double(gpu_ns - gpu_ns_previous_) / 1e6 / double(presented_interval);
          sample.gpu_wait_ms_per_swap = double(wait_ns - ns_waits_gpu_previous_) / 1e6 / double(presented_interval);
          sample.gpu_gap_ms_per_swap = double(categories[kGpuGapBetweenJobs] - gpu_categories_previous_[kGpuGapBetweenJobs]) /
              1e6 / double(presented_interval);
        }
        bo2zm::native::performance::Publish(sample);
      }
      bo2zm::native::performance::g_copies.store(copies, std::memory_order_relaxed);
      bo2zm::native::performance::g_copies_depth.store(targets_->CopiesDepth(), std::memory_order_relaxed);
      bo2zm::native::performance::g_clears.store(clears, std::memory_order_relaxed);
      bo2zm::native::performance::g_clears_depth.store(targets_->ClearsDepth(), std::memory_order_relaxed);
      BO2ZM_REPORT_BY(defer_report, "[native] C2: copies={} (of depth {}) clears={} (of depth {}) presented={} "
                  "rejected={}; GPU {:.2f} ms per Swap ({} jobs measured)",
                  copies, targets_->CopiesDepth(), clears,
                  targets_->ClearsDepth(), presented, rejections,
                  presented_interval
                      ? double(gpu_ns - gpu_ns_previous_) / 1e6 / double(presented_interval)
                      : 0.0,
                  gpu_jobs - gpu_jobs_previous_);

      {
        const auto d = [&](size_t i) { return occlusion_counters_[i] - occlusion_counters_previous_[i]; };
        uint64_t oc[5] = {};
        targets_->StatsOcclusion(oc);
        if (d(0) || d(1) || d(2) || d(4)) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2 occlusion: mode {}; game queries begun {}, ended with a "
                      "measurement {} and without a measurement yet {}, without their end {}, faked without a pair {}; "
                      "written measurements: average {:.1f}, historical maximum {}; ended by target: small (scale 1) {} "
                      "(maximum {}), scene (AA mode scale) {} (maximum {}); host: spans {} (without room {}), queries "
                      "published {}, samples {} (historical maximum per query {})",
                      REXCVAR_GET(bo2zm_native_occlusion), d(0), d(1), d(2), d(3), d(4),
                      d(1) ? double(d(5)) / double(d(1)) : 0.0, occlusion_counters_[6], occlusion_per_target_[0],
                      occlusion_max_per_target_[0], occlusion_per_target_[1], occlusion_max_per_target_[1],
                      oc[0] - occlusion_host_previous_[0], oc[1] - occlusion_host_previous_[1],
                      oc[2] - occlusion_host_previous_[2], oc[3] - occlusion_host_previous_[3], oc[4]);
        }
        occlusion_per_target_ = {};
        occlusion_counters_previous_ = occlusion_counters_;
        std::copy(std::begin(oc), std::end(oc), occlusion_host_previous_);
      }
      {
        uint64_t waits = 0, ns_waits = 0;
        targets_->WaitsGpu(waits, ns_waits);
        const double ms_waits = double(ns_waits - ns_waits_gpu_previous_) / 1e6;
        BO2ZM_REPORT_BY(defer_report, "[native] C2: ring waits for the GPU: {} ({:.1f} ms; {:.2f} ms per Swap)",
                    waits - waits_gpu_previous_, ms_waits,
                    presented_interval ? ms_waits / double(presented_interval) : 0.0);
        waits_gpu_previous_ = waits;
        ns_waits_gpu_previous_ = ns_waits;

        uint64_t jobs = 0, ns_jobs = 0;
        targets_->DurationJobsGpu(jobs, ns_jobs);
        const uint64_t d_jobs = jobs - jobs_gpu_previous_;
        const double ms_jobs = double(ns_jobs - ns_job_gpu_previous_) / 1e6;
        BO2ZM_REPORT_BY(defer_report, "[native] C2: GPU jobs by wall clock: {} ({:.1f} ms; {:.2f} ms each; {:.2f} ms "
                    "per Swap)",
                    d_jobs, ms_jobs, d_jobs ? ms_jobs / double(d_jobs) : 0.0,
                    presented_interval ? ms_jobs / double(presented_interval) : 0.0);
        jobs_gpu_previous_ = jobs;
        ns_job_gpu_previous_ = ns_jobs;
      }
      {
        uint64_t cost[6] = {};
        targets_->CostRecord(cost);
        uint64_t dc[6] = {};
        for (size_t i = 0; i < 6; ++i) {
          dc[i] = cost[i] - cost_record_previous_[i];
          cost_record_previous_[i] = cost[i];
        }
        BO2ZM_REPORT_BY(defer_report, "[native] C2: command buffers begun {} ({:.1f} ms in Record, {:.1f} ms resetting "
                    "pools); readbacks {} ({:.2f} M texels, {:.1f} ms writing them)",
                    dc[0], double(dc[1]) / 1e6, double(dc[2]) / 1e6, dc[3], double(dc[4]) / 1e6,
                    double(dc[5]) / 1e6);
      }
      if (presented_interval && category_jobs_interval == gpu_jobs - gpu_jobs_previous_ && category_jobs_interval) {
        const auto ms = [&](uint32_t c) {
          return double(categories[c] - gpu_categories_previous_[c]) / 1e6 /
                 double(presented_interval);
        };
        BO2ZM_REPORT_BY(defer_report, "[native] C2: GPU per Swap: shadows {:.2f} ms, scene {:.2f}, pitch640-1279 {:.2f}, "
                    "320 (cube and blur) {:.2f}, minor {:.2f}, copies {:.2f}, clears {:.2f}, rest {:.2f}, scene without "
                    "depth {:.2f}, gap between jobs {:.2f}",
                    ms(kGpuShadows), ms(kGpuScene), ms(kGpuReflection), ms(kGpu320),
                    ms(kGpuSmaller), ms(kGpuCopies), ms(kGpuClears), ms(kGpuOthers),
                    ms(kGpuSceneNoDepth), ms(kGpuGapBetweenJobs));
      } else if (presented_interval) {
        BO2ZM_REPORT_BY(defer_report, "[native] C2: GPU category split unavailable: {} of {} timed jobs have "
                        "category marks (endpoints keep total timing; pass classification uses target widths)",
                        category_jobs_interval, gpu_jobs - gpu_jobs_previous_);
      }
      gpu_categories_previous_ = categories;

      if (presented_interval) {
        std::array<uint64_t, 4> copies_t{}, pixels_t{};
        targets_->CopiesPerSize(copies_t, pixels_t);
        static constexpr const char* kBuckets[] = {"<=64x64", "<=320x320", "<=1024x1024", "larger"};
        std::string line;
        for (uint32_t c = 0; c < 4; ++c) {
          const uint64_t n = copies_t[c] - copies_bucket_previous_[c];
          const uint64_t px = pixels_t[c] - pixels_bucket_previous_[c];
          if (n) {
            line += fmt::format(" {} {:.1f} copies and {:.2f} Mpixels", kBuckets[c],
                                 double(n) / double(presented_interval),
                                 double(px) / 1e6 / double(presented_interval));
          }
        }
        if (!line.empty()) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2 copies per frame and size:{}", line);
        }
        copies_bucket_previous_ = copies_t;
        pixels_bucket_previous_ = pixels_t;
      }


      {
        std::array<uint64_t, kGpuCategories> frag{}, vert{}, prim{};
        targets_->StatsPipeline(frag, vert, prim);
        static constexpr const char* kTypesStat[] = {"rest",  "shadows", "scene",  "reflection",
                                                      "cube",   "minor", "copies",  "clears",
                                                      "scene without depth", "gap"};
        std::string line;
        for (uint32_t c = 0; presented_interval && c < kGpuCategories; ++c) {
          const uint64_t f = frag[c] - fragments_category_previous_[c];
          const uint64_t v = vert[c] - vertices_category_previous_[c];
          const uint64_t p = prim[c] - primitives_category_previous_[c];
          if (f || v) {
            line += fmt::format(" {} {:.2f} M fragments, {:.0f} k vertices, {:.0f} k primitives;",
                                 kTypesStat[c], double(f) / 1e6 / double(presented_interval),
                                 double(v) / 1e3 / double(presented_interval),
                                 double(p) / 1e3 / double(presented_interval));
          }
        }
        if (!line.empty()) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2 per frame and pass type:{}", line);
        }
        fragments_category_previous_ = frag;
        vertices_category_previous_ = vert;
        primitives_category_previous_ = prim;
      }


      {
        std::vector<uint64_t> frag_ps, draw_ps;
        uint64_t frames = 0;
        targets_->StatsPerShader(frag_ps, draw_ps, frames);
        if (frames > frames_diagnostic_previous_) {
          frames_diagnostic_previous_ = frames;
          static constexpr const char* kTypesPs[] = {"rest",  "shadows", "scene",  "reflection",
                                                     "cube",   "minor", "copies",  "clears",
                                                     "scene without depth", "gap"};
          const size_t per_category = frag_ps.size() / kGpuCategories;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            uint64_t total = 0;
            std::vector<uint32_t> order;
            for (size_t i = 0; i < per_category; ++i) {
              const size_t e = size_t(c) * per_category + i;
              total += frag_ps[e];
              if (frag_ps[e]) {
                order.push_back(uint32_t(e));
              }
            }
            uint64_t draws = 0;
            for (size_t i = 0; i < per_category; ++i) {
              draws += draw_ps[size_t(c) * per_category + i];
            }
            if (!total && !draws) {
              continue;
            }
            std::sort(order.begin(), order.end(),
                      [&](uint32_t a, uint32_t b) { return frag_ps[a] > frag_ps[b]; });
            std::string line;
            for (size_t i = 0; i < order.size() && i < 8; ++i) {
              const uint32_t e = order[i];
              const uint32_t ps = uint32_t(e % per_category);
              line += fmt::format(" PS {} {:.2f} M ({:.0f} %, {:.1f} draws)",
                                   ps ? fmt::format("n{}", ps - 1) : std::string("none"),
                                   double(frag_ps[e]) / 1e6 / double(frames),
                                   100.0 * double(frag_ps[e]) / double(total),
                                   double(draw_ps[e]) / double(frames));
            }
            BO2ZM_REPORT_BY(defer_report, "[native] C2 {} fragments per pixel shader ({} frames, {:.2f} M and {:.0f} "
                        "draws per frame):{}",
                        kTypesPs[c], frames, double(total) / 1e6 / double(frames),
                        double(draws) / double(frames), line);
          }
        }
      }


      {
        const uint64_t mark = targets_->MarkGpuFinalNs();
        const Clock::time_point now = Clock::now();
        if (mark != 0 && mark != mark_gpu_previous_ns_) {
          if (mark_gpu_previous_ns_ != 0 && mark > mark_gpu_previous_ns_) {
            const double real_ns = double(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - clock_mark_previous_).count());
            const double advance_ns = double(mark - mark_gpu_previous_ns_);
            scale_real_ns_ += real_ns;
            scale_marks_ns_ += advance_ns;
            const double scale = scale_real_ns_ / scale_marks_ns_;
            BO2ZM_REPORT_BY(defer_report, "[native] C2: GPU mark scale {:.3f} in this interval ({:.3f} accumulated; "
                        "real time between reports / advance of the marks); real GPU per Swap {:.2f} ms",
                        real_ns / advance_ns, scale,
                        presented_interval
                            ? double(gpu_ns - gpu_ns_previous_) / 1e6 / double(presented_interval) * scale
                            : 0.0);
          }
          mark_gpu_previous_ns_ = mark;
          clock_mark_previous_ = now;
        }
      }

      {
        const uint64_t precise = targets_->JobsMarksPrecise();
        BO2ZM_REPORT_BY(defer_report, "[native] C2: precise marks in {} of {} measured "
                                      "jobs", precise - jobs_precise_previous_,
                    gpu_jobs - gpu_jobs_previous_);
        jobs_precise_previous_ = precise;
      }

      {
        std::array<uint64_t, kBucketsSwap> buckets{};
        uint64_t overlaps = 0;
        double worst_ms = 0.0;
        targets_->IntervalsBetweenSwaps(buckets, overlaps, worst_ms);
        uint64_t total = 0;
        for (uint32_t i = 0; i < kBucketsSwap; ++i) {
          total += buckets[i] - buckets_swap_previous_[i];
        }
        if (total) {
          const auto c = [&](uint32_t i) { return buckets[i] - buckets_swap_previous_[i]; };
          BO2ZM_REPORT_BY(defer_report, "[native] C2: intervals between Swaps: <15 ms {} | 15-18 {} | 18-25 {} | 25-30 "
                      "{} | 30-36 {} | 36-50 {} | 50-60 {} | 60-75 {} | 75-100 {} | 100-150 {} | >=150 {}; worst frame "
                      "{:.1f} ms; overlapping jobs on the GPU {}",
                      c(0), c(1), c(2), c(3), c(4), c(5), c(6), c(7), c(8), c(9), c(10), worst_ms,
                      overlaps - overlaps_gpu_previous_);
        }
        buckets_swap_previous_ = buckets;
        bo2zm::guard30::Report();
        {

          static uint64_t writes_prev = 0, same_prev = 0, blocks_prev = 0, blocks_same_prev = 0;
          const uint64_t writes = constants_written_ - writes_prev;
          const uint64_t same = constants_no_change_ - same_prev;
          const uint64_t blocks = blocks_constants_ - blocks_prev;
          const uint64_t blocks_same = blocks_constants_no_change_ - blocks_same_prev;
          writes_prev = constants_written_; same_prev = constants_no_change_;
          blocks_prev = blocks_constants_; blocks_same_prev = blocks_constants_no_change_;
          if (writes > 0 || blocks > 0) {
            BO2ZM_REPORT_BY(defer_report, "[native] C6 game constants: {} single writes ({} unchanged, {:.1f} %) | {} "
                        "blocks ({} unchanged, {:.1f} %)",
                        writes, same, writes ? 100.0 * double(same) / double(writes) : 0.0,
                        blocks, blocks_same, blocks ? 100.0 * double(blocks_same) / double(blocks) : 0.0);
          }
        }
        {


          static uint64_t fe_prev = 0, fs_prev = 0, fg_prev = 0;
          static uint64_t ee_prev = 0, is_prev = 0, eg_prev = 0, draw_prev = 0;
          const uint64_t fe = fetch_written_ - fe_prev, fs = fetch_no_change_ - fs_prev;
          const uint64_t fg = generation_fetch_ - fg_prev;
          const uint64_t ee = framing_written_ - ee_prev, es = framing_no_change_ - is_prev;
          const uint64_t eg = generation_framing_ - eg_prev;


          const uint64_t draw_total = draws_.load(std::memory_order_relaxed);
          const uint64_t draw = draw_total - draw_prev;
          fe_prev = fetch_written_; fs_prev = fetch_no_change_; fg_prev = generation_fetch_;
          ee_prev = framing_written_; is_prev = framing_no_change_; eg_prev = generation_framing_;
          draw_prev = draw_total;
          if (fe > 0 || ee > 0) {
            BO2ZM_REPORT_BY(defer_report, "[native] C6 generations: fetch {} writes ({} unchanged), {} changes ({:.2f} "
                        "per draw) | framing {} writes ({} unchanged), {} changes ({:.2f} per draw); draws {}",
                        fe, fs, fg, draw ? double(fg) / double(draw) : 0.0,
                        ee, es, eg, draw ? double(eg) / double(draw) : 0.0, draw);
          }
        }
        overlaps_gpu_previous_ = overlaps;
      }

      {
        uint64_t c[12] = {};
        targets_->CostPresent(c);
        const auto d = [&](int i) { return c[i] - cost_present_previous_[i]; };
        const auto average_of = [&](int ns, int times) { return d(times) ? double(d(ns)) / 1e6 / double(d(times)) : 0.0; };
        if (d(0) || d(2) || d(5) || d(8)) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2: present (ms per call): wait for the previous output {:.2f} ({}); "
                      "work submit: queue lock {:.2f}, vkQueueSubmit {:.2f} ({}); output submit: lock {:.2f}, "
                      "vkQueueSubmit {:.2f} ({}); RefreshGuestOutput: before the call {:.2f}, inside {:.2f}, after "
                      "{:.2f} ({})",
                      average_of(1, 0), d(0), average_of(3, 2), average_of(4, 2), d(2), average_of(6, 5), average_of(7, 5), d(5),
                      average_of(9, 8), average_of(10, 8), average_of(11, 8), d(8));
        }
        std::copy(c, c + 12, cost_present_previous_);
      }
      gpu_ns_previous_ = gpu_ns;
      gpu_jobs_previous_ = gpu_jobs;
      gpu_category_jobs_previous_ = category_jobs;
      presented_previous_gpu_ = presented;
      const DrawStats d = targets_->DrawsStats();
      std::string causes;
      for (const auto& [cause, n] : d.causes) {
        causes += fmt::format(" {}={}", cause, n);
      }

      const uint64_t recorded = d.drawn - drawn_previous_;
      const uint64_t timed = d.drawn_timed - timed_previous_;
      if (recorded && timed) {
        static constexpr const char* kStages[] = {"state",  "indices",  "textures",
                                                  "pass",    "uploads",  "pipeline",
                                                  "record",  "(of the pass stage, the pass change)",
                                                  "[of the pass change, close the previous one]",
                                                  "[targets]", "[render pass and framebuffer]",
                                                  "[open the pass]"};
        std::string stages;
        for (size_t i = 0; i < d.stages_ns.size(); ++i) {
          stages += fmt::format(" {} {:.1f}", kStages[i],
                                double(d.stages_ns[i] - stages_previous_[i]) / 1e3 / double(timed));
        }


        BO2ZM_REPORT_BY(defer_report, "[native] C6 stages (us per RECORDED draw, sample of {} = 1 in {:.0f} recorded; "
                    "recorded {}):{}",
                    timed, double(recorded) / double(timed), recorded, stages);
      }


      BO2ZM_REPORT_BY(defer_report, "[native] C6 {}", bo2zm::scenery_lod::Summary());


      {
        const uint64_t soon = d.vegetation_soon - vegetation_soon_previous_;
        vegetation_soon_previous_ = d.vegetation_soon;
        if (presented_interval) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 shadow vegetation dropped early: {:.0f} per frame ({} in the "
                      "interval); saves the indices, textures, upload and pass of each one",
                      double(soon) / double(presented_interval), soon);
        }
      }
      {

        const uint64_t with = d.scene_with_discard, no = d.scene_no_discard;
        const uint64_t d_with = with - discard_with_previous_, d_without = no - discard_no_previous_;
        discard_with_previous_ = with;
        discard_no_previous_ = no;
        if (d_with + d_without) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 early discard in the scene: {} draws allow it, {} prevent it "
                      "(alpha test, kill or depth) = {:.0f} %",
                      d_without, d_with, 100.0 * double(d_with) / double(d_with + d_without));
        }
      }
      drawn_previous_ = d.drawn;
      timed_previous_ = d.drawn_timed;
      stages_previous_ = d.stages_ns;
      {
        const std::array<uint64_t, 20> counters = {d.passes, d.submits_full,
                                                      d.ns_submits_full, d.bytes_vertices,
                                                      d.bytes_indices, d.samplers,
                                                      d.samplers_cache, d.ns_passes,
                                                      d.ns_vertices, d.inputs_computed,
                                                      d.ns_inputs, d.inputs_reused,
                                                      d.passes_per_generation, d.passes_per_target,
                                                      d.passes_resumed, d.ns_render_pass,
                                                      d.bytes_repeated_frame,
                                                      d.bytes_equal_earlier, d.ns_hash_vertices,
                                                      d.texels_passes};

        const std::array<uint64_t, 3> dedupe = {d.dedupe_hits, d.dedupe_bytes,
                                                d.dedupe_collisions};
        const auto ddelta = [&](size_t i) { return dedupe[i] - dedupe_previous_[i]; };
        const auto delta = [&](size_t i) { return counters[i] - counters_previous_[i]; };
        BO2ZM_REPORT_BY(defer_report, "[native] C6 counters: passes {} submits because the upload buffer was full {} "
                    "({:.1f} ms) vertices {:.1f} MB indices {:.1f} MB samplers {} (cache {}); {:.1f} ms in pass changes "
                    "({:.1f} us each), {:.1f} ms copying vertices",
                    delta(0), delta(1), double(delta(2)) / 1e6, double(delta(3)) / 1048576.0,
                    double(delta(4)) / 1048576.0, delta(5), delta(6), double(delta(7)) / 1e6,
                    delta(0) ? double(delta(7)) / 1e3 / double(delta(0)) : 0.0,
                    timed ? double(delta(8)) / 1e6 * double(recorded) / double(timed) : 0.0);
        if (presented_interval) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 deduplicated vertices: {} bindings reused, {:.2f} MB NOT copied "
                      "({:.2f} MB per frame, {:.0f} % of those uploaded){}",
                      ddelta(0), double(ddelta(1)) / 1048576.0,
                      double(ddelta(1)) / 1048576.0 / double(presented_interval),
                      delta(3) + ddelta(1)
                          ? 100.0 * double(ddelta(1)) / double(delta(3) + ddelta(1))
                          : 0.0,
                      ddelta(2) ? fmt::format("; *** {} COLLISIONS: the table is too small ***",
                                              ddelta(2))
                                : "");
        }
        dedupe_previous_ = dedupe;
        BO2ZM_REPORT_BY(defer_report, "[native] C6 counters: vertex inputs computed {} ({:.1f} ms, {:.1f} us each), "
                    "reused {}",
                    delta(9), double(delta(10)) / 1e6,
                    delta(9) ? double(delta(10)) / 1e3 / double(delta(9)) : 0.0, delta(11));
        BO2ZM_REPORT_BY(defer_report, "[native] C6 passes: because of a new command buffer {}, because of targets {}, "
                    "resumed after a copy or clear {}; {:.1f} ms in vkCmdBegin/EndRenderPass; open area {:.1f} Mtexels "
                    "({:.2f} Mtexels per frame)",
                    delta(12), delta(13), delta(14), double(delta(15)) / 1e6,
                    double(delta(19)) / 1e6,
                    presented_interval ? double(delta(19)) / 1e6 / double(presented_interval)
                                          : 0.0);
        if (presented_interval) {
          static constexpr const char* kTypes[] = {"rest",  "shadows", "scene",  "reflection",
                                                   "cube",   "minor", "copies",  "clears",
                                                   "scene without depth", "gap"};
          std::string area;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            const uint64_t t = d.texels_per_category[c] - texels_category_previous_[c];
            const uint64_t n = d.passes_per_category[c] - passes_category_previous_[c];
            if (!t && !n) {
              continue;
            }
            const uint64_t draw = d.draws_per_category[c] - draws_category_previous_[c];
            const uint64_t tri = d.triangles_per_category[c] - triangles_category_previous_[c];
            area += fmt::format(" {} {:.2f} Mtexels in {:.1f} passes ({:.0f} draws, {:.0f} k triangles)",
                                kTypes[c], double(t) / 1e6 / double(presented_interval),
                                double(n) / double(presented_interval),
                                double(draw) / double(presented_interval),
                                double(tri) / 1e3 / double(presented_interval));
          }
          BO2ZM_REPORT_BY(defer_report, "[native] C6 open area per frame and target type:{}", area);
        }
        if (presented_interval) {

          BO2ZM_REPORT_BY(defer_report, "[native] C6 colorless draws per frame: {:.0f} with a pixel shader to spare, "
                      "{:.0f} that need it (alpha test, kill or depth); in the shadow map {:.0f} with the alpha test "
                      "set and {:.0f} without it",
                      double(d.draws_ps_useless - draws_ps_useless_previous_) /
                          double(presented_interval),
                      double(d.draws_ps_needed - draws_ps_needed_previous_) /
                          double(presented_interval),
                      double(d.shadows_alpha_active - shadows_alpha_active_previous_) /
                          double(presented_interval),
                      double(d.shadows_alpha_off - shadows_alpha_off_previous_) /
                          double(presented_interval));
        }
        shadows_alpha_active_previous_ = d.shadows_alpha_active;
        shadows_alpha_off_previous_ = d.shadows_alpha_off;
        draws_ps_useless_previous_ = d.draws_ps_useless;
        draws_ps_needed_previous_ = d.draws_ps_needed;
        texels_category_previous_ = d.texels_per_category;
        passes_category_previous_ = d.passes_per_category;
        draws_category_previous_ = d.draws_per_category;
        triangles_category_previous_ = d.triangles_per_category;

        BO2ZM_REPORT_BY(defer_report, "[native] C6 constants by UBO: {} of {} submits", d.submits_ubo - submits_ubo_previous_,
                    d.submits - submits_previous_);
        {
          const uint64_t observed = d.shared_observed - shared_observed_previous_;
          const uint64_t changed = d.shared_changed - shared_changed_previous_;
          shared_observed_previous_ = d.shared_observed;
          shared_changed_previous_ = d.shared_changed;
          if (observed > 0) {
            BO2ZM_REPORT_BY(defer_report, "[native] C6 shared constants: {} of {} draws change the 488 bytes ({:.1f} "
                        "%); the others only pay for the memcmp",
                        changed, observed, 100.0 * double(changed) / double(observed));
          }
        }
        submits_ubo_previous_ = d.submits_ubo;
        submits_previous_ = d.submits;

        {
          uint64_t total = 0;
          for (uint64_t n : writes_ramp_) {
            total += n;
          }
          if (total != writes_ramp_previous_) {
            writes_ramp_previous_ = total;
            uint32_t different = 0;
            for (uint32_t i = 0; i < 256; ++i) {
              const uint16_t identity = uint16_t(i * 0x3FF / 0xFF);
              if (ramp_gamma_[i][0] != identity || ramp_gamma_[i][1] != identity ||
                  ramp_gamma_[i][2] != identity) {
                ++different;
              }
            }
            const auto sample = [&](uint32_t i) {
              return fmt::format("[{}]={}/{}/{}", i, ramp_gamma_[i][0], ramp_gamma_[i][1], ramp_gamma_[i][2]);
            };
            BO2ZM_REPORT_BY(defer_report, "[native] C2 gamma ramp: writes mode {} index {} sequential {} PWL {} 30 bits "
                        "{} 1926 {} mask {} (value {}); entries of the 256 table different from the identity {}; {} {} "
                        "{} {} {} (identity: i*1023/255); version {}",
                        writes_ramp_[0], writes_ramp_[1], writes_ramp_[2], writes_ramp_[3],
                        writes_ramp_[4], writes_ramp_[5], writes_ramp_[6], mask_ramp_, different,
                        sample(0), sample(64), sample(128), sample(192), sample(255), version_ramp_);

            bool channels_equal = true;
            for (const auto& input : ramp_gamma_) {
              channels_equal = channels_equal && input[0] == input[1] && input[0] == input[2];
            }
            static constexpr const char* kNamesChannel[3] = {"red", "green", "blue"};
            for (uint32_t c = 0; c < (channels_equal ? 1u : 3u); ++c) {
              std::string list;
              list.reserve(256 * 5);
              for (uint32_t i = 0; i < 256; ++i) {
                list += fmt::format("{}{}", i ? " " : "", ramp_gamma_[i][c]);
              }
              BO2ZM_REPORT_BY(defer_report, "[native] C2 gamma ramp, table {}: {}",
                          channels_equal ? "of the three channels" : kNamesChannel[c], list);
            }
          }
        }
        if (delta(16) || delta(17)) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 repeated vertices: {:.1f} MB copied; {:.1f} MB repeated in the "
                      "same frame and {:.1f} MB equal to an earlier frame; {:.1f} ms in hashes",
                      double(delta(3)) / 1048576.0, double(delta(16)) / 1048576.0,
                      double(delta(17)) / 1048576.0, double(delta(18)) / 1e6);
        }
        counters_previous_ = counters;
      }
      BO2ZM_REPORT_BY(defer_report, "[native] C6: drawn={} rejected={} pipelines={} ({} ms creating) textures={} ({} "
                  "MB) texture_uploads={} MB_uploaded={}; rejections by cause:{}",
                  d.drawn, d.rejected, d.pipelines, d.ms_pipelines, d.textures,
                  d.megs_textures, d.uploads_texture, d.megs_uploaded, causes);
    }
    if (shaders_.loaded()) {
      const NativeShaderStats e = shaders_.Stats();
      BO2ZM_REPORT_BY(defer_report, "[native] C5a: loads={} distinct={} identified={} no_identify={} ambiguous={}; "
                  "draws with VS and PS={} without VS={} without PS={}; VS/PS pairs={}",
                  e.loads + loads_cached_, e.different, e.identified, e.no_identify, e.ambiguous,
                  draws_identified_, draws_no_vs_, draws_no_ps_, pairs_count_);

      {
        const uint64_t no_memcmp = im_i_no_memcmp_;
        const uint64_t with_memcmp = im_i_with_memcmp_;
        BO2ZM_REPORT_BY(defer_report,
                            "[native] C5a IM_LOAD without memcmp: phase {}; in the interval {} loads "
                            "without reading the microcode ({:.1f} %) and {} with memcmp ({} compared with the shortcut "
                            "and equal, {} because of a write to their slot, {} not recorded because of a half-done "
                            "write); checked to apply {} of {}{} | reported writes since boot: {} in-place patches, {} "
                            "in the ring copy, {} others, {} creations",
                            NamePhaseImShortcut(), no_memcmp,
                            no_memcmp + with_memcmp
                                ? 100.0 * double(no_memcmp) / double(no_memcmp + with_memcmp)
                                : 0.0,
                            with_memcmp, im_i_checked_, im_i_version_changed_, im_i_cancelled_,
                            std::min(im_shortcut_checked_, kImShortcutALook), kImShortcutALook,
                            im_shortcut_phase_ == kImShortcutObserving && im_shortcut_checked_ >= kImShortcutALook
                                ? " (no in-place patch seen: the sub_825A2FB8 hook does not run)"
                                : "",
                            microcode::g_patches_in_room.load(std::memory_order_relaxed),
                            microcode::g_patches_in_copy.load(std::memory_order_relaxed),
                            microcode::g_patches_others.load(std::memory_order_relaxed),
                            microcode::g_creations.load(std::memory_order_relaxed));
        im_i_no_memcmp_ = im_i_with_memcmp_ = im_i_checked_ = im_i_version_changed_ = im_i_cancelled_ = 0;
      }

      if (imm_phase_ != kImmNoBegin) {
        const uint64_t total =
            imm_i_hits_ + imm_i_checked_ + imm_i_misses_ + imm_i_back_ + imm_i_no_cache_;
        uint32_t occupied = 0;
        for (const auto& per_type : imm_cache_) {
          for (const auto& set : per_type) {
            for (const ImmediateLoad& via : set) {
              occupied += via.signature != 0 ? 1 : 0;
            }
          }
        }
        BO2ZM_REPORT_BY(defer_report,
                            "[native] C5a IM_LOAD_IMMEDIATE with a cache: phase {}; in the interval {} "
                            "loads: {} hits without flipping or identifying ({:.1f} %), {} hits checked against the "
                            "usual path, {} misses (new ways), {} because the ring wrapped around and {} without a "
                            "cache; ways in use {} of {}; hits checked since boot {}",
                            NamePhaseImmediate(), total, imm_i_hits_,
                            total ? 100.0 * double(imm_i_hits_) / double(total) : 0.0, imm_i_checked_,
                            imm_i_misses_, imm_i_back_, imm_i_no_cache_, occupied,
                            2 * kImmWays * (uint32_t(1) << kImmBitsSet), imm_checked_);
        imm_i_hits_ = imm_i_checked_ = imm_i_misses_ = imm_i_back_ = imm_i_no_cache_ = 0;
      }

      BO2ZM_REPORT_BY(defer_report,
                          "[native] C6 fences with pending vertex copies (measurement only): {} of {} fences "
                          "and interrupts in the interval (at most {} pending copies)",
                          fences_with_copies_, fences_total_, fences_copies_max_);
      fences_with_copies_ = fences_total_ = 0;
      fences_copies_max_ = 0;

      BO2ZM_REPORT_BY(defer_report,
                          "[native] C6 game ahead of the ring (measurement only): when the ring finished a "
                          "frame, the game was 0 Swaps ahead in {}, 1 in {} and 2 or more in {} (at most {}; difference "
                          "at the first frame {})",
                          game_ahead_[0], game_ahead_[1], game_ahead_[2], game_ahead_max_,
                          game_ahead_first_);
      game_ahead_[0] = game_ahead_[1] = game_ahead_[2] = 0;
      game_ahead_max_ = 0;
      const HookStats g = HooksStats();
      BO2ZM_REPORT_BY(defer_report, "[native] C5b: shaders created VS={} (known {}) PS={} (known {}); draws paired={} "
                  "without_record={} records skipped={} lost={}; with VS={} with PS={}; records with shaders different "
                  "from the IM_LOADs={} (draws left without a record because of it={}); draws with the ring's "
                  "identity={}; VS variants={} (different length {}, elements without a fetch {})",
                  g.created_vs, g.known_vs, g.created_ps, g.known_ps,
                  draws_paired_, draws_no_register_, registers_skipped_, g.lost,
                  draws_with_vs_, draws_with_ps_, candidates_incoherent_,
                  draws_incoherent_, draws_per_im_load_, variants_views_.size(),
                  variants_length_different_, elements_no_fetch_);
    }
    if (!histogram_annotated_ && swaps_.load() >= 60) {
      histogram_annotated_ = true;
      std::string lines;
      for (uint32_t op = 0; op < opcodes_.size(); ++op) {
        const uint64_t n = opcodes_[op].load();
        if (n) {
          lines += fmt::format(" {:02X}={}", op, n);
        }
      }
      BO2ZM_REPORT_BY(defer_report, "[native] type 3 packets by opcode (hex=count):{}", lines);
    }
  }

#undef BO2ZM_REPORT_BY

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  bool mmio_registered_ = false;

  std::vector<uint32_t> registers_;
  std::vector<std::atomic<uint8_t>> seen_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> generation_ring_{0};
  std::atomic<uint32_t> read_returned_{0};
  std::atomic<uint32_t> pointer_write_{0};

  std::atomic<uint64_t> writes_wptr_{0};
  uint64_t writes_wptr_previous_ = 0;

  int core_ring_ = -1;
  uint64_t migrations_ring_ = 0;
  uint64_t migrations_ring_previous_ = 0;
  uint64_t loops_ring_ = 0;
  uint64_t loops_ring_previous_ = 0;
  uint64_t loops_ring_with_data_ = 0;
  uint64_t loops_ring_with_data_previous_ = 0;
  uint64_t waits_ring_exhausted_ = 0;
  uint64_t waits_ring_exhausted_previous_ = 0;
  std::atomic<uint32_t> counter_{0};
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFull;

  std::atomic<bool> active_{false};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> thread_vblank_;
  rex::system::object_ref<rex::system::XHostThread> thread_ring_;

  std::atomic<uint64_t> swaps_{0};
  std::atomic<uint64_t> vblanks_{0};
  std::atomic<uint64_t> packets_{0};
  std::atomic<uint64_t> interrupts_{0};
  uint64_t fences_total_ = 0;
  uint64_t fences_with_copies_ = 0;
  size_t fences_copies_max_ = 0;
  uint64_t game_ahead_[3] = {};
  int64_t game_ahead_max_ = 0;
  int64_t game_ahead_first_ = 0;
  bool game_ahead_first_valid_ = false;
  std::atomic<uint64_t> indirect_{0};
  std::atomic<uint64_t> writes_memory_{0};
  std::atomic<uint64_t> waits_exhausted_{0};
  std::atomic<uint64_t> draws_{0};
  std::atomic<uint64_t> copies_{0};
  std::atomic<uint32_t> last_copy_target_{0};
  std::atomic<uint32_t> last_copy_control_{0};
  std::atomic<uint32_t> last_copy_info_{0};
  std::atomic<uint32_t> last_copy_pitch_{0};
  std::atomic<uint32_t> swap_frontbuffer_{0};
  std::atomic<uint32_t> swap_width_{0};
  std::atomic<uint32_t> swap_height_{0};
  std::array<std::atomic<uint64_t>, 128> opcodes_{};
  bool histogram_annotated_ = false;

  std::unique_ptr<NativeTargets> targets_;
  bool targets_failed_ = false;

  NativeShaders shaders_;
  const ShaderInput* vs_current_ = nullptr;
  const ShaderInput* ps_current_ = nullptr;
  std::vector<uint32_t> microcode_;
  uint64_t draws_identified_ = 0;
  uint64_t draws_no_vs_ = 0;
  uint64_t draws_no_ps_ = 0;
  static constexpr uint32_t kPairSlots = 4096;
  std::array<uint64_t, kPairSlots> pairs_{};
  uint32_t pairs_count_ = 0;

  std::deque<DrawRecord> pending_;
  std::unordered_map<uint32_t, const ShaderInput*> objects_;
  uint64_t generation_objects_ = UINT64_MAX;

  struct ObjectMemo {
    uint32_t object = UINT32_MAX;
    const ShaderInput* input = nullptr;
  };
  std::array<ObjectMemo, 16> memo_objects_{};
  uint64_t last_pair_shaders_ = UINT64_MAX;
  std::span<const uint32_t> vs_microcode_;
  const ShaderInput* vs_draw_ = nullptr;
  const ShaderInput* ps_draw_ = nullptr;
  uint64_t draws_paired_ = 0;
  uint64_t draws_no_register_ = 0;
  uint64_t registers_skipped_ = 0;
  uint64_t draws_with_vs_ = 0;
  uint64_t draws_with_ps_ = 0;
  uint64_t candidates_incoherent_ = 0;
  uint64_t draws_incoherent_ = 0;
  const ShaderInput* coherence_vs_ = nullptr;
  uint64_t coherence_generation_ = UINT64_MAX;
  bool coherence_ = false;
  const ShaderInput* annotated_vs_ = nullptr;
  uint64_t annotated_generation_ = UINT64_MAX;
  uint32_t warnings_c5b_ = 0;
  uint64_t variants_length_different_ = 0;
  uint64_t elements_no_fetch_ = 0;
  std::unordered_set<uint64_t> variants_views_;

  uint64_t generation_vs_ = 0;

  struct ShaderLoad {
    uint32_t address_type = 0;
    uint32_t size = 0;
    std::vector<uint32_t> raw;
    std::vector<uint32_t> host;
    const ShaderInput* input = nullptr;
    uint64_t hash = 0;
    uint64_t generation = 0;
    uint64_t use = 0;


    bool validated = false;
    uint32_t version_slot = 0;
    uint32_t version_global = 0;
  };
  std::array<std::array<std::array<ShaderLoad, 2>, 128>, 2> loads_cache_{};
  uint64_t loads_tic_ = 0;
  uint64_t loads_cached_ = 0;
  uint64_t generations_microcode_ = 0;

  static constexpr int kImShortcutNoBegin = -1;
  static constexpr int kImShortcutObserving = 0;
  static constexpr int kImShortcutApplying = 1;
  static constexpr int kImShortcutOff = 2;
  static constexpr uint64_t kImShortcutALook = 200000;
  static constexpr uint64_t kImShortcutCheckEvery = 4096;
  int im_shortcut_phase_ = kImShortcutNoBegin;
  uint64_t im_shortcut_turn_ = 0;
  uint64_t im_shortcut_checked_ = 0;

  uint64_t im_i_no_memcmp_ = 0;
  uint64_t im_i_with_memcmp_ = 0;
  uint64_t im_i_checked_ = 0;
  uint64_t im_i_version_changed_ = 0;
  uint64_t im_i_cancelled_ = 0;
  std::vector<uint32_t> vs_immediate_;

  static constexpr uint32_t kImmBitsSet = 5;
  static constexpr uint32_t kImmWays = 8;
  static constexpr int kImmNoBegin = -1;
  static constexpr int kImmObserving = 0;
  static constexpr int kImmApplying = 1;
  static constexpr int kImmOff = 2;
  static constexpr uint64_t kImmALook = 20000;
  static constexpr uint64_t kImmCheckEvery = 1024;
  std::array<std::array<std::array<ImmediateLoad, kImmWays>, (size_t(1) << kImmBitsSet)>, 2> imm_cache_{};
  std::array<std::array<uint8_t, (size_t(1) << kImmBitsSet)>, 2> imm_last_way_{};
  int imm_phase_ = kImmNoBegin;
  uint64_t imm_turn_ = 0;
  uint64_t imm_tic_ = 0;
  uint64_t imm_checked_ = 0;

  uint64_t imm_i_hits_ = 0;
  uint64_t imm_i_checked_ = 0;
  uint64_t imm_i_misses_ = 0;
  uint64_t imm_i_back_ = 0;
  uint64_t imm_i_no_cache_ = 0;
  const ShaderInput* mapped_vs_ = nullptr;
  uint64_t mapped_generation_ = UINT64_MAX;


  uint64_t constants_written_ = 0;
  uint64_t constants_no_change_ = 0;
  uint64_t blocks_constants_ = 0;
  uint64_t blocks_constants_no_change_ = 0;
  uint64_t generation_constants_vs_ = 0;
  int registers_in_block_ = -1;
  uint64_t generation_constants_ps_ = 0;


  uint64_t generation_fetch_ = 0;
  uint64_t generation_framing_ = 0;
  uint64_t fetch_written_ = 0;
  uint64_t fetch_no_change_ = 0;
  uint64_t framing_written_ = 0;
  uint64_t framing_no_change_ = 0;

  uint64_t time_ring_ns_ = 0;
  uint64_t time_draws_ns_ = 0;
  uint64_t time_copies_ns_ = 0;
  uint64_t time_present_ns_ = 0;
  uint64_t draws_measured_ = 0;
  uint64_t phase_stopwatch_draws_ = 0;
  uint64_t copies_measures_ = 0;
  uint64_t presents_measures_ = 0;

  uint64_t time_registers_ns_ = 0;
  uint64_t count_registers_ = 0;
  uint64_t words_block_ = 0;

  static constexpr uint64_t kBlocksAVerify = 200000;
  int registers_vector_ = -1;
  uint64_t blocks_verified_ = 0;
  std::vector<uint32_t> copy_verification_;

  MirrorSnapshot shadow_shot_;
  uint64_t shadow_draws_ = 0;
  uint64_t shadow_lost_ = 0;
  uint64_t shadow_state_compared_ = 0;
  uint64_t shadow_state_different_ = 0;
  uint64_t shadow_fetch_compared_ = 0;
  uint64_t shadow_fetch_different_ = 0;
  uint64_t shadow_constants_compared_ = 0;
  uint64_t shadow_constants_different_ = 0;
  const int32_t ring_spin_ns_ = REXCVAR_GET(bo2zm_native_ring_spin_ns_ab);
  uint64_t shadow_constants_read_compared_ = 0;
  uint64_t shadow_constants_read_different_ = 0;
  uint64_t shadow_constants_read_unknown_ = 0;
  std::unordered_map<uint32_t, uint64_t> shadow_per_register_;
  std::unordered_map<uint32_t, uint64_t> shadow_per_register_read_;
  uint64_t shadow_from_[5][2] = {};

  static constexpr uint64_t kSpansFastAVerify = 200000;
  int marker_fast_ = 1;
  uint64_t spans_fast_verified_ = 0;
  std::vector<uint32_t> copy_marker_;
  std::vector<uint32_t> marker_plane_;
  uint64_t markers_applied_ = 0;
  uint64_t markers_checked_ = 0;
  uint64_t markers_different_ = 0;
  uint64_t markers_bad_ = 0;
  uint64_t marker_spans_ = 0;
  uint64_t marker_spans_slow_ = 0;
  uint64_t marker_words_ = 0;
  uint32_t warnings_marker_ = 0;


  DrawRecord draw_marker_;
  uint32_t draw_marker_mode_ = 0;
  uint16_t vegetation_game_ = 0;
  uint64_t draws_with_marker_register_ = 0;
  uint64_t marker_rejected_registers_ = 0;
  uint64_t checks_draw_ = 0;
  uint64_t checks_draw_different_ = 0;
  uint64_t checks_draw_other_register_ = 0;
  uint32_t warnings_draw_marker_ = 0;
  uint64_t words_loose_ = 0;
  uint64_t stopwatch_packets_ = 0;
  std::array<uint64_t, 128> time_opcode_ns_{};
  std::array<uint64_t, 128> count_opcode_{};

  struct Wait {
    uint64_t times = 0;
    uint64_t loops = 0;
    uint32_t reference = 0;
    uint32_t mask = 0;
    uint32_t info = 0;
  };
  std::unordered_map<uint64_t, Wait> waits_;

  uint64_t drawn_previous_ = 0;
  uint64_t timed_previous_ = 0;
  uint64_t vegetation_soon_previous_ = 0;
  std::array<uint64_t, kStagesDraw> stages_previous_{};
  uint64_t discard_with_previous_ = 0;
  uint64_t discard_no_previous_ = 0;
  std::array<uint64_t, 20> counters_previous_{};
  std::array<uint64_t, 3> dedupe_previous_{};
  std::array<uint64_t, kGpuCategories> texels_category_previous_{};
  std::array<uint64_t, kGpuCategories> passes_category_previous_{};
  uint64_t draws_ps_useless_previous_ = 0;
  uint64_t shadows_alpha_active_previous_ = 0;
  uint64_t shadows_alpha_off_previous_ = 0;
  uint64_t draws_ps_needed_previous_ = 0;
  std::array<uint64_t, kGpuCategories> draws_category_previous_{};
  std::array<uint64_t, kGpuCategories> triangles_category_previous_{};
  uint64_t submits_ubo_previous_ = 0;
  uint64_t shared_observed_previous_ = 0;
  uint64_t shared_changed_previous_ = 0;
  std::array<uint64_t, kBucketsSwap> buckets_swap_previous_{};
  uint64_t overlaps_gpu_previous_ = 0;
  uint64_t cost_present_previous_[12] = {};
  uint64_t submits_previous_ = 0;

  std::array<std::array<uint16_t, 3>, 256> ramp_gamma_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  uint32_t component_ramp_ = 0;
  uint32_t mask_ramp_ = 0b111;
  uint64_t version_ramp_ = 0;
  uint64_t version_ramp_submitted_ = 0;
  std::array<uint64_t, kRegRampLast - kRegRampFirst + 1> writes_ramp_{};
  uint64_t writes_ramp_previous_ = 0;

  uint32_t occlusion_base_ = 0;
  std::unordered_set<uint32_t> bases_occlusion_;
  uint32_t warnings_occlusion_ = 0;


  std::array<uint64_t, 2> occlusion_per_target_{};
  std::array<uint32_t, 2> occlusion_max_per_target_{};
  uint32_t warnings_occlusion_scene_ = 0;

  std::string constants_list_text_;
  std::unordered_set<uint32_t> constants_list_;
  std::unordered_map<uint32_t, Clock::time_point> constants_last_;

  bool occlusion_test_started_ = false;
  bool occlusion_test_fake_ = false;
  Clock::time_point occlusion_test_start_{};


  std::array<uint64_t, 7> occlusion_counters_{};
  std::array<uint64_t, 7> occlusion_counters_previous_{};
  uint64_t occlusion_host_previous_[5] = {};

  uint64_t gpu_ns_previous_ = 0;
  uint64_t waits_gpu_previous_ = 0;
  uint64_t ns_waits_gpu_previous_ = 0;
  uint64_t jobs_gpu_previous_ = 0;
  uint64_t ns_job_gpu_previous_ = 0;
  uint64_t cost_record_previous_[6] = {};
  uint64_t gpu_jobs_previous_ = 0;
  uint64_t gpu_category_jobs_previous_ = 0;
  uint64_t presented_previous_gpu_ = 0;
  std::array<uint64_t, kGpuCategories> gpu_categories_previous_{};
  std::array<uint64_t, kGpuCategories> fragments_category_previous_{};
  std::array<uint64_t, kGpuCategories> vertices_category_previous_{};
  std::array<uint64_t, kGpuCategories> primitives_category_previous_{};
  uint64_t frames_diagnostic_previous_ = 0;
  std::array<uint64_t, 4> copies_bucket_previous_{};
  std::array<uint64_t, 4> pixels_bucket_previous_{};

  uint64_t mark_gpu_previous_ns_ = 0;
  uint64_t jobs_precise_previous_ = 0;
  Clock::time_point clock_mark_previous_{};
  double scale_real_ns_ = 0.0;
  double scale_marks_ns_ = 0.0;
  Clock::time_point last_time_report_{};
  Clock::time_point last_report_{};

  Clock::time_point start_system_{};
  bool tracing_ = false;
  bool trace_done_ = false;

  bool diag_constants_active_ = false;
  std::unordered_set<uint32_t> textures_dumped_;
  uint32_t traces_ = 0;
  uint32_t reason_paired_ = 0;
  uint32_t object_vs_ = 0;
  uint32_t object_ps_ = 0;

  std::unordered_map<uint64_t, const ShaderInput*> vs_per_microcode_;
  uint64_t hash_vs_ = 0;
  uint64_t hash_vs_generation_ = UINT64_MAX;
  uint64_t draws_per_im_load_ = 0;


  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  std::array<Framebuffer, rex::ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      framebuffers_{};
  size_t next_framebuffer_ = 0;
};

}

uint64_t SwapsNative() {
  return g_swaps_native.load(std::memory_order_relaxed);
}

bool IsActive() {

  return bo2zm::native::PortedRendererActive();
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateSystemGraphics() {
  return std::make_unique<NativeRenderSystem>();
}

}
