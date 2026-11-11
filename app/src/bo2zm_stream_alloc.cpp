#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(bo2zm_native_stream_alloc, true, "BO2",
                    "The streaming pool's allocator search (sub_828F1C10) in native code, checked against the game's "
                    "own first (bo2zm_stream_alloc.cpp); false = the game's code only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828F1C10);
REX_EXTERN(__imp__sub_828EF698);
REX_EXTERN(__imp__sub_828EF578);
REX_EXTERN(__imp__sub_828EF108);
REX_EXTERN(__imp__sub_828F1A88);
REX_EXTERN(__imp__sub_828F16F8);
REX_EXTERN(__imp__sub_828EF958);
REX_EXTERN(__imp__sub_828F0F98);
REX_EXTERN(__imp__sub_828F1170);

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kPool = 0x84BA9F00;
constexpr uint32_t kCountA = kPool + 0x25900;
constexpr uint32_t kCountB = kPool + 0x25904;
constexpr uint32_t kTableB = kPool + 0x19F00;
constexpr uint32_t kStateA = kPool + 0x25B28;
constexpr uint32_t kStateB = kPool + 0x25B2C;
constexpr uint32_t kStateC = kPool + 0x25B30;
constexpr uint32_t kEntryA = 20;
constexpr uint32_t kEntryB = 8;
constexpr uint32_t kFrame = 0x110;
constexpr uint32_t kReturnFromJob = 0x828F2440;
constexpr uint32_t kInitScan = 0x828F1CA8;


constexpr uint32_t kLrState1170 = 0x828F1C74;
constexpr uint32_t kLrState0F98 = 0x828F1C84;
constexpr uint32_t kLrLater1 = 0x828F1C88;
constexpr uint32_t kLrFallback = 0x828F1E8C;
constexpr uint32_t kLrLater2 = 0x828F1E98;
constexpr uint32_t kLrWindow = 0x828F1EC8;
constexpr uint32_t kLrPlace = 0x828F1EE0;


constexpr uint32_t kLock6Owner = 0x83DE0218 + 6 * 28 + 0x18;

constexpr uint64_t kChecked = 20000;
constexpr uint64_t kCheckEvery = 1024;

bool g_on = false;
std::atomic<bool> g_off{false};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_checks{0};
std::atomic<uint64_t> g_mismatches{0};


std::atomic<uint64_t> g_searches{0};
std::atomic<uint64_t> g_searches_job{0};
std::atomic<uint64_t> g_searches_unlocked{0};
std::atomic<uint64_t> g_native{0};
std::atomic<uint64_t> g_ns{0};
std::atomic<uint64_t> g_steps{0};
std::atomic<uint64_t> g_ns_scan{0};
std::atomic<uint64_t> g_ns_fallback{0};
std::atomic<uint64_t> g_ns_states{0};
std::atomic<uint64_t> g_ns_later{0};

std::atomic<uint64_t> g_remembered{0};
std::atomic<uint64_t> g_remember_checks{0};
std::atomic<bool> g_remember_off{false};
std::atomic<uint64_t> g_states_0f98{0};
std::atomic<uint64_t> g_states_1170{0};
std::array<std::atomic<uint64_t>, 8> g_results{};
std::atomic<int64_t> g_last_report_ns{0};

inline uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
inline uint8_t Read8(const uint8_t* base, uint32_t address) { return base[address]; }
inline void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}


struct It {
  uint32_t state = 0;
  uint32_t cur = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t head_a = 0;
  uint32_t head_b = 0;
};

void Init(const uint8_t* base, It& it) {
  it = It{};
  it.state = 3;
  const int32_t count_a = int32_t(Read32(base, kCountA));
  if (count_a > 0) {
    it.a = kPool;
    it.head_a = Read32(base, kPool);
    it.cur = Read32(base, kPool);
    if (Read8(base, kPool + 16) != 0) {
      it.a = kPool + kEntryA;
      if (it.a == kPool + uint32_t(count_a) * kEntryA) {
        it.a = 0;
      }
    } else {
      it.state = 2;
    }
  }
  if (int32_t(Read32(base, kCountB)) > 0) {
    it.b = kTableB;
    it.head_b = Read32(base, kTableB);
    const uint32_t start = Read32(base, kTableB);
    if (start < it.cur || it.a == 0) {
      it.cur = start;
      it.state = 1;
    }
  }
}

