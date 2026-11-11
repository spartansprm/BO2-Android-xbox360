#include "bo2zm_debug_watch.h"

#include <rex/cvar.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(bo2zm_debug_watch_volume, false, "BO2",
                    "Diagnostics (PC): log the code that writes the model lighting volume's pages (the first 3D "
                    "texture updated by pages). Off for play")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_debug_watch_vertices, false, "BO2",
                    "Diagnostics (PC): log the code that writes the memory the draws read vertices from (the pages "
                    "the renderer's vertex copies read during frames 3000-3300, watched from there). Off for play")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_debug_watch_s, 60, "BO2", "Diagnostics (PC): how long bo2zm_debug_watch_* watches")
    .range(1, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REX_PLATFORM_WIN32

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace bo2zm::debug {
namespace {

constexpr int kFrames = 6;
constexpr int kRearmMs = 500;

struct Source {
  std::array<uint64_t, kFrames> rva{};
  uint64_t count = 0;
  uint32_t page = 0;
};

std::mutex g_mutex;
std::unordered_map<std::string, Source> g_sources;
std::vector<uint8_t*> g_host_pages;
std::unordered_map<uintptr_t, uint32_t> g_page_of;
std::atomic<bool> g_active{false};
uintptr_t g_image = 0;
std::unordered_set<uint32_t> g_written;
std::unordered_map<uint32_t, uint64_t> g_bytes_of;
std::thread g_thread;

struct Joiner {
  ~Joiner() {
    g_active.store(false);
    if (g_thread.joinable()) {
      g_thread.join();
    }
  }
} g_joiner;

void Protect(DWORD protection) {
  for (uint8_t* page : g_host_pages) {
    DWORD old = 0;
    VirtualProtect(page, 4096, protection, &old);
  }
}


std::array<uint64_t, kFrames> Unwind(const CONTEXT* fault) {
  std::array<uint64_t, kFrames> rva{};
  CONTEXT c = *fault;
  for (int i = 0; i < kFrames; ++i) {
    const uint64_t pc = c.Rip;
    if (pc == 0) {
      break;
    }
    rva[i] = pc >= g_image ? pc - g_image : pc;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(pc, &image_base, nullptr);
    if (!entry) {

      c.Rip = *reinterpret_cast<const uint64_t*>(c.Rsp);
      c.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, entry, &c, &handler_data, &establisher, nullptr);
  }
  return rva;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (!g_active.load(std::memory_order_relaxed) ||
      info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      info->ExceptionRecord->NumberParameters < 2 || info->ExceptionRecord->ExceptionInformation[0] != 1) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t address = uintptr_t(info->ExceptionRecord->ExceptionInformation[1]);
  const uintptr_t page = address & ~uintptr_t(4095);
  const auto it = g_page_of.find(page);
  if (it == g_page_of.end()) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
  const auto rva = Unwind(info->ContextRecord);
  std::string key;
  for (const uint64_t r : rva) {
    key += fmt::format("{:x},", r);
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Source& s = g_sources[key];
    if (s.count++ == 0) {
      s.rva = rva;
      s.page = it->second;
    }
    g_written.insert(it->second);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void Report(double seconds) {
  std::vector<Source> list;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [key, s] : g_sources) {
      list.push_back(s);
    }
    if (!g_bytes_of.empty()) {

      std::map<uint32_t, std::pair<uint64_t, uint64_t>> regions;
      uint64_t written = 0, unwritten = 0;
      for (const auto& [page, bytes] : g_bytes_of) {
        auto& r = regions[page >> 24];
        if (g_written.count(page)) {
          written += bytes;
          r.first += bytes;
        } else {
          unwritten += bytes;
          r.second += bytes;
        }
      }
      REXLOG_INFO("[watch] {} of {} watched pages written so far; bytes read from them: {:.1f} MB from written pages, "
                  "{:.1f} MB from pages never written ({:.0f} %)",
                  g_written.size(), g_bytes_of.size(), double(written) / 1e6, double(unwritten) / 1e6,
                  100.0 * double(unwritten) / double(std::max<uint64_t>(1, written + unwritten)));
      for (const auto& [region, r] : regions) {
        REXLOG_INFO("[watch]   physical {:08X}-{:08X}: {:.2f} MB from written pages, {:.2f} MB from never written",
                    region << 24, ((region + 1) << 24) - 1, double(r.first) / 1e6, double(r.second) / 1e6);
      }
    }
  }
  std::sort(list.begin(), list.end(), [](const Source& a, const Source& b) { return a.count > b.count; });
  REXLOG_INFO("[watch] after {:.0f} s: {} different writers of the watched pages (image base {:x})", seconds,
              list.size(), g_image);
  for (size_t i = 0; i < list.size() && i < 20; ++i) {
    const Source& s = list[i];
    std::string frames;
    for (const uint64_t r : s.rva) {
      if (r) {
        frames += fmt::format("{}{:x}", frames.empty() ? "" : " <- ", r);
      }
    }
    REXLOG_INFO("[watch]   {} writes (first to physical {:08X}): rva {}", s.count, s.page, frames);
  }
}

void Loop() {
  const auto start = std::chrono::steady_clock::now();
  const auto stop = start + std::chrono::seconds(REXCVAR_GET(bo2zm_debug_watch_s));
  auto next_report = start + std::chrono::seconds(5);
  while (g_active.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kRearmMs));
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_report || now >= stop) {
      next_report = now + std::chrono::seconds(5);
      Report(std::chrono::duration<double>(now - start).count());
    }
    if (now >= stop) {
      break;
    }
    Protect(PAGE_READONLY);
  }
  Protect(PAGE_READWRITE);
  g_active.store(false);
  REXLOG_INFO("[watch] done: the pages are writable again");
}

}

