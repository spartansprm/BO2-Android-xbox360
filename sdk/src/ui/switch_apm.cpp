#include "rex/ui/switch_apm.h"

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <atomic>
#include <cstdio>

namespace {

std::atomic<int> g_requested{0};
std::atomic<bool> g_done{false};
/*
 * The escape hatch. By default no configuration that raises the RAM clock is accepted, even if that
 * means staying with the low GPU clock. If the firmware turned out not to have the "high GPU +
 * unchanged RAM" pair, setting this to true takes the RAM 1600 one anyway.
 */
std::atomic<bool> g_allow_ram_1600{false};
// Whether the memory watchdog below may be armed. Off when the player sets clocks with sys-clk: the
// watchdog would lower a memory clock the player raised on purpose.
std::atomic<bool> g_watch_memory{true};

/*
 * Watchdog state. g_emc_original is the memory clock the console had before we touched anything, in
 * Hz, and it is 0 while we have not changed the configuration: with 0 the watchdog does nothing at
 * all (it does not even open clkrst).
 */
std::atomic<u32> g_emc_original{0};
std::atomic<int> g_emc_ticks{0};
std::atomic<int> g_emc_corrections{0};
std::atomic<bool> g_warning_emc{false};
std::atomic<bool> g_emc_gave_up{false};

/*
 * How often the watchdog looks. For the first kTicksConsecutive ticks (one second each) it always looks,
 * which is when the system finishes applying the change (the profile already saw 1600 in its first
 * sample, at 8 s). From then on, one of every kTicksSpaced, to catch late raises (mode change,
 * waking from sleep) without opening clkrst every second for the whole session.
 */
constexpr int kTicksConsecutive = 20;
constexpr int kTicksSpaced = 30;
/*
 * And a cap on corrections. Measured: eight clkrst reductions were all accepted and all reverted by
 * pcv, which reimposes the EMC of the active apm configuration. With the corrected table this should
 * never fire; if it does, the chosen configuration is not the one we thought, and insisting fixes
 * nothing. It is logged and left alone.
 */
constexpr int kMaxEmcCorrections = 3;


constexpr u64 kPollStepNs = 150000000ULL;
constexpr int kPollSteps = 10;  // 10 x 150 ms = 1,5 s
constexpr u32 kMarginHz = 20000000u;  // 20 MHz: 1597.5 and 1331.2 are nowhere near each other

/*
 * Real hardware clocks, in Hz. Returns false if clkrst is not available. It is opened and closed on
 * every call on purpose: this runs only a few times per session.
 */
bool ReadClocks(u32& gpu_hz, u32& emc_hz) {
  gpu_hz = 0;
  emc_hz = 0;
  if (R_FAILED(clkrstInitialize())) {
    return false;
  }
  bool ok = true;
  ClkrstSession ses_gpu{};
  ClkrstSession ses_emc{};
  if (R_SUCCEEDED(clkrstOpenSession(&ses_gpu, PcvModuleId_GPU, 3))) {
    if (R_FAILED(clkrstGetClockRate(&ses_gpu, &gpu_hz))) ok = false;
    clkrstCloseSession(&ses_gpu);
  } else {
    ok = false;
  }
  if (R_SUCCEEDED(clkrstOpenSession(&ses_emc, PcvModuleId_EMC, 3))) {
    if (R_FAILED(clkrstGetClockRate(&ses_emc, &emc_hz))) ok = false;
    clkrstCloseSession(&ses_emc);
  } else {
    ok = false;
  }
  clkrstExit();
  return ok;
}

/*
 * Polls the clocks for kPollSteps x kPollStepNs and returns true if the memory clock stayed where
 * it was the whole time. As soon as one read sees it moved, it stops and returns false, leaving the
 * last values read in gpu/emc. A single read is not enough: one read at 250 ms said the memory was
 * unchanged when it was about to go up a moment later.
 */
bool MemoryStillDuring(u32 emc0, u32& gpu, u32& emc) {
  for (int i = 0; i < kPollSteps; ++i) {
    svcSleepThread(kPollStepNs);
    if (!ReadClocks(gpu, emc)) {
      return false;
    }
    if ((emc > emc0 ? emc - emc0 : emc0 - emc) > kMarginHz) {
      return false;
    }
  }
  return true;
}

/*
 * Sets the memory module clock to hz. Returns whether the request was accepted (not whether the clock
 * already changed: that is checked later, which is exactly the lesson of the first attempt).
 */
bool SetMemory(u32 hz) {
  bool ok = false;
  if (R_SUCCEEDED(clkrstInitialize())) {
    ClkrstSession ses{};
    if (R_SUCCEEDED(clkrstOpenSession(&ses, PcvModuleId_EMC, 3))) {
      ok = R_SUCCEEDED(clkrstSetClockRate(&ses, hz));
      clkrstCloseSession(&ses);
    }
    clkrstExit();
  }
  return ok;
}

struct Candidate {
  int mhz;
  uint32_t config;
  int emc;  // log only: what Horizon's table says this configuration sets
};
/*
 * The correct table. The first two are confirmed by a console log:
 *
 *   0x00020003  GPU 307.2  EMC 1331.2   <- "at start: config 0x00020003 ... memory 1331.2"
 *   0x92220007  GPU 460.8  EMC 1600     <- "SET 0x92220007" and the memory ended at 1600
 *
 * and they match Horizon's PerformanceConfiguration table exactly. The one we want is the sibling of
 * the last one, 0x92220008 = GPU 460.8 with EMC 1331.2, which is exactly the pair a commercial game
 * uses. An earlier list lacked it (it had 0x92220006, which does not exist), so the 1600 one always
 * got in.
 *
 * They are sorted by EMC: first the one that does not touch the memory. Requesting one that does not
 * exist returns an error and changes nothing, so it is enough to list them and let the check decide.
 */
constexpr Candidate kCandidates[] = {
    {460, 0x92220008u, 1331},  // GPU 460.8 + EMC 1331.2: the good one
    {460, 0x92220007u, 1600},  // GPU 460.8 + EMC 1600: raises the RAM clock
    {384, 0x00020004u, 1331},  // GPU 384 + EMC 1331,2
    {384, 0x00010000u, 1600},  // GPU 384 + EMC 1600
};


}  // namespace