inline uint32_t EndA(const uint8_t* base) { return kPool + Read32(base, kCountA) * kEntryA; }
inline uint32_t EndB(const uint8_t* base) { return kTableB + Read32(base, kCountB) * kEntryB; }


inline uint32_t AfterRun(const uint8_t* base, It& it, uint32_t end) {
  if (it.b != 0 && Read32(base, it.b) == end) {
    it.state = 1;
    return 1;
  }
  it.state = 3;
  return 3;
}


inline uint32_t SkipFlagged(const uint8_t* base, It& it, uint32_t entry) {
  it.a = entry + kEntryA;
  if (it.a == EndA(base)) {
    it.a = 0;
  }
  it.state = 3;
  return 3;
}

uint32_t Step(const uint8_t* base, It& it, uint32_t mode) {
  if (mode == 0 || mode > 3) {
    return 0;
  }
  if (mode == 2) {
    const uint32_t entry = it.a;
    const uint32_t next = entry + kEntryA;
    const uint32_t end = Read32(base, entry + 4);
    it.cur = end;
    if (next == EndA(base)) {
      it.a = 0;
      return AfterRun(base, it, end);
    }
    it.a = next;
    if (Read32(base, next) != end) {
      return AfterRun(base, it, end);
    }
    if (Read8(base, next + 16) == 0) {
      it.state = 2;
      return 2;
    }
    return SkipFlagged(base, it, next);
  }
  if (mode == 1) {
    const uint32_t entry = it.b;
    const uint32_t next = entry + kEntryB;
    const uint32_t end = Read32(base, entry + 4);
    it.cur = end;
    if (next == EndB(base)) {
      it.b = 0;
      if (it.a == 0 || Read32(base, it.a) != end) {
        it.state = 3;
        return 3;
      }
      if (Read8(base, it.a + 16) == 0) {
        it.state = 2;
        return 2;
      }
      return SkipFlagged(base, it, it.a);
    }
    it.b = next;
    if (it.a == 0) {
      return AfterRun(base, it, end);
    }
    if (Read32(base, it.a) != end) {
      return AfterRun(base, it, end);
    }
    if (Read8(base, it.a + 16) == 0) {
      it.state = 2;
      return 2;
    }
    return SkipFlagged(base, it, it.a);
  }

  if (it.b != 0 && it.a != 0) {
    if (Read32(base, it.b) >= Read32(base, it.a)) {
      it.cur = Read32(base, it.a);
      it.state = 2;
      return 2;
    }
    it.cur = Read32(base, it.b);
    it.state = 1;
    return 1;
  }
  if (it.a != 0) {
    it.cur = Read32(base, it.a);
    it.state = 2;
    return 2;
  }
  if (it.b != 0) {
    it.cur = Read32(base, it.b);
    it.state = 1;
    return 1;
  }
  it.state = 0;
  return 0;
}


struct Outcome {
  enum Kind : uint8_t { kPlace, kWindow, kFallback } kind = kFallback;
  uint32_t a = 0;
  uint32_t b = 0;
};

