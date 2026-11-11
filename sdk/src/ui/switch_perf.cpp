#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <switch.h>

#include "rex/ui/switch_apm.h"
#include "rex/ui/switch_saltynx.h"
#include "rex/ui/switch_sysclk.h"

extern "C" {
void _start(void);
/* rexglue log folder, ending in '/' (switch_crash_hooks.c). */
const char* RexSwitchLogDir(void);
/*
 * From the guest memory core (guest_memory_switch.cpp): how much backing is
 * committed and how much is mapped in total, counting the 360 mirrors.
 */
size_t RexGmCommittedBytes(void);
size_t RexGmMappedBytes(void);
/* 0 = untested, 1 = permissions (watched pages readable), 2 = unmapping. */
int RexGmProtectionMode(void);
/* switch_core3.cpp: the core-3 meter's totals (only with a 4-core forwarder). */
bool RexSwitchCore3Totals(u64* elapsed, u64* busy, u64* ours, u32* worst_permille, u64* clamped);
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid);
Result __real_threadClose(Thread* t);
Result __real_nvFenceWait(NvFence* f, s32 timeout_us);
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __real_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable);
Result __real_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova);
Result __real_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset);
void __real_armDCacheClean(void* addr, size_t size);
Result __real_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);
Result __real_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence);
void __real_svcSleepThread(s64 nano);
}

namespace {

// RexSwitchPerfSetStackSampling: 1 on, 0 off, -1 the flag file decides.
std::atomic<int> g_stacks_forced{-1};
// RexSwitchPerfSetReport: whether rex_profile.log is written. Off, the thread registry, the counters and the overlays
// go on; only the file is left alone (the app turns it off for a player's quiet log).
std::atomic<bool> g_report{true};

constexpr size_t kMaxThreads = 160;
constexpr u64 kSampleIntervalNs = 1000000ULL;  // 1 ms
constexpr u64 kSecondNs = 1000000000ULL;      // tick for the console overlays

/* internal resolution published to the overlays. The game sets it when choosing the video mode. */
std::atomic<uint32_t> g_resolution{(1280u << 16) | 720u};
constexpr u64 kPassiveIntervalNs = 100000000ULL;  // 100 ms, without pausing threads
// Next to the rexglue logs, in <NRO folder>/logs/rex/.
std::string StackFlagPath() {
  return std::string(RexSwitchLogDir()) + "profile_stacks.flag";
}
constexpr u64 kReportSeconds = 10;
/*
 * Let the game start first. This only delays stack sampling and the first report, which read the
 * SD and pause threads. The console overlays (FPS and resolution) are published from second zero:
 * see the warm-up loop in ProfilerMain.
 */
constexpr u64 kStartDelayNs = 8000000000ULL;
constexpr size_t kMaxSamples = 1 << 15;
constexpr size_t kFrames = 10;
constexpr unsigned kCounterCount = 33;
// In <NRO folder>/logs/rex/ (switch_crash_hooks.c computes it at startup).
std::string ReportPath() {
  return std::string(RexSwitchLogDir()) + "rex_profile.log";
}

struct Slot {
  std::atomic<u32> handle{0};
  char name[32]{};
  // Only the profiler thread touches these:
  u32 seen = 0;
  u64 last_ticks = 0;
  bool busy = true;
  double cpu = 0.0;
};
Slot g_slots[kMaxThreads];

struct Sample {
  u64 pc;
  u64 lr;
  u64 frames[kFrames];  // return addresses along the x29 chain
  u16 slot;
  u64 tick;  // when it was taken (armGetSystemTick), to separate those from long frames
};

/*
 * Long frame windows. The renderer calls RexSwitchPerfHitch with the interval of every frame over
 * 45 ms; the report separates the samples taken inside those intervals and says what each thread was
 * doing right then (section "during the hitches"). Only works with profile_stacks.flag.
 */
constexpr size_t kMaxHitches = 256;
struct WindowHitch {
  u64 start;
  u64 end;
};
WindowHitch g_hitches[kMaxHitches];
std::atomic<u32> g_num_hitches{0};
Sample g_samples[kMaxSamples];
size_t g_sample_count = 0;

std::atomic<u64> g_counters[kCounterCount];

/*
 * Wrapped libnx calls (see the file header): how many, how much time inside and, where it makes
 * sense, how many bytes. They are constant-initialized, so they are valid even if someone calls
 * before the constructors.
 */
enum : unsigned {
  kFenceQuery,     // nvFenceWait with wait 0
  kFenceWait,       // nvFenceWait with a wait
  kKickoff,           // nvGpuChannelKickoff
  kNvMapCached,     // nvMapCreate with CPU cache
  kNvMapNoCache,     // nvMapCreate without CPU cache
  kReserveAddress,  // nvAddressSpaceAllocFixed
  kMappingGpu,          // nvioctlNvhostAsGpu_MapBufferEx
  kCacheClean,        // armDCacheClean
  kQueueBuffer,       // nwindowQueueBuffer
  kDequeueBuffer,     // bqDequeueBuffer
  kSleep0,            // svcSleepThread(0)
  kSleepYield,        // svcSleepThread(-1 o -2)
  kSleepShort,        // up to 1 ms
  kSleepLong,        // over 1 ms
  kCallsCount,
};
struct Calls {
  std::atomic<u64> n{0};
  std::atomic<u64> ticks{0};
  std::atomic<u64> bytes{0};
};
Calls g_calls[kCallsCount];

inline void Annotate(unsigned id, u64 since, u64 bytes = 0) {
  const u64 ticks = armGetSystemTick() - since;
  Calls& l = g_calls[id];
  l.n.fetch_add(1, std::memory_order_relaxed);
  l.ticks.fetch_add(ticks, std::memory_order_relaxed);
  if (bytes) {
    l.bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

/*
 * Automatic A/B tests. Each report (10 s) removes one part of the GPU work: those commands (draw or
 * dispatch) stop being recorded, but everything else stays the same, so the state does not break.
 * The image looks wrong while the mode lasts; that is expected. If with "nothing" it is just as
 * slow, the cost is not in the work but in something fixed (submissions, driver).
 */
struct Mode {
  u32 mask;
  const char* name;
};
constexpr Mode kModes[] = {
    {0, "normal"},
    {1, "no draws"},
    {2, "no transfers"},
    {4, "no resolves"},
    {8, "no texture uploads"},
    {16, "no present effect"},
    {32, "no tiled dispatch"},
    {63, "nothing"},
};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

/*
 * A/B tests, started by hand
 *
 * They do not rotate on their own from startup: the game takes a while to reach gameplay, and on the
 * way it would spend its time in modes with drawing turned off (left on by mistake, the game did not
 * even reach the title screen).
 *
 * Instead the player starts the rotation with L+R+ZL during gameplay, which is the only place where
 * the measurement is useful. From then on it removes one part of the GPU work per report, goes once
 * through the eight modes and returns to "normal".
 *
 * How to read it. If with "nothing" the game is just as slow, the cost is not in the GPU work but
 * in something fixed: submissions, the driver, or the guest itself. If FPS shoots up, the mode where
 * it rises most points to the culprit.
 */
std::atomic<bool> g_ab_active{false};
std::atomic<u32> g_skip_mask{0};

extern "C" void RexSwitchPerfToggleAb(void) {
  const bool fresh = !g_ab_active.load(std::memory_order_relaxed);
  g_ab_active.store(fresh, std::memory_order_relaxed);
  if (!fresh) {
    g_skip_mask.store(0, std::memory_order_relaxed);
  }
}

/*
 * Horizon limits how much memory a process may map (LimitableResource_Memory). Our
 * guest memory model maps each chunk several times (the shadow and once per 360
 * view), so that limit is the real ceiling, not free memory. When it runs out,
 * even committing 4 KB fails with 2001-0103 (resource exhausted).
 */
void MappingLimit(u64* used_mb, u64* cap_mb, u64* process_mb, u64* total_mb) {
  *used_mb = *cap_mb = *process_mb = *total_mb = 0;
  u64 v = 0;
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *process_mb = v >> 20;
  }
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *total_mb = v >> 20;
  }
  Handle reslimit = INVALID_HANDLE;
  if (R_FAILED(svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) {
    return;
  }
  reslimit = static_cast<Handle>(v);
  s64 actual = 0, cap = 0;
  if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&actual, reslimit, LimitableResource_Memory))) {
    *used_mb = static_cast<u64>(actual) >> 20;
  }
  if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&cap, reslimit, LimitableResource_Memory))) {
    *cap_mb = static_cast<u64>(cap) >> 20;
  }
  svcCloseHandle(reslimit);
}

