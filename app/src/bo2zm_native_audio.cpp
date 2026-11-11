#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/intrinsics.h>
#include <rex/system/xmemory.h>
#include <simde/arm/neon.h>

#include "bo2zm_game.h"
#include "bo2zm_benchmark.h"
#include "bo2zm_gpu_skin.h"
#include "bo2zm_native_limiter.h"
#include "bo2zm_native_lzx.h"
#include "bo2zm_native_skin.h"
#include "bo2zm_page_writes.h"

#if defined(BO2ZM_HAS_LOCALS) && __has_include("bo2zm_locals.h")
#include "bo2zm_locals.h"
#endif

REXCVAR_DEFINE_BOOL(bo2zm_native_audio, true, "BO2",
                    "Native versions of the sound mixer's hottest functions (checked against the game's own code "
                    "first, switched off at the first difference). false = the game's code only");
REXCVAR_DEFINE_BOOL(bo2zm_native_skinning, true, "BO2",
                    "The game's CPU vertex skinning kernels (sub_82912D78, sub_82913378, sub_82913B50) rewritten with "
                    "vector registers (bo2zm_native_skin.cpp), checked against the game's own first. false = the "
                    "game's code only");
REXCVAR_DEFINE_BOOL(bo2zm_native_skinning_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_skinning (on = the rewrites, off = "
                    "the game's code)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_lzx, true, "BO2",
                    "The game's fastfile stream decoder (sub_827E7058, the assets streamed while playing) rewritten "
                    "natively (bo2zm_native_lzx.cpp), checked against the game's own first. false = the game's code "
                    "only");
REXCVAR_DEFINE_BOOL(bo2zm_native_lzx_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is bo2zm_native_lzx (on = the rewrite, off = the "
                    "game's code)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_lzx_compare, false, "BO2",
                    "Test: once the rewrite runs alone, the stream decoder's calls alternate between the rewrite and "
                    "the game's code, each timed whole, and a \"[native_lzx] alternating\" line every 10 s gives "
                    "both per call and per MB (the checked phase times the rewrite second, on data the game's code "
                    "has just brought into the cache)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_audio_fast, true, "BO2",
                    "Faster forms of the native sound functions with the same results (scale and add in one pass, "
                    "the voice resamplers' conversion and interpolation in vector registers). false = the forms "
                    "before them");
REXCVAR_DEFINE_INT32(bo2zm_native_audio_fast_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_native_audio_fast. 0 = not "
                     "part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_audio_check_only, false, "BO2",
                    "Test helper: the native sound functions stay in the checked phase for the whole run (each call "
                    "runs both versions and compares them)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8258AD40);
REX_EXTERN(__imp__sub_8258B0D0);
REX_EXTERN(__imp__sub_8258AE90);
REX_EXTERN(__imp__sub_82592EE8);
REX_EXTERN(__imp__sub_8258C310);

namespace {

using Function = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kChecksForApply = 20000;
constexpr uint32_t kCheckOneOfEvery = 1024;
constexpr uint32_t kMaxBytesChecked = 1u << 20;

inline uint8_t* Host(uint8_t* base, uint32_t guest) {
  return rex::memory::GuestPtr<uint8_t*>(base, guest);
}


inline bool Contiguous(uint32_t guest, uint32_t bytes) {
  const uint64_t end = uint64_t(guest) + bytes;
  return end <= 0x1'0000'0000ull && !(guest < 0xE0000000u && end > 0xE0000000ull);
}

inline float Load(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

inline void Save(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

inline void Save32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

using V = simde__m128;
using VI = simde__m128i;

inline VI Mask16() { return simde_mm_loadu_si128(reinterpret_cast<const VI*>(rex::ppc::VectorMaskL)); }


inline V LoadV(uint8_t* base, uint32_t ea) {
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const VI*>(Host(base, ea & ~15u))), Mask16()));
}

inline void SaveV(uint8_t* base, uint32_t ea, V v) {
  simde_mm_store_si128(reinterpret_cast<VI*>(Host(base, ea & ~15u)),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(v), Mask16()));
}

inline V Merge(float e0, float e1, float e2, float e3) { return simde_mm_set_ps(e0, e1, e2, e3); }

inline float Element(V v, int k) {
  alignas(16) float t[4];
  simde_mm_store_ps(t, v);
  return t[3 - k];
}

inline VI Mask(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  alignas(16) uint8_t m[16];
  Save32(m, w0);
  Save32(m + 4, w1);
  Save32(m + 8, w2);
  Save32(m + 12, w3);
  return simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const VI*>(m)), Mask16());
}

uint64_t NowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}


using F4 = simde_float32x4_t;
inline F4 LoadBE(const uint8_t* p) {
  return simde_vreinterpretq_f32_u8(simde_vrev32q_u8(simde_vld1q_u8(p)));
}
inline void StoreBE(uint8_t* p, F4 v) {
  simde_vst1q_u8(p, simde_vrev32q_u8(simde_vreinterpretq_u8_f32(v)));
}


inline void Rounded(F4& v) {
#if defined(__aarch64__)
  asm("" : "+w"(v));
#endif
}

inline bool Apart(uint32_t a, uint32_t b, uint32_t bytes) {
  return uint64_t(a) + bytes <= b || uint64_t(b) + bytes <= a;
}

bool AudioFast() {
  static const int factor = REXCVAR_GET(bo2zm_native_audio_fast_ab);
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  return REXCVAR_GET(bo2zm_native_audio_fast);
}


struct FastCount {
  std::atomic<uint64_t> fast{0}, other{0};
  uint64_t fast_said = 0, other_said = 0;
};
FastCount g_fast_mix, g_fast_interpolation;


struct Region {
  uint32_t guest = 0;
  uint32_t bytes = 0;
};

struct Regions {
  static constexpr int kMax = 20;
  Region r[kMax];
  int n = 0;
  bool valid = true;
  void Append(uint32_t guest, uint32_t bytes) {
    if (bytes == 0) {
      return;
    }
    if (n == kMax || bytes > kMaxBytesChecked || !Contiguous(guest, bytes)) {
      valid = false;
      return;
    }
    r[n++] = {guest, bytes};
  }
};


enum class Group { kAudio, kSkinning, kStreams };

bool Enabled(Group group) {
  switch (group) {
    case Group::kSkinning:
      return REXCVAR_GET(bo2zm_native_skinning);
    case Group::kStreams:
      return REXCVAR_GET(bo2zm_native_lzx);
    default:
      return REXCVAR_GET(bo2zm_native_audio);
  }
}

struct Replacement {
  const char* label;

  float tolerance = 0.0f;
  bool compare_r3 = true;


  uint32_t for_apply = kChecksForApply;
  std::atomic<int> phase{0};
  std::atomic<uint32_t> close_calls{0};
  std::atomic<uint32_t> checked{0};
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> ns_original{0};
  std::atomic<uint64_t> ns_native{0};
  std::atomic<uint64_t> timed{0};
  std::atomic<uint64_t> calls_said{0};
  std::atomic<uint32_t> checked_said{0};
  const char* tag = "native_audio";
  Group group = Group::kAudio;

  bool skin_near = false;
};

Replacement g_scalar{"scale (sub_8258AD40)"};
Replacement g_mix{"scale and add (sub_8258B0D0)"};
Replacement g_filter{"one-pole filter (sub_8258AE90)"};
Replacement g_biquad{"biquad, local copy (sub_8258B208)"};


Replacement g_biquad_native{"biquad (sub_8258B208), native"};
Replacement g_fill{"128-byte block fill (sub_827D3060), native"};


Replacement g_reverb{"reverb core (sub_82593500)", 1e-4f, false, 4000};
Replacement g_voice_mono{"voice resampler, mono (sub_8243F620)", 0.0f, false};
Replacement g_voice_stereo{"voice resampler, stereo (sub_82440658)", 0.0f, false};


Replacement g_skin1{"skinning, one bone per vertex (sub_82912D78), vector rewrite", 0.0f, false, 3000};
Replacement g_skin2{"skinning, two bones per vertex (sub_82913378), vector rewrite", 0.0f, false, 3000};
Replacement g_skin3{"skinning, three bones per vertex (sub_82913B50), vector rewrite", 0.0f, false, 3000};
struct SkinTags {
  SkinTags() {
    for (Replacement* r : {&g_skin1, &g_skin2, &g_skin3}) {
      r->tag = "native_skin";
      r->group = Group::kSkinning;
      r->skin_near = true;
    }
  }
} g_skin_tags;


