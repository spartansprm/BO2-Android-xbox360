#include "bo2zm_benchmark.h"

#include <algorithm>
#include <atomic>
#if defined(__SWITCH__)
#include <malloc.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>

#include "bo2zm_cheats.h"
#include "bo2zm_lifecycle.h"
#include "bo2zm_preset.h"
#include "native/render/bo2zm_native_capture.h"

#if defined(__SWITCH__)
#include "core/threading_switch.h"
#endif

REXCVAR_DEFINE_BOOL(bo2zm_benchmark, false, "BO2",
                    "Test helper: once in a match, a fixed 3-minute route with the test pad and a frame-time "
                    "summary in the log ('[benchmark]'). Use with bo2zm_test_god")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_stall_at_s, 0, "BO2",
                     "Test helper, with bo2zm_benchmark: this many seconds into the route the renderer thread stops "
                     "for 90 s, once, so no frame is presented: it tests a frame watchdog set below 90 s without "
                     "a real freeze, since the game goes on by itself if the watchdog does not act. 0 = off")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_ab_s, 0, "BO2",
                     "Test helper, with bo2zm_benchmark: a 2x2 A/B inside the route, changing arm every N seconds "
                     "(bo2zm_benchmark.h, AbArm); the summary gets one line per arm and per factor. 0 = no A/B")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DEFINE_INT32(bo2zm_benchmark_priority_main_ab, 0, "BO2",
                     "Test (Switch), with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) whose on arms run the game's "
                     "Main thread at bo2zm_benchmark_priority_raised instead of 0x3B. 0 = not part of the A/B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_priority_backend_ab, 0, "BO2",
                     "Test (Switch), with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) whose on arms run the game's "
                     "Backend (render) thread at bo2zm_benchmark_priority_raised instead of 0x3B. 0 = not part of "
                     "the A/B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_priority_raised, 0x3A, "BO2",
                     "Test (Switch): the Horizon priority of a game thread raised by bo2zm_benchmark_priority_*_ab "
                     "(0x2C-0x3B; lower is more urgent)")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_benchmark_xma_cache_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is the SDK's apu_xma_cache (decoded XMA frames "
                    "reused) in each arm, back to its own value after the route")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REXCVAR_DECLARE(int32_t, bo2zm_ipak_mb_s_in_match);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_ipak_mb_s_b, -1, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the SDK's switch_io_ipak_mb_s (the .ipak read rate limit) in the "
                     "A/B arms with factor 1 on; the arms with it off read at bo2zm_ipak_mb_s_in_match (or the SDK's "
                     "rate when that is 0). -1 = not part of the A/B")
    .range(-1, 1024)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_s, 180, "BO2",
                     "Test helper, with bo2zm_benchmark: the route's length in seconds (180; a soak test runs longer)")
    .range(30, 7200)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_next_round_s, 0, "BO2",
                     "Test helper, with bo2zm_benchmark: during the route, go to the next round every this many "
                     "seconds (a soak test's round changes and spawns). 0 = off")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_round, 0, "BO2",
                     "Test helper, with bo2zm_benchmark: once in a match, go to this round first (the Extras menu's "
                     "round pick) and start the route when the game shows it. 0 = the route starts on the first round")
    .range(0, 255)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(bo2zm_test_spot, "", "BO2",
                      "Test helper, with bo2zm_benchmark: \"map x y z pitch yaw\" (MARK LOCATION's line). The route "
                      "stands player 0 still at those feet, looking that way, with no zombies, and takes a burst of "
                      "bo2zm_test_spot_shots screenshots 4 s in (captures/spot_*), to compare frames for flicker. "
                      "\"map spawn pitch yaw\" stands where the player spawned (a map without marked spots). Empty = "
                      "the usual route")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_spot_turns, 1, "BO2",
                     "Test helper, with bo2zm_test_spot: bursts in this many directions, turning 360 / N degrees "
                     "between them, one every 8 s (bo2zm_benchmark_s must cover bo2zm_test_spot_first_s + 8 x N s)")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_spot_first_s, 4, "BO2",
                     "Test helper, with bo2zm_test_spot: seconds into the route before the first burst. Mob of the "
                     "Dead starts every player in Afterlife, whose swirling screen changes every frame, for ~66 s: 75 "
                     "there")
    .range(4, 300)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_spot_shots, 12, "BO2",
                     "Test helper, with bo2zm_test_spot: screenshots in the burst, taken one after another")
    .range(1, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_spot_every_ms, 0, "BO2",
                     "Test helper, with bo2zm_test_spot: the burst's screenshots this many ms apart (0 = as fast as "
                     "they can be taken: ~0.3 s apart on the Switch, ~20 ms on the PC); the same spacing on both makes "
                     "their bursts cover the same time")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_benchmark_net_idle_us_b, -1, "BO2",
                     "Test (Switch), with bo2zm_benchmark_ab_s: the SDK's switch_net_idle_poll_us in the A/B arms with "
                     "factor 1 on. -1 = not part of the A/B")
    .range(-1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(BO2ZM_PGO_GENERATE)


extern "C" void __gcov_reset(void);
extern "C" void __gcov_dump(void);
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>


extern "C" FILE* __real_fopen(const char* name, const char* mode);
extern "C" __attribute__((used)) FILE* __wrap_fopen(const char* name, const char* mode) {
  static const char kFolder[] = "/switch/bo2zm/pgo";
  constexpr size_t kLength = sizeof(kFolder) - 1;
  if (name && std::strncmp(name, kFolder, kLength) == 0 && name[kLength] == char(92)) {
    std::string fixed(name);
    fixed[kLength] = '/';
    return __real_fopen(fixed.c_str(), mode);
  }
  return __real_fopen(name, mode);
}
#endif

namespace bo2zm::benchmark {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kWait = 10.0;

double Duration() {
  static const double seconds = double(REXCVAR_GET(bo2zm_benchmark_s));
  return seconds;
}
constexpr double kSettle = 0.5;


constexpr uint32_t kLookRight = 1u << 24;
constexpr uint32_t kLookLeft = 1u << 25;


constexpr int kMoveTilt = 45;
constexpr int kLookTilt = 100;
struct Span {
  double since, until;
  uint32_t buttons;
};
constexpr Span kPath[] = {
    {0.0, 30.0, kLookRight},
    {30.0, 60.0, kLookLeft},
};

std::atomic<int64_t> g_start{0};
std::atomic<uint64_t> g_frames_seen{0};
std::atomic<bool> g_off{false};
std::mutex g_mutex;
std::vector<float> g_ms;
std::vector<float> g_arm_ms[4];
Clock::time_point g_earlier{};
bool g_summarized = false;

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}


struct TestSpot {
  bool valid = false;
  bool at_spawn = false;
  std::string map;
  float feet[3] = {};
  float pitch = 0.0f, yaw = 0.0f;
};
const TestSpot& ParsedSpot() {
  static const TestSpot spot = [] {
    TestSpot s;
    const std::string& text = REXCVAR_GET(bo2zm_test_spot);
    char map[64] = {};
    s.valid = !text.empty() && std::sscanf(text.c_str(), "%63s %f %f %f %f %f", map, &s.feet[0], &s.feet[1],
                                           &s.feet[2], &s.pitch, &s.yaw) == 6;
    if (!s.valid && !text.empty()) {
      s.at_spawn = std::sscanf(text.c_str(), "%63s spawn %f %f", map, &s.pitch, &s.yaw) == 3;
      s.valid = s.at_spawn;
    }
    s.map = map;
    if (!text.empty() && !s.valid) {
      REXLOG_WARN("[spot] bo2zm_test_spot \"{}\" is neither \"map x y z pitch yaw\" nor \"map spawn pitch yaw\": the "
                  "usual route runs", text);
    }
    return s;
  }();
  return spot;
}


void StepSpot(double t) {
  static int step = 0;
  static int turn = 0;
  static TestSpot held;
  constexpr double kBetweenBursts = 8.0, kTurnAhead = 2.0;
  static const double kFirstBurst = double(REXCVAR_GET(bo2zm_test_spot_first_s));
  const TestSpot& spot = ParsedSpot();
  const int turns = REXCVAR_GET(bo2zm_test_spot_turns);
  if (step == 0 && t >= 0.0) {
    held = spot;
    if (spot.at_spawn) {
      bo2zm::cheats::Spot now;
      if (!bo2zm::cheats::PlayerSpot(now)) {
        return;
      }
      for (int i = 0; i < 3; ++i) {
        held.feet[i] = now.feet[i];
      }
    }
    step = 1;
    bo2zm::cheats::HoldAt(held.feet, held.pitch, held.yaw);
    bo2zm::cheats::KillZombiesNow();
    const std::string loaded = bo2zm::preset::LoadedMap();
    REXLOG_INFO("[spot] standing at {:.1f} {:.1f} {:.1f}{}, looking pitch {:.1f} yaw {:.1f} ({}; loaded: {})",
                held.feet[0], held.feet[1], held.feet[2], spot.at_spawn ? " (the spawn point)" : "", held.pitch,
                held.yaw, spot.map, loaded);
    if (loaded.compare(0, spot.map.size() + 1, spot.map + " ") != 0) {
      REXLOG_WARN("[spot] the loaded map is not the spot's {}: check bo2zm_test_start", spot.map);
    }
  } else if (step == 1 && turn > 0 && turn < turns && t >= kFirstBurst + turn * kBetweenBursts - kTurnAhead &&
             t < kFirstBurst + turn * kBetweenBursts) {

    static int turned = 0;
    if (turned < turn) {
      turned = turn;
      const float yaw = std::remainder(held.yaw + 360.0f * float(turn) / float(turns), 360.0f);
      bo2zm::cheats::HoldAt(held.feet, held.pitch, yaw);
    }
  } else if (step == 1 && turn < turns && t >= kFirstBurst + turn * kBetweenBursts) {
    ++turn;
    if (turn == turns) {
      step = 2;
    }
    bo2zm::cheats::Spot now;
    if (bo2zm::cheats::PlayerSpot(now)) {
      REXLOG_INFO("[spot] the player is at {:.1f} {:.1f} {:.1f} looking pitch {:.1f} yaw {:.1f} (camera {:.1f} {:.1f} "
                  "{:.1f})", now.feet[0], now.feet[1], now.feet[2], now.angles[0], now.angles[1], now.camera[0],
                  now.camera[1], now.camera[2]);
    }
    const std::time_t clock = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&clock));
    const int shots = REXCVAR_GET(bo2zm_test_spot_shots);
    const int every_ms = REXCVAR_GET(bo2zm_test_spot_every_ms);
    for (int i = 0; i < shots; ++i) {
      bo2zm::capture::RequestCapture(fmt::format("spot_{}_{}_{:02}", spot.map, stamp, i + 1),
                                     std::chrono::milliseconds(i * every_ms));
    }
    REXLOG_INFO("[spot] a burst of {} screenshots: captures/spot_{}_{}_01.png on (direction {} of {})", shots, spot.map,
                stamp, turn, turns);
  } else if (step == 2 && t >= Duration()) {
    step = 3;
    bo2zm::cheats::ReleaseHold();
  }
}