u64 g_base = 0, g_text_lo = 0, g_text_hi = 0;
Thread g_thread;

void Register(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = 0;
    if (s.handle.compare_exchange_strong(expected, h)) {
      s.name[0] = 0;
      return;
    }
  }
}

void Unregister(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = h;
    if (s.handle.compare_exchange_strong(expected, 0)) {
      return;
    }
  }
}

bool InText(u64 addr) { return addr >= g_text_lo && addr < g_text_hi; }

void PrintAddr(FILE* f, u64 addr) {
  if (InText(addr)) {
    std::fprintf(f, "image+0x%" PRIx64, addr - g_base);
  } else {
    std::fprintf(f, "0x%016" PRIx64, addr);
  }
}

/*
 * Is the pc right after a system call? Then the thread is waiting in the kernel,
 * not using CPU.
 */
bool AfterSvc(u64 pc) {
  if (!InText(pc) || pc < g_text_lo + 4) {
    return false;
  }
  const u32 insn = *reinterpret_cast<const u32*>(pc - 4);
  return (insn & 0xFFE0001Fu) == 0xD4000001u;
}

struct Count {
  u64 addr;
  u32 n;
};

std::vector<Count> Histogram(std::vector<u64>& addrs) {
  std::sort(addrs.begin(), addrs.end());
  std::vector<Count> out;
  for (u64 a : addrs) {
    if (out.empty() || out.back().addr != a) {
      out.push_back({a, 1});
    } else {
      ++out.back().n;
    }
  }
  std::sort(out.begin(), out.end(), [](const Count& x, const Count& y) { return x.n > y.n; });
  return out;
}

