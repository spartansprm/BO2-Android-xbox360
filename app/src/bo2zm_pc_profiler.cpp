#include <rex/platform.h>

#if REX_PLATFORM_WIN32

#include <windows.h>

#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bo2zm_pc_profile, false, "BO2",
                    "Test helper (PC): sample the busy threads' stacks every 2 ms and write pc_profile_<pid>.txt next "
                    "to the executable (tools/pc_profile_report.py reads it)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_pc_profile_from_s, 40, "BO2", "Test helper (PC): seconds before sampling starts")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_pc_profile_for_s, 180, "BO2", "Test helper (PC): seconds of sampling")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_pc_profile_stalls_ms, 0, "BO2",
                     "Test helper (PC), with bo2zm_pc_profile: also samples the game's main threads (Main, Backend, "
                     "Stream, the job workers, Server, the ring) every 2 ms even while they are blocked, and keeps "
                     "the samples taken while no frame has been presented for this many ms in "
                     "pc_stalls_<pid>.txt (same format; where each thread was during the freezes). 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr int kFrames = 24;
constexpr auto kInterval = std::chrono::milliseconds(2);
constexpr double kBusyShare = 0.02;

struct Module {
  uint64_t base = 0, end = 0;
  std::string name;
};

struct Stack {
  uint64_t frames[kFrames] = {};
  int n = 0;
  bool operator==(const Stack& o) const {
    return n == o.n && std::equal(frames, frames + n, o.frames);
  }
};
struct StackHash {
  size_t operator()(const Stack& s) const {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < s.n; ++i) {
      h = (h ^ s.frames[i]) * 1099511628211ull;
    }
    return size_t(h);
  }
};

struct ThreadInfo {
  DWORD tid = 0;
  HANDLE handle = nullptr;
  std::string name;
  uint64_t last_cpu_100ns = 0;
  double share = 0.0;
  uint64_t cpu_window_100ns = 0;
  uint64_t samples = 0;
  std::unordered_map<Stack, uint32_t, StackHash> stacks;
};

std::vector<Module> g_modules;


std::atomic<int64_t> g_last_frame_ns{0};

bool Watched(const std::string& name) {
  for (const char* prefix : {"Main XThread", "Backend", "Stream", "JQ Worker", "Server", "GPU native ring"}) {
    if (name.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

void LoadModules() {
  g_modules.clear();
  HMODULE handles[1024];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
    return;
  }
  for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
    MODULEINFO info{};
    char path[MAX_PATH] = {};
    if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)) ||
        !GetModuleBaseNameA(GetCurrentProcess(), handles[i], path, sizeof(path))) {
      continue;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
    g_modules.push_back({base, base + info.SizeOfImage, path});
  }
  std::sort(g_modules.begin(), g_modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
}

std::string Name(uint64_t address) {
  auto it = std::upper_bound(g_modules.begin(), g_modules.end(), address,
                             [](uint64_t a, const Module& m) { return a < m.base; });
  if (it != g_modules.begin()) {
    --it;
    if (address < it->end) {
      char text[64];
      std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(address - it->base));
      return it->name + text;
    }
  }
  char text[32];
  std::snprintf(text, sizeof(text), "?+0x%llx", static_cast<unsigned long long>(address));
  return text;
}

uint64_t CpuOf(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
    return 0;
  }
  const auto value = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return value(kernel) + value(user);
}

std::string ThreadName(HANDLE thread) {
  PWSTR text = nullptr;
  std::string name;
  if (SUCCEEDED(GetThreadDescription(thread, &text)) && text) {
    char narrow[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    name = narrow;
    LocalFree(text);
  }
  return name.empty() ? "(no name)" : name;
}


void Sample(ThreadInfo& t) {
  if (SuspendThread(t.handle) == DWORD(-1)) {
    return;
  }
  CONTEXT context{};
  context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  Stack stack;
  if (GetThreadContext(t.handle, &context)) {
    for (int i = 0; i < kFrames; ++i) {
      if (context.Rip == 0) {
        break;
      }
      stack.frames[stack.n++] = context.Rip;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!function) {
        DWORD64 ret = 0;
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp), &ret, sizeof(ret),
                               &read) ||
            read != sizeof(ret)) {
          break;
        }
        context.Rip = ret;
        context.Rsp += 8;
        continue;
      }
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                       nullptr);
    }
  }
  ResumeThread(t.handle);
  if (stack.n > 0) {
    ++t.stacks[stack];
    ++t.samples;
  }
}

