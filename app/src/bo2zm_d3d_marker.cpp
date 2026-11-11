#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "native/render/bo2zm_deferred_report.h"
#include "native/render/bo2zm_microcode_versions.h"
#include "native/render/bo2zm_native_hooks.h"

REXCVAR_DEFINE_BOOL(bo2zm_d3d_marker, true, "BO2",
                    "D3D state marker: each draw's register dumps reach the native renderer as one NOP packet "
                    "instead of type-0 packets, after a self-check against the game's packets. false = the game's "
                    "packets always")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_d3d_marker_check_only, false, "BO2",
                    "Test helper for bo2zm_d3d_marker: stay in the check phase for the whole run (the game's "
                    "packets plus a check marker on every dump), to verify a whole route")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82174F38);
REX_EXTERN(__imp__sub_82175178);
REX_EXTERN(__imp__sub_821752D0);
REX_EXTERN(__imp__sub_82175420);
REX_EXTERN(__imp__sub_82169BF8);
REX_EXTERN(__imp__sub_821698D8);

namespace {

namespace mc = bo2zm::native::microcode;
using bo2zm::native::kMarkerApply;
using bo2zm::native::kMarkerCheck;
using bo2zm::native::kMarkerMagic;


inline uint8_t* Pointer(uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}
inline uint32_t Read32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Pointer(base, address), 4);
  return __builtin_bswap32(v);
}
inline void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Pointer(base, address), &value, 4);
}

inline void CopyWords(uint8_t* base, uint32_t target, uint32_t source, uint32_t n) {
  if (Pointer(base, target + n * 4 - 4) - Pointer(base, target) == ptrdiff_t(n * 4 - 4) &&
      Pointer(base, source + n * 4 - 4) - Pointer(base, source) == ptrdiff_t(n * 4 - 4)) {
    std::memmove(Pointer(base, target), Pointer(base, source), size_t(n) * 4);
    return;
  }
  for (uint32_t i = 0; i < n; ++i) {
    std::memcpy(Pointer(base, target + i * 4), Pointer(base, source + i * 4), 4);
  }
}


template <typename T>
inline void Add(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}


enum Dumper : uint32_t { kRegisters, kFetch, kConstants };

constexpr uint32_t kPadding = 0x80000000u;
constexpr uint32_t kLoadMax = 0x4000;

constexpr uint32_t kCloseFetch[4] = {0x00025000u, 0u, 0x00025000u, 0u};


constexpr uint32_t kGroups = 12;
constexpr const char* kNameGroup[kGroups] = {"0x2000", "0x2080", "0x2100", "0x2180", "0x2200", "0x2280",
                                               "0x2300", "0x2380", "VS",     "PS",     "fetch",  "bool"};
inline uint32_t GroupOf(uint32_t reg_index) {
  if (reg_index < 0x2400) {
    return (reg_index - 0x2000) >> 7;
  }
  return reg_index < 0x4400 ? 8u : reg_index < 0x4800 ? 9u : reg_index < 0x4900 ? 10u : 11u;
}

constexpr uint64_t kChecksGame = 20000;
constexpr uint64_t kChecksRing = 20000;
constexpr uint32_t kMinPerGroup = 64;
constexpr uint64_t kCheckEvery = 1024;

struct Span {
  uint32_t reg_index;
  uint32_t count;
  uint32_t source;
};


struct DumpRecord {
  Span spans[32];
  uint32_t n = 0;
  uint32_t words = 0;
  uint32_t groups = 0;
};


inline bool Collect(uint64_t mask, uint32_t register_base, uint32_t source_base, uint32_t per_bit, DumpRecord& v) {
  uint32_t bit = 0;
  while (mask != 0) {
    const uint32_t z = uint32_t(__builtin_clzll(mask));
    bit += z;
    mask <<= z;
    const uint64_t inverted = ~mask;
    const uint32_t n = inverted ? uint32_t(__builtin_clzll(inverted)) : 64u;
    Span& t = v.spans[v.n++];
    t.reg_index = register_base + bit * per_bit;
    t.count = n * per_bit;
    t.source = source_base + bit * per_bit * 4;
    if (!bo2zm::native::DumpSpan(t.reg_index, t.count)) {
      return false;
    }
    v.words += 1 + t.count;
    v.groups |= 1u << GroupOf(t.reg_index);
    bit += n;
    mask = n >= 64 ? 0 : (mask << n);
  }
  return true;
}