void Report(u64 elapsed_ticks, u64 tick_freq, u64 counters_last[kCounterCount],
            const char* mode) {
  const double seconds = double(elapsed_ticks) / double(tick_freq);

  u64 counters_now[kCounterCount];
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_now[i] = g_counters[i].load(std::memory_order_relaxed);
  }

  // CPU of each thread over the interval, as % of one core.
  std::vector<size_t> order;
  for (size_t i = 0; i < kMaxThreads; ++i) {
    Slot& s = g_slots[i];
    const u32 h = s.handle.load();
    if (!h) {
      s.seen = 0;
      continue;
    }
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) {
      continue;
    }
    if (s.seen != h) {  // new thread: no interval yet
      s.seen = h;
      s.last_ticks = ticks;
      s.busy = true;
      s.cpu = 0.0;
      continue;
    }
    s.cpu = double(ticks - s.last_ticks) * 100.0 / double(elapsed_ticks);
    s.last_ticks = ticks;
    /*
     * All threads are sampled, including those that use no CPU. In a hang caused by
     * a wait, the thread that matters is exactly that one: the stopped one.
     */
    s.busy = true;
    order.push_back(i);
  }
  std::sort(order.begin(), order.end(),
            [](size_t a, size_t b) { return g_slots[a].cpu > g_slots[b].cpu; });

  if (!g_report.load(std::memory_order_relaxed)) {
    g_sample_count = 0;
    return;
  }
  FILE* f = std::fopen(ReportPath().c_str(), "a");
  if (!f) {
    g_sample_count = 0;
    return;
  }

  u64 lim_used = 0, lim_cap = 0, proc_used = 0, proc_total = 0;
  MappingLimit(&lim_used, &lim_cap, &proc_used, &proc_total);

  double total_cpu = 0.0;
  for (size_t i : order) {
    total_cpu += g_slots[i].cpu;
  }
  std::fprintf(f,
               "==== %.1f s | mode: %s | game %.1f fps | CPU total %.0f%% (400%% = 4 cores) | faults/s: "
               "emulated read %.0f, SDK handler %.0f, emulated retry %.0f, SEH %.0f, "
               "physical committed %.0f, views %.0f | guest %zu/%zu MB (backing/mapped) | "
               "mapping limit %llu/%llu MB, process %llu/%llu MB | "
               "samples %zu | watch: %s | samplers: %.0f new/s, %.0f stalls/s\n",
               seconds, mode, double(counters_now[0] - counters_last[0]) / seconds, total_cpu,
               double(counters_now[1] - counters_last[1]) / seconds,
               double(counters_now[2] - counters_last[2]) / seconds,
               double(counters_now[3] - counters_last[3]) / seconds,
               double(counters_now[4] - counters_last[4]) / seconds,
               double(counters_now[17] - counters_last[17]) / seconds,
               double(counters_now[18] - counters_last[18]) / seconds,
               RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20,
               (u64)lim_used, (u64)lim_cap, (u64)proc_used, (u64)proc_total, g_sample_count,
                (RexGmProtectionMode() == 1   ? "permissions (readable pages)"
                 : RexGmProtectionMode() == 2 ? "unmapping (every read faults)"
                                              : "untested"),
                double(counters_now[20] - counters_last[20]) / seconds,
                double(counters_now[19] - counters_last[19]) / seconds);
  // Work the game sends to the GPU, per presented frame. One screen is
  // 1280x720 = 921,600 pixels.
  const auto delta = [&](unsigned id) { return double(counters_now[id] - counters_last[id]); };
  const double frames = delta(0);
  // Mix clipping before conversion to 16 bits (21 samples above 1.0, 22 buffers with a peak of 0.98 or
  // more, 23 maximum peak of the interval in ten-thousandths) and the console mode, for the crackling
  // in handheld mode.
  const u64 audio_peak = g_counters[23].exchange(0, std::memory_order_relaxed);
  std::fprintf(f,
               "     audio: %.0f client blocks mixed, %.0f requests without data, "
               "%.0f/%.0f audout buffers with non-zero PCM, %.0f clipped samples (above 1.0 before clipping), "
               "%.0f buffers with a peak of 0.98 or more, maximum peak %.3f | console in %s mode\n",
               delta(24), delta(25), delta(26), delta(27), delta(21), delta(22), double(audio_peak) / 10000.0,
               rex::ui::switch_saltynx::DockMode(appletGetOperationMode() == AppletOperationMode_Console)
                   ? "docked"
                   : "handheld");
  // Reverse-NX state as is, without interpretation. A whole test session was wasted because its
  // overlay said "Docked" while "Controlled by system" was Yes, and in that case its mode does not
  // rule: it only mirrors the console's. This shows at a glance which of the two is happening.
  {
    const auto reverse = rex::ui::switch_saltynx::StateReverse();
    const bool real = appletGetOperationMode() == AppletOperationMode_Console;
    if (!reverse.any) {
      std::fprintf(f, "     Reverse-NX: no block (console %s, it rules)\n",
                   real ? "in the dock" : "in the hands");
    } else {
      std::fprintf(f,
                   "     Reverse-NX: says %s, ruled by %s (Controlled by system %s), the game has asked: %s; "
                   "real console %s -> obeying %s\n",
                   reverse.in_dock ? "docked" : "handheld", reverse.by_default ? "the console" : "Reverse-NX",
                   reverse.by_default ? "Yes" : "No", reverse.plugin_active ? "yes" : "no",
                   real ? "in the dock" : "in the hands",
                   rex::ui::switch_saltynx::DockMode(real) ? "docked" : "handheld");
    }
  }
  // Real console clocks (clkrst, 8.0.0+) and the cores the process may use. It tells whether the
  // session ran overclocked and with which CPU limit it was measured: performance tests run without
  // overclock.
  {
    static bool started = false;
    static bool has_clkrst = false;
    static ClkrstSession sessions[3]{};
    static u64 core_mask = 0;
    if (!started) {
      started = true;
      if (R_FAILED(svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        core_mask = 0;
      }
      if (R_SUCCEEDED(clkrstInitialize())) {
        static const PcvModuleId modules[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        has_clkrst = true;
        for (unsigned i = 0; i < 3; ++i) {
          if (R_FAILED(clkrstOpenSession(&sessions[i], modules[i], 3))) {
            has_clkrst = false;
          }
        }
      }
    }
    u32 hz[3] = {0, 0, 0};
    if (has_clkrst) {
      for (unsigned i = 0; i < 3; ++i) {
        clkrstGetClockRate(&sessions[i], &hz[i]);
      }
    }
    unsigned cores = 0;
    for (unsigned i = 0; i < 64; ++i) {
      cores += unsigned((core_mask >> i) & 1);
    }
    // SoC and board temperature (ts service, sessions on [10.0.0+]). The system lowers clocks around
    // 70 degrees and Erista gets there before Mariko: without this, "it is slow" cannot be told apart
    // from "it is hot". On the OLED model the board sensor reads wrong, because it sits next to the
    // charging IC.
    static bool ts_started = false;
    static bool has_ts = false;
    static TsSession ts_soc{}, ts_pcb{};
    if (!ts_started) {
      ts_started = true;
      if (R_SUCCEEDED(tsInitialize())) {
        has_ts = R_SUCCEEDED(tsOpenSession(&ts_soc, TsDeviceCode_LocationExternal)) &&
                 R_SUCCEEDED(tsOpenSession(&ts_pcb, TsDeviceCode_LocationInternal));
      }
    }
    float degrees_soc = 0.0f, degrees_pcb = 0.0f;
    if (has_ts) {
      tsSessionGetTemperature(&ts_soc, &degrees_soc);
      tsSessionGetTemperature(&ts_pcb, &degrees_pcb);
    }
    std::fprintf(f,
                 "     clocks: CPU %.1f MHz, GPU %.1f MHz, memory %.1f MHz%s | process cores %u "
                 "(mask 0x%llX) | SoC %.1f C, board %.1f C%s\n",
                 double(hz[0]) / 1.0e6, double(hz[1]) / 1.0e6, double(hz[2]) / 1.0e6,
                 has_clkrst ? "" : " (clkrst not available)", cores,
                 (unsigned long long)core_mask, double(degrees_soc), double(degrees_pcb),
                 has_ts ? "" : " (ts not available)");
  }
  // Core 3 (switch_core3.cpp): with a forwarder that allows it, how busy it was and how much of that was ours.
  {
    static u64 last[3] = {0, 0, 0};
    u64 elapsed = 0, busy = 0, ours = 0, clamped = 0;
    u32 worst = 0;
    if (RexSwitchCore3Totals(&elapsed, &busy, &ours, &worst, &clamped) && elapsed > last[0]) {
      const double e = double(elapsed - last[0]);
      const double b = double(busy - last[1]), o = double(ours - last[2]);
      std::fprintf(f,
                   "     core 3: busy %.1f%% (our threads pinned there %.1f%%, the rest %.1f%%), busiest second %.1f%% | "
                   "%llu core masks kept off core 3\n",
                   100.0 * b / e, 100.0 * o / e, 100.0 * (b - o) / e, double(worst) / 10.0,
                   (unsigned long long)clamped);
      last[0] = elapsed;
      last[1] = busy;
      last[2] = ours;
    }
  }
  // libnx calls: per second, with the ms per second the calling threads spend inside.
  {
    static u64 last[kCallsCount][3];
    double n[kCallsCount], ms[kCallsCount], mb[kCallsCount];
    for (unsigned i = 0; i < kCallsCount; ++i) {
      const u64 vn = g_calls[i].n.load(std::memory_order_relaxed);
      const u64 vt = g_calls[i].ticks.load(std::memory_order_relaxed);
      const u64 vb = g_calls[i].bytes.load(std::memory_order_relaxed);
      n[i] = double(vn - last[i][0]) / seconds;
      ms[i] = double(vt - last[i][1]) * 1000.0 / double(tick_freq) / seconds;
      mb[i] = double(vb - last[i][2]) / 1048576.0 / seconds;
      last[i][0] = vn;
      last[i][1] = vt;
      last[i][2] = vb;
    }
    std::fprintf(f,
                 "     libnx per second (calls and ms inside): fences queried %.0f (%.1f ms), waited %.0f "
                 "(%.1f ms) | kickoff %.0f (%.1f ms) | new NvMaps %.1f cached (%.1f MB) and %.1f uncached "
                 "(%.1f MB), %.1f ms | GPU addresses %.1f (%.1f ms), mappings %.1f (%.1f MB, %.1f ms) | "
                 "armDCacheClean %.0f (%.1f MB, %.1f ms) | window queue %.1f (%.1f ms), dequeue buffer %.1f "
                 "(%.1f ms) | svcSleepThread: 0 %.0f, yield %.0f (%.1f ms), up to 1 ms %.0f (%.1f ms), longer %.0f\n",
                 n[kFenceQuery], ms[kFenceQuery], n[kFenceWait], ms[kFenceWait], n[kKickoff],
                 ms[kKickoff], n[kNvMapCached], mb[kNvMapCached], n[kNvMapNoCache], mb[kNvMapNoCache],
                 ms[kNvMapCached] + ms[kNvMapNoCache], n[kReserveAddress], ms[kReserveAddress],
                 n[kMappingGpu], mb[kMappingGpu], ms[kMappingGpu], n[kCacheClean], mb[kCacheClean], ms[kCacheClean],
                 n[kQueueBuffer], ms[kQueueBuffer], n[kDequeueBuffer], ms[kDequeueBuffer], n[kSleep0],
                 n[kSleepYield], ms[kSleepYield], n[kSleepShort], ms[kSleepShort], n[kSleepLong]);
  }
  if (frames > 0) {
    std::fprintf(f,
                 "     per frame: %.0f draws | scissor %.1f screens | %.1f submits | "
                 "%.1f resolves (%.2f screens) | transfers: %.1f calls, %.1f render "
                 "targets, %.1f draws | %.1f textures uploaded | %.2f MB of shared "
                 "memory uploaded | pipelines created in the interval: %.0f\n",
                 delta(5) / frames, delta(6) / frames / 921600.0, delta(7) / frames,
                 delta(9) / frames, delta(10) / frames / 921600.0, delta(11) / frames,
                 delta(12) / frames, delta(13) / frames, delta(14) / frames,
                 delta(15) / frames / 1048576.0, delta(16));
    std::fprintf(f,
                 "     draws by surface width: 1600+ %.0f | 1280-1599 %.0f | "
                 "640-1279 %.0f | 256-639 %.0f | under 256 %.0f\n",
                 delta(28) / frames, delta(29) / frames, delta(30) / frames, delta(31) / frames,
                 delta(32) / frames);
  }

  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = counters_now[i];
  }

  std::vector<Sample> samples(g_samples, g_samples + g_sample_count);
  g_sample_count = 0;
  std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.slot < b.slot; });

  // What each thread was doing during the long frames of this interval.
  {
    const u32 nt = std::min<u32>(g_num_hitches.exchange(0), kMaxHitches);
    if (nt && !samples.empty()) {
      std::vector<WindowHitch> v(g_hitches, g_hitches + nt);
      std::sort(v.begin(), v.end(), [](const WindowHitch& a, const WindowHitch& b) { return a.start < b.start; });
      double ms_total = 0.0;
      for (const WindowHitch& w : v) {
        ms_total += double(w.end - w.start) * 1000.0 / double(tick_freq);
      }
      const auto inside = [&v](u64 t) {
        auto it = std::upper_bound(v.begin(), v.end(), t, [](u64 x, const WindowHitch& w) { return x < w.start; });
        return it != v.begin() && t <= (it - 1)->end;
      };
      std::fprintf(f, "\n== during the hitches: %u frames over 45 ms (%.0f ms in total) ==\n", nt, ms_total);
      for (size_t i : order) {
        std::vector<u64> pcs;
        std::vector<const Sample*> all;
        size_t inside_svc = 0;
        for (const Sample& m : samples) {
          if (m.slot != i || !inside(m.tick)) {
            continue;
          }
          all.push_back(&m);
          if (AfterSvc(m.pc)) {
            ++inside_svc;
          } else {
            pcs.push_back(m.pc);
          }
        }
        if (all.size() < 5) {
          continue;
        }
        const Slot& s = g_slots[i];
        std::fprintf(f, "-- thread \"%s\": %zu samples in the hitches, %.0f%% waiting in the kernel\n",
                     s.name[0] ? s.name : "?", all.size(), double(inside_svc) * 100.0 / double(all.size()));
        const auto h = Histogram(pcs);
        for (size_t k = 0; k < h.size() && k < 25; ++k) {
          std::fprintf(f, "   %5.1f%%  pc ", double(h[k].n) * 100.0 / double(all.size()));
          PrintAddr(f, h[k].addr);
          std::fputc('\n', f);
        }
        std::sort(all.begin(), all.end(), [](const Sample* a, const Sample* b) {
          return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
        });
        std::vector<std::pair<const Sample*, u32>> unique_stacks;
        for (const Sample* m : all) {
          if (unique_stacks.empty() || std::memcmp(unique_stacks.back().first->frames, m->frames, sizeof(m->frames)) != 0) {
            unique_stacks.push_back({m, 1});
          } else {
            ++unique_stacks.back().second;
          }
        }
        std::sort(unique_stacks.begin(), unique_stacks.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t k = 0; k < unique_stacks.size() && k < 10; ++k) {
          std::fprintf(f, "   %5.1f%%  stack", double(unique_stacks[k].second) * 100.0 / double(all.size()));
          for (size_t j = 0; j < kFrames && unique_stacks[k].first->frames[j]; ++j) {
            std::fputs(j == 0 ? " " : " <- ", f);
            PrintAddr(f, unique_stacks[k].first->frames[j]);
          }
          std::fputc('\n', f);
        }
      }
    }
  }

  for (size_t i : order) {
    const Slot& s = g_slots[i];
    /*
     * Threads that use no CPU are listed too, with less detail: in a hang their
     * stacks are needed to know what they are waiting for.
     */
    const bool active = s.cpu >= 1.0;
    u64 tid = 0;
    svcGetThreadId(&tid, s.handle.load());
    // Horizon priority, preferred core and allowed cores: who can take the CPU from whom.
    s32 priority = -1;
    s32 preferred = -1;
    u64 mask = 0;
    svcGetThreadPriority(&priority, s.handle.load());
    svcGetThreadCoreMask(&preferred, &mask, s.handle.load());
    std::fprintf(f,
                 "\n-- thread %" PRIu64 " \"%s\": CPU %.1f%% | priority 0x%X, preferred core %d, "
                 "mask 0x%llX\n",
                 tid, s.name[0] ? s.name : "?", s.cpu, static_cast<unsigned>(priority),
                 static_cast<int>(preferred), static_cast<unsigned long long>(mask));

    auto lo = std::lower_bound(samples.begin(), samples.end(), i,
                               [](const Sample& a, size_t v) { return a.slot < v; });
    auto hi = std::upper_bound(samples.begin(), samples.end(), i,
                               [](size_t v, const Sample& a) { return v < a.slot; });
    const size_t n = size_t(hi - lo);
    if (n == 0) {
      std::fprintf(f, "   no samples\n");
      continue;
    }
    std::vector<u64> pcs, lrs;
    size_t in_svc = 0;
    for (auto it = lo; it != hi; ++it) {
      if (AfterSvc(it->pc)) {
        ++in_svc;
        continue;
      }
      pcs.push_back(it->pc);
      lrs.push_back(it->lr);
    }
    std::fprintf(f, "   %zu samples, %.0f%% waiting in the kernel\n", n,
                 double(in_svc) * 100.0 / double(n));
    const auto pc_hist = Histogram(pcs);
    const size_t busy = pcs.size();
    for (size_t k = 0; k < pc_hist.size() && k < (active ? 80u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  pc ", double(pc_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, pc_hist[k].addr);
      std::fputc('\n', f);
    }
    const auto lr_hist = Histogram(lrs);
    for (size_t k = 0; k < lr_hist.size() && k < (active ? 40u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  lr ", double(lr_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, lr_hist[k].addr);
      std::fputc('\n', f);
    }
    // Pc and lr of the same sample, so the samples of a leaf (memcpy, memset...), whose lr is its call
    // site, can be put on their callers. Stack sampling only. Caution: on the console the lr of the ring thread's
    // memcpy samples pointed just after system calls (a condition variable wait, an IPC), not at callers, probably
    // because the context handed back keeps an older lr; the BO2_MEMCPY_CALLERS test build counts callers instead.
    if (active) {
      std::vector<std::pair<u64, u64>> pairs;
      pairs.reserve(pcs.size());
      for (size_t k = 0; k < pcs.size(); ++k) {
        pairs.emplace_back(pcs[k], lrs[k]);
      }
      std::sort(pairs.begin(), pairs.end());
      std::vector<std::pair<std::pair<u64, u64>, u32>> counted;
      for (const auto& pl : pairs) {
        if (counted.empty() || counted.back().first != pl) {
          counted.push_back({pl, 1});
        } else {
          ++counted.back().second;
        }
      }
      std::sort(counted.begin(), counted.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
      for (size_t k = 0; k < counted.size() && k < 120u; ++k) {
        std::fprintf(f, "   %5.1f%%  pair ", double(counted[k].second) * 100.0 / double(busy));
        PrintAddr(f, counted[k].first.first);
        std::fputs(" lr ", f);
        PrintAddr(f, counted[k].first.second);
        std::fputc('\n', f);
      }
    }

    // Full stacks, waits included: they say what the thread is waiting for.
    std::vector<const Sample*> by_stack;
    for (auto it = lo; it != hi; ++it) {
      by_stack.push_back(&*it);
    }
    std::sort(by_stack.begin(), by_stack.end(), [](const Sample* a, const Sample* b) {
      return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
    });
    struct StackCount {
      const Sample* s;
      u32 n;
    };
    std::vector<StackCount> stacks;
    for (const Sample* s : by_stack) {
      if (stacks.empty() ||
          std::memcmp(stacks.back().s->frames, s->frames, sizeof(s->frames)) != 0) {
        stacks.push_back({s, 1});
      } else {
        ++stacks.back().n;
      }
    }
    std::sort(stacks.begin(), stacks.end(),
              [](const StackCount& a, const StackCount& b) { return a.n > b.n; });
    for (size_t k = 0; k < stacks.size() && k < (active ? 30u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  stack", double(stacks[k].n) * 100.0 / double(n));
      for (size_t j = 0; j < kFrames && stacks[k].s->frames[j]; ++j) {
        std::fputs(j == 0 ? " " : " <- ", f);
        PrintAddr(f, stacks[k].s->frames[j]);
      }
      std::fputc('\n', f);
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
}

/*
 * The one-second tick for the console overlays, separate from the profile loop.
 *
 * Two loops use it: the warm-up one (from second zero) and the normal one (from kStartDelayNs on).
 * When it lived only in the normal loop there was neither FPS nor resolution until 8 seconds had
 * passed: they appeared with the publisher logo instead of with the game's own logo, which comes much
 * earlier.
 *
 * It is cheap: two counter reads and a few bytes in shared memory. Update also re-attaches the
 * block if it is missing, so the overlay can be opened at any time.
 */
struct TicOverlay {
  u64 tic = 0;
  u64 presented = 0;
  double media[10] = {};
  size_t pos = 0;

  void Boot(u64 now) {
    tic = now;
    presented = g_counters[0].load(std::memory_order_relaxed);
    // Publish what is known right away: the resolution is valid from the first instant (g_resolution
    // starts at 1280x720 and the game corrects it when choosing the video mode), so the overlay's RES
    // row can show up even before the first frame.
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(0.0, 0.0, res >> 16, res & 0xFFFF, presented);
  }

  // Once per second. `with_clocks` turns off the Reverse-NX and clock sysmodule part, which is better
  // left alone during warm-up.
  void Step(u64 now, u64 freq, bool with_clocks) {
    if (now - tic < freq) {
      return;
    }
    const double seconds = double(now - tic) / double(freq);
    const u64 now_presented = g_counters[0].load(std::memory_order_relaxed);
    const double fps = seconds > 0.0 ? double(now_presented - presented) / seconds : 0.0;
    media[pos] = fps;
    pos = (pos + 1) % 10;
    double suma = 0.0;
    size_t how_many = 0;
    for (double v : media) {
      if (v > 0.0) {
        suma += v;
        ++how_many;
      }
    }
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(fps, how_many ? suma / double(how_many) : fps, res >> 16, res & 0xFFFF,
                                        now_presented);
    // Reverse-NX is also told that the game keeps asking for the mode (its overlay requires it to show
    // the controls) and, if the system rules, the real mode is mirrored.
    // And if requested, the clock sysmodule is asked for its docked clocks.
    if (with_clocks) {
      const bool docked_real = appletGetOperationMode() == AppletOperationMode_Console;
      const bool docked = rex::ui::switch_saltynx::DockMode(docked_real);
      rex::ui::switch_sysclk::FollowMode(docked, docked_real);
    }
    /*
     * The memory clock is watched here too. It is outside the if above on purpose: the
     * system raises the EMC right after we request the GPU profile, that is, during
     * warm-up, which is when with_clocks is false. It is one clkrst read per second for
     * the first 20 and one every 30 after that; when we have not changed anything it
     * does not even do that.
     */
    RexSwitchApmWatch();
    presented = now_presented;
    tic = now;
  }
};

void ProfilerMain(void*) {
  const u64 tick_freq = armGetSystemTickFreq();

  /*
   * The overlay first, without waiting for kStartDelayNs. Those 8 seconds are for stack sampling and
   * the first report (which read the SD and pause threads), not for this: publishing FPS and
   * resolution is a few bytes in shared memory, and it has to show already at the game's first logo.
   * During warm-up the tick keeps running every second, so attaching the block is also retried if
   * SaltyNX is slow.
   */
  TicOverlay overlay;
  const u64 start = armGetSystemTick();
  overlay.Boot(start);
  const u64 warmup = (kStartDelayNs / 1000000ULL) * tick_freq / 1000ULL;
  while (armGetSystemTick() - start < warmup) {
    __real_svcSleepThread(kPassiveIntervalNs);
    overlay.Step(armGetSystemTick(), tick_freq, false);
  }

  // Invasive sampling is only enabled explicitly, for diagnostics.
  // The normal mode keeps FPS, CPU and counters, without stopping the game.
  bool sample_stacks = false;
  if (FILE* flag = std::fopen(StackFlagPath().c_str(), "r")) {
    sample_stacks = true;
    std::fclose(flag);
  }
  // Decided here, 8 s after start: the game has applied its settings long before.
  if (const int forced = g_stacks_forced.load(std::memory_order_relaxed); forced == 0 || forced == 1) {
    sample_stacks = forced == 1;
  }

  u64 counters_last[kCounterCount]{};
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  // The file starts over with a header when the report is on; off, last run's file goes, so none is left (the
  // report may come on later in the run: the header is written then).
  bool header = false;
  const auto write_header = [&] {
    if (FILE* f = std::fopen(ReportPath().c_str(), "w")) {
      std::fprintf(f, "image 0x%016" PRIx64 ", code up to 0x%016" PRIx64 "\n\n", g_base,
                   g_text_hi);
      std::fprintf(f, "Stack sampling: %s\n", sample_stacks ? "on (1 ms)" : "off");
      std::fclose(f);
    }
    header = true;
  };
  if (g_report.load(std::memory_order_relaxed)) {
    write_header();
  } else {
    std::remove(ReportPath().c_str());
  }

  // The console overlays read from SaltyNX's shared memory. That is already running from second
  // zero (see the warm-up loop above); only the normal loop continues here.

  // A short first report just to set the starting CPU time.
  u64 report_start = armGetSystemTick();
  Report(1, tick_freq, counters_last, "startup");
  report_start = armGetSystemTick();

  size_t rr = 0;
  size_t mode_index = 0;
  while (true) {
    __real_svcSleepThread(sample_stacks ? kSampleIntervalNs : kPassiveIntervalNs);

    for (size_t tries = 0; sample_stacks && tries < kMaxThreads; ++tries) {
      const size_t idx = rr;
      rr = (rr + 1) % kMaxThreads;
      Slot& s = g_slots[idx];
      const u32 h = s.handle.load();
      if (!h || !s.busy) {
        continue;
      }
      if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
        break;
      }
      ThreadContext ctx;
      const Result rc = svcGetThreadContext3(&ctx, h);
      if (R_SUCCEEDED(rc) && g_sample_count < kMaxSamples) {
        Sample& out = g_samples[g_sample_count++];
        out = {};
        out.pc = ctx.pc.x;
        out.lr = ctx.lr;
        out.slot = static_cast<u16>(idx);
        out.tick = armGetSystemTick();
        // The stack, with the thread still paused: it is our own memory and
        // there are no locks. Only frames within the region of its sp.
        MemoryInfo smi;
        u32 spi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, ctx.sp)) && (smi.perm & Perm_R)) {
          const u64 lo = smi.addr, hi = smi.addr + smi.size;
          u64 fp = ctx.fp;
          for (size_t k = 0; k < kFrames && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; ++k) {
            out.frames[k] = reinterpret_cast<const u64*>(fp)[1];
            const u64 next = reinterpret_cast<const u64*>(fp)[0];
            if (next <= fp) {
              break;
            }
            fp = next;
          }
        }
      }
      svcSetThreadActivity(h, ThreadActivity_Runnable);
      break;
    }

    const u64 now = armGetSystemTick();
    // Once per second, FPS and resolution for the overlay (the profile report runs every 10 s).
    overlay.Step(now, tick_freq, true);
    if (now - report_start >= kReportSeconds * tick_freq) {
      if (!header && g_report.load(std::memory_order_relaxed)) {
        write_header();
      }
      Report(now - report_start, tick_freq, counters_last, kModes[mode_index].name);
      if (g_ab_active.load(std::memory_order_relaxed)) {
        mode_index = (mode_index + 1) % kModeCount;
        g_skip_mask.store(kModes[mode_index].mask, std::memory_order_relaxed);
        if (mode_index == 0) {
          // Full cycle: it turns itself off and the game looks right again.
          g_ab_active.store(false, std::memory_order_relaxed);
        }
      }
      report_start = armGetSystemTick();
    }
  }
}

/* Priority 102: after switch_crash_hooks.c, before the SDK. */
__attribute__((constructor(102))) void StartProfiler() {
  g_base = reinterpret_cast<u64>(&_start);
  MemoryInfo mi;
  u32 page_info = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, g_base))) {
    g_text_lo = mi.addr;
    g_text_hi = mi.addr + mi.size;
  }
  Register(envGetMainThreadHandle());
  std::strncpy(g_slots[0].name, "main (UI)", sizeof(g_slots[0].name) - 1);
  // 0x2A: above everything in the game (audio runs at 0x2B), so the samples
  // are taken on time. It sleeps almost all the time.
  if (R_SUCCEEDED(__real_threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A,
                                      -2))) {
    threadStart(&g_thread);
  }
}

}  // namespace