int AbSeconds() {
  static const int seconds = IsActive() ? std::max(0, REXCVAR_GET(bo2zm_benchmark_ab_s)) : 0;
  return seconds;
}


int ArmAt(double t, double* into_span) {
  const int seconds = AbSeconds();
  if (seconds <= 0 || t < 0.0 || t >= Duration()) {
    return -1;
  }
  const int span = int(t / seconds);
  if (into_span) {
    *into_span = t - double(span) * seconds;
  }
  return (span + int(t / 60.0)) % 4;
}


struct Line {
  size_t frames = 0;
  double fps = 0.0;
  double median = 0.0;
  double p90 = 0.0;
  double p99 = 0.0;
};
Line Describe(std::vector<float> ms) {
  Line line;
  if (ms.empty()) {
    return line;
  }
  std::sort(ms.begin(), ms.end());
  const auto pct = [&](double p) { return ms[std::min(ms.size() - 1, size_t(p * double(ms.size())))]; };
  double sum = 0.0;
  for (float v : ms) {
    sum += v;
  }
  line.frames = ms.size();
  line.fps = sum > 0.0 ? 1000.0 * double(ms.size()) / sum : 0.0;
  line.median = pct(0.5);
  line.p90 = pct(0.9);
  line.p99 = pct(0.99);
  return line;
}

