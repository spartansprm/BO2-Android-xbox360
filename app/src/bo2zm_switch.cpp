#include "bo2zm_switch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
extern "C" char** __system_argv;
#include "core/threading_switch.h"
extern "C" void RexSwitchPerfSetStackSampling(int forced);
#include "bo2zm_switch_mutex.h"
#endif

#ifndef BO2ZM_BUILD_ID
#define BO2ZM_BUILD_ID "dev"
#endif

REXCVAR_DEFINE_BOOL(bo2zm_xaudio_threads_priority, true, "BO2",
                    "Switch: the game's two audio engine threads run at the audio priority (0x2B) instead of the "
                    "guest one (0x3B). false = the guest priority");

REXCVAR_DEFINE_INT32(bo2zm_profile_stacks, -1, "BO2",
                     "Switch: stack sampling in rex_profile.log (pauses every thread each millisecond, which costs "
                     "frames): 1 = on, 0 = off, -1 = on when logs/rex/profile_stacks.flag exists. Test tomls set it "
                     "per run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_malloc_spin, false, "BO2",
                    "Switch: the C heap's lock (every malloc and free) spins a few microseconds before it sleeps in "
                    "the kernel, as the host mutexes do (bo2zm_switch_mutex.cpp). false = it sleeps at once, as "
                    "newlib's own lock did")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_malloc_spin_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_malloc_spin. 0 = not "
                     "part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_audio_off_ring_core, true, "BO2",
                    "Switch: the audio threads run on every core but the ring thread's (bo2zm_native_ring_core). "
                    "Sets the SDK's switch_audio_core_mask unless the toml gives it");


REXCVAR_DEFINE_INT32(bo2zm_cpu_cores, 3, "BO2",
                     "Switch, experimental: 4 = the game also uses the fourth CPU core (the threads bo2zm_core3_threads "
                     "names), which only a forwarder made with four cores allows (sphaira: CPU Cores 4); started any "
                     "other way, 4 changes nothing. 3 = the three cores every launch has. Extras > EXPERIMENTAL > CPU "
                     "CORES sets it at once and saves the pick (cpu_cores); a value set here wins over the saved pick")
    .range(3, 4);