extern "C" {

/* Each registered thread's handle, name and CPU time so far (system ticks), for the app's own measurements (the
 * benchmark's CPU per A/B arm). Returns how many it wrote. */
size_t RexSwitchPerfThreadTicks(u32* handles, char (*names)[32], u64* ticks, size_t max) {
  size_t n = 0;
  for (size_t i = 0; i < kMaxThreads && n < max; ++i) {
    const u32 h = g_slots[i].handle.load();
    u64 t = 0;
    if (!h || R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, h, UINT64_MAX))) {
      continue;
    }
    handles[n] = h;
    std::memcpy(names[n], g_slots[i].name, sizeof(names[n]));
    names[n][31] = 0;
    ticks[n] = t;
    ++n;
  }
  return n;
}

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid) {
  const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
  if (R_SUCCEEDED(rc) && t) {
    Register(t->handle);
  }
  return rc;
}

Result __wrap_threadClose(Thread* t) {
  if (t) {
    Unregister(t->handle);
  }
  return __real_threadClose(t);
}

/* libnx calls that cost IPC or walk memory. See g_calls. */
Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {
  const u64 since = armGetSystemTick();
  const Result rc = __real_nvFenceWait(f, timeout_us);
  Annotate(timeout_us == 0 ? kFenceQuery : kFenceWait, since);
  return rc;
}

Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
  const u64 since = armGetSystemTick();
  const Result rc = __real_nvGpuChannelKickoff(c);
  Annotate(kKickoff, since);
  return rc;
}