#if defined(__SWITCH__)


void ApplyAbNet(int arm) {
  static const int32_t b = REXCVAR_GET(bo2zm_benchmark_net_idle_us_b);
  if (b < 0) {
    return;
  }
  static const int32_t usual = rex::cvar::Query<int32_t>("switch_net_idle_poll_us");
  static int applied = -1;
  const int on = arm >= 0 && (arm & 1) ? 1 : 0;
  if (on == applied) {
    return;
  }
  if (applied < 0) {
    REXLOG_INFO("[benchmark] A/B factor 1 = switch_net_idle_poll_us {} instead of {}", b, usual);
  }
  applied = on;
  rex::cvar::SetFlagByName("switch_net_idle_poll_us", std::to_string(on ? b : usual));
}
#endif


void ApplyAbXmaCache(int arm) {
  static const bool ab = REXCVAR_GET(bo2zm_benchmark_xma_cache_ab);
  if (!ab) {
    return;
  }
  static const bool usual = rex::cvar::Query<bool>("apu_xma_cache");
  static int applied = -1;
  const int on = arm >= 0 ? ((arm & 1) ? 1 : 0) : (usual ? 1 : 0);
  if (on == applied) {
    return;
  }
  if (applied < 0) {
    REXLOG_INFO("[benchmark] A/B factor 1 = apu_xma_cache (decoded XMA frames reused)");
  }
  applied = on;
  rex::cvar::SetFlagByName("apu_xma_cache", on ? "true" : "false");
}