Outcome Scan(const uint8_t* base, uint32_t size_raw, uint64_t& steps) {
  const int32_t size = int32_t(size_raw);
  It it1;
  It it2;
  Init(base, it1);
  Init(base, it2);
  uint32_t r3 = it1.state;
  uint32_t r21 = 0, r22 = 0;
  int32_t r23 = 0x7FFFFFFF, r25 = 0x7FFFFFFF;
  for (;;) {

    while (r3 != 1) {
      if (r3 == 0) {
        Outcome o;
        if (r22 != 0) {
          o.kind = Outcome::kWindow;
          o.a = r22;
          o.b = r21;
        }
        return o;
      }
      r3 = Step(base, it1, r3);
      ++steps;
    }
    const uint32_t block = it1.b;
    const uint32_t start = it1.cur;
    const uint32_t free_size = Read32(base, block + 4) - Read32(base, block);
    uint32_t r30 = free_size + start;
    const uint32_t r26 = (start + 0xFFF) & 0xFFFFF000u;
    uint32_t r29 = r30 - r26;
    if (int32_t(r29) >= size) {
      Outcome o;
      o.kind = Outcome::kPlace;
      o.a = r26;
      return o;
    }
    uint32_t r27 = 0;
    int32_t r31 = 0;
    int32_t r28 = 0;
    it2 = it1;
    uint32_t s = Step(base, it2, it2.state);
    ++steps;
    bool next = false;
    for (;;) {
      if (s == 0) {
        next = true;
        break;
      }
      if (s == 2) {
        const uint32_t entry = it2.a;
        const int32_t used = int32_t(Read32(base, entry + 4) - Read32(base, entry));
        if (used >= size) {
          next = true;
          break;
        }
        if (used > r31) {
          if (used > r25 || (r23 <= 1 && used == r25)) {
            next = true;
            break;
          }
          r31 = used;
          r28 = 1;
          r27 += uint32_t(used);
          r30 += uint32_t(used);
        } else {
          if (used == r31) {
            if (r28 >= r23 && r25 == r31) {
              next = true;
              break;
            }
            ++r28;
          }
          r27 += uint32_t(used);
          r30 += uint32_t(used);
        }
      } else if (s == 1) {
        const uint32_t entry = it2.b;
        const uint32_t more = Read32(base, entry + 4) - Read32(base, entry);
        r29 += more;
        r30 += more;
      } else if (s == 3) {
        next = true;
        break;
      }
      if (int32_t(r29 + r27) >= size) {

        if (r31 < r25 || (r31 == r25 && r28 < r23)) {
          r25 = r31;
          r23 = r28;
          r22 = r26;
          r21 = r30;
        }
        next = true;
        break;
      }
      s = Step(base, it2, it2.state);
      ++steps;
      if (s == 0) {

        r3 = Step(base, it1, it1.state);
        ++steps;
        break;
      }
    }
    if (next) {
      r3 = Step(base, it1, it1.state);
      ++steps;
    }
  }
}


constexpr size_t kRemembered = 64;

struct Remembered {
  uint64_t low = 0;
  uint64_t high = 0;
  uint32_t size = 0;
  bool valid = false;
  Outcome outcome;
};
thread_local std::array<Remembered, kRemembered> t_remembered{};
thread_local uint32_t t_remembered_next = 0;
thread_local uint64_t t_remembered_sampled = 0;

bool Same(const Outcome& x, const Outcome& y) { return x.kind == y.kind && x.a == y.a && x.b == y.b; }


XXH128_hash_t HashTables(const uint8_t* base, uint32_t count_a, uint32_t count_b) {
  const XXH128_hash_t a =
      XXH3_128bits_withSeed(base + kPool, size_t(count_a) * kEntryA, (uint64_t(count_a) << 32) | count_b);
  return XXH3_128bits_withSeed(base + kTableB, size_t(count_b) * kEntryB, a.low64 ^ a.high64 ^ 0x9E3779B97F4A7C15ull);
}

std::atomic<uint64_t> g_remember_raced{0};
std::atomic<uint64_t> g_check_raced{0};

Outcome ScanRemembered(const uint8_t* base, uint32_t size, uint64_t& steps) {
  if (g_remember_off.load(std::memory_order_relaxed)) {
    return Scan(base, size, steps);
  }
  const uint32_t count_a = Read32(base, kCountA);
  const uint32_t count_b = Read32(base, kCountB);

  if (count_a > 0x10000 || count_b > 0x10000) {
    return Scan(base, size, steps);
  }
  const XXH128_hash_t h = HashTables(base, count_a, count_b);
  for (const Remembered& r : t_remembered) {
    if (r.valid && r.size == size && r.low == h.low64 && r.high == h.high64) {
      const uint64_t n = g_remember_checks.load(std::memory_order_relaxed);
      if (n < kChecked || (++t_remembered_sampled % kCheckEvery) == 0) {
        g_remember_checks.fetch_add(1, std::memory_order_relaxed);
        const Outcome scanned = Scan(base, size, steps);
        const XXH128_hash_t after = HashTables(base, Read32(base, kCountA), Read32(base, kCountB));
        if (after.low64 != h.low64 || after.high64 != h.high64) {
          g_check_raced.fetch_add(1, std::memory_order_relaxed);
          return scanned;
        }
        if (!Same(scanned, r.outcome)) {
          if (!g_remember_off.exchange(true, std::memory_order_relaxed)) {
            REXLOG_ERROR("[stream_alloc] remembered outcome MISMATCH after {} checks (size {:08X}): kind {} {:08X} "
                         "{:08X} remembered, the scan gives kind {} {:08X} {:08X}; reuse is OFF for the session",
                         n, size, int(r.outcome.kind), r.outcome.a, r.outcome.b, int(scanned.kind), scanned.a,
                         scanned.b);
          }
          return scanned;
        }
      }
      g_remembered.fetch_add(1, std::memory_order_relaxed);
      return r.outcome;
    }
  }
  const Outcome o = Scan(base, size, steps);


  const XXH128_hash_t after = HashTables(base, Read32(base, kCountA), Read32(base, kCountB));
  if (after.low64 != h.low64 || after.high64 != h.high64) {
    g_remember_raced.fetch_add(1, std::memory_order_relaxed);
    return o;
  }
  Remembered& slot = t_remembered[t_remembered_next++ % kRemembered];
  slot.low = h.low64;
  slot.high = h.high64;
  slot.size = size;
  slot.outcome = o;
  slot.valid = true;
  return o;
}