Result __wrap_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable) {
  const u64 since = armGetSystemTick();
  const Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, is_cpu_cacheable);
  Annotate(is_cpu_cacheable ? kNvMapCached : kNvMapNoCache, since, size);
  return rc;
}

Result __wrap_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova) {
  const u64 since = armGetSystemTick();
  const Result rc = __real_nvAddressSpaceAllocFixed(a, sparse, size, iova);
  Annotate(kReserveAddress, since, size);
  return rc;
}

Result __wrap_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset) {
  const u64 since = armGetSystemTick();
  const Result rc = __real_nvioctlNvhostAsGpu_MapBufferEx(fd, flags, kind, nvmap_handle, page_size, buffer_offset,
                                                          mapping_size, input_offset, offset);
  Annotate(kMappingGpu, since, mapping_size);
  return rc;
}

void __wrap_armDCacheClean(void* addr, size_t size) {
  const u64 since = armGetSystemTick();
  __real_armDCacheClean(addr, size);
  Annotate(kCacheClean, since, size);
}

/*
 * The presentation interval (RexSwitchPerfSwapInterval).
 *
 * It is reapplied on every present, not when the chain is created, on purpose: the WSI sets it to 1
 * when creating the swapchain, and the chain is recreated when switching from docked to handheld.
 * With 0 nothing is touched and the swapchain mode rules (IMMEDIATE -> interval 0).
 *
 * libnx only accepts interval 0 if the NWindow has three or more buffers; the Horizon WSI always
 * configures exactly 3, so the condition holds. 2 is always accepted.
 *
 * Warning: it is 0 and must stay that way. Measured: with the current frame time, setting 2 sends
 * 22 % of the frames to 66.7 ms. It only makes sense with a median frame time below 31 ms.
 */