inline bool SimulateDump(Dumper dumper, const DumpRecord& v, uint32_t p, uint32_t end, uint32_t& final) {
  for (uint32_t i = 0; i < v.n; ++i) {
    const uint32_t count = v.spans[i].count;
    switch (dumper) {
      case kRegisters:
        if (uint64_t(p) + 4ull * count >= end) {
          return false;
        }
        break;
      case kFetch:
        if (uint64_t(p) + 4ull * (count + 5) >= end) {
          return false;
        }
        p += p & 7;
        break;
      case kConstants:
        if (uint64_t(p) + 4ull * (count + 3) >= end) {
          return false;
        }
        p += (p & 15) == 4 ? 4 : (p & 15) == 0 ? 8 : (p & 15) == 12 ? 12 : 0;
        break;
    }
    p += 4 + 4 * count;
  }
  if (dumper == kFetch) {
    p += 16;
  }
  final = p;
  return true;
}

struct Mismatch {
  const char* what = "";
  uint32_t reg_index = 0;
  uint32_t expected = 0;
  uint32_t seen = 0;
};


inline bool MatchPackets(uint8_t* base, Dumper dumper, uint32_t since, uint32_t until, const DumpRecord& v,
                              Mismatch& d) {
  uint32_t p = since + 4;
  const uint32_t end = until + 4;
  for (uint32_t i = 0; i < v.n; ++i) {
    const Span& t = v.spans[i];
    for (uint32_t padding = 0; padding < 3 && p < end && Read32(base, p) == kPadding; ++padding) {
      p += 4;
    }
    const uint32_t header = ((t.count - 1) << 16) | t.reg_index;
    if (p >= end || Read32(base, p) != header) {
      d = Mismatch{"header", t.reg_index, header, p < end ? Read32(base, p) : 0};
      return false;
    }
    p += 4;
    if ((end - p) / 4 < t.count) {
      d = Mismatch{"short packet", t.reg_index, t.count, (end - p) / 4};
      return false;
    }
    for (uint32_t k = 0; k < t.count; ++k) {
      const uint32_t seen = Read32(base, p + 4 * k);
      const uint32_t expected = Read32(base, t.source + 4 * k);
      if (seen != expected) {
        d = Mismatch{"value", t.reg_index + k, expected, seen};
        return false;
      }
    }
    p += 4 * t.count;
  }
  if (dumper == kFetch) {
    for (uint32_t k = 0; k < 4; ++k, p += 4) {
      if (p >= end || Read32(base, p) != kCloseFetch[k]) {
        d = Mismatch{"fetch close", 0x5000, kCloseFetch[k], p < end ? Read32(base, p) : 0};
        return false;
      }
    }
  }
  if (p != end) {
    d = Mismatch{"extra words", 0, end, p};
    return false;
  }
  return true;
}


struct OpenGroup {
  bool valid = false;
  uint32_t vulkan_device = 0;
  uint32_t header = 0;
  uint32_t end = 0;
  uint32_t load = 0;
  uint32_t sequence = 0;
  uint32_t publications = 0;
};
thread_local OpenGroup t_open;


std::atomic<uint32_t> g_threads{0};
thread_local uint32_t t_sequence = 0;
inline uint32_t Sequence() {
  if (t_sequence == 0) {
    t_sequence = (g_threads.fetch_add(1, std::memory_order_relaxed) + 1) << 24;
  }
  ++t_sequence;
  return t_sequence;
}

inline uint32_t HeaderNop(uint32_t load) {
  return 0xC0001000u | ((load - 1) << 16);
}

inline void WriteSpans(uint8_t* base, uint32_t p, const DumpRecord& v) {
  for (uint32_t i = 0; i < v.n; ++i) {
    const Span& t = v.spans[i];
    Write32(base, p, (t.count << 16) | t.reg_index);
    CopyWords(base, p + 4, t.source, t.count);
    p += 4 + t.count * 4;
  }
}


inline bool WriteMarker(uint8_t* base, uint32_t dev, const DumpRecord& v, uint32_t mode, uint32_t sequence) {
  const uint32_t load = 2 + v.words;
  const uint32_t write = Read32(base, dev + 48);
  const uint32_t end = Read32(base, dev + 52);
  if (load > kLoadMax || uint64_t(write) + 4ull * load >= end) {
    return false;
  }
  const uint32_t p = write + 4;
  Write32(base, p, HeaderNop(load));
  Write32(base, p + 4, kMarkerMagic | mode);
  Write32(base, p + 8, sequence);
  WriteSpans(base, p + 12, v);
  const uint32_t last_word = p + 4 * load;
  Write32(base, dev + 48, last_word);
  if (mode == kMarkerApply) {
    t_open = OpenGroup{true, dev, p, last_word, load, sequence, bo2zm::native::g_publications_ring.load(std::memory_order_relaxed)};
  } else {
    t_open.valid = false;
  }
  return true;
}