using GuestFunction = void (*)(PPCContext&, uint8_t*);


inline void Call(PPCContext& ctx, uint8_t* base, GuestFunction f, uint32_t lr) {
  ctx.lr = lr;
  f(ctx, base);
}


struct Check {
  bool active = false;
  bool predicted = false;
  Outcome prediction;
  uint32_t size = 0;
  uint32_t out = 0;
};
thread_local Check t_check;

void Mismatch(const char* what, const Outcome& p, uint32_t a, uint32_t b) {
  g_mismatches.fetch_add(1, std::memory_order_relaxed);
  if (!g_off.exchange(true, std::memory_order_relaxed)) {
    REXLOG_ERROR("[stream_alloc] MISMATCH after {} checked searches: the game's search went to {} ({:08X}, {:08X}), "
                 "the native scan predicted {} ({:08X}, {:08X}); the native search is OFF for the session",
                 g_checks.load(std::memory_order_relaxed), what, a, b,
                 p.kind == Outcome::kPlace ? "place" : p.kind == Outcome::kWindow ? "window" : "fallback", p.a, p.b);
  }
}


void Verify(Outcome::Kind kind, const char* what, uint32_t a, uint32_t b) {
  if (!t_check.active || !t_check.predicted) {
    return;
  }
  t_check.predicted = false;
  const Outcome& p = t_check.prediction;
  bool same = p.kind == kind;
  if (same && kind == Outcome::kPlace) {
    same = p.a == a && p.a + t_check.size == b;
  } else if (same && kind == Outcome::kWindow) {
    same = p.a == a && p.b == b;
  }
  if (!same) {
    Mismatch(what, p, a, b);
  }
}