bool WatchVertices() {
  static const bool on = REXCVAR_GET(bo2zm_debug_watch_vertices);
  return on;
}

void WatchPhysicalPages(const std::vector<uint32_t>& pages, Watch what, const std::vector<uint64_t>* bytes) {
  static std::atomic<bool> armed{false};
  const bool on = what == Watch::kVertices ? REXCVAR_GET(bo2zm_debug_watch_vertices)
                                           : REXCVAR_GET(bo2zm_debug_watch_volume);
  if (!on || pages.empty() || armed.exchange(true)) {
    return;
  }
  auto* memory = rex::Runtime::instance() ? rex::Runtime::instance()->memory() : nullptr;
  if (!memory) {
    return;
  }
  g_image = uintptr_t(GetModuleHandleW(nullptr));
  if (bytes && bytes->size() == pages.size()) {
    for (size_t i = 0; i < pages.size(); ++i) {
      g_bytes_of[pages[i] & 0x1FFFF000u] += (*bytes)[i];
    }
  }
  uint8_t* const base = memory->virtual_membase();
  for (const uint32_t physical : pages) {
    const uint32_t p = physical & 0x1FFFF000u;
    uint8_t* views[3] = {base + 0xA0000000ull + p, base + 0xC0000000ull + p,
                         p >= 0x1000 ? base + 0xE0000000ull + p - 0x1000 : nullptr};
    for (uint8_t* host : views) {
      if (!host) {
        continue;
      }
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(host, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        continue;
      }
      g_host_pages.push_back(host);
      g_page_of[uintptr_t(host)] = p;
    }
  }
  AddVectoredExceptionHandler(1, Handler);
  g_active.store(true);
  REXLOG_INFO("[watch] watching {} physical pages ({} host pages in the guest views) for {} s", pages.size(),
              g_host_pages.size(), REXCVAR_GET(bo2zm_debug_watch_s));
  Protect(PAGE_READONLY);
  g_thread = std::thread(Loop);
}

}

#else

namespace bo2zm::debug {
void WatchPhysicalPages(const std::vector<uint32_t>&, Watch, const std::vector<uint64_t>*) {}
bool WatchVertices() { return false; }
}

#endif