Replacement g_lzx{"fastfile stream decoder (sub_827E7058), native", 0.0f, true, 2000};
struct LzxTags {
  LzxTags() {
    g_lzx.tag = "native_lzx";
    g_lzx.group = Group::kStreams;
  }
} g_lzx_tags;


Replacement g_limiter{"limiter (sub_8258BE08), native", 1.0f / 1048576.0f, true, 5000};
Replacement* const kAll[] = {&g_scalar, &g_mix,   &g_filter, &g_biquad, &g_biquad_native, &g_fill,  &g_reverb,
                             &g_voice_mono, &g_voice_stereo, &g_skin1,  &g_skin2, &g_skin3,  &g_lzx,
                             &g_limiter};

std::atomic<uint64_t> g_next_summary{0};

void ReportSoundPool();

void Summarize() {
  const uint64_t now = NowNs();
  uint64_t next = g_next_summary.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_summary.compare_exchange_strong(next, now + 10'000'000'000ull);
    return;
  }
  if (now < next || !g_next_summary.compare_exchange_strong(next, now + 10'000'000'000ull)) {
    return;
  }
  for (Replacement* r : kAll) {
    const uint64_t calls = r->calls.load(std::memory_order_relaxed);
    const uint32_t checked = r->checked.load(std::memory_order_relaxed);
    const uint64_t interval = calls - r->calls_said.exchange(calls, std::memory_order_relaxed);
    const uint32_t comp_interval =
        checked - r->checked_said.exchange(checked, std::memory_order_relaxed);
    const uint64_t n = r->timed.load(std::memory_order_relaxed);
    const double us_original = n ? double(r->ns_original.load(std::memory_order_relaxed)) / double(n) / 1e3 : 0.0;
    const double us_native = n ? double(r->ns_native.load(std::memory_order_relaxed)) / double(n) / 1e3 : 0.0;
    const int phase = r->phase.load(std::memory_order_relaxed);
    REXLOG_INFO(
        "[{}] {}: {} calls in 10 s, {} checked ({} since start, {} of them within the tolerance but not "
        "identical), phase {}; per call {:.2f} us original, {:.2f} us native ({:.1f}x)",
        r->tag, r->label, interval, comp_interval, checked, r->close_calls.load(std::memory_order_relaxed),
        phase == 0 ? "checking" : phase == 1 ? "native" : "OFF (difference)", us_original, us_native,
        us_native > 0 ? us_original / us_native : 0.0);
  }
  {
    std::string line;
    for (auto [name, c] : {std::pair<const char*, FastCount*>{"scale and add", &g_fast_mix},
                           std::pair<const char*, FastCount*>{"resampler interpolation", &g_fast_interpolation}}) {
      const uint64_t fast = c->fast.load(std::memory_order_relaxed), other = c->other.load(std::memory_order_relaxed);
      line += fmt::format("{}{} {} fast, {} not", line.empty() ? "" : "; ", name, fast - c->fast_said,
                          other - c->other_said);
      c->fast_said = fast;
      c->other_said = other;
    }
    REXLOG_INFO("[native_audio] fast forms, last 10 s: {}", line);
  }
  ReportSoundPool();
  bo2zm::native_skin::Report();
  bo2zm::gpu_skin::Report();
}

void Save(uint8_t* base, const Regions& rg, std::vector<uint8_t>& copy) {
  size_t total = 0;
  for (int i = 0; i < rg.n; ++i) {
    total += rg.r[i].bytes;
  }
  copy.resize(total);
  size_t pos = 0;
  for (int i = 0; i < rg.n; ++i) {
    std::memcpy(copy.data() + pos, Host(base, rg.r[i].guest), rg.r[i].bytes);
    pos += rg.r[i].bytes;
  }
}

void Restore(uint8_t* base, const Regions& rg, const std::vector<uint8_t>& copy) {
  size_t pos = 0;
  for (int i = 0; i < rg.n; ++i) {
    std::memcpy(Host(base, rg.r[i].guest), copy.data() + pos, rg.r[i].bytes);
    pos += rg.r[i].bytes;
  }
}


bool Near(const uint8_t* a, const uint8_t* b, float tolerance) {
  const float x = Load(a);
  const float y = Load(b);
  auto normal_or_zero = [](float v) { return v == 0.0f || std::fpclassify(v) == FP_NORMAL; };
  if (!normal_or_zero(x) || !normal_or_zero(y)) {
    return false;
  }
  const float mismatch = std::fabs(x - y);
  return mismatch <= std::max(tolerance * std::max(std::fabs(x), std::fabs(y)), 1e-9f);
}


bool NearSkin(const uint8_t* a, const uint8_t* b) {
  if (Near(a, b, 1.0f / 1048576.0f)) {
    return true;
  }
  uint32_t x, y;
  std::memcpy(&x, a, 4);
  std::memcpy(&y, b, 4);
  x = __builtin_bswap32(x);
  y = __builtin_bswap32(y);
  for (int shift : {22, 12, 2}) {
    const int32_t fx = int32_t(x << shift) >> 22;
    const int32_t fy = int32_t(y << shift) >> 22;
    if (fx - fy > 1 || fy - fx > 1) {
      return false;
    }
  }
  const int32_t wx = int32_t(x >> 30), wy = int32_t(y >> 30);
  return wx - wy <= 1 && wy - wx <= 1;
}


int Compare(Replacement& r, uint8_t* base, const Regions& rg, const std::vector<uint8_t>& original,
             const PPCContext& input, const PPCContext& ctx_original, const PPCContext& ctx_native) {
  size_t pos = 0;
  bool close = false;
  for (int i = 0; i < rg.n; ++i) {
    const uint8_t* native = Host(base, rg.r[i].guest);
    if (std::memcmp(native, original.data() + pos, rg.r[i].bytes) != 0 && (r.tolerance > 0.0f || r.skin_near)) {
      bool all_near = (rg.r[i].bytes % 4) == 0;
      for (uint32_t w = 0; all_near && w < rg.r[i].bytes; w += 4) {
        if (std::memcmp(native + w, original.data() + pos + w, 4) != 0 &&
            !(r.skin_near ? NearSkin(original.data() + pos + w, native + w)
                          : Near(original.data() + pos + w, native + w, r.tolerance))) {
          all_near = false;
        }
      }
      if (all_near) {
        close = true;
        pos += rg.r[i].bytes;
        continue;
      }
    }
    if (std::memcmp(native, original.data() + pos, rg.r[i].bytes) != 0) {
      uint32_t j = 0;
      while (j < rg.r[i].bytes && native[j] == original[pos + j]) {
        ++j;
      }
      const uint32_t w = j & ~3u;
      uint32_t a = 0, b = 0;
      if (w + 4 <= rg.r[i].bytes) {
        std::memcpy(&a, original.data() + pos + w, 4);
        std::memcpy(&b, native + w, 4);
      }
      r.phase.store(2, std::memory_order_relaxed);
      REXLOG_WARN(
          "[{}] {}: MISMATCH at {:08X} (region {} of {}, {} bytes): original {:08X}, native {:08X}; r3 {:08X} "
          "r4 {:08X} r5 {:08X} r6 {:08X} r7 {:08X} f1 {} f2 {}. The native version is off for this session",
          r.tag, r.label, rg.r[i].guest + w, i + 1, rg.n, rg.r[i].bytes, __builtin_bswap32(a), __builtin_bswap32(b),
          input.r3.u32, input.r4.u32, input.r5.u32, input.r6.u32, input.r7.u32, input.f1.f64,
          input.f2.f64);
      return 0;
    }
    pos += rg.r[i].bytes;
  }
  if (r.compare_r3 && ctx_native.r3.u64 != ctx_original.r3.u64) {
    r.phase.store(2, std::memory_order_relaxed);
    REXLOG_WARN("[{}] {}: MISMATCH in r3: original {:X}, native {:X}. Off for this session", r.tag, r.label,
                ctx_original.r3.u64, ctx_native.r3.u64);
    return 0;
  }
  return close ? 2 : 1;
}


thread_local int t_checking = 0;


bool CompareContext(Replacement& r, const PPCContext& original, const PPCContext& native) {
  const auto* a = reinterpret_cast<const uint8_t*>(&original);
  const auto* b = reinterpret_cast<const uint8_t*>(&native);
  for (size_t i = 0; i < sizeof(PPCContext); ++i) {
    if (a[i] != b[i]) {
      r.phase.store(2, std::memory_order_relaxed);
      REXLOG_WARN("[{}] {}: MISMATCH in the register file at byte {} of {} (original {:02X}, copy {:02X}). "
                  "Off for this session",
                  r.tag, r.label, i, sizeof(PPCContext), a[i], b[i]);
      return false;
    }
  }
  return true;
}

void DisableSnapshotValidation(Replacement& r, const char* stage) noexcept {
  r.phase.store(2, std::memory_order_relaxed);
  try {
    REXLOG_WARN("[{}] {}: validation snapshot allocation failed {}; native replacement disabled, using the original",
                r.tag, r.label, stage);
  } catch (...) {

  }
}

template <typename FRegions>
void Call(Replacement& r, PPCContext& ctx, uint8_t* base, Function original, Function native, FRegions regions,
            bool context_whole = false) {
  if (t_checking) {
    original(ctx, base);
    return;
  }
  const uint64_t call = r.calls.fetch_add(1, std::memory_order_relaxed);
  int phase = r.phase.load(std::memory_order_relaxed);
  if (phase == 2 || !Enabled(r.group)) [[unlikely]] {
    original(ctx, base);
    return;
  }
  if (phase == 1 && (call % kCheckOneOfEvery) != 0) [[likely]] {
    native(ctx, base);
    if ((call & 255) == 0) {
      Summarize();
    }
    return;
  }
  const Regions rg = regions(ctx, base);
  if (!rg.valid) {
    original(ctx, base);
    return;
  }
  std::vector<uint8_t> t_before;
  std::vector<uint8_t> t_original;
  const PPCContext ctx_input = ctx;
  bool snapshot_failed = false;
  try {
    Save(base, rg, t_before);
  } catch (const std::bad_alloc&) {
    std::vector<uint8_t>().swap(t_before);
    DisableSnapshotValidation(r, "before the original call");
    snapshot_failed = true;
  }
  if (snapshot_failed) {
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    original(ctx, base);
    return;
  }
  ++t_checking;
  const uint64_t t0 = NowNs();
  original(ctx, base);
  const uint64_t t1 = NowNs();
  --t_checking;
  try {
    Save(base, rg, t_original);
  } catch (const std::bad_alloc&) {

    std::vector<uint8_t>().swap(t_before);
    std::vector<uint8_t>().swap(t_original);
    DisableSnapshotValidation(r, "after the original call");
    snapshot_failed = true;
  }
  if (snapshot_failed) {
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    return;
  }
  const PPCContext ctx_original = ctx;
  Restore(base, rg, t_before);
  ctx = ctx_input;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  const uint64_t t2 = NowNs();
  native(ctx, base);
  const uint64_t t3 = NowNs();
  const int result = Compare(r, base, rg, t_original, ctx_input, ctx_original, ctx);
  const bool equal = result != 0 && (!context_whole || CompareContext(r, ctx_original, ctx));
  if (equal && result == 2) {
    r.close_calls.fetch_add(1, std::memory_order_relaxed);
  }

  Restore(base, rg, t_original);
  ctx = ctx_original;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  r.ns_original.fetch_add(t1 - t0, std::memory_order_relaxed);
  r.ns_native.fetch_add(t3 - t2, std::memory_order_relaxed);
  r.timed.fetch_add(1, std::memory_order_relaxed);
  if (equal) {
    const uint32_t n = r.checked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (phase == 0 && n >= r.for_apply && !REXCVAR_GET(bo2zm_native_audio_check_only)) {
      int expected = 0;
      if (r.phase.compare_exchange_strong(expected, 1)) {
        REXLOG_INFO("[{}] {}: {} calls identical to the game's: the native version runs alone now (1 in {} "
                    "still checked)",
                    r.tag, r.label, n, kCheckOneOfEvery);
      }
    }
  }
  Summarize();
}


inline uint32_t VectorsScalar(uint32_t n, uint32_t src) {
  const uint32_t end = src + (n << 2);
  if (src >= end) {
    return 0;
  }
  return ((end - src - 1) >> 4) + 1;
}

void Scale(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r3.u32;
  const uint32_t src = ctx.r5.u32;
  const uint32_t dst = ctx.r6.u32;
  ctx.fpscr.disableFlushMode();
  const float g = float(ctx.f1.f64);
  Save(Host(base, ctx.r1.u32 + 28), g);
  const uint32_t vectors = VectorsScalar(n, src);
  if (vectors == 0) {
    return;
  }
  ctx.fpscr.enableFlushMode();

  const V gv = simde_mm_set1_ps(g);
  for (uint32_t k = 0; k < vectors; ++k) {
    SaveV(base, dst + 16 * k, simde_mm_mul_ps(gv, LoadV(base, src + 16 * k)));
  }
}

Regions RegionsScalar(const PPCContext& ctx, uint8_t*) {
  Regions rg;
  rg.Append(ctx.r1.u32 + 28, 4);
  const uint32_t vectors = VectorsScalar(ctx.r3.u32, ctx.r5.u32);
  if (!Contiguous(ctx.r5.u32 & ~15u, vectors * 16)) {
    rg.valid = false;
  }
  rg.Append(ctx.r6.u32 & ~15u, vectors * 16);
  return rg;
}


bool MixFast(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r3.u32, src = ctx.r5.u32, tmp = ctx.r6.u32, acc = ctx.r7.u32;
  const uint32_t bytes = n * 4;
  if (n == 0 || n > (1u << 20) || (n & 3) || ((src | tmp | acc) & 15) || !Apart(src, tmp, bytes) ||
      !Apart(src, acc, bytes) || !Apart(tmp, acc, bytes) || !Contiguous(src, bytes) || !Contiguous(tmp, bytes) ||
      !Contiguous(acc, bytes)) {
    return false;
  }
  const uint32_t r1 = ctx.r1.u32;
  Save32(Host(base, r1 - 8), uint32_t(ctx.lr));
  Save32(Host(base, r1 - 96), r1);
  ctx.fpscr.disableFlushMode();
  const float g = float(ctx.f1.f64);
  Save(Host(base, r1 - 96 + 28), g);
  ctx.fpscr.enableFlushMode();
  const F4 gain = simde_vdupq_n_f32(g);
  const uint8_t* s = Host(base, src);
  uint8_t* t = Host(base, tmp);
  uint8_t* a = Host(base, acc);
  for (uint32_t k = 0; k < n / 4; ++k) {
    F4 product = simde_vmulq_f32(gain, LoadBE(s + 16 * k));
    Rounded(product);
    StoreBE(t + 16 * k, product);
    StoreBE(a + 16 * k, simde_vaddq_f32(product, LoadBE(a + 16 * k)));
  }
  return true;
}

void Mix(PPCContext& ctx, uint8_t* base) {
  if (AudioFast()) {
    if (MixFast(ctx, base)) {
      g_fast_mix.fast.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_fast_mix.other.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t r1 = ctx.r1.u32;
  const uint32_t n = ctx.r3.u32;
  const uint32_t tmp = ctx.r6.u32;
  const uint32_t acc = ctx.r7.u32;
  Save32(Host(base, r1 - 8), uint32_t(ctx.lr));
  Save32(Host(base, r1 - 96), r1);
  ctx.r1.u64 = r1 - 96;
  Scale(ctx, base);
  ctx.r1.u64 = r1;
  if (n == 0) {
    return;
  }
  const uint32_t vectors = ((n - 1) >> 2) + 1;
  ctx.fpscr.enableFlushMode();
  for (uint32_t k = 0; k < vectors; ++k) {
    SaveV(base, acc + 16 * k, simde_mm_add_ps(LoadV(base, tmp + 16 * k), LoadV(base, acc + 16 * k)));
  }
}

Regions RegionsMix(const PPCContext& ctx, uint8_t*) {
  Regions rg;
  const uint32_t n = ctx.r3.u32;
  rg.Append(ctx.r1.u32 - 96, 96);
  const uint32_t vectors = VectorsScalar(n, ctx.r5.u32);
  const uint32_t vectors_sum = n ? ((n - 1) >> 2) + 1 : 0;
  if (!Contiguous(ctx.r5.u32 & ~15u, vectors * 16)) {
    rg.valid = false;
  }
  rg.Append(ctx.r6.u32 & ~15u, std::max(vectors, vectors_sum) * 16);
  rg.Append(ctx.r7.u32 & ~15u, vectors_sum * 16);
  return rg;
}


void Filter(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r3.u32;
  uint8_t* y = Host(base, ctx.r4.u32);
  uint8_t* state = Host(base, ctx.r7.u32);
  const double a = ctx.f1.f64;
  const double b = ctx.f2.f64;
  ctx.fpscr.disableFlushMode();
  float earlier = Load(state);
  for (uint32_t i = 0; i < n; ++i) {
    const float product = float(double(earlier) * b);
    const float output = float(std::fma(double(Load(y + 4 * i)), a, -double(product)));
    Save(y + 4 * i, output);
    earlier = output;
  }
  Save(state, earlier);
}

Regions RegionsFilter(const PPCContext& ctx, uint8_t*) {
  Regions rg;
  rg.Append(ctx.r4.u32, ctx.r3.u32 * 4);
  rg.Append(ctx.r7.u32, 4);
  return rg;
}


Regions RegionsBiquad(const PPCContext& ctx, uint8_t*) {
  Regions rg;
  rg.Append(ctx.r1.u32 - 288, 288);
  rg.Append(ctx.r4.u32, 16);
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t end = r6 + (ctx.r5.u32 << 2);
  if (r6 < end) {
    rg.Append(r6 & ~15u, (((end - r6 - 1) >> 4) + 1) * 16);
  }
  return rg;
}


Regions RegionsBiquadNative(const PPCContext& ctx, uint8_t*) {
  Regions rg;
  rg.Append(ctx.r4.u32, 16);
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t end = r6 + (ctx.r5.u32 << 2);
  if (r6 < end) {
    rg.Append(r6 & ~15u, (((end - r6 - 1) >> 4) + 1) * 16);
  }
  if (!Contiguous(ctx.r4.u32, 16)) {
    rg.valid = false;
  }
  return rg;
}

inline F4 Dot(F4 c, F4 s) {
  return simde_vdupq_n_f32(simde_vaddvq_f32(simde_vmulq_f32(c, s)));
}
template <int kLane>
inline F4 State(F4 s, F4 u, F4 y) {

  F4 t = simde_vcopyq_laneq_f32(s, 3, s, 2);
  t = simde_vcopyq_laneq_f32(t, 1, s, 0);
  t = simde_vcopyq_laneq_f32(t, 0, u, kLane);
  return simde_vcopyq_laneq_f32(t, 2, y, kLane);
}

void BiquadNative(PPCContext& ctx, uint8_t* base) {
  const uint8_t* coefficients = Host(base, ctx.r3.u32);
  uint8_t* state = Host(base, ctx.r4.u32);
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t end = r6 + (ctx.r5.u32 << 2);
  const float b0 = Load(coefficients), b1 = Load(coefficients + 4), b2 = Load(coefficients + 8);
  const float a1 = Load(coefficients + 12), a2 = Load(coefficients + 16);
  const float c_lanes[4] = {b1, b2, -a1, -a2};
  const float s_lanes[4] = {Load(state), Load(state + 4), Load(state + 8), Load(state + 12)};
  F4 s = simde_vld1q_f32(s_lanes);
  if (r6 < end) {
    ctx.fpscr.enableFlushMode();
    const F4 c = simde_vld1q_f32(c_lanes);
    const F4 b0v = simde_vdupq_n_f32(b0);
    const F4 k = LoadBE(Host(base, BO2_GAME_ADDR(0x82070E10, 0x8206BCA0)));
    const uint32_t vectors = ((end - r6 - 1) >> 4) + 1;
    for (uint32_t i = 0; i < vectors; ++i) {
      uint8_t* at = Host(base, (r6 + 16 * i) & ~15u);
      const F4 u0 = LoadBE(at);
      const F4 y0 = simde_vfmaq_f32(Dot(c, s), u0, b0v);
      const F4 u1 = simde_vaddq_f32(k, u0);
      s = State<0>(s, u1, y0);
      const F4 y1 = simde_vfmaq_f32(Dot(c, s), u1, b0v);
      const F4 u2 = simde_vaddq_f32(k, u1);
      s = State<1>(s, u2, y1);
      const F4 y2 = simde_vfmaq_f32(Dot(c, s), u2, b0v);
      const F4 u3 = simde_vaddq_f32(k, u2);
      s = State<2>(s, u3, y2);
      const F4 y3 = simde_vfmaq_f32(Dot(c, s), u3, b0v);
      const F4 u4 = simde_vaddq_f32(k, u3);
      s = State<3>(s, u4, y3);
      F4 out = simde_vcopyq_laneq_f32(y0, 1, y1, 1);
      out = simde_vcopyq_laneq_f32(out, 2, y2, 2);
      StoreBE(at, simde_vcopyq_laneq_f32(out, 3, y3, 3));
    }
  }
  ctx.fpscr.disableFlushMode();
  float s_out[4];
  simde_vst1q_f32(s_out, s);
  for (int i = 0; i < 4; ++i) {
    Save(state + 4 * i, s_out[i]);
  }
  ctx.r3.u64 = 0;
}


REX_EXTERN(__imp__sub_827D3060);
Regions RegionsFill(const PPCContext& ctx, uint8_t*) {
  Regions rg;
  const uint32_t blocks = (ctx.r5.u32 >> 7) & 0x1FFFFFF;
  if ((ctx.r3.u32 & 127) != 0 || !Contiguous(ctx.r3.u32, blocks * 128)) {
    rg.valid = false;
    return rg;
  }
  rg.Append(ctx.r3.u32, blocks * 128);
  return rg;
}

void Fill(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t blocks = (ctx.r5.u32 >> 7) & 0x1FFFFFF;
  if ((dst & 127) != 0 || !Contiguous(dst, blocks * 128)) {
    __imp__sub_827D3060(ctx, base);
    return;
  }
  std::memset(Host(base, dst), int(ctx.r4.u32 & 0xFF), size_t(blocks) * 128);
}


inline V Perm(V a, V b, VI c) {
  return simde_mm_castsi128_ps(rex::ppc::simde_mm_perm_epi8_(simde_mm_castps_si128(a), simde_mm_castps_si128(b), c));
}
inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

void Reverb(PPCContext& ctx, uint8_t* base) {
  const uint32_t r1 = ctx.r1.u32;
  const uint32_t params = ctx.r4.u32;
  const uint32_t lines = ctx.r5.u32;
  const uint32_t n = ctx.r6.u32;
  const uint32_t buf0 = ctx.r7.u32, buf1 = ctx.r8.u32, buf2 = ctx.r9.u32, buf3 = ctx.r10.u32;


  const uint32_t stack_frame = r1 - BO2_GAME_ADDR(0x310, 0x300);
  Save32(Host(base, stack_frame), r1);
  Save32(Host(base, r1 + 0x24), lines);
  Save32(Host(base, r1 + 0x1C), params);
  {
    const uint64_t lr = ctx.lr;
    ctx.r1.u64 = stack_frame;
    ctx.r5.u64 = n;
#if !defined(BO2_GAME_CAMPAIGN)
    ctx.r11.u64 = 0xFFFFFFFF82AA0000ull;
    ctx.r10.u64 = Load32(Host(base, 0x82AA0000u - 0x3340));
#endif
    ctx.r12.u64 = lr;
    ctx.lr = BO2_GAME_ADDR(0x82593548, 0x824A32BC);
    __imp__sub_82592EE8(ctx, base);
    ctx.r1.u64 = r1;
    ctx.lr = lr;
  }

  auto P = [&](uint32_t off) { return Load(Host(base, params + off)); };


  ctx.fpscr.disableFlushMode();
  const double c0 = Load(Host(base, BO2_GAME_ADDR(0x82000DB0, 0x82000C60)));
  const double c13 = Load(Host(base, BO2_GAME_ADDR(0x820034CC, 0x82002A6C)));
  const float f12 = P(0x18C), f11 = P(0x198), f5 = P(0x188), f8 = P(0x194), f7 = P(0x190);
  const float f10 = float(c0 - double(f12));
  const float f9 = float(c0 - double(f11));
  const float f2 = float(double(f5) * c13);
  const float f6 = float(c0 - double(f8));
  const float f4 = float(c0 - double(f7));
  const float f3 = float(double(f10) * double(P(0x168)));
  const float f1 = float(double(f9) * double(P(0x16C)));
  const float f31 = P(0x1C4);
  const float square = float(double(f31) * double(f31));
  const float square_minus_one = float(double(square) - c0);

  const V v59 = simde_mm_set1_ps(f8), v8 = simde_mm_set1_ps(f6), v58 = simde_mm_set1_ps(f7),
          v4 = simde_mm_set1_ps(f4), v3 = simde_mm_set1_ps(P(0x1AC)), v2 = simde_mm_set1_ps(P(0x170)),
          v57 = simde_mm_set1_ps(f2), v31 = simde_mm_set1_ps(f3), v56 = simde_mm_set1_ps(f12),
          v30 = simde_mm_set1_ps(f1), v55 = simde_mm_set1_ps(f11), v62 = simde_mm_set1_ps(square),
          v61 = simde_mm_set1_ps(square_minus_one);
  const V v54 = LoadV(base, BO2_GAME_ADDR(0x820711F0, 0x8206C070)), v29 = LoadV(base, BO2_GAME_ADDR(0x820711E0, 0x8206C060));
  const VI m100 = Mask(0x08090A0B, 0x0C0D0E0F, 0x00010203, 0x00010203);
  const VI m110 = Mask(0x00010203, 0x10111213, 0, 0);
  const VI m130 = Mask(0x00010203, 0x04050607, 0x10111213, 0);
  const VI m70 = Mask(0x00010203, 0x04050607, 0x08090A0B, 0x10111213);
  const VI m1b0 = Mask(0x0C0D0E0F, 0x08090A0B, 0x04050607, 0x04050607);
  const V k0 = LoadV(base, params + 0), k16 = LoadV(base, params + 16),
          k32 = LoadV(base, params + 32), k48 = LoadV(base, params + 48),
          k128 = LoadV(base, params + 128), k144 = LoadV(base, params + 144),
          k160 = LoadV(base, params + 160), k176 = LoadV(base, params + 176);


  uint32_t t[32];
  for (int i = 0; i < 16; ++i) {
    t[i] = Load32(Host(base, params + 64 + 4 * i));
    t[16 + i] = Load32(Host(base, params + 192 + 4 * i));
  }
  uint8_t* line[4];
  for (int k = 0; k < 4; ++k) {
    line[k] = Host(base, lines + 0x20000u * k);
  }

  V v13 = LoadV(base, params + 256), v11 = LoadV(base, params + 272), v10 = LoadV(base, params + 288),
    v0 = LoadV(base, params + 304), v63 = LoadV(base, params + 320), v60 = LoadV(base, params + 336);
  uint32_t pos = Load32(Host(base, params + 0x250));

  ctx.fpscr.enableFlushMode();
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t p = pos + 0x8000;
    auto tap = [&](int k, int j) { return Load(line[k] + (((p - t[j]) << 2) & 0x1FFFCu)); };
    auto taps_vector = [&](int j) { return Merge(tap(0, j), tap(1, j + 1), tap(2, j + 2), tap(3, j + 3)); };
    const V input = Merge(Load(Host(base, buf0 + 4 * i)), Load(Host(base, buf3 + 4 * i)),
                             Load(Host(base, buf1 + 4 * i)), Load(Host(base, buf2 + 4 * i)));
    const V t480 = taps_vector(0), t496 = taps_vector(4), t512 = taps_vector(8), t528 = taps_vector(12);
    const V t544 = taps_vector(16), t560 = taps_vector(20), t576 = taps_vector(24),
            t592 = taps_vector(28);

    const V a13 = simde_mm_mul_ps(v59, v13);
    const V v41 = simde_mm_mul_ps(v63, v62);
    const V v12 = simde_mm_mul_ps(v55, v0);
    const V v39 = simde_mm_mul_ps(v60, v62);
    const V v9 = simde_mm_mul_ps(v58, v10);
    const V v44 = simde_mm_dp_ps(k128, t544, 0xFF);
    const V v45 = simde_mm_dp_ps(k144, t560, 0xFF);
    const V v36 = simde_mm_dp_ps(k160, t576, 0xFF);
    const V v35 = simde_mm_dp_ps(k0, t480, 0xFF);
    const V v37 = simde_mm_dp_ps(k16, t496, 0xFF);
    const V v34 = simde_mm_dp_ps(k32, t512, 0xFF);
    const V v33 = Perm(v44, v45, m110);
    const V b10 = simde_mm_mul_ps(v56, v11);
    v13 = simde_mm_add_ps(simde_mm_mul_ps(input, v8), a13);
    const V v51 = Perm(v33, v36, m130);
    const V v48 = simde_mm_dp_ps(k48, t528, 0xFF);
    const V v53 = simde_mm_dp_ps(k176, t592, 0xFF);
    const V p63 = Perm(v35, v37, m110);
    const V v49 = Perm(p63, v34, m130);
    const V p0 = Perm(v51, v53, m70);
    const V p11 = Perm(v49, v48, m70);
    v0 = simde_mm_add_ps(simde_mm_mul_ps(p0, v30), v12);
    v11 = simde_mm_add_ps(simde_mm_mul_ps(p11, v31), b10);
    const V c12 = Perm(v0, v0, m1b0);
    const V c10 = Perm(v0, v0, m100);
    V d12 = simde_mm_add_ps(simde_mm_mul_ps(c12, v29), c10);
    d12 = simde_mm_mul_ps(d12, v54);
    v10 = simde_mm_add_ps(simde_mm_mul_ps(d12, v4), v9);
    const V v46 = simde_mm_add_ps(d12, v13);
    V e12 = simde_mm_add_ps(simde_mm_mul_ps(v13, v3), v10);
    const uint32_t write = pos << 2;
    for (int k = 0; k < 4; ++k) {
      Save(line[k] + write, Element(v46, k));
    }
    e12 = simde_mm_add_ps(simde_mm_mul_ps(v11, v2), e12);
    const V g45 = simde_mm_mul_ps(v57, e12);
    const V g44 = simde_mm_mul_ps(g45, v61);
    v63 = simde_mm_sub_ps(g44, v41);
    const V v43 = simde_mm_mul_ps(v63, v61);
    v60 = simde_mm_sub_ps(v43, v39);
    Save(Host(base, buf2 + 4 * i), Element(v60, 0));
    Save(Host(base, buf1 + 4 * i), Element(v60, 1));
    Save(Host(base, buf3 + 4 * i), Element(v60, 2));
    Save(Host(base, buf0 + 4 * i), Element(v60, 3));
    pos = (pos + 1) & 0x7FFF;
  }


  SaveV(base, params + 256, v13);
  Save32(Host(base, params + 592), pos);
  SaveV(base, params + 288, v10);
  SaveV(base, params + 272, v11);
  SaveV(base, params + 304, v0);
  SaveV(base, params + 320, v63);
  SaveV(base, params + 336, v60);
  ctx.fpscr.disableFlushMode();
}


Regions RegionsReverb(const PPCContext& ctx, uint8_t* base) {
  Regions rg;
  const uint32_t n = ctx.r6.u32;
  if (n > 32768) {
    rg.valid = false;
    return rg;
  }
  rg.Append(ctx.r3.u32, 0x68);
  rg.Append(ctx.r4.u32, 0x260);
  rg.Append(ctx.r1.u32 + 0x1C, 4);
  rg.Append(ctx.r1.u32 + 0x24, 4);
  for (uint32_t b : {ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32}) {
    rg.Append(b, 4 * n);
  }
  const uint32_t pos = Load32(Host(base, ctx.r4.u32 + 0x250)) & 0x7FFF;
  const uint32_t first = std::min(n, 32768 - pos);
  for (int k = 0; k < 4; ++k) {
    const uint32_t line = ctx.r5.u32 + 0x20000u * k;
    rg.Append(line + 4 * pos, 4 * first);
    rg.Append(line, 4 * (n - first));
  }
  return rg;
}


struct Resampler {
  uint32_t stack_frame;
  uint32_t clean;
  uint32_t mask;
  uint32_t step_stack_frame;
  bool stereo;
  uint32_t values;
  uint32_t back;
};
constexpr Resampler kMono{288, 512, 0x3FF, 2, false, 256, BO2_GAME_ADDR(0x82440648, 0x82333078)};
constexpr Resampler kStereo{272, 1024, 0x1FF, 4, true, 512, BO2_GAME_ADDR(0x82442584, 0x82334FB4)};

inline uint16_t Load16Raw(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
inline void Save16Raw(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }

inline int64_t Fctidz(double x) {
  return std::isnan(x) ? int64_t(0x8000000000000000ULL)
         : (x > double(LLONG_MAX)) ? LLONG_MAX
                                    : simde_mm_cvttsd_si64(simde_mm_load_sd(&x));
}


inline void CleanBlocks(uint8_t* base, uint32_t dst, uint32_t bytes) {
  for (uint32_t k = 0; k < (bytes >> 7); ++k) {
    std::memset(Host(base, (dst + 128 * k) & ~127u), 0, 128);
  }
}

void Resample(const Resampler& m, PPCContext& ctx, uint8_t* base) {
  const uint32_t input = ctx.r1.u32;
  const uint32_t state = ctx.r3.u32;
  const uint32_t source_kind = ctx.r4.u32;
  const uint32_t position = ctx.r6.u32;
  const uint32_t output = ctx.r7.u32;
  uint32_t wants = ctx.r8.u32;
  const uint32_t b_s0 = ctx.r9.u32, b_s1 = ctx.r10.u32;
  const double tone = ctx.f1.f64;
  const uint32_t b_frac = Load32(Host(base, input + 84));

  Save32(Host(base, input - m.stack_frame), input);
  Save32(Host(base, input + 52), output);
  CleanBlocks(base, b_s0, m.clean);
  CleanBlocks(base, b_s1, m.clean);
  CleanBlocks(base, b_frac, m.clean);

  ctx.fpscr.disableFlushMode();
  const float pos = Load(Host(base, position));
  uint32_t remaining = Load32(Host(base, state + 8));
  const uint32_t written = Load32(Host(base, state + 4));
  const double c = Load(Host(base, BO2_GAME_ADDR(0x82013004, 0x82026878)));
  const uint32_t step = uint32_t(Fctidz(double(float(tone * c))));
  uint32_t frac = uint32_t(Fctidz(double(float(double(pos) * c))));
  const uint32_t available = step ? ((remaining << 15) & 0xFFFF8000u) / step : 0;
  uint32_t i0 = (written - remaining) & m.mask;
  uint32_t i1 = (i0 + 1) & m.mask;
  if (available <= 4) {
    Save32(Host(base, state + 8), 0);
    return;
  }
  if (!m.stereo && available < wants) {
    return;
  }
  if (m.stereo && available <= wants) {
    wants = available;
  }

  const uint8_t* f = Host(base, source_kind);
  uint8_t* s0 = Host(base, b_s0);
  uint8_t* s1 = Host(base, b_s1);
  uint8_t* fr = Host(base, b_frac);
  for (uint32_t i = 0; i < wants; ++i) {
    const uint16_t fraction = __builtin_bswap16(uint16_t(frac));
    const uint8_t* a = f + m.step_stack_frame * i0;
    const uint8_t* b = f + m.step_stack_frame * i1;
    Save16Raw(s0 + 2 * i, Load16Raw(a));
    Save16Raw(s1 + 2 * i, Load16Raw(b));
    Save16Raw(fr + 2 * i, fraction);
    if (m.stereo) {
      Save16Raw(s0 + 512 + 2 * i, Load16Raw(a + 2));
      Save16Raw(s1 + 512 + 2 * i, Load16Raw(b + 2));
      Save16Raw(fr + 512 + 2 * i, fraction);
    }
    frac += step;
    const uint32_t advance = (frac >> 15) & 0x1FFFF;
    frac &= 0x7FFF;
    i0 = (i0 + advance) & m.mask;
    i1 = (i1 + advance) & m.mask;
    remaining -= advance;
  }
  if (wants != 0) {
    Save32(Host(base, state + 8), remaining);
  }
  if (remaining <= 2 || remaining > m.mask + 1) {
    Save32(Host(base, state + 8), 0);
  }


  const uint32_t target[3] = {Load32(Host(base, input + 100)), Load32(Host(base, input + 108)),
                               Load32(Host(base, input + 116))};

  const uint8_t* source[3] = {Host(base, b_s0 & ~15u), Host(base, b_s1 & ~15u), Host(base, b_frac & ~15u)};
  ctx.fpscr.enableFlushMode();
  const bool fast = AudioFast();
  for (int k = 0; k < 3; ++k) {
    uint8_t* d = Host(base, target[k] & ~15u);
    const uint8_t* o = source[k];
    if (fast) {

      for (uint32_t i = 0; i < m.values; i += 8) {
        const simde_int16x8_t h = simde_vreinterpretq_s16_u8(simde_vrev16q_u8(simde_vld1q_u8(o + 2 * i)));
        StoreBE(d + 4 * i, simde_vcvtq_n_f32_s32(simde_vmovl_s16(simde_vget_low_s16(h)), 15));
        StoreBE(d + 4 * i + 16, simde_vcvtq_n_f32_s32(simde_vmovl_high_s16(h), 15));
      }
      continue;
    }
    for (uint32_t i = 0; i < m.values; ++i) {
      const int16_t v = int16_t(__builtin_bswap16(Load16Raw(o + 2 * i)));
      Save(d + 4 * i, float(int32_t(v)) * 0x1p-15f);
    }
  }


  ctx.fpscr.disableFlushMode();
  const double fraction_d = double(float(double(int64_t(uint64_t(frac)))));
  Save(Host(base, position), float(fraction_d * double(Load(Host(base, BO2_GAME_ADDR(0x82020D90, 0x82007B20))))));


  const uint32_t one_minus = Load32(Host(base, input + 92));
  ctx.r1.u64 = input - m.stack_frame;
  ctx.r3.u64 = target[0];
  ctx.r4.u64 = target[1];
  ctx.r5.u64 = target[2];
  ctx.r6.u64 = one_minus;
  ctx.r7.u64 = output;
  ctx.r8.u64 = m.values;
  const uint32_t arrays[5] = {target[0], target[1], target[2], one_minus, output};
  const uint32_t bytes = m.values * 4;
  bool apart = fast;
  for (int i = 0; i < 5 && apart; ++i) {
    apart = (arrays[i] & 15) == 0 && Contiguous(arrays[i], bytes);
    for (int j = 0; j < i && apart; ++j) {
      apart = Apart(arrays[i], arrays[j], bytes);
    }
  }
  if (apart) {
    g_fast_interpolation.fast.fetch_add(1, std::memory_order_relaxed);
    ctx.fpscr.enableFlushMode();
    const F4 one = LoadBE(Host(base, BO2_GAME_ADDR(0x820D5460, 0x82088C70)));
    uint8_t* s0 = Host(base, target[0]);
    uint8_t* s1 = Host(base, target[1]);
    const uint8_t* fr = Host(base, target[2]);
    uint8_t* om = Host(base, one_minus);
    uint8_t* out = Host(base, output);
    for (uint32_t k = 0; k < m.values / 4; ++k) {
      const F4 f = LoadBE(fr + 16 * k);
      const F4 rest = simde_vsubq_f32(one, f);
      F4 a = simde_vmulq_f32(LoadBE(s1 + 16 * k), f);
      F4 b = simde_vmulq_f32(LoadBE(s0 + 16 * k), rest);
      Rounded(a);
      Rounded(b);
      StoreBE(om + 16 * k, rest);
      StoreBE(s1 + 16 * k, a);
      StoreBE(s0 + 16 * k, b);
      StoreBE(out + 16 * k, simde_vaddq_f32(b, a));
    }
  } else {
    if (fast) {
      g_fast_interpolation.other.fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t lr = ctx.lr;
    ctx.lr = m.back;
    __imp__sub_8258C310(ctx, base);
    ctx.lr = lr;
  }
  ctx.r1.u64 = input;
  ctx.fpscr.disableFlushMode();
}

template <const Resampler& m>
void ResampleWith(PPCContext& ctx, uint8_t* base) {
  Resample(m, ctx, base);
}


template <const Resampler& m>
Regions RegionsResampler(const PPCContext& ctx, uint8_t* base) {
  Regions rg;
  const uint32_t input = ctx.r1.u32;
  for (uint32_t b : {ctx.r9.u32, ctx.r10.u32, Load32(Host(base, input + 84))}) {
    rg.Append(b & ~127u, m.clean + 128);
  }
  for (uint32_t off : {92u, 100u, 108u, 116u}) {
    rg.Append(Load32(Host(base, input + off)) & ~15u, m.values * 4 + 16);
  }
  rg.Append(ctx.r7.u32 & ~15u, m.values * 4 + 16);
  rg.Append(ctx.r3.u32 + 8, 4);
  rg.Append(ctx.r6.u32, 4);
  rg.Append(input + 52, 4);
  return rg;
}


Regions RegionsSkin(const PPCContext& ctx, uint8_t*) {
  Regions rg;
  rg.Append(ctx.r7.u32 & ~15u, ctx.r5.u32 * 32);
  return rg;
}


bool SkinningOffInThisArm() {
#if !defined(__aarch64__)
  static const bool check_only = REXCVAR_GET(bo2zm_native_audio_check_only);
  if (!check_only) {
    return true;
  }
#endif
  static const bool ab = REXCVAR_GET(bo2zm_native_skinning_ab);
  if (!ab) {
    return false;
  }
  const int arm = bo2zm::benchmark::AbArm();
  return arm >= 0 && (arm & 1) == 0;
}


Regions RegionsLzx(const PPCContext& ctx, uint8_t* base) {
  Regions rg;
  const uint32_t s = ctx.r3.u32;
  const uint32_t pos = ctx.r4.u32;
  const uint32_t end = ctx.r4.u32 + ctx.r5.u32;
  if (int32_t(pos) < int32_t(end)) {
    rg.Append(Load32(Host(base, s)) + pos, end - pos + 272);
  }
  rg.Append(s + 12, 12);
  rg.Append(s + 11012, 4);
  rg.Append(s + 11952, 12);
  return rg;
}

bool LzxOffInThisArm() {
  static const bool ab = REXCVAR_GET(bo2zm_native_lzx_ab);
  if (!ab) {
    return false;
  }
  const int arm = bo2zm::benchmark::AbArm();
  return arm >= 0 && (arm & 1) == 0;
}
}

REX_EXTERN(__imp__sub_827E7058);

namespace {

struct LzxSide {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> ns{0};
  std::atomic<uint64_t> bytes{0};
};
LzxSide g_lzx_side[2];
std::atomic<uint64_t> g_lzx_turn{0};
std::atomic<uint64_t> g_lzx_next_report{0};

void LzxCompare(PPCContext& ctx, uint8_t* base) {
  const int side = int(g_lzx_turn.fetch_add(1, std::memory_order_relaxed) & 1);
  const uint64_t bytes = ctx.r5.u32;
  const uint64_t t0 = NowNs();
  if (side == 0) {
    bo2zm::native_lzx::Decode(ctx, base);
  } else {
    __imp__sub_827E7058(ctx, base);
  }
  const uint64_t t1 = NowNs();
  g_lzx_side[side].calls.fetch_add(1, std::memory_order_relaxed);
  g_lzx_side[side].ns.fetch_add(t1 - t0, std::memory_order_relaxed);
  g_lzx_side[side].bytes.fetch_add(bytes, std::memory_order_relaxed);
  uint64_t next = g_lzx_next_report.load(std::memory_order_relaxed);
  if (next == 0) {
    g_lzx_next_report.compare_exchange_strong(next, t1 + 10'000'000'000ull);
  } else if (t1 >= next && g_lzx_next_report.compare_exchange_strong(next, t1 + 10'000'000'000ull)) {
    uint64_t n[2], ns[2], b[2];
    for (int i = 0; i < 2; ++i) {
      n[i] = g_lzx_side[i].calls.exchange(0, std::memory_order_relaxed);
      ns[i] = g_lzx_side[i].ns.exchange(0, std::memory_order_relaxed);
      b[i] = g_lzx_side[i].bytes.exchange(0, std::memory_order_relaxed);
    }
    auto per_call = [](uint64_t t, uint64_t c) { return c ? double(t) / double(c) / 1e3 : 0.0; };
    auto per_mb = [](uint64_t t, uint64_t by) { return by ? double(t) / 1e6 / (double(by) / 1e6) : 0.0; };
    REXLOG_INFO("[native_lzx] alternating, last 10 s: rewrite {} calls, {:.1f} us per call, {:.2f} ms per MB "
                "asked; game's code {} calls, {:.1f} us per call, {:.2f} ms per MB asked",
                n[0], per_call(ns[0], n[0]), per_mb(ns[0], b[0]), n[1], per_call(ns[1], n[1]), per_mb(ns[1], b[1]));
  }
}
}

REX_HOOK_RAW(sub_827E7058) {
  if (LzxOffInThisArm()) {
    __imp__sub_827E7058(ctx, base);
    return;
  }
  static const bool compare = REXCVAR_GET(bo2zm_native_lzx_compare);
  if (compare && g_lzx.phase.load(std::memory_order_relaxed) == 1) {
    LzxCompare(ctx, base);
    return;
  }
  Call(g_lzx, ctx, base, __imp__sub_827E7058, bo2zm::native_lzx::Decode, RegionsLzx);
}

REX_EXTERN(__imp__sub_82912D78);
REX_EXTERN(__imp__sub_82913378);
REX_EXTERN(__imp__sub_82913B50);
REX_EXTERN(__imp__sub_82914AA0);

namespace {


enum class OnGpu { kNo, kRecorded, kRecordedAndCpu };
OnGpu SkinOnGpu(int kernel, PPCContext& ctx, uint8_t* base) {
  if (!bo2zm::gpu_skin::WantedNow()) {
    return OnGpu::kNo;
  }
  const bool check = bo2zm::gpu_skin::CheckNext();
  if (!bo2zm::native_skin::RecordGpu(kernel, ctx, base, check)) {
    return OnGpu::kNo;
  }
  return check ? OnGpu::kRecordedAndCpu : OnGpu::kRecorded;
}
}


REX_HOOK_RAW(sub_82912D78) {
  const uint32_t out = ctx.r7.u32 & ~15u, bytes = ctx.r5.u32 * 32;
  if (SkinOnGpu(0, ctx, base) != OnGpu::kRecorded) {
    if (SkinningOffInThisArm()) {
      __imp__sub_82912D78(ctx, base);
    } else {
      Call(g_skin1, ctx, base, __imp__sub_82912D78, bo2zm::native_skin::Skin1, RegionsSkin);
    }
  }
  bo2zm::page_writes::MarkGuest(out, bytes);
}
REX_HOOK_RAW(sub_82913378) {
  const uint32_t out = ctx.r7.u32 & ~15u, bytes = ctx.r5.u32 * 32;
  if (SkinOnGpu(1, ctx, base) != OnGpu::kRecorded) {
    if (SkinningOffInThisArm()) {
      __imp__sub_82913378(ctx, base);
    } else {
      Call(g_skin2, ctx, base, __imp__sub_82913378, bo2zm::native_skin::Skin2, RegionsSkin);
    }
  }
  bo2zm::page_writes::MarkGuest(out, bytes);
}
REX_HOOK_RAW(sub_82913B50) {
  const uint32_t out = ctx.r7.u32 & ~15u, bytes = ctx.r5.u32 * 32;
  if (SkinOnGpu(2, ctx, base) != OnGpu::kRecorded) {
    if (SkinningOffInThisArm()) {
      __imp__sub_82913B50(ctx, base);
    } else {
      Call(g_skin3, ctx, base, __imp__sub_82913B50, bo2zm::native_skin::Skin3, RegionsSkin);
    }
  }
  bo2zm::page_writes::MarkGuest(out, bytes);
}


REX_HOOK_RAW(sub_82914AA0) {
  if (bo2zm::gpu_skin::Enabled()) {
    const uint8_t* groups = rex::memory::GuestPtr<uint8_t*>(base, ctx.r5.u32);
    int32_t total = 0;
    for (int g = 0; g < 4; ++g) {
      total += int16_t(uint16_t(groups[2 * g]) << 8 | groups[2 * g + 1]);
    }
    if (total > 0) {
      bo2zm::gpu_skin::Forget(ctx.r7.u32 & ~15u, uint32_t(total) * 32);
    }
  }
  __imp__sub_82914AA0(ctx, base);
}

REX_EXTERN(__imp__sub_8258B208);
REX_HOOK_RAW(sub_8258B208) {
  if (AudioFast()) {
    Call(g_biquad_native, ctx, base, __imp__sub_8258B208, BiquadNative, RegionsBiquadNative);
    return;
  }
#if defined(BO2ZM_LOCALS_sub_8258B208)
  Call(g_biquad, ctx, base, __imp__sub_8258B208, local_sub_8258B208, RegionsBiquad, true);
#else
  __imp__sub_8258B208(ctx, base);
#endif
}


REX_EXTERN(__imp__sub_8258BE08);
namespace {
Regions RegionsLimiter(const PPCContext& ctx, uint8_t* base) {
  Regions rg;
  const uint32_t n = ctx.r3.u32, s = ctx.r6.u32, x = ctx.r7.u32;
  if (n > 4096 || !Contiguous(s, 24)) {
    rg.valid = false;
    return rg;
  }
  if (n != 0) {
    const uint32_t first = x & ~15u;
    uint64_t end = std::max<uint64_t>(uint64_t(x) + 4ull * n, uint64_t(first) + 16ull * (((n - 1) >> 2) + 1));
    if (n > 4) {
      end = std::max<uint64_t>(end, uint64_t((x + 16) & ~15u) + 16ull * (((n - 5) >> 2) + 1));
    }
    rg.Append(first, uint32_t(end - first));
  }
  rg.Append(s, 24);
  rg.Append(s + ((Load32(Host(base, s)) + 32) << 2), 4 * n);
  rg.Append(ctx.r8.u32, 4);
  return rg;
}
}
REX_HOOK_RAW(sub_8258BE08) {
  if (!bo2zm::native_limiter::OnInThisArm()) {
    __imp__sub_8258BE08(ctx, base);
    return;
  }
  Call(g_limiter, ctx, base, __imp__sub_8258BE08, bo2zm::native_limiter::Limit, RegionsLimiter);
}


REX_HOOK_RAW(sub_827D3060) {
  if (AudioFast()) {
    Call(g_fill, ctx, base, __imp__sub_827D3060, Fill, RegionsFill);
    return;
  }
  __imp__sub_827D3060(ctx, base);
}

REX_EXTERN(__imp__sub_82593500);
REX_HOOK_RAW(sub_82593500) { Call(g_reverb, ctx, base, __imp__sub_82593500, Reverb, RegionsReverb); }

REX_EXTERN(__imp__sub_8243F620);
REX_EXTERN(__imp__sub_82440658);
REX_HOOK_RAW(sub_8243F620) {
  Call(g_voice_mono, ctx, base, __imp__sub_8243F620, ResampleWith<kMono>, RegionsResampler<kMono>);
}
REX_HOOK_RAW(sub_82440658) {
  Call(g_voice_stereo, ctx, base, __imp__sub_82440658, ResampleWith<kStereo>, RegionsResampler<kStereo>);
}

REX_HOOK_RAW(sub_8258AD40) { Call(g_scalar, ctx, base, __imp__sub_8258AD40, Scale, RegionsScalar); }
REX_HOOK_RAW(sub_8258B0D0) { Call(g_mix, ctx, base, __imp__sub_8258B0D0, Mix, RegionsMix); }
REX_HOOK_RAW(sub_8258AE90) {
  if (ctx.r3.u32 == 0) [[unlikely]] {
    __imp__sub_8258AE90(ctx, base);
    return;
  }
  Call(g_filter, ctx, base, __imp__sub_8258AE90, Filter, RegionsFilter);
}


namespace {
constexpr uint32_t kPoolHead = BO2_GAME_ADDR(0x82994E24, 0x82760BA4);
constexpr uint32_t kPoolNodes = 200;
std::atomic<uint64_t> g_pool_taken{0};
std::atomic<uint64_t> g_pool_given{0};
uint64_t g_pool_taken_said = 0;
uint64_t g_pool_given_said = 0;
std::atomic<uint8_t*> g_pool_base{nullptr};
std::atomic<uint64_t> g_pool_most{0};
std::atomic<uint64_t> g_pool_waits{0};
std::atomic<uint64_t> g_pool_wait_us{0};
uint64_t g_pool_waits_said = 0;
constexpr auto kPoolWait = std::chrono::milliseconds(50);


uint32_t PoolFree(uint8_t* base) {
  uint32_t n = 0;
  for (uint32_t node = Load32(Host(base, kPoolHead)); node != 0 && n < 4 * kPoolNodes; ++n) {
    node = Load32(Host(base, node + 4));
  }
  return n;
}

void ReportSoundPool() {
  uint8_t* base = g_pool_base.load(std::memory_order_relaxed);
  if (!base) {
    return;
  }
  const uint64_t taken = g_pool_taken.load(std::memory_order_relaxed);
  const uint64_t given = g_pool_given.load(std::memory_order_relaxed);
  const uint64_t waits = g_pool_waits.load(std::memory_order_relaxed);
  REXLOG_INFO("[sound_pool] last 10 s: {} taken, {} given back, at most {} of {} nodes in use, {} takes waited for an "
              "empty list; {} free now (since start: {} taken, {} given back, {} waits, {} us waiting)",
              taken - g_pool_taken_said, given - g_pool_given_said, g_pool_most.exchange(0, std::memory_order_relaxed),
              kPoolNodes, waits - g_pool_waits_said, PoolFree(base), taken, given, waits,
              g_pool_wait_us.load(std::memory_order_relaxed));
  g_pool_taken_said = taken;
  g_pool_given_said = given;
  g_pool_waits_said = waits;
}
}

REX_EXTERN(__imp__sub_82589CC0);
REX_EXTERN(__imp__sub_82589D48);
REX_HOOK_RAW(sub_82589CC0) {
  g_pool_base.store(base, std::memory_order_relaxed);
  const uint64_t taken = g_pool_taken.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t given = g_pool_given.load(std::memory_order_relaxed);
  const uint64_t in_use = taken > given ? taken - given : 0;
  uint64_t most = g_pool_most.load(std::memory_order_relaxed);
  while (in_use > most && !g_pool_most.compare_exchange_weak(most, in_use, std::memory_order_relaxed)) {
  }
  if (Load32(Host(base, kPoolHead)) == 0) {
    const auto t0 = std::chrono::steady_clock::now();
    while (Load32(Host(base, kPoolHead)) == 0 && std::chrono::steady_clock::now() - t0 < kPoolWait) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const uint64_t us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    const uint64_t n = g_pool_waits.fetch_add(1, std::memory_order_relaxed) + 1;
    g_pool_wait_us.fetch_add(us, std::memory_order_relaxed);
    if (Load32(Host(base, kPoolHead)) == 0) {
      REXLOG_ERROR("[sound_pool] still EMPTY after {} us: the game takes a node from an empty pool and writes "
                   "through a null pointer next (taken {}, given back {} since start)",
                   us, taken, g_pool_given.load(std::memory_order_relaxed));
    } else if (n <= 8) {
      REXLOG_WARN("[sound_pool] the list was empty: waited {} us for a node to come back (wait {})", us, n);
    }
  }
  __imp__sub_82589CC0(ctx, base);
}
REX_HOOK_RAW(sub_82589D48) {
  g_pool_given.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82589D48(ctx, base);
}