inline bool Extender(uint8_t* base, uint32_t dev, const DumpRecord& v) {
  OpenGroup& a = t_open;
  if (!a.valid || a.vulkan_device != dev) {
    return false;
  }
  const uint32_t write = Read32(base, dev + 48);
  const uint32_t end = Read32(base, dev + 52);
  const uint32_t load = a.load + v.words;
  if (write != a.end || a.publications != bo2zm::native::g_publications_ring.load(std::memory_order_relaxed) ||
      load > kLoadMax || uint64_t(write) + 4ull * v.words >= end ||
      Read32(base, a.header) != HeaderNop(a.load) || Read32(base, a.header + 4) != (kMarkerMagic | kMarkerApply) ||
      Read32(base, a.header + 8) != a.sequence) {
    a.valid = false;
    return false;
  }
  WriteSpans(base, write + 4, v);
  a.end = write + 4 * v.words;
  a.load = load;
  Write32(base, a.header, HeaderNop(load));
  Write32(base, dev + 48, a.end);
  return true;
}


std::atomic<bool> g_consumer{false};
std::atomic<bool> g_off{false};
std::atomic<bool> g_applying{false};
std::atomic<uint64_t> g_equal_game{0};
std::atomic<uint64_t> g_equal_ring{0};
std::atomic<uint32_t> g_checked_group[kGroups];
std::atomic<uint32_t> g_groups_ready{0};
thread_local uint64_t t_turn = 0;
std::atomic<bool> g_different_ring{false};
std::atomic<uint32_t> g_different_sequence{0};
std::atomic<uint32_t> g_different_register{0};
std::atomic<uint32_t> g_different_in_registers{0};
std::atomic<uint32_t> g_different_in_marker{0};


enum Counter : uint32_t {
  kCalls,
  kNew,
  kExtended,
  kSpans,
  kWords,
  kChecked,
  kNoRoom,
  kNoComparable,
  kOutOfGroup,
  kCounters
};
struct ThreadCounters {
  std::atomic<uint64_t> v[kCounters]{};
};
constexpr int kMaxThreads = 64;
std::atomic<ThreadCounters*> g_threads_counters[kMaxThreads];
std::atomic<int> g_n_threads_counters{0};
thread_local ThreadCounters* t_counters = nullptr;
uint64_t g_previous[kCounters];
std::atomic<int64_t> g_i_next_ms{0};
thread_local uint32_t t_calls = 0;