void Report(const uint8_t* base) {
  const int64_t now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  int64_t last = g_last_report_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10'000'000'000 || !g_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  const double seconds = double(now - last) / 1e9;
  const uint64_t searches = g_searches.exchange(0, std::memory_order_relaxed);
  const uint64_t from_job = g_searches_job.exchange(0, std::memory_order_relaxed);
  const uint64_t unlocked = g_searches_unlocked.exchange(0, std::memory_order_relaxed);
  const uint64_t native = g_native.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t steps = g_steps.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_scan = g_ns_scan.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_fallback = g_ns_fallback.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_states = g_ns_states.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_later = g_ns_later.exchange(0, std::memory_order_relaxed);
  const uint64_t remembered = g_remembered.exchange(0, std::memory_order_relaxed);
  const uint64_t states_0f98 = g_states_0f98.exchange(0, std::memory_order_relaxed);
  const uint64_t states_1170 = g_states_1170.exchange(0, std::memory_order_relaxed);
  uint64_t results[8];
  for (int i = 0; i < 8; ++i) {
    results[i] = g_results[i].exchange(0, std::memory_order_relaxed);
  }
  REXLOG_INFO("[stream_alloc] {} of {} searches ran without critical section 6 held by their thread", unlocked,
              searches);
  REXLOG_INFO("[stream_alloc] remembered outcomes{}: {} of {} native searches did not scan (the same size and tables "
              "among the thread's last {}); {} checked by scanning anyway, 0 differences so far; {} scans not kept (the tables "
              "changed while they ran); {} checks inconclusive (the tables changed under their scan)",
              g_remember_off.load(std::memory_order_relaxed) ? " (OFF after a MISMATCH)" : "", remembered, native,
              kRemembered, g_remember_checks.load(std::memory_order_relaxed),
              g_remember_raced.load(std::memory_order_relaxed), g_check_raced.load(std::memory_order_relaxed));
  REXLOG_INFO("[stream_alloc] {:.0f} s: {} searches ({} from the lock 6 job, {} native), {:.1f} ms in them ({:.1f} us "
              "each; native ones: {:.1f} us scanning, {:.1f} us in the eviction fallback, {:.1f} us in the pool-state "
              "calls ({} sub_828F0F98, {} sub_828F1170), {:.1f} us in sub_828EF958), {:.0f} native iterator "
              "steps per native search; results: 1 (later) {}, 2 {}, 3 (no room) {}, "
              "5 (placed) {}, other {}; tables now A {} B {}; checks {} ({} differences){}",
              seconds, searches, from_job, native, double(ns) / 1e6,
              searches ? double(ns) / 1e3 / double(searches) : 0.0,
              native ? double(ns_scan) / 1e3 / double(native) : 0.0,
              native ? double(ns_fallback) / 1e3 / double(native) : 0.0,
              native ? double(ns_states) / 1e3 / double(native) : 0.0, states_0f98, states_1170,
              native ? double(ns_later) / 1e3 / double(native) : 0.0,
              native ? double(steps) / double(native) : 0.0,
              results[1], results[2], results[3], results[5], results[0] + results[4] + results[6] + results[7],
              Read32(base, kCountA), Read32(base, kCountB), g_checks.load(std::memory_order_relaxed),
              g_mismatches.load(std::memory_order_relaxed),
              g_off.load(std::memory_order_relaxed) ? " [native OFF after a MISMATCH]"
                                                    : (g_on ? "" : " [native off by bo2zm_native_stream_alloc]"));


  const uint32_t count_a = std::min<uint32_t>(Read32(base, kCountA), 8192);
  const uint32_t count_b = std::min<uint32_t>(Read32(base, kCountB), 8192);
  uint32_t lo = UINT32_MAX, hi = 0;
  uint64_t used = 0, free = 0;
  for (uint32_t i = 0; i < count_a; ++i) {
    const uint32_t start = Read32(base, kPool + i * 20), end = Read32(base, kPool + i * 20 + 4);
    if (end > start) {
      lo = std::min(lo, start);
      hi = std::max(hi, end);
      used += end - start;
    }
  }
  for (uint32_t i = 0; i < count_b; ++i) {
    const uint32_t start = Read32(base, kTableB + i * 8), end = Read32(base, kTableB + i * 8 + 4);
    if (end > start) {
      lo = std::min(lo, start);
      hi = std::max(hi, end);
      free += end - start;
    }
  }
  REXLOG_INFO("[stream_alloc] pool {:08X}-{:08X} ({:.1f} MB): {} blocks in use ({:.1f} MB), {} free ({:.1f} MB)", lo,
              hi, hi > lo ? double(hi - lo) / 1048576.0 : 0.0, count_a, double(used) / 1048576.0, count_b,
              double(free) / 1048576.0);
}