std::atomic<unsigned> g_interval_swap{0};
// The game's VSYNC setting (RexSwitchPerfSwapIntervalForce): an interval applied even when it is 0, so
// turning VSYNC off puts back the interval it changed. -1 = not set: g_interval_swap rules.
std::atomic<int> g_interval_forced{-1};

Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  const u64 since = armGetSystemTick();
  const int forced = g_interval_forced.load(std::memory_order_relaxed);
  const unsigned requested = forced >= 0 ? unsigned(forced) : g_interval_swap.load(std::memory_order_relaxed);
  if ((forced >= 0 || requested != 0) && nw != nullptr && nw->swap_interval != requested) {
    nwindowSetSwapInterval(nw, requested);
  }
  const Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
  Annotate(kQueueBuffer, since);
  return rc;
}

// The presenter's longest dequeueBuffer wait since the last RexSwitchPerfTakeDequeueWaitUs, in
// microseconds. With the window's three buffers it only waits when two finished frames are already queued for the
// screen.
std::atomic<uint32_t> g_dequeue_wait_us{0};

Result __wrap_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence) {
  const u64 since = armGetSystemTick();
  const Result rc = __real_bqDequeueBuffer(b, async, width, height, format, usage, buf, fence);
  Annotate(kDequeueBuffer, since);
  const uint32_t us = uint32_t(armTicksToNs(armGetSystemTick() - since) / 1000);
  uint32_t longest = g_dequeue_wait_us.load(std::memory_order_relaxed);
  while (us > longest && !g_dequeue_wait_us.compare_exchange_weak(longest, us, std::memory_order_relaxed)) {
  }
  return rc;
}