[[gnu::noinline, gnu::cold]] ThreadCounters& RegisterCounters() {
  t_counters = new ThreadCounters;
  const int i = g_n_threads_counters.fetch_add(1, std::memory_order_relaxed);
  if (i < kMaxThreads) {
    g_threads_counters[i].store(t_counters, std::memory_order_release);
  }
  return *t_counters;
}
inline void Count(Counter k, uint64_t n = 1) {
  ThreadCounters& c = t_counters ? *t_counters : RegisterCounters();
  c.v[k].store(c.v[k].load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

void TurnOff(const char* side, const char* what, uint32_t sequence, uint32_t reg_index, uint32_t expected,
            uint32_t seen) {
  if (g_off.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  t_open.valid = false;
  REXLOG_ERROR("[d3d_marker] MISMATCH in the {} ({}): marker {:08X}, group {}, register {:04X}, expected {:08X}, seen "
               "{:08X}. Marker OFF for the rest of the session: the dumps are the game's own again",
               side, what, sequence, reg_index >= 0x2000 ? kNameGroup[GroupOf(reg_index)] : "?", reg_index, expected,
               seen);
}

void AnnotateEqualGame(const DumpRecord& v) {
  Add(g_equal_game, uint64_t(1));
  uint32_t ready = g_groups_ready.load(std::memory_order_relaxed);
  for (uint32_t g = 0; g < kGroups; ++g) {
    if ((v.groups >> g) & 1) {
      const uint32_t times = g_checked_group[g].load(std::memory_order_relaxed) + 1;
      g_checked_group[g].store(times, std::memory_order_relaxed);
      if (times >= kMinPerGroup) {
        ready |= 1u << g;
      }
    }
  }
  g_groups_ready.store(ready, std::memory_order_relaxed);
}

uint32_t DecideMode(uint32_t groups) {
  static const bool only_check = REXCVAR_GET(bo2zm_d3d_marker_check_only);
  if (only_check) {
    return kMarkerCheck;
  }
  if (!g_applying.load(std::memory_order_relaxed)) {
    const uint64_t game = g_equal_game.load(std::memory_order_relaxed);
    const uint64_t ring = g_equal_ring.load(std::memory_order_relaxed);
    if (game < kChecksGame || ring < kChecksRing) {
      return kMarkerCheck;
    }
    if (!g_applying.exchange(true, std::memory_order_relaxed)) {
      REXLOG_INFO("[d3d_marker] {} dumps checked on the game thread and {} on the ring thread, 0 mismatches: the dumps "
                  "now write only the marker (1 in {} is still checked)",
                  game, ring, kCheckEvery);
    }
  }
  if ((groups & ~g_groups_ready.load(std::memory_order_relaxed)) != 0) {
    return kMarkerCheck;
  }
  return (++t_turn & (kCheckEvery - 1)) == 0 ? kMarkerCheck : kMarkerApply;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Report() {
  const int64_t now = NowMs();
  int64_t next = g_i_next_ms.load(std::memory_order_relaxed);
  if (now < next || !g_i_next_ms.compare_exchange_strong(next, now + 10000)) {
    return;
  }
  if (next == 0) {
    REXLOG_INFO("[d3d_marker] D3D state marker on (bo2zm_d3d_marker): starts by checking");
    return;
  }
  uint64_t d[kCounters] = {};
  const int threads = std::min(g_n_threads_counters.load(std::memory_order_relaxed), kMaxThreads);
  for (int i = 0; i < threads; ++i) {
    if (const ThreadCounters* c = g_threads_counters[i].load(std::memory_order_acquire)) {
      for (uint32_t k = 0; k < kCounters; ++k) {
        d[k] += c->v[k].load(std::memory_order_relaxed);
      }
    }
  }
  for (uint32_t k = 0; k < kCounters; ++k) {
    const uint64_t total = d[k];
    d[k] -= g_previous[k];
    g_previous[k] = total;
  }
  const uint64_t new_ones = d[kNew];
  const uint64_t extended = d[kExtended];
  const uint64_t spans = d[kSpans];
  const uint64_t words = d[kWords];
  uint32_t n_ready = 0;
  for (uint32_t ready = g_groups_ready.load(std::memory_order_relaxed); ready; ready &= ready - 1) {
    ++n_ready;
  }
  BO2ZM_REPORT_DEFERRED(
      "[d3d_marker] last 10 s: {} dumps: {} in markers ({} new markers and {} appended to an open one; {:.1f} spans and "
      "{:.1f} words per marker), {} checked against the game, {} without room, {} not comparable, {} with spans outside "
      "the groups | phase {} | checked: game {} of {}, ring {} of {}, groups ready {} | microcode notified since boot: "
      "{} creations, {} patches in place, {} in a copy, {} others",
      d[kCalls], new_ones + extended, new_ones, extended, new_ones ? double(spans) / double(new_ones) : 0.0,
      new_ones ? double(words) / double(new_ones) : 0.0, d[kChecked], d[kNoRoom], d[kNoComparable],
      d[kOutOfGroup],
      g_off.load(std::memory_order_relaxed)    ? "OFF"
      : g_applying.load(std::memory_order_relaxed) ? "applying"
                                                    : "observing",
      g_equal_game.load(std::memory_order_relaxed), kChecksGame,
      g_equal_ring.load(std::memory_order_relaxed), kChecksRing, n_ready,
      mc::g_creations.load(std::memory_order_relaxed), mc::g_patches_in_room.load(std::memory_order_relaxed),
      mc::g_patches_in_copy.load(std::memory_order_relaxed), mc::g_patches_others.load(std::memory_order_relaxed));

  static uint64_t previous_records = 0, previous_checking = 0;
  uint64_t records = 0, checking = 0;
  bo2zm::native::StreamRecordCounts(records, checking);
  if (records != previous_records) {
    BO2ZM_REPORT_DEFERRED("[d3d_marker] last 10 s: {} draw records in the stream ({} in check mode)",
                          records - previous_records, checking - previous_checking);
  }
  previous_records = records;
  previous_checking = checking;
}


template <typename Original>
void Dump(PPCContext& ctx, uint8_t* base, Dumper dumper, uint32_t register_base, uint32_t source_base,
            uint32_t per_bit, Original original) {
  static const bool active = REXCVAR_GET(bo2zm_d3d_marker);
  const uint32_t dev = ctx.r3.u32;
  const uint64_t mask = ctx.r4.u64;
  if (!active || mask == 0 || !g_consumer.load(std::memory_order_relaxed) ||
      g_off.load(std::memory_order_relaxed)) {
    t_open.valid = false;
    original(ctx, base);
    return;
  }
  if (g_different_ring.load(std::memory_order_acquire)) {
    TurnOff("ring thread", "registers after the packets", g_different_sequence.load(std::memory_order_relaxed),
           g_different_register.load(std::memory_order_relaxed), g_different_in_marker.load(std::memory_order_relaxed),
           g_different_in_registers.load(std::memory_order_relaxed));
    original(ctx, base);
    return;
  }
  Count(kCalls);
  if ((++t_calls & 4095) == 0) {
    Report();
  }
  DumpRecord v;
  if (!Collect(mask, register_base, source_base, per_bit, v)) {
    Count(kOutOfGroup);
    t_open.valid = false;
    original(ctx, base);
    return;
  }
  const uint32_t mode = DecideMode(v.groups);
  if (mode == kMarkerApply) {
    bool done = Extender(base, dev, v);
    if (done) {
      Count(kExtended);
    } else if (WriteMarker(base, dev, v, kMarkerApply, Sequence())) {
      Count(kNew);
      done = true;
    }
    if (done) {
      Count(kSpans, v.n);
      Count(kWords, v.words - v.n);
      ctx.r3.u64 = dev;
      ctx.r12.u64 = uint32_t(ctx.lr);
      return;
    }
    Count(kNoRoom);
  }

  t_open.valid = false;
  const uint32_t since = Read32(base, dev + 48);
  uint32_t foreseen = 0;
  const bool fast = SimulateDump(dumper, v, since, Read32(base, dev + 52), foreseen);
  original(ctx, base);
  if (mode != kMarkerCheck) {
    return;
  }
  const uint32_t until = Read32(base, dev + 48);
  Mismatch d;
  const uint32_t sequence = Sequence();
  if (!fast || until != foreseen) {
    Count(kNoComparable);
  } else if (!MatchPackets(base, dumper, since, until, v, d)) {
    TurnOff("game thread", d.what, sequence, d.reg_index, d.expected, d.seen);
  } else {
    AnnotateEqualGame(v);
    if (WriteMarker(base, dev, v, kMarkerCheck, sequence)) {
      Count(kChecked);
    } else {
      Count(kNoRoom);
    }
  }
}


#ifndef BO2ZM_MARKER_BACK_PATCH_IN_ROOM
#define BO2ZM_MARKER_BACK_PATCH_IN_ROOM 0x82176220u
#define BO2ZM_MARKER_BACK_PATCH_ON_BIND 0x82176D20u
#define BO2ZM_MARKER_BACK_PATCH_COPY 0x82176080u
#endif
constexpr uint32_t kBackPatchInRoom = BO2ZM_MARKER_BACK_PATCH_IN_ROOM;
constexpr uint32_t kBackPatchOnBind = BO2ZM_MARKER_BACK_PATCH_ON_BIND;
constexpr uint32_t kBackPatchCopy = BO2ZM_MARKER_BACK_PATCH_COPY;

inline void WarnSlot(uint32_t start) {
  const uint32_t slot = mc::SlotOf(mc::Physical(start) & ~uint32_t(3));
  mc::BeginWrite(slot);
  mc::FinishWrite(slot);
}


void WarnRegister(uint8_t* base, uint32_t object, bool vertices) {
  if (!object) {
    return;
  }
  if (vertices) {
    const uint32_t physical = Read32(base, object + 0x20);
    const uint32_t programs = (Read32(base, object + 0x368) & 0x20) ? 2 : 1;
    for (uint32_t n = 0; n < programs; ++n) {
      const uint32_t o = Read32(base, object + 0x380 + 8 * n);
      WarnSlot(physical + Read32(base, object + 0x368 + o));
    }
  } else {
    const uint32_t o = Read32(base, object + 0x40);
    WarnSlot(Read32(base, object + 0x18) + Read32(base, object + 0x28 + o));
  }
  mc::Count(mc::g_creations);
}

}

namespace bo2zm::native {

void ActivateConsumerMarkers(bool active) {
  g_consumer.store(active, std::memory_order_release);
}


void BeforeDraw(uint8_t* base, uint32_t device, DrawFunction function, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7) {
  static const bool active = REXCVAR_GET(bo2zm_d3d_marker);
  if (!active || !g_consumer.load(std::memory_order_relaxed)) {
    return;
  }
  DrawRecord record;
  uint32_t mode = 0;
  if (!RecordForStream(function, base, device, r4, r5, r6, r7, record, mode)) {
    return;
  }
  const uint32_t load = 2 + kWordsDraw;
  const uint32_t write = Read32(base, device + 48);
  const uint32_t end = Read32(base, device + 52);
  if (uint64_t(write) + 4ull * load >= end) {
    return;
  }
  const uint32_t p = write + 4;
  Write32(base, p, HeaderNop(load));
  Write32(base, p + 4, kMarkerMagic | (mode << 4));
  Write32(base, p + 8, Sequence());
  Write32(base, p + 12, uint32_t(record.function) | (uint32_t(record.vegetation) << 8));
  Write32(base, p + 16, record.vs);
  Write32(base, p + 20, record.ps);
  for (uint32_t i = 0; i < 4; ++i) {
    Write32(base, p + 24 + 4 * i, record.args[i]);
  }
  Write32(base, p + 40, uint32_t(record.shadow >> 32));
  Write32(base, p + 44, uint32_t(record.shadow));
  Write32(base, device + 48, p + 4 * load);
  t_open.valid = false;
}

void AnnotateCheckMarker(bool equal, uint32_t sequence, uint32_t reg_index, uint32_t in_registers,
                                uint32_t in_marker) {
  if (equal) {
    Add(g_equal_ring, uint64_t(1));
    return;
  }
  if (g_different_ring.load(std::memory_order_relaxed)) {
    return;
  }
  g_different_sequence.store(sequence, std::memory_order_relaxed);
  g_different_register.store(reg_index, std::memory_order_relaxed);
  g_different_in_registers.store(in_registers, std::memory_order_relaxed);
  g_different_in_marker.store(in_marker, std::memory_order_relaxed);
  g_different_ring.store(true, std::memory_order_release);
}

}

REX_HOOK_RAW(sub_82174F38) {


  bo2zm::native::LearnGroupMirror(ctx.r5.u32, ctx.r4.u64, ctx.r6.u32 - ctx.r3.u32);
  Dump(ctx, base, kRegisters, ctx.r5.u32, ctx.r6.u32, 1, [](PPCContext& c, uint8_t* b) { __imp__sub_82174F38(c, b); });
}

REX_HOOK_RAW(sub_82175178) {
  Dump(ctx, base, kFetch, 0x4800, ctx.r3.u32 + 1152, 6, [](PPCContext& c, uint8_t* b) { __imp__sub_82175178(c, b); });
}

REX_HOOK_RAW(sub_821752D0) {
  Dump(ctx, base, kConstants, ctx.r5.u32, ctx.r6.u32, 16,
         [](PPCContext& c, uint8_t* b) { __imp__sub_821752D0(c, b); });
}

REX_HOOK_RAW(sub_82175420) {
  const uint32_t back = uint32_t(ctx.lr);
  const uint32_t target = ctx.r4.u32;
  if (back == kBackPatchCopy) {
    mc::Count(mc::g_patches_in_copy);
    __imp__sub_82175420(ctx, base);
    return;
  }
  if (back == kBackPatchInRoom || back == kBackPatchOnBind) {
    mc::Count(mc::g_patches_in_room);
  } else {
    mc::Count(mc::g_patches_others);
    mc::g_other_return.store(back, std::memory_order_relaxed);
    mc::g_other_target.store(target, std::memory_order_relaxed);
  }
  const uint32_t slot = mc::SlotOf(mc::Physical(target) & ~uint32_t(3));
  mc::BeginWrite(slot);
  __imp__sub_82175420(ctx, base);
  mc::FinishWrite(slot);
}

REX_HOOK_RAW(sub_82169BF8) {
  const uint32_t object = ctx.r3.u32;
  bo2zm::native::RegisterShaderObject(base, object, ctx.r4.u32, true);
  __imp__sub_82169BF8(ctx, base);
  WarnRegister(base, object, true);
}

REX_HOOK_RAW(sub_821698D8) {
  const uint32_t object = ctx.r3.u32;
  bo2zm::native::RegisterShaderObject(base, object, ctx.r4.u32, false);
  __imp__sub_821698D8(ctx, base);
  WarnRegister(base, object, false);
}