extern "C" void RexSwitchApmRequestGpuMhz(int mhz) {
  g_requested.store(mhz, std::memory_order_relaxed);
}

extern "C" void RexSwitchApmAllowRam1600(int allow) {
  g_allow_ram_1600.store(allow != 0, std::memory_order_relaxed);
}

extern "C" void RexSwitchApmWatchMemory(int watch) {
  g_watch_memory.store(watch != 0, std::memory_order_relaxed);
}

extern "C" void RexSwitchApmApply(void) {
  const int mhz = g_requested.load(std::memory_order_relaxed);
  if (mhz <= 0 || g_done.exchange(true)) {
    return;
  }

  ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
  if (R_FAILED(apmGetPerformanceMode(&mode))) {
    std::fprintf(stderr, "[apm] could not read the performance mode; nothing is touched\n");
    return;
  }
  if (mode != ApmPerformanceMode_Normal) {
    std::fprintf(stderr, "[apm] the console is docked (mode %d): nothing is requested\n", int(mode));
    return;
  }

  uint32_t config_original = 0;
  const bool has_original = R_SUCCEEDED(apmGetPerformanceConfiguration(mode, &config_original));

  u32 gpu0 = 0, emc0 = 0;
  if (!ReadClocks(gpu0, emc0)) {
    std::fprintf(stderr,
                 "[apm] cannot read the real clocks (clkrst): NOTHING is changed, because without "
                 "checking there is no guarantee the memory stays where it is\n");
    return;
  }
  std::fprintf(stderr, "[apm] at start: config 0x%08X, GPU %.1f MHz, memory %.1f MHz\n",
               has_original ? config_original : 0u, double(gpu0) / 1e6, double(emc0) / 1e6);

  const u32 goal_gpu_hz = u32(mhz) * 1000000u;

  /*
   * They are tried in table order (low EMC first) and the first one that raises the GPU without
   * moving the memory is accepted. The check is the 1.5 s poll, not a single read: a read at 250 ms
   * once said the memory was still at 1331.2, and one second later it was at 1600.
   */
  for (const Candidate& c : kCandidates) {
    if (c.mhz != mhz) {
      continue;
    }
    if (R_FAILED(apmSetPerformanceConfiguration(mode, c.config))) {
      continue;  // that configuration does not exist on this firmware
    }
    u32 gpu1 = 0, emc1 = 0;
    const bool really_still = MemoryStillDuring(emc0, gpu1, emc1);
    const bool memory_still =
        really_still || g_allow_ram_1600.load(std::memory_order_relaxed);
    const bool gpu_raised = gpu1 + kMarginHz >= goal_gpu_hz;
    if (gpu_raised && memory_still) {
      if (!really_still) {
        std::fprintf(stderr,
                     "[apm] 0x%08X raises the memory to %.1f MHz, but bo2zm_switch_ram_1600 is "
                     "true: accepted and the memory is NOT watched\n",
                     c.config, double(emc1) / 1e6);
        std::fprintf(stderr, "[apm] SET 0x%08X: GPU %.1f MHz (was %.1f), memory %.1f MHz\n",
                     c.config, double(gpu1) / 1e6, double(gpu0) / 1e6, double(emc1) / 1e6);
        return;  // watchdog not armed: lowering it would fight what the cvar asks for
      }
      /*
       * Accepted. The watchdog is armed anyway: the system may reimpose the EMC later,
       * on a mode change or when waking from sleep.
       */
      if (g_watch_memory.load(std::memory_order_relaxed)) {
        g_emc_original.store(emc0, std::memory_order_relaxed);
      }
      std::fprintf(stderr,
                   "[apm] SET configuration 0x%08X (table: GPU %d, EMC %d): GPU %.1f MHz (was "
                   "%.1f) and the memory stays at %.1f MHz, checked for %.1f s in a row; "
                   "now watched\n",
                   c.config, c.mhz, c.emc, double(gpu1) / 1e6, double(gpu0) / 1e6,
                   double(emc1) / 1e6, double(kPollStepNs) * kPollSteps / 1e9);
      return;
    }
    std::fprintf(stderr, "[apm] discarded 0x%08X: GPU %.1f MHz, memory %.1f MHz (%s)\n", c.config,
                 double(gpu1) / 1e6, double(emc1) / 1e6,
                 !gpu_raised ? "the GPU does not reach the request" : "IT MOVES THE MEMORY");
  }

  /*
   * None worked. The memory is not lowered by hand: that was tried eight times with clkrst, all
   * eight were accepted and all eight were reverted by the system in under a second. pcv reimposes
   * the EMC of the active apm configuration, and clkrst cannot fight that.
   *
   * So the console is left as it was, and the log says exactly what trade is available and how to
   * take it (bo2zm_switch_ram_1600).
   */
  if (has_original && R_SUCCEEDED(apmSetPerformanceConfiguration(mode, config_original))) {
    u32 gpu2 = 0, emc2 = 0;
    MemoryStillDuring(emc0, gpu2, emc2);
    std::fprintf(stderr,
                 "[apm] this firmware has NO %d MHz configuration that leaves the memory "
                 "at %.1f MHz; going back to the original 0x%08X (GPU %.1f MHz, memory %.1f MHz).\n"
                 "[apm] IF YOU PREFER THE HIGH GPU EVEN IF THE RAM GOES UP TO 1600: set "
                 "bo2zm_switch_ram_1600 = true in bo2zm.toml\n",
                 mhz, double(emc0) / 1e6, config_original, double(gpu2) / 1e6, double(emc2) / 1e6);
  } else {
    std::fprintf(stderr, "[apm] no %d MHz configuration worked and the original could NOT be "
                         "restored: check the clocks in the overlay\n",
                 mhz);
  }
}

