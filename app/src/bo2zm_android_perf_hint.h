// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace bo2zm::android_perf {

struct Snapshot {
  bool available = false;
  bool enabled = true;
  uint32_t threads = 0;
  uint64_t reports = 0;
  uint64_t report_failures = 0;
  double target_ms = 0;
  double last_actual_ms = 0;
  double thermal_headroom = std::nan("");
  const char* state = "not_started";
};

#if defined(__ANDROID__)

namespace detail {
using ManagerGet = void* (*)();
using SessionCreate = void* (*)(void*, const int32_t*, size_t, int64_t);
using SessionReport = int (*)(void*, int64_t);
using SessionUpdateTarget = int (*)(void*, int64_t);
using SessionClose = void (*)(void*);
using ThermalAcquire = void* (*)();
using ThermalHeadroom = float (*)(void*, int);

struct State {
  std::mutex mutex;
  bool resolved = false;
  bool enabled = true;
  ManagerGet get_manager = nullptr;
  SessionCreate create = nullptr;
  SessionReport report = nullptr;
  SessionUpdateTarget update_target = nullptr;
  SessionClose close = nullptr;
  ThermalAcquire thermal_acquire = nullptr;
  ThermalHeadroom thermal_headroom = nullptr;
  void* thermal_manager = nullptr;
  void* session = nullptr;
  std::vector<int32_t> tids;
  int64_t target_ns = 22'000'000;
  std::atomic<uint64_t> reports{0};
  std::atomic<uint64_t> failures{0};
  std::atomic<int64_t> last_actual_ns{0};

  std::chrono::steady_clock::time_point last_swap{};
  double headroom = std::nan("");
  std::chrono::steady_clock::time_point last_headroom{};
  const char* state = "not_started";
};

inline State& S() {
  static State state;
  return state;
}


inline void ResolveLocked(State& s) {
  if (s.resolved) {
    return;
  }
  s.resolved = true;
  if (const char* off = std::getenv("BO2ZM_NO_ADPF"); off && off[0] == '1') {
    s.enabled = false;
  }
  void* lib = dlopen("libandroid.so", RTLD_NOW);
  if (!lib) {
    s.state = "libandroid_unavailable";
    return;
  }
  s.get_manager = reinterpret_cast<ManagerGet>(dlsym(lib, "APerformanceHint_getManager"));
  s.create = reinterpret_cast<SessionCreate>(dlsym(lib, "APerformanceHint_createSession"));
  s.report = reinterpret_cast<SessionReport>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
  s.update_target = reinterpret_cast<SessionUpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
  s.close = reinterpret_cast<SessionClose>(dlsym(lib, "APerformanceHint_closeSession"));
  s.thermal_acquire = reinterpret_cast<ThermalAcquire>(dlsym(lib, "AThermal_acquireManager"));
  s.thermal_headroom = reinterpret_cast<ThermalHeadroom>(dlsym(lib, "AThermal_getThermalHeadroom"));
  if (!s.get_manager || !s.create || !s.report || !s.close) {
    s.state = "api_33_not_available";
    return;
  }
  s.state = "resolved";
}


inline void RebuildSessionLocked(State& s) {
  if (!s.enabled || s.get_manager == nullptr || s.create == nullptr || s.tids.empty()) {
    return;
  }
  void* manager = s.get_manager();
  if (!manager) {
    s.state = "no_manager";
    return;
  }
  void* fresh = s.create(manager, s.tids.data(), s.tids.size(), s.target_ns);
  if (!fresh) {
    s.state = "create_session_failed";
    return;
  }
  if (s.session) {
    s.close(s.session);
  }
  s.session = fresh;
  s.state = "active";
}
}

inline void SetEnabled(bool enabled) {
  auto& s = detail::S();
  std::lock_guard lock(s.mutex);
  s.enabled = enabled;
  if (!enabled && s.session) {
    s.close(s.session);
    s.session = nullptr;
    s.state = "disabled";
  }
}