uint32_t RexSwitchPerfTakeDequeueWaitUs(void) {
  return g_dequeue_wait_us.exchange(0, std::memory_order_relaxed);
}

void __wrap_svcSleepThread(s64 nano) {
  const u64 since = armGetSystemTick();
  __real_svcSleepThread(nano);
  Annotate(nano == 0 ? kSleep0 : nano < 0 ? kSleepYield : nano <= 1000000 ? kSleepShort : kSleepLong, since);
}

/*
 * Counters: 0 = guest present, 1 = read emulated through the shadow,
 * 2 = fault resolved by an SDK handler, 3 = retry emulated through the
 * shadow, 4 = SEH. 28..32 = draws per surface width from IssueDraw
 * (1600 or more, 1280-1599, 640-1279, 256-639, under 256).
 */
// Stack sampling pauses every thread each millisecond: it costs frames and changes how threads share the cores, so
// the game's settings may decide it per run (bo2zm_profile_stacks): 1 on, 0 off, -1 (default) on when
// logs/rex/profile_stacks.flag exists. Only before the profiler decides, 8 s after start.
void RexSwitchPerfSetStackSampling(int forced) {
  g_stacks_forced.store(forced, std::memory_order_relaxed);
}

// Whether the 10 s report goes to rex_profile.log (on by default). Any time; set off before 8 s after start, it also
// deletes the last run's file.
void RexSwitchPerfSetReport(bool on) {
  g_report.store(on, std::memory_order_relaxed);
}