void Write(const std::map<DWORD, ThreadInfo>& threads, double seconds, const char* prefix = "pc_profile_") {
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  const auto path = std::filesystem::path(exe).parent_path() /
                    (prefix + std::to_string(GetCurrentProcessId()) + ".txt");
  FILE* f = std::fopen(path.string().c_str(), "w");
  if (!f) {
    return;
  }
  std::fprintf(f, "seconds %.1f\n", seconds);
  for (const auto& [tid, t] : threads) {
    if (t.samples == 0) {
      continue;
    }
    std::fprintf(f, "thread %s | tid %lu | cpu_ms %.1f | samples %llu\n", t.name.c_str(), tid,
                 double(t.cpu_window_100ns) / 1e4, static_cast<unsigned long long>(t.samples));
    std::vector<std::pair<uint32_t, const Stack*>> order;
    for (const auto& [stack, count] : t.stacks) {
      order.emplace_back(count, &stack);
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [count, stack] : order) {
      std::fprintf(f, "%u", count);
      for (int i = 0; i < stack->n; ++i) {
        std::fprintf(f, " %s", Name(stack->frames[i]).c_str());
      }
      std::fprintf(f, "\n");
    }
  }
  std::fclose(f);
}

void Loop() {
  using Clock = std::chrono::steady_clock;
  std::this_thread::sleep_for(std::chrono::seconds(std::max(0, REXCVAR_GET(bo2zm_pc_profile_from_s))));
  LoadModules();
  const DWORD self = GetCurrentThreadId();
  std::map<DWORD, ThreadInfo> threads;

  const int64_t stall_ns = int64_t(std::max(0, REXCVAR_GET(bo2zm_pc_profile_stalls_ms))) * 1000000;
  std::map<DWORD, ThreadInfo> stalls;
  uint64_t stalls_seen = 0;
  bool in_stall = false;
  const auto start = Clock::now();
  const auto stop = start + std::chrono::seconds(std::max(1, REXCVAR_GET(bo2zm_pc_profile_for_s)));
  auto next_refresh = start;
  auto next_write = start + std::chrono::seconds(10);
  REXLOG_INFO("[pc_profile] sampling the busy threads every 2 ms for {} s", REXCVAR_GET(bo2zm_pc_profile_for_s));
  while (Clock::now() < stop) {
    const auto now = Clock::now();
    if (now >= next_refresh) {
      next_refresh = now + std::chrono::seconds(1);
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 entry{sizeof(entry)};
      if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
        do {
          if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self ||
              threads.count(entry.th32ThreadID)) {
            continue;
          }
          HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                entry.th32ThreadID);
          if (!h) {
            continue;
          }
          ThreadInfo& t = threads[entry.th32ThreadID];
          t.tid = entry.th32ThreadID;
          t.handle = h;
          t.name = ThreadName(h);
          t.last_cpu_100ns = CpuOf(h);
        } while (Thread32Next(snapshot, &entry));
      }
      if (snapshot != INVALID_HANDLE_VALUE) {
        CloseHandle(snapshot);
      }
      for (auto& [tid, t] : threads) {
        const uint64_t cpu = CpuOf(t.handle);
        const uint64_t delta = cpu >= t.last_cpu_100ns ? cpu - t.last_cpu_100ns : 0;
        t.last_cpu_100ns = cpu;
        t.cpu_window_100ns += delta;
        t.share = double(delta) / 1e7;
        if (t.name == "(no name)") {
          t.name = ThreadName(t.handle);
        }
      }
    }
    const int64_t last_frame = g_last_frame_ns.load(std::memory_order_relaxed);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    const bool stalled = stall_ns > 0 && last_frame != 0 && now_ns - last_frame > stall_ns;
    if (stalled && !in_stall) {
      ++stalls_seen;
    }
    in_stall = stalled;
    for (auto& [tid, t] : threads) {
      if (stalled && Watched(t.name)) {
        ThreadInfo& s = stalls[tid];
        if (!s.handle) {
          s.tid = tid;
          s.handle = t.handle;
        }
        s.name = t.name;
        Sample(s);
        s.cpu_window_100ns = s.samples * 20000;
      }
      if (t.share > kBusyShare) {
        Sample(t);
      }
    }
    if (Clock::now() >= next_write) {
      next_write += std::chrono::seconds(10);
      Write(threads, std::chrono::duration<double>(Clock::now() - start).count());
      if (stall_ns > 0) {
        Write(stalls, double(stalls_seen), "pc_stalls_");
      }
    }
    std::this_thread::sleep_for(kInterval);
  }
  Write(threads, std::chrono::duration<double>(Clock::now() - start).count());
  if (stall_ns > 0) {
    Write(stalls, double(stalls_seen), "pc_stalls_");
    REXLOG_INFO("[pc_profile] {} freezes of more than {} ms sampled", stalls_seen, stall_ns / 1000000);
  }
  REXLOG_INFO("[pc_profile] done");
  for (auto& [tid, t] : threads) {
    CloseHandle(t.handle);
  }
}

}

namespace bo2zm::pc_profiler {
void Start() {
  if (REXCVAR_GET(bo2zm_pc_profile)) {
    std::thread(Loop).detach();
  }
}
void NoteFrame() {
  g_last_frame_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
}
}

#else

namespace bo2zm::pc_profiler {
void Start() {}
void NoteFrame() {}
}

#endif