inline void SetTargetMs(double ms) {
  auto& s = detail::S();
  std::lock_guard lock(s.mutex);
  if (ms >= 5.0 && ms <= 100.0) {
    s.target_ns = int64_t(ms * 1e6);
    if (s.session && s.update_target) {
      s.update_target(s.session, s.target_ns);
    }
  }
}


inline void RegisterCurrentThread() {
  auto& s = detail::S();
  std::lock_guard lock(s.mutex);
  detail::ResolveLocked(s);
  if (!s.enabled) {
    return;
  }
  const int32_t tid = int32_t(gettid());
  for (int32_t known : s.tids) {
    if (known == tid) {
      return;
    }
  }
  s.tids.push_back(tid);
  detail::RebuildSessionLocked(s);
}


inline void OnGuestSwap() {
  auto& s = detail::S();
  thread_local bool registered = false;
  if (!registered) {
    registered = true;
    RegisterCurrentThread();
  }
  const auto now = std::chrono::steady_clock::now();
  const bool first = s.last_swap.time_since_epoch().count() == 0;
  const int64_t actual =
      first ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.last_swap).count();
  s.last_swap = now;
  if (first || actual <= 0 || actual > 1'000'000'000) {
    return;
  }
  void* session;
  auto report = s.report;
  {
    std::lock_guard lock(s.mutex);
    session = s.session;
    report = s.report;
  }
  if (!session || !report) {
    return;
  }
  s.last_actual_ns.store(actual, std::memory_order_relaxed);
  if (report(session, actual) == 0) {
    s.reports.fetch_add(1, std::memory_order_relaxed);
  } else {
    s.failures.fetch_add(1, std::memory_order_relaxed);
  }

  if (now - s.last_headroom > std::chrono::seconds(5)) {
    s.last_headroom = now;
    std::lock_guard lock(s.mutex);
    if (s.thermal_acquire && s.thermal_headroom) {
      if (!s.thermal_manager) {
        s.thermal_manager = s.thermal_acquire();
      }
      if (s.thermal_manager) {
        s.headroom = double(s.thermal_headroom(s.thermal_manager, 10));
      }
    }
  }
}

inline Snapshot Read() {
  auto& s = detail::S();
  std::lock_guard lock(s.mutex);
  Snapshot r;
  r.available = s.session != nullptr;
  r.enabled = s.enabled;
  r.threads = uint32_t(s.tids.size());
  r.reports = s.reports.load(std::memory_order_relaxed);
  r.report_failures = s.failures.load(std::memory_order_relaxed);
  r.target_ms = double(s.target_ns) / 1e6;
  r.last_actual_ms = double(s.last_actual_ns.load(std::memory_order_relaxed)) / 1e6;
  r.thermal_headroom = s.headroom;
  r.state = s.state;
  return r;
}

#else

inline void SetEnabled(bool) {}
inline void SetTargetMs(double) {}
inline void RegisterCurrentThread() {}
inline void OnGuestSwap() {}
inline Snapshot Read() {
  Snapshot r;
  r.state = "not_android";
  return r;
}

#endif


inline std::string Json() {
  const Snapshot s = Read();
  std::string out = "{\"state\":\"";
  out += s.state;
  out += "\",\"session_active\":";
  out += s.available ? "true" : "false";
  out += ",\"enabled\":";
  out += s.enabled ? "true" : "false";
  out += ",\"threads\":" + std::to_string(s.threads);
  out += ",\"reports\":" + std::to_string(s.reports);
  out += ",\"report_failures\":" + std::to_string(s.report_failures);
  out += ",\"target_ms\":" + std::to_string(s.target_ms);
  out += ",\"last_actual_ms\":" + std::to_string(s.last_actual_ms);
  out += ",\"thermal_headroom\":";
  out += std::isnan(s.thermal_headroom) ? std::string("null") : std::to_string(s.thermal_headroom);
  out += "}";
  return out;
}

}