REXCVAR_DEFINE_STRING(bo2zm_core3_threads, "Server:0xF:59,Sound Mix:0x8:43,XMA Decoder:0x8:43", "BO2",
                      "Switch, experimental, 4-core forwarder only: threads moved to core 3, as comma-separated "
                      "\"name prefix[:core mask[:priority]]\" (mask 0x8 = core 3 alone, the default; 0xF = cores "
                      "0-3; priority kept when left out, 63 = time-sliced with the system's background threads). "
                      "Threads alone on core 3 at different priorities: only the audio band (43 and below) may be "
                      "above the others, else all get 63. \"\" = none moved")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_core3_failsafe_percent, 85, "BO2",
                     "Switch, experimental: the four cores switch themselves off when our threads pinned to core 3 hold "
                     "more than this share of it (%) for bo2zm_core3_failsafe_s seconds in a row, so the system's "
                     "services there can run (a whole-console freeze otherwise; switch_core3.cpp). In play they take "
                     "~30 %. Test tomls lower it to see it act")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_core3_failsafe_s, 2, "BO2",
                     "Switch, experimental: seconds in a row over bo2zm_core3_failsafe_percent before the failsafe acts")
    .range(1, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_core3_clamp, true, "BO2",
                    "Switch, experimental: every thread a core-3 rule does not name stays on cores 0-2, as with a "
                    "normal launch. false = libnx's default with a 4-core forwarder: every thread may use core 3")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_core3_meter, true, "BO2",
                    "Switch, experimental, 4-core forwarder only: a thread on core 3 measures its load every second "
                    "([core3] lines, rex_profile.log), split into our threads and the system")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_BOOL(bo2zm_mesa_shader_cache, true, "BO2",
                    "Switch: Mesa keeps its own shader cache on the SD (sdmc:/.mesa). false = off for this process "
                    "(MESA_SHADER_CACHE_DISABLE): the game's own pipeline file still holds every pipeline the renderer "
                    "made; the few made without it are compiled at each start. For an A/B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
extern "C" void RexSwitchCore3SetLog(void (*log)(const char*));
extern "C" bool RexSwitchCore3Configure(const char* rules, bool clamp, bool meter, bool active);
extern "C" bool RexSwitchCore3SetActive(bool on);
extern "C" bool RexSwitchCore3Active(void);
extern "C" bool RexSwitchCore3Tripped(void);
extern "C" void RexSwitchCore3SetHogLimit(int percent, int seconds);
#endif

namespace bo2zm {


static void LogBuildIdentity() {
  std::error_code ec;
#if defined(__SWITCH__)
  const std::filesystem::path exe = (__system_argv && __system_argv[0]) ? __system_argv[0] : "";
#else
  const std::filesystem::path exe = rex::filesystem::GetExecutablePath();
#endif
  const auto size = std::filesystem::file_size(exe, ec);
  const auto mtime = std::filesystem::last_write_time(exe, ec);
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
  REXLOG_INFO("[bo2zm] build {} | executable {} size {} mtime {}", BO2ZM_BUILD_ID, exe.string(), size,
              secs);
}

const char* BuildId() { return BO2ZM_BUILD_ID; }

int CpuCoresSetting() {
  return REXCVAR_GET(bo2zm_cpu_cores);
}

void SetCpuCoresSetting(int cores) {
  rex::cvar::SetFlagByName("bo2zm_cpu_cores", cores == 4 ? "4" : "3");
#if defined(__SWITCH__)
  RexSwitchCore3SetActive(cores == 4);
#endif
}

bool CoresSwitchedOff() {
#if defined(__SWITCH__)
  return RexSwitchCore3Tripped();
#else
  return false;
#endif
}

int CoresInUse() {
#if defined(__SWITCH__)
  return RexSwitchCore3Active() ? 4 : 3;
#else
  return 0;
#endif
}

#if defined(__SWITCH__)


static std::atomic<uint64_t> g_default_display_id{~uint64_t(0)};
extern "C" Result __real_viOpenDisplay(const char* display_name, ViDisplay* display);
extern "C" Result __wrap_viOpenDisplay(const char* display_name, ViDisplay* display) {
  const Result rc = __real_viOpenDisplay(display_name, display);
  if (R_SUCCEEDED(rc) && display && display_name && std::strcmp(display_name, "Default") == 0) {
    g_default_display_id.store(display->display_id, std::memory_order_relaxed);
  }
  return rc;
}


static Event* RefreshEvent() {
  static Event event{};
  static const bool taken = [] {
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    const uint64_t id = g_default_display_id.load(std::memory_order_relaxed);
    if (id != ~uint64_t(0)) {
      ViDisplay display{};
      display.display_id = id;
      display.initialized = true;
      rc = viGetDisplayVsyncEvent(&display, &event);
    }
    if (R_FAILED(rc)) {
      static ViDisplay display{};
      rc = viOpenDefaultDisplay(&display);
      if (R_SUCCEEDED(rc)) {
        rc = viGetDisplayVsyncEvent(&display, &event);
      }
    }
    if (R_FAILED(rc)) {
      REXLOG_WARN("[switch] no screen refresh signal (vi 0x{:X}, default display id {}): VSYNC leaves the pacing to "
                  "the screen",
                  rc, id == ~uint64_t(0) ? std::string("not seen") : std::to_string(id));
      return false;
    }
    REXLOG_INFO("[switch] screen refresh signal taken (default display id {}): with VSYNC each frame in a match starts "
                "on a refresh",
                id);
    return true;
  }();
  return taken ? &event : nullptr;
}
#endif

bool HasRefreshSignal() {
#if defined(__SWITCH__)
  return RefreshEvent() != nullptr;
#else
  return false;
#endif
}

bool WaitForNextRefresh(int timeout_ms) {
#if defined(__SWITCH__)
  Event* event = RefreshEvent();
  if (!event) {
    return false;
  }
  eventClear(event);
  return R_SUCCEEDED(eventWait(event, uint64_t(timeout_ms) * 1000000));
#else
  (void)timeout_ms;
  return false;
#endif
}

int ProcessCores() {
#if defined(__SWITCH__)
  static const int cores = [] {
    u64 mask = 0;
    return R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ? __builtin_popcountll(mask) : 3;
  }();
  return cores;
#else
  return 0;
#endif
}

void InitSwitchServices() {
  LogBuildIdentity();
#if defined(__SWITCH__)
  if (REXCVAR_GET(bo2zm_malloc_spin)) {
    SetMallocSpin(true);
    REXLOG_INFO("[switch] the C heap's lock spins before it sleeps (bo2zm_malloc_spin)");
  }
  if (const int32_t factor = REXCVAR_GET(bo2zm_malloc_spin_ab)) {
    SetMallocSpinFactor(factor);
    REXLOG_INFO("[switch] the C heap's lock spins in the A/B arms of factor {} (bo2zm_malloc_spin_ab)", factor);
  }
  if (const int32_t stacks = REXCVAR_GET(bo2zm_profile_stacks); stacks == 0 || stacks == 1) {
    RexSwitchPerfSetStackSampling(stacks);
    REXLOG_INFO("[switch] stack sampling {} (bo2zm_profile_stacks)", stacks ? "on" : "off");
  }

  RexSwitchCore3SetLog([](const char* text) { REXLOG_INFO("{}", text); });
  RexSwitchCore3SetHogLimit(REXCVAR_GET(bo2zm_core3_failsafe_percent), REXCVAR_GET(bo2zm_core3_failsafe_s));
  RexSwitchCore3Configure(REXCVAR_GET(bo2zm_core3_threads).c_str(), REXCVAR_GET(bo2zm_core3_clamp),
                          REXCVAR_GET(bo2zm_core3_meter), REXCVAR_GET(bo2zm_cpu_cores) == 4);


  if (rex::cvar::Query<bool>("bo2zm_native_test_fresh_pipelines") ||
      rex::cvar::Query<bool>("bo2zm_native_test_cold_pipelines")) {
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    REXLOG_INFO("[switch] Mesa's shader cache off (bo2zm_native_test_fresh_pipelines or _cold_pipelines): every "
                "shader is compiled");
  } else if (!REXCVAR_GET(bo2zm_mesa_shader_cache)) {
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    REXLOG_INFO("[switch] Mesa's shader cache off (bo2zm_mesa_shader_cache = false)");
  }


  if (rex::cvar::GetFlagByName("switch_audio_guest_threads").empty()) {
    rex::cvar::SetFlagByName("switch_audio_guest_threads", "Sound Mix");
  }
  if (REXCVAR_GET(bo2zm_audio_off_ring_core) && rex::cvar::Query<int32_t>("switch_audio_core_mask") == 0) {
    const int32_t core = rex::cvar::Query<int32_t>("bo2zm_native_ring_core");
    if (core >= 0 && core <= 2) {
      const int mask = 0x7 & ~(1 << core);
      rex::cvar::SetFlagByName("switch_audio_core_mask", std::to_string(mask));
      REXLOG_INFO("[switch] audio threads on cores mask {:#x} (the ring thread prefers core {})", mask, core);
    }
  }
  const Result rc = socketInitializeDefault();
  if (R_SUCCEEDED(rc)) {
    REXLOG_INFO("[switch] sockets ready (bsd service)");
  } else {
    REXLOG_ERROR("[switch] socketInitializeDefault failed: {:#x}", rc);
  }
#endif
}

}


REX_EXTERN(__imp__sub_8281A248);
REX_EXTERN(__imp__sub_8281A2A8);

namespace {
void AudioThread([[maybe_unused]] const char* which) {
#if defined(__SWITCH__)
  if (!REXCVAR_GET(bo2zm_xaudio_threads_priority)) {
    return;
  }
  const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
  if (R_SUCCEEDED(rc)) {
    REXLOG_INFO("[switch] audio engine {} thread: priority 0x2b", which);
  } else {
    REXLOG_WARN("[switch] audio engine {} thread: svcSetThreadPriority failed: {:#x}", which, rc);
  }

  const int32_t value = rex::cvar::Query<int32_t>("switch_audio_core_mask");
  if (value > 0) {
    const bool accepted = RexSwitchSetCurrentThreadCoreMask(uint64_t(value));
    REXLOG_INFO("[switch] audio engine {} thread: core mask {:#x}{}", which, value,
                accepted ? "" : " REFUSED by the kernel");
  }
#else
  REXLOG_INFO("[switch] audio engine {} thread started (the priority changes on the Switch only)", which);
#endif
}
}

REX_HOOK_RAW(sub_8281A248) {
  AudioThread("main");
  __imp__sub_8281A248(ctx, base);
}
REX_HOOK_RAW(sub_8281A2A8) {
  AudioThread("second");
  __imp__sub_8281A2A8(ctx, base);
}