void ApplyAbIpak(int arm) {
  static const int32_t b = REXCVAR_GET(bo2zm_benchmark_ipak_mb_s_b);
  if (b < 0) {
    return;
  }
  static const int32_t in_match = REXCVAR_GET(bo2zm_ipak_mb_s_in_match);
  static const int32_t usual = in_match > 0 ? in_match : rex::cvar::Query<int32_t>("switch_io_ipak_mb_s");
  static int applied = -1;
  const int on = arm >= 0 && (arm & 1) ? 1 : 0;
  if (on == applied) {
    return;
  }
  if (applied < 0) {
    REXLOG_INFO("[benchmark] A/B factor 1 = switch_io_ipak_mb_s {} instead of {}", b, usual);
  }
  applied = on;
  rex::cvar::SetFlagByName("switch_io_ipak_mb_s", std::to_string(on ? b : usual));
}

#if defined(__SWITCH__)


}
}
extern "C" size_t RexSwitchPerfThreadTicks(uint32_t* handles, char (*names)[32], uint64_t* ticks, size_t max);
namespace bo2zm::benchmark {
namespace {
struct ArmCpu {
  std::map<std::string, uint64_t> ticks;
  uint64_t elapsed = 0;
};
ArmCpu g_arm_cpu[4];


uint64_t SystemTick() {
  uint64_t t;
  asm volatile("mrs %0, cntpct_el0" : "=r"(t));
  return t;
}

void SampleArmCpu(int arm_ended) {
  constexpr size_t kMax = 160;
  static uint32_t handles[kMax];
  static char names[kMax][32];
  static uint64_t ticks[kMax];
  static std::map<uint32_t, uint64_t> last;
  static uint64_t last_tick = 0;
  const size_t n = RexSwitchPerfThreadTicks(handles, names, ticks, kMax);
  const uint64_t now = SystemTick();
  if (arm_ended >= 0 && arm_ended < 4 && last_tick != 0) {
    ArmCpu& a = g_arm_cpu[arm_ended];
    a.elapsed += now - last_tick;
    for (size_t i = 0; i < n; ++i) {
      const auto it = last.find(handles[i]);
      if (it != last.end() && ticks[i] >= it->second) {
        a.ticks[names[i][0] ? names[i] : "?"] += ticks[i] - it->second;
      }
    }
  }
  last.clear();
  for (size_t i = 0; i < n; ++i) {
    last[handles[i]] = ticks[i];
  }
  last_tick = now;
}

void SummarizeArmCpu() {
  for (int bit = 0; bit < 2; ++bit) {
    std::map<std::string, uint64_t> on, off;
    uint64_t on_elapsed = 0, off_elapsed = 0;
    for (int arm = 0; arm < 4; ++arm) {
      const bool is_on = (arm >> bit) & 1;
      for (const auto& [name, t] : g_arm_cpu[arm].ticks) {
        (is_on ? on : off)[name] += t;
      }
      (is_on ? on_elapsed : off_elapsed) += g_arm_cpu[arm].elapsed;
    }
    if (on_elapsed == 0 || off_elapsed == 0) {
      continue;
    }
    std::vector<std::pair<double, std::string>> rows;
    double total_on = 0.0, total_off = 0.0;
    std::map<std::string, bool> names;
    for (const auto& [name, t] : on) {
      names[name] = true;
    }
    for (const auto& [name, t] : off) {
      names[name] = true;
    }
    std::string line;
    for (const auto& [name, unused] : names) {
      const double a = 100.0 * double(on[name]) / double(on_elapsed);
      const double b = 100.0 * double(off[name]) / double(off_elapsed);
      total_on += a;
      total_off += b;
      if (a >= 2.0 || b >= 2.0) {
        rows.push_back({-b, fmt::format("{} {:.1f} / {:.1f}", name, a, b)});
      }
    }
    std::sort(rows.begin(), rows.end());
    for (const auto& row : rows) {
      line += ", " + row.second;
    }
    REXLOG_INFO("[benchmark] A/B factor {} CPU, % of a core on / off: all threads {:.1f} / {:.1f}{}", bit + 1,
                total_on, total_off, line);
  }
}
#endif


constexpr int kGuestPriority = 0x3B;
std::atomic<int> g_want_priority[3] = {kGuestPriority, kGuestPriority, kGuestPriority};

void ApplyAbPriorities(int arm) {
  static const int main_factor = REXCVAR_GET(bo2zm_benchmark_priority_main_ab);
  static const int backend_factor = REXCVAR_GET(bo2zm_benchmark_priority_backend_ab);
  if (main_factor == 0 && backend_factor == 0) {
    return;
  }
  static int applied_arm = -2;
  if (arm == applied_arm) {
    return;
  }
  if (applied_arm == -2) {
    REXLOG_INFO("[benchmark] A/B: factor {} = the Main thread, factor {} = the Backend thread at priority {:#x} "
                "instead of {:#x} (each thread sets its own)",
                main_factor, backend_factor, REXCVAR_GET(bo2zm_benchmark_priority_raised), kGuestPriority);
  }
  applied_arm = arm;
  static const int raised = REXCVAR_GET(bo2zm_benchmark_priority_raised);
  auto want = [&](int factor) {
    return (factor == 1 || factor == 2) && arm >= 0 && (arm & factor) != 0 ? raised : kGuestPriority;
  };
  g_want_priority[1].store(want(main_factor), std::memory_order_relaxed);
  g_want_priority[2].store(want(backend_factor), std::memory_order_relaxed);
}

void SummarizeAb() {
  for (int arm = 0; arm < 4; ++arm) {
    const Line l = Describe(g_arm_ms[arm]);
    REXLOG_INFO("[benchmark] A/B arm {} (factor 1 {}, factor 2 {}): {} frames, {:.1f} FPS, median {:.1f} ms, "
                "p90 {:.1f} ms, p99 {:.1f} ms",
                arm, (arm & 1) ? "on" : "off", (arm & 2) ? "on" : "off", l.frames, l.fps, l.median, l.p90, l.p99);
  }
  for (int bit = 0; bit < 2; ++bit) {
    std::vector<float> on, off;
    for (int arm = 0; arm < 4; ++arm) {
      std::vector<float>& to = (arm >> bit) & 1 ? on : off;
      to.insert(to.end(), g_arm_ms[arm].begin(), g_arm_ms[arm].end());
    }
    const Line a = Describe(on);
    const Line b = Describe(off);
    REXLOG_INFO("[benchmark] A/B factor {}: on {:.1f} FPS (median {:.1f} ms, p90 {:.1f} ms, {} frames), off {:.1f} FPS "
                "(median {:.1f} ms, p90 {:.1f} ms, {} frames): {:+.1f} %",
                bit + 1, a.fps, a.median, a.p90, a.frames, b.fps, b.median, b.p90, b.frames,
                b.fps > 0.0 ? 100.0 * (a.fps / b.fps - 1.0) : 0.0);
  }
#if defined(__SWITCH__)
  SummarizeArmCpu();
#endif
}


double g_next_partial = 30.0;
void SummarizeSoFar(double seconds) {
  std::string line;
  const Line all = Describe(g_ms);
  char part[160];
  std::snprintf(part, sizeof(part), "%.1f FPS (%zu frames)", all.fps, all.frames);
  line += part;
  for (int bit = 0; AbSeconds() > 0 && bit < 2; ++bit) {
    std::vector<float> on, off;
    for (int arm = 0; arm < 4; ++arm) {
      std::vector<float>& to = (arm >> bit) & 1 ? on : off;
      to.insert(to.end(), g_arm_ms[arm].begin(), g_arm_ms[arm].end());
    }
    const Line a = Describe(on);
    const Line b = Describe(off);
    std::snprintf(part, sizeof(part), "; factor %d on %.1f, off %.1f FPS (%+.1f %%)", bit + 1, a.fps, b.fps,
                  b.fps > 0.0 ? 100.0 * (a.fps / b.fps - 1.0) : 0.0);
    line += part;
  }
#if defined(__SWITCH__)

  const struct mallinfo heap = mallinfo();
  std::snprintf(part, sizeof(part), "; heap %.1f MB in use of %.1f MB taken", double(heap.uordblks) / 1048576.0,
                double(heap.arena) / 1048576.0);
  line += part;
#endif

  std::snprintf(part, sizeof(part), "; parent script variables %d of 24576", bo2zm::cheats::ScriptParentsInUse());
  line += part;
  REXLOG_INFO("[benchmark] so far, {:.0f} s of {:.0f}: {}", seconds, Duration(), line);
}

void Summarize(double seconds) {
  std::vector<float> ms = g_ms;
  if (ms.empty()) {
    REXLOG_WARN("[benchmark] no frames measured in the window");
    return;
  }
  std::sort(ms.begin(), ms.end());
  const auto pct = [&](double p) { return ms[std::min(ms.size() - 1, size_t(p * double(ms.size())))]; };
  size_t over33 = 0, over50 = 0;
  for (float v : ms) {
    over33 += v > 33.4f;
    over50 += v > 50.0f;
  }
  const double n = double(ms.size());
  REXLOG_INFO("[benchmark] {} frames in {:.0f} s: {:.1f} FPS average, median {:.1f} ms ({:.1f} FPS), p90 {:.1f} ms, "
              "p99 {:.1f} ms, worst {:.1f} ms; over 33.3 ms {:.1f} %, over 50 ms {:.1f} %",
              ms.size(), seconds, n / seconds, pct(0.5), 1000.0 / pct(0.5), pct(0.9), pct(0.99), ms.back(),
              100.0 * double(over33) / n, 100.0 * double(over50) / n);
  if (AbSeconds() > 0) {
    SummarizeAb();
  }
}

}

bool IsActive() {
  static const bool active = REXCVAR_GET(bo2zm_benchmark);
  return active && !g_off.load(std::memory_order_relaxed);
}

void Disable() {
  g_off.store(true, std::memory_order_relaxed);
}

void AnnotatePlayer(bool present) {
  if (!present || !IsActive() || g_start.load(std::memory_order_relaxed) != 0) {
    return;
  }


  static const int round = REXCVAR_GET(bo2zm_benchmark_round);
  if (round > 1) {
    static int64_t asked = 0;
    const int64_t now = Now();
    if (asked == 0) {
      asked = now;
      bo2zm::cheats::GoToRound(round);
      REXLOG_INFO("[benchmark] player in a match: going to round {} first (bo2zm_benchmark_round)", round);
      return;
    }
    const int shown = bo2zm::cheats::CurrentRound();
    if (shown < round && now - asked < int64_t(120e9)) {


      static int64_t last = 0;
      if (shown <= 1 && now - std::max(asked, last) >= int64_t(10e9)) {
        last = now;
        bo2zm::cheats::GoToRound(round);
        REXLOG_INFO("[benchmark] still on round {}: asking for round {} again", shown, round);
      }
      return;
    }
    if (shown < round) {
      REXLOG_WARN("[benchmark] round {} not reached in 120 s (the game shows {}): the route starts anyway", round,
                  shown);
    } else {
      REXLOG_INFO("[benchmark] the game is on round {}", shown);
    }
  }
  g_start.store(Now() + int64_t(kWait * 1e9), std::memory_order_relaxed);
  REXLOG_INFO("[benchmark] player in a match: the {:.0f} s route starts in {:.0f} s", Duration(), kWait);
  if (ParsedSpot().valid) {
    bo2zm::cheats::SetStopSpawns(true);
    REXLOG_INFO("[spot] the route is a test spot: {} (no zombie spawns from here)", REXCVAR_GET(bo2zm_test_spot));
  }
  if (AbSeconds() > 0) {
    REXLOG_INFO("[benchmark] A/B in the route: the arm changes every {} s (factor 1 = bit 0, factor 2 = bit 1)",
                AbSeconds());
  }
}

void ApplyOwnPriority() {
#if defined(__SWITCH__)
  static const bool any = REXCVAR_GET(bo2zm_benchmark_priority_main_ab) != 0 ||
                          REXCVAR_GET(bo2zm_benchmark_priority_backend_ab) != 0;
  if (!any) [[likely]] {
    return;
  }
  thread_local int role = -1;
  thread_local int applied = kGuestPriority;
  thread_local uint32_t changes = 0;
  if (role < 0) {
    role = 0;
    if (rex::system::XThread* self = rex::system::XThread::GetCurrentThread(); self && self->thread()) {
      const std::string_view name = self->thread()->name();
      role = name.starts_with("Main XThread") ? 1 : name.starts_with("Backend") ? 2 : 0;
    }
  }
  if (role == 0) {
    return;
  }
  const int want = g_want_priority[role].load(std::memory_order_relaxed);
  if (want != applied) {
    const bool ok = RexSwitchSetCurrentThreadPriorityOk(want);
    applied = want;
    if (++changes <= 2 || !ok) {
      REXLOG_INFO("[benchmark] the {} thread set its own priority to {:#x}{}", role == 1 ? "Main" : "Backend", want,
                  ok ? "" : " (REFUSED by the kernel)");
    }
  }
#endif
}

std::atomic<int> g_arm_cached{-1};
int AbArmCached() {
  return g_arm_cached.load(std::memory_order_relaxed);
}

int AbArm() {
  const int64_t start = g_start.load(std::memory_order_relaxed);
  if (start == 0 || AbSeconds() <= 0) {
    return -1;
  }
  return ArmAt(double(Now() - start) / 1e9, nullptr);
}

bool PathTilts(int& move_percent, int& look_percent) {
  if (PathButtons() == 0) {
    return false;
  }
  move_percent = kMoveTilt;
  look_percent = kLookTilt;
  return true;
}

uint32_t PathButtons() {
  const int64_t start = g_start.load(std::memory_order_relaxed);
  if (start == 0 || ParsedSpot().valid) {
    return 0;
  }
  const double t = double(Now() - start) / 1e9;
  if (t < 0.0 || t >= Duration()) {
    return 0;
  }
  const double minute = std::fmod(t, 60.0);
  for (const Span& span : kPath) {
    if (minute >= span.since && minute < span.until) {
      return span.buttons;
    }
  }
  return 0;
}

uint64_t FramesSeen() {
  return g_frames_seen.load(std::memory_order_relaxed);
}

void AnnotateFrame() {
  g_frames_seen.fetch_add(1, std::memory_order_relaxed);
  const int64_t start = g_start.load(std::memory_order_relaxed);
  if (start == 0) {
    return;
  }
  const auto now = Clock::now();
  const double t = double(Now() - start) / 1e9;
  if (static const int stall_at = REXCVAR_GET(bo2zm_test_stall_at_s); stall_at > 0 && t >= stall_at) {
    static std::atomic<bool> stalled{false};
    if (!stalled.exchange(true)) {
      REXLOG_WARN("[benchmark] bo2zm_test_stall_at_s: the renderer thread stops for 90 s (a watchdog test)");
      std::this_thread::sleep_for(std::chrono::seconds(90));
      REXLOG_WARN("[benchmark] bo2zm_test_stall_at_s: going on (the watchdog did not restart the game)");
    }
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (ParsedSpot().valid) {
    StepSpot(t);
  }
#if defined(__SWITCH__)
  ApplyAbNet(ArmAt(t, nullptr));
#endif
  ApplyAbXmaCache(t >= 0.0 && t < Duration() ? ArmAt(t, nullptr) : -1);
  ApplyAbIpak(t >= 0.0 && t < Duration() ? ArmAt(t, nullptr) : -1);
  ApplyAbPriorities(t >= 0.0 && t < Duration() ? ArmAt(t, nullptr) : -1);
  g_arm_cached.store(AbSeconds() > 0 && t >= 0.0 && t < Duration() ? ArmAt(t, nullptr) : -1, std::memory_order_relaxed);
#if defined(__SWITCH__)
  if (AbSeconds() > 0 && !g_summarized) {
    static int cpu_arm = -1;
    const int arm = t >= 0.0 && t < Duration() ? ArmAt(t, nullptr) : -1;
    if (arm != cpu_arm) {
      SampleArmCpu(cpu_arm);
      cpu_arm = arm;
    }
  }
#endif
  if (g_summarized || t < 0.0) {
    g_earlier = now;
    return;
  }
  if (t >= Duration()) {
    g_summarized = true;
    Summarize(Duration());
#if defined(BO2ZM_PGO_GENERATE)
    mkdir("/switch/bo2zm/pgo", 0777);
    __gcov_dump();
    REXLOG_INFO("[pgo] profile of the route written to sdmc:/switch/bo2zm/pgo: copy that folder to the PC");
#endif
    lifecycle::OnBenchmarkDone();
    return;
  }
#if defined(BO2ZM_PGO_GENERATE)
  static bool reset = false;
  if (!reset) {
    reset = true;
    __gcov_reset();
    REXLOG_INFO("[pgo] counters reset at the start of the route");
  }
#endif

  static const int next_round_s = REXCVAR_GET(bo2zm_benchmark_next_round_s);
  static double next_round_at = double(next_round_s);
  if (next_round_s > 0 && t >= next_round_at) {
    next_round_at += next_round_s;
    const int round = bo2zm::cheats::CurrentRound() + 1;
    bo2zm::cheats::GoToRound(round);
    REXLOG_INFO("[benchmark] {:.0f} s: going to round {} (bo2zm_benchmark_next_round_s)", t, round);
  }
  if (t >= g_next_partial) {
    g_next_partial += 30.0;
    SummarizeSoFar(t);
  }
  if (g_earlier.time_since_epoch().count() != 0) {
    const float ms = float(std::chrono::duration<double, std::milli>(now - g_earlier).count());
    g_ms.push_back(ms);
    double into = 0.0;
    const int arm = ArmAt(t, &into);
    if (arm >= 0 && into >= kSettle) {
      g_arm_ms[arm].push_back(ms);
    }
  }
  g_earlier = now;
}

}