void Search(PPCContext& ctx, uint8_t* base) {
  const uint32_t lr_entry = uint32_t(ctx.lr);
  const uint32_t r1_entry = ctx.r1.u32;
  const uint32_t size = ctx.r3.u32;
  const uint32_t out = ctx.r5.u32;
  const double priority = ctx.f1.f64;
  ctx.r1.u32 = r1_entry - kFrame;
  Write32(base, ctx.r1.u32, r1_entry);
  uint32_t result = 0;

  const uint32_t state_a = Read32(base, kStateA);
  const auto t_states = Clock::now();
  if (state_a != 0 || Read32(base, kStateB) != 0) {
    g_states_0f98.fetch_add(1, std::memory_order_relaxed);

    ctx.r10.u64 = state_a;
    ctx.r11.u64 = state_a == 0 ? 32 : uint64_t(__builtin_clz(state_a));
    ctx.r3.u64 = state_a == 0 ? 1 : 0;
    Call(ctx, base, __imp__sub_828F0F98, kLrState0F98);
  } else if (Read32(base, kStateC) != 0) {
    g_states_1170.fetch_add(1, std::memory_order_relaxed);
    Call(ctx, base, __imp__sub_828F1170, kLrState1170);
  }
  g_ns_states.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_states).count()),
      std::memory_order_relaxed);
  const auto t_later = Clock::now();
  Call(ctx, base, __imp__sub_828EF958, kLrLater1);
  g_ns_later.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_later).count()),
      std::memory_order_relaxed);
  if ((ctx.r3.u32 & 0xFF) != 0) {
    result = 1;
  } else {
    for (;;) {
      uint64_t steps = 0;
      const auto t_scan = Clock::now();
      const Outcome o = ScanRemembered(base, size, steps);
      g_ns_scan.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_scan).count()),
          std::memory_order_relaxed);
      g_steps.fetch_add(steps, std::memory_order_relaxed);
      if (o.kind == Outcome::kPlace) {
        ctx.r3.u64 = o.a;
        ctx.r4.u64 = o.a + size;
        Call(ctx, base, __imp__sub_828EF108, kLrPlace);
        Write32(base, out, o.a);
        result = 5;
        break;
      }
      if (o.kind == Outcome::kWindow) {
        ctx.r3.u64 = o.a;
        ctx.r4.u64 = o.b;
        ctx.r5.u64 = size;
        ctx.r7.u64 = out;
        ctx.f1.f64 = priority;
        Call(ctx, base, __imp__sub_828F1A88, kLrWindow);
        result = ctx.r3.u32;
        break;
      }
      ctx.r3.u64 = size;
      ctx.r5.u64 = out;
      ctx.f1.f64 = priority;
      const auto t_fallback = Clock::now();
      Call(ctx, base, __imp__sub_828F16F8, kLrFallback);
      g_ns_fallback.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t_fallback).count()),
          std::memory_order_relaxed);
      if (ctx.r3.u32 != 2) {
        result = ctx.r3.u32;
        break;
      }
      Call(ctx, base, __imp__sub_828EF958, kLrLater2);
      if ((ctx.r3.u32 & 0xFF) != 0) {
        result = 1;
        break;
      }
    }
  }
  ctx.r3.u64 = result;
  ctx.r1.u32 = r1_entry;
  ctx.lr = lr_entry;
}

}


REX_HOOK_RAW(sub_828F1C10) {
  static const bool on = [] {
    g_on = REXCVAR_GET(bo2zm_native_stream_alloc);
    return g_on;
  }();
  const uint32_t lr = uint32_t(ctx.lr);
  const auto t0 = Clock::now();
  const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed);
  if (Read32(base, kLock6Owner) != Read32(base, ctx.r13.u32 + 0x100)) {
    g_searches_unlocked.fetch_add(1, std::memory_order_relaxed);
  }
  if (on && !g_off.load(std::memory_order_relaxed) && n >= kChecked && n % kCheckEvery != 0) {
    Search(ctx, base);
    g_native.fetch_add(1, std::memory_order_relaxed);
  } else {
    const bool check = on && !g_off.load(std::memory_order_relaxed);
    if (check) {
      t_check.active = true;
      t_check.predicted = false;
      t_check.size = ctx.r3.u32;
      t_check.out = ctx.r5.u32;
      g_checks.fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_828F1C10(ctx, base);
    t_check.active = false;
  }
  g_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()),
                 std::memory_order_relaxed);
  g_searches.fetch_add(1, std::memory_order_relaxed);
  if (lr == kReturnFromJob) {
    g_searches_job.fetch_add(1, std::memory_order_relaxed);
  }
  g_results[ctx.r3.u32 & 7].fetch_add(1, std::memory_order_relaxed);
  Report(base);
}


REX_HOOK_RAW(sub_828EF578) {
  if (t_check.active && uint32_t(ctx.lr) == kInitScan) {
    uint64_t steps = 0;
    t_check.prediction = Scan(base, t_check.size, steps);
    t_check.predicted = true;
  }
  __imp__sub_828EF578(ctx, base);
}


REX_HOOK_RAW(sub_828EF108) {
  if (t_check.active && uint32_t(ctx.lr) == kLrPlace) {
    Verify(Outcome::kPlace, "place", ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_828EF108(ctx, base);
}
REX_HOOK_RAW(sub_828F1A88) {
  if (t_check.active && uint32_t(ctx.lr) == kLrWindow) {
    Verify(Outcome::kWindow, "window", ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_828F1A88(ctx, base);
}
REX_HOOK_RAW(sub_828F16F8) {
  if (t_check.active && uint32_t(ctx.lr) == kLrFallback) {
    Verify(Outcome::kFallback, "fallback", ctx.r3.u32, 0);
  }
  __imp__sub_828F16F8(ctx, base);
}
