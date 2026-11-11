#include "bo2zm_game.h"
#include "bo2zm_native_limiter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <simde/arm/neon.h>

#include "bo2zm_benchmark.h"

REXCVAR_DEFINE_BOOL(bo2zm_native_limiter, true, "BO2",
                    "The sound limiter sub_8258BE08 (sd_mix_master's five channels) rewritten natively "
                    "(bo2zm_native_limiter.cpp), checked against the game's own first. false = the game's code");
REXCVAR_DEFINE_INT32(bo2zm_native_limiter_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the A/B factor (1 or 2) that is bo2zm_native_limiter (on = the "
                     "rewrite, off = the game's code). 0 = not part of the A/B")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_limiter_pow, true, "BO2",
                    "The limiter rewrite computes its gains with the host's pow (they are rounded to a float right "
                    "after, as the game rounds its own double pow's). false = the game's pow, bit for bit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


REX_EXTERN(__imp__sub_8258BE08);
REX_EXTERN(__imp__sub_82594280);
REX_EXTERN(__imp__sub_82594228);
REX_EXTERN(__imp__sub_82918340);

namespace bo2zm::native_limiter {
namespace {

using Function = void (*)(PPCContext&, uint8_t*);
using F4 = simde_float32x4_t;

constexpr Function kLevel = __imp__sub_82594280;
constexpr Function kMakeup = __imp__sub_82594228;
constexpr Function kPow = __imp__sub_82918340;


constexpr uint32_t kOne = BO2_GAME_ADDR(0x82000DB0, 0x82000C60);
constexpr uint32_t kHalf = BO2_GAME_ADDR(0x82000DB8, 0x82000C68);
constexpr uint32_t kZero = BO2_GAME_ADDR(0x82000DC4, 0x82000C74);
constexpr uint32_t kTwo = BO2_GAME_ADDR(0x8200142C, 0x82001318);
constexpr uint32_t kFour = BO2_GAME_ADDR(0x820035E8, 0x82002A64);
constexpr uint32_t kLeastStart = BO2_GAME_ADDR(0x82003CB4, 0x8202236C);
constexpr uint32_t kTimeFloorD = BO2_GAME_ADDR(0x82004618, 0x82023568);
constexpr uint32_t kRise = BO2_GAME_ADDR(0x820119E0, 0x82019544);
constexpr uint32_t kDcPole = BO2_GAME_ADDR(0x8205B700, 0x8206BCB8);
constexpr uint32_t kTimeFloor = BO2_GAME_ADDR(0x8206926C, 0x8202379C);
constexpr uint32_t kSignMask = BO2_GAME_ADDR(0x82070DE0, 0x8206BC70);
constexpr uint32_t kTiny = BO2_GAME_ADDR(0x82070E08, 0x8206BC98);
constexpr uint32_t kEvery = BO2_GAME_ADDR(0x82A7D29C, 0x8284901C);

inline uint8_t* Host(uint8_t* base, uint32_t guest) {
  return rex::memory::GuestPtr<uint8_t*>(base, guest);
}

inline bool Contiguous(uint32_t guest, uint32_t bytes) {
  const uint64_t end = uint64_t(guest) + bytes;
  return end <= 0x1'0000'0000ull && !(guest < 0xE0000000u && end > 0xE0000000ull);
}
inline bool Apart(uint32_t a, uint32_t a_bytes, uint32_t b, uint32_t b_bytes) {
  return uint64_t(a) + a_bytes <= b || uint64_t(b) + b_bytes <= a;
}
inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void Save32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline float FromBits(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline uint32_t ToBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
inline float Load(const uint8_t* p) { return FromBits(Load32(p)); }
inline void Save(uint8_t* p, float f) { Save32(p, ToBits(f)); }
inline double Load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  v = __builtin_bswap64(v);
  double d;
  std::memcpy(&d, &v, 8);
  return d;
}
inline F4 LoadBE(const uint8_t* p) {
  return simde_vreinterpretq_f32_u8(simde_vrev32q_u8(simde_vld1q_u8(p)));
}
inline void StoreBE(uint8_t* p, F4 v) {
  simde_vst1q_u8(p, simde_vrev32q_u8(simde_vreinterpretq_u8_f32(v)));
}


inline void Fence() { std::atomic_signal_fence(std::memory_order_seq_cst); }
inline void Ready(float& v) {
#if defined(__aarch64__)
  asm volatile("" : "+w"(v));
#elif defined(__x86_64__)
  asm volatile("" : "+x"(v));
#endif
}


void Scale(PPCContext& ctx, uint8_t* x, uint32_t n, float g) {
  ctx.fpscr.enableFlushMode();
  Fence();
  const F4 gain = simde_vdupq_n_f32(g);
  for (uint32_t k = 0; k < n / 4; ++k) {
    StoreBE(x + 16 * k, simde_vmulq_f32(gain, LoadBE(x + 16 * k)));
  }
  Fence();
}


double GameF1(PPCContext& ctx, uint8_t* base, Function f, uint32_t sp, uint32_t back, double f1) {
  const uint64_t r1 = ctx.r1.u64;
  const uint64_t lr = ctx.lr;
  ctx.r1.u64 = sp;
  ctx.lr = back;
  ctx.f1.f64 = f1;
  f(ctx, base);
  ctx.r1.u64 = r1;
  ctx.lr = lr;
  ctx.fpscr.disableFlushMode();
  return ctx.f1.f64;
}


float Coefficient(uint8_t* base, float time, double rate) {
  double t = time;
  if (t < Load64(Host(base, kTimeFloorD))) {
    t = Load(Host(base, kTimeFloor));
  }
  const float one = Load(Host(base, kOne));
  float a = float(t * rate);
  a = a * a;
  const float root = std::sqrt(float(std::fma(double(a), double(Load(Host(base, kFour))), double(one))));
  const float twice = a * Load(Host(base, kTwo));
  return (root - one) / twice;
}


struct Counts {
  std::atomic<uint64_t> native{0}, game{0}, host_pow{0}, game_pow{0};
  uint64_t native_said = 0, game_said = 0, host_pow_said = 0, game_pow_said = 0;
};
Counts g_counts;
std::atomic<uint64_t> g_next_report{0};

uint64_t NowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void MaybeReport() {
  const uint64_t now = NowNs();
  uint64_t next = g_next_report.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_report.compare_exchange_strong(next, now + 10'000'000'000ull);
    return;
  }
  if (now < next || !g_next_report.compare_exchange_strong(next, now + 10'000'000'000ull)) {
    return;
  }
  Counts& c = g_counts;
  const uint64_t native = c.native.load(std::memory_order_relaxed), game = c.game.load(std::memory_order_relaxed);
  const uint64_t host_pow = c.host_pow.load(std::memory_order_relaxed);
  const uint64_t game_pow = c.game_pow.load(std::memory_order_relaxed);
  REXLOG_INFO("[native_limiter] last 10 s: {} blocks native, {} given to the game's function (a shape it does not "
              "take); gains computed with the host's pow {}, with the game's {}",
              native - c.native_said, game - c.game_said, host_pow - c.host_pow_said, game_pow - c.game_pow_said);
  c.native_said = native;
  c.game_said = game;
  c.host_pow_said = host_pow;
  c.game_pow_said = game_pow;
}

}

bool OnInThisArm() {
  static const int factor = REXCVAR_GET(bo2zm_native_limiter_ab);
  if (factor != 0) {
    const int arm = bo2zm::benchmark::AbArm();
    if (arm >= 0) {
      return (arm & factor) != 0;
    }
  }
  return REXCVAR_GET(bo2zm_native_limiter);
}

void Limit(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64;
  const uint32_t n = ctx.r3.u32, p = ctx.r5.u32, s = ctx.r6.u32, x = ctx.r7.u32, out = ctx.r8.u32;
  const uint32_t r1 = ctx.r1.u32;
  const uint32_t bytes = n * 4;
  auto game = [&] {
    g_counts.game.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_8258BE08(ctx, base);
  };
  if (!Contiguous(p, 28) || !Contiguous(s, 24)) {
    game();
    return;
  }
  uint8_t* const P = Host(base, p);
  if (Load32(P) == 0) {
    return;
  }
  uint8_t* const S = Host(base, s);
  const uint32_t position = Load32(S);
  const uint32_t copy = s + ((position + 32) << 2);
  if (n == 0 || n > 512 || (n & 31) != 0 || (x & 15) != 0 || (r1 & 15) != 0 || (copy & 127) != 0 ||
      !Contiguous(x, bytes) || !Contiguous(copy, bytes) || !Contiguous(out, 4) || !Apart(x, bytes, s, 24) ||
      !Apart(x, bytes, copy, bytes) || !Apart(x, bytes, out, 4) || !Apart(x, bytes, p, 28) ||
      !Apart(copy, bytes, s, 24) || !Apart(copy, bytes, out, 4) || !Apart(copy, bytes, p, 28) ||
      !Apart(out, 4, s, 24) || !Apart(out, 4, p, 28)) {
    game();
    return;
  }
  uint8_t* const X = Host(base, x);
  uint8_t* const C = Host(base, copy);
  const uint32_t sp = r1 - 256;


  Scale(ctx, X, n, Load(P + 4));
  ctx.fpscr.disableFlushMode();
  Fence();


  const double rate = ctx.f1.f64;
  const float attack = Coefficient(base, Load(P + 20), rate);
  const float release = Coefficient(base, Load(P + 24), rate);
  const float threshold = Load(P + 12);
  const double level = GameF1(ctx, base, kLevel, sp, BO2_GAME_ADDR(0x8258BE70, 0x8249BBC0), threshold);
  const float one = Load(Host(base, kOne));
  const float slope = one / Load(P + 16);
  const double makeup_db = float(std::fma(double(slope), level, -level));
  const float makeup = float(GameF1(ctx, base, kMakeup, sp, BO2_GAME_ADDR(0x8258BE88, 0x8249BBD8), makeup_db));
  const float exponent = slope - one;
  const float inverse = one / makeup;


  const double pole = Load(Host(base, kDcPole));
  const float tiny = Load(Host(base, kTiny));
  const float zero = Load(Host(base, kZero));
  const float rise = Load(Host(base, kRise));
  uint32_t sign[4];
  for (int k = 0; k < 4; ++k) {
    sign[k] = Load32(Host(base, kSignMask + 4 * k));
  }
  const uint32_t every = Load32(Host(base, kEvery));
  float in_prev = Load(S + 16), dc = Load(S + 20);
  float envelope = Load(S + 4), gain = Load(S + 8);
  float least = Load(Host(base, kLeastStart));
  static const bool host_pow_wanted = REXCVAR_GET(bo2zm_native_limiter_pow);
  const bool host_pow = host_pow_wanted && std::isfinite(exponent) && exponent != std::trunc(exponent) &&
                        std::fabs(exponent) < 1024.0f;
  uint32_t host_pows = 0, game_pows = 0;

  for (uint32_t i = 0; i < n; ++i) {

    const float in = Load(X + 4 * i);
    const float step = in - in_prev;
    in_prev = in;
    dc = float(std::fma(double(dc), pole, double(step)));

    Save(C + 4 * i, dc);
    const float level_in = i < 4 ? dc : FromBits(ToBits(dc) & ~sign[i & 3]);


    const float before = envelope;
    float up = level_in - envelope;
    up = up >= 0.0f ? up : zero;
    float e = float(std::fma(double(up), double(attack), double(envelope)));
    e = float(-std::fma(double(envelope), double(release), -double(e)));
    envelope = e + tiny;


    if ((every & i) == 0 || envelope > before + rise) {
      if (envelope < threshold) {
        gain = makeup;
      } else {
        const float base_value = envelope * threshold;
        double power;
        if (host_pow && base_value > 0.0f && base_value < INFINITY) {
          power = std::pow(double(base_value), double(exponent));
          ++host_pows;
        } else {
          ctx.f2.f64 = exponent;
          power = GameF1(ctx, base, kPow, sp, BO2_GAME_ADDR(0x8258BFD0, 0x8249BD20), base_value);
          ++game_pows;
        }
        gain = float(power) * inverse;
      }
    }

    const float drop = least - gain;
    Save(X + 4 * i, dc * gain);
    least = drop >= 0.0f ? gain : least;
  }


  Save(S + 16, in_prev);
  Save(S + 20, dc);
  Save32(S, (position + n) & 0x1FF);
  Save(S + 8, gain);
  Save(Host(base, out), inverse * least);
  Save(S + 4, envelope);
  float post = Load(P + 8) * Load(Host(base, kHalf));
  Ready(post);
  Fence();
  Scale(ctx, X, n, post);
  ctx.r3.u64 = r3;

  g_counts.native.fetch_add(1, std::memory_order_relaxed);
  if (host_pows) {
    g_counts.host_pow.fetch_add(host_pows, std::memory_order_relaxed);
  }
  if (game_pows) {
    g_counts.game_pow.fetch_add(game_pows, std::memory_order_relaxed);
  }
  MaybeReport();
}

}