// A long frame, in armGetSystemTick ticks (see WindowHitch).
void RexSwitchPerfHitch(u64 start, u64 end) {
  const u32 i = g_num_hitches.fetch_add(1);
  if (i < kMaxHitches) {
    g_hitches[i] = {start, end};
  }
}

void RexSwitchPerfResolution(unsigned width_px, unsigned height_px) {
  g_resolution.store(((width_px & 0xFFFF) << 16) | (height_px & 0xFFFF), std::memory_order_relaxed);
}

// See g_interval_swap, next to the nwindowQueueBuffer wrapper. 0 = touch nothing (the normal case).
void RexSwitchPerfSwapInterval(unsigned vblanks) {
  g_interval_swap.store(vblanks, std::memory_order_relaxed);
}

// The interval for the game's VSYNC setting, applied even when 0 (see g_interval_forced).
void RexSwitchPerfSwapIntervalForce(int vblanks) {
  g_interval_forced.store(vblanks < 0 ? -1 : std::min(vblanks, 4), std::memory_order_relaxed);
}

void RexSwitchPerfCount(unsigned id) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(1, std::memory_order_relaxed);
  }
  if (id == 0) {
    // Counter 0 is presents. The console overlay clears the "alive" mark and the resolution mark and
    // only waits 100 ms, so it has to be answered on every frame, not once per second. It is writing a
    // few bytes to shared memory.
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Heartbeat(res >> 16, res & 0xFFFF);
  }
}

/*
 * 5 draws, 6 scissor area of the draws (pixels), 7 queue submissions,
 * 9 resolves, 10 resolve area, 11 render target transfer calls,
 * 12 render targets transferred, 13 transfer draws, 14 textures loaded,
 * 15 shared memory bytes uploaded, 16 pipelines created, 17 physical memory
 * chunks committed on touch, 18 chunks mapped into a view on touch,
 * 21 clipped audio samples, 22 audio buffers with a peak of 0.98 or more.
 */
void RexSwitchPerfAdd(unsigned id, u64 value) {
  if (id < kCounterCount) {
    g_counters[id].fetch_add(value, std::memory_order_relaxed);
  }
}

/* max counter (23 = audio mix peak in ten-thousandths; the report resets it to 0). */
void RexSwitchPerfMax(unsigned id, u64 value) {
  if (id >= kCounterCount) {
    return;
  }
  u64 actual = g_counters[id].load(std::memory_order_relaxed);
  while (value > actual && !g_counters[id].compare_exchange_weak(actual, value, std::memory_order_relaxed)) {
  }
}

/*
 * Bits: 1 draws, 2 transfers, 4 resolves, 8 texture loads,
 * 16 presentation effect, 32 tiled dispatch.
 */
bool RexSwitchPerfSkip(unsigned bit) {
  return (g_skip_mask.load(std::memory_order_relaxed) & bit) != 0;
}

void RexSwitchPerfSetThreadName(u32 handle, const char* name) {
  if (!handle || !name) {
    return;
  }
  for (auto& s : g_slots) {
    if (s.handle.load() == handle) {
      char tmp[sizeof(s.name)]{};
      std::strncpy(tmp, name, sizeof(tmp) - 1);
      std::memcpy(s.name, tmp, sizeof(tmp));
      return;
    }
  }
}

}  // extern "C"