extern "C" void RexSwitchApmWatch(void) {
  const u32 emc0 = g_emc_original.load(std::memory_order_relaxed);
  if (emc0 == 0 || g_emc_gave_up.load(std::memory_order_relaxed)) {
    return;  // nothing changed, or we already know this firmware does not allow it
  }

  const int tic = g_emc_ticks.fetch_add(1, std::memory_order_relaxed);
  if (tic >= kTicksConsecutive && (tic % kTicksSpaced) != 0) {
    return;
  }

  u32 gpu = 0, emc = 0;
  if (!ReadClocks(gpu, emc) || emc <= emc0 + kMarginHz) {
    return;  // where it should be
  }

  /* It moved: the configuration raised it after we checked. */
  const bool lowered = SetMemory(emc0);
  const int n = g_emc_corrections.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!g_warning_emc.exchange(true)) {
    std::fprintf(stderr,
                 "[apm] the memory had gone up by itself to %.1f MHz (t=%d s): lowering it to %.1f with clkrst "
                 "(%s)\n",
                 double(emc) / 1e6, tic, double(emc0) / 1e6, lowered ? "accepted" : "REJECTED");
  }
  if (!lowered || n >= kMaxEmcCorrections) {
    g_emc_gave_up.store(true, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[apm] giving up on the memory after %d attempt(s): this firmware "
                 "reimposes it. The GPU stays at the request; the RAM, at whatever the system sets\n",
                 n);
  }
}

#else   // !REX_PLATFORM_SWITCH
extern "C" void RexSwitchApmRequestGpuMhz(int) {}
extern "C" void RexSwitchApmAllowRam1600(int) {}
extern "C" void RexSwitchApmApply(void) {}
extern "C" void RexSwitchApmWatchMemory(int) {}
extern "C" void RexSwitchApmWatch(void) {}
#endif  // REX_PLATFORM_SWITCH
