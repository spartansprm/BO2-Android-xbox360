#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t* memory;
bool control;
bool gameplay_mode = true;
std::array<int, 2> jump_levels{1, 4};
std::array<float, 4> fov_scales{1.5f, 0.9f, 1.25f, 1.1f};
constexpr uint32_t actor_ps[] = {0x10000, 0x20000};
constexpr uint32_t position = 0x30000, output = 0x40000, stack = 0x110000, cg_array = 0x90000000;
unsigned control_calls[4]{};
struct Config {
  float global = 1.0f, floor = 1.0f, base_fov = 65.0f, step = 18.0f;
  float ads = 0.6f, viewmodel = 0.8f, weapon_fov = 40.0f, alternate = 33.0f;
  uint32_t weapon = 1;
  bool special = false, reticle = false, no_scale = false, vehicle = false, reaim = false;
  bool mutate_position = false;
} config;
struct Visit {
  uint32_t function, lr;
  bool operator==(const Visit&) const = default;
};
std::vector<Visit> visits;

template <typename T> T Load(uint8_t* base, uint32_t address) {
  T value;
  std::memcpy(&value, base + address, sizeof(value));
  if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
  else if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
  else return __builtin_bswap64(value);
}
template <typename T> void Store(uint8_t* base, uint32_t address, T value) {
  if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
  else value = __builtin_bswap64(value);
  std::memcpy(base + address, &value, sizeof(value));
}
void Put(uint32_t address, uint32_t value) { Store(memory, address, value); }
void PutFloat(uint32_t address, float value) { Put(address, std::bit_cast<uint32_t>(value)); }
float GetFloat(uint32_t address) { return std::bit_cast<float>(Load<uint32_t>(memory, address)); }
bool SameFloat(float a, float b) {
  return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b) || (std::isnan(a) && std::isnan(b));
}
void Commit(uint32_t address, size_t size) {
  assert((address & 4095) == 0 && (size & 4095) == 0);
#ifdef _WIN32
  assert(VirtualAlloc(memory + address, size, MEM_COMMIT, PAGE_READWRITE));
#else
  assert(mprotect(memory + address, size, PROT_READ | PROT_WRITE) == 0);
#endif
}

namespace bo2zm::cheats {
int JumpLevelForState(const uint8_t* base, uint32_t ps) {
  assert(base == memory);
  if (!gameplay_mode) return 0;
  for (unsigned i = 0; i < 2; ++i) if (ps == actor_ps[i]) return jump_levels[i];
  return 0;
}
float FieldOfViewScaleForLocal(int local) {
  if (!gameplay_mode) return 1.0f;
  return local >= 0 && local < 4 ? fov_scales[local] : 0.0f;
}
}

float current_control_scale = 1.0f;
void Fake(uint32_t address, PPCContext& ctx, uint8_t* base) {
  assert(base == memory);
  visits.push_back({address, uint32_t(ctx.lr)});
  switch (address) {
    case 0x82252D70: ctx.r3.u64 = config.weapon; break;
    case 0x826BF970: case 0x826BF988: ctx.r3.u64 = 0; break;
    case 0x82253418: ctx.r3.u64 = config.special; break;
    case 0x821E94D0: ctx.r3.u64 = config.reticle; break;
    case 0x821AF630: ctx.r3.u64 = config.no_scale; break;
    case 0x826B4030: ctx.r3.u64 = config.vehicle; break;
    case 0x826B5048: ctx.r3.u64 = config.reaim; break;
    case 0x82235FD0: ctx.r3.u64 = 0x94002000; break;
    case 0x821AFB58: ctx.f1.f64 = 42.0; break;
    case 0x826C2F30:
      PutFloat(ctx.r4.u32, config.weapon_fov);
      PutFloat(ctx.r5.u32, 0.0f); PutFloat(ctx.r6.u32, 0.0f);
      break;
    case 0x821AF6F8:
      assert(ctx.r4.u32 >= cg_array && ctx.r5.u32 == config.weapon);
      ctx.f1.f64 = float(float(ctx.f1.f64) * (ctx.r7.u32 ? config.viewmodel : config.ads));
      break;
    case 0x82496780:
      switch (ctx.r3.u32) {
        case 1: ctx.f1.f64 = config.base_fov; break;
        case 2: ctx.f1.f64 = config.alternate; break;
        case 3: ctx.f1.f64 = config.floor; break;
        case 4: ctx.f1.f64 = control ? float(config.global * current_control_scale) : config.global; break;
        case 5:
          ctx.f1.f64 = config.step;
          if (config.mutate_position) PutFloat(position + 8, 91.0f);
          break;
        default: assert(false);
      }
      break;
    default: assert(false);
  }
}
namespace bo2zm::guest {
template <uint32_t Address> void Call(PPCContext& ctx, uint8_t* base) { Fake(Address, ctx, base); }
}

#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_GUEST_BARRIER() ((void)0)
#define REX_LOAD_U16(a) Load<uint16_t>(base, uint32_t(a))
#define REX_LOAD_U32(a) Load<uint32_t>(base, uint32_t(a))
#define REX_LOAD_U64(a) Load<uint64_t>(base, uint32_t(a))
#define REX_STORE_U32(a,v) Store<uint32_t>(base, uint32_t(a), uint32_t(v))
#define REX_STORE_U64(a,v) Store<uint64_t>(base, uint32_t(a), uint64_t(v))
#define DEFINE_REX_FUNC(name) extern "C" REX_FUNC(__imp__##name)

#define REX_HOOK_RAW(name) extern "C" REX_FUNC(name)


PPCContext Context(uint32_t a, uint32_t b = 0, uint32_t c = 0) {
  PPCContext ctx{};
  ctx.r1.u64 = stack;
  ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c;
  ctx.r7.u64 = 11; ctx.f1.f64 = 3.5; ctx.lr = 0x12345678;
  return ctx;
}
void Init() {
  config = {};
  PutFloat(0x82000DC4, 0.0f); PutFloat(0x8200130C, 0.1f); PutFloat(0x8200142C, 2.0f);
  PutFloat(0x82000DC0, 0.0f); PutFloat(0x82003608, 55.0f); PutFloat(0x820035E4, 60.0f);
  PutFloat(0x82085654, 39.0f);
  Put(0x82BBAE68, cg_array); Put(0x82C70F44, 4);
  Put(0x82BC2774, 1); Put(0x82BE5DD4, 2); Put(0x82B25EC8, 3); Put(0x82BBC568, 4); Put(0x845BA164, 5);
  for (unsigned l = 0; l < 4; ++l) {
    const auto cg = cg_array + l * 0x87700;
    std::memset(memory + cg, 0, 0x87700);
    Put(cg + 40, 0x94001000);
    Put(0x82BBC554 + l * 4, 0x94000000);
    Put(0x94000000 + 876, 0x40000000);
    PutFloat(0x94002000 + 112, 130.0f);
  }
  visits.clear(); control = false; gameplay_mode = true;
}

void JumpCase(unsigned actor, int level, float start_z, float z, float previous_z,
              int gravity, float velocity, bool alias = false, bool mutate = false, bool mode = true) {
  Init();
  gameplay_mode = mode;
  if (actor < 2) jump_levels[actor] = level;
  const uint32_t ps = actor < 2 ? actor_ps[actor] : 0x48000;
  const uint32_t out = alias ? position + 8 : output;
  const int selected = bo2zm::cheats::JumpLevelForState(memory, ps);
  const float height = selected > 0 && selected < 6 ? kHeights[selected] : 39.0f;
  const auto prepare = [&] {
    std::memset(memory + ps, 0, 2048);
    PutFloat(ps + 196, start_z); PutFloat(ps + 48, z);
    Put(ps + 140, uint32_t(gravity)); PutFloat(ps + 60, velocity);
    PutFloat(position + 8, previous_z); PutFloat(out, alias ? previous_z : -999.0f);
    config.mutate_position = mutate;
    visits.clear();
  };
  const std::array<PPCFunc*, 3> originals{__imp__sub_82687AF8, __imp__sub_82687B80, __imp__sub_82687D98};
  const std::array<PPCFunc*, 3> hooks{sub_82687AF8, sub_82687B80, sub_82687D98};
  for (unsigned i = 0; i < 3; ++i) {
    prepare(); PutFloat(0x82085654, height);
    auto reference = Context(ps, position, out); control = true;
    originals[i](reference, memory);
    const auto expected_visits = visits;
    const float expected_out = GetFloat(out), expected_speed = GetFloat(ps + 60);
    prepare(); PutFloat(0x82085654, 39.0f);
    control_calls[i + 1] = 0;
    auto actual = Context(ps, position, out); control = false;
    hooks[i](actual, memory);
    assert(SameFloat(GetFloat(out), expected_out));
    assert(SameFloat(GetFloat(ps + 60), expected_speed));
    if (i < 2) assert(actual.r3.u64 == reference.r3.u64);
    assert(actual.r1.u64 == stack && actual.r4.u64 == position && actual.r5.u64 == out);
    assert(GetFloat(0x82085654) == 39.0f && Load<uint32_t>(memory, 0x845BA164) == 5);
    assert(visits == expected_visits);
    assert(control_calls[i + 1] == (height == 39.0f ? 1u : 0u));
  }
}

void FovCase(unsigned local, unsigned branch, unsigned viewmodel, float scale, float global, float floor) {
  Init();
  fov_scales[local] = scale;
  const auto cg = cg_array + local * 0x87700, ps = cg + 0x480A8;
  config.global = global; config.floor = floor;
  switch (branch) {
    case 1: config.special = true; break;
    case 2: Put(ps + 1156, 4); PutFloat(ps + 480, 1.0f); break;
    case 3: Put(ps + 1156, 4); PutFloat(ps + 480, 0.0f); break;
    case 4: config.reticle = true; Put(cg + 0x480A0, 1); Put(0x94001000 + 268, 0x4000); break;
    case 5: config.no_scale = true; break;
    case 6: Put(ps + 252, 0x300); break;
    case 7: config.vehicle = true; break;
    case 8: config.vehicle = config.reaim = true; break;
    case 9: Put(ps + 1156, 4); config.weapon = 0; break;
    case 10: config.weapon_fov = std::numeric_limits<float>::quiet_NaN(); Put(ps + 1156, 4); break;
    case 11: Put(ps + 1156, 4); Put(ps + 256, 0x10000000); break;
  }
  const auto input = Context(local, viewmodel);
  auto reference = input; control = true;
  current_control_scale = scale > 0.0f && std::isfinite(scale) ? scale : 1.0f;
  __imp__sub_821AFC90(reference, memory);
  const auto expected_visits = visits;
  visits.clear(); control = false;
  control_calls[0] = 0;
  auto actual = input;
  sub_821AFC90(actual, memory);
  if (!SameFloat(float(reference.f1.f64), float(actual.f1.f64))) {
    std::printf("FOV mismatch local%u branch%u vm%u scale%g global%g floor%g: %g != %g\n",
                local, branch, viewmodel, scale, global, floor, reference.f1.f64, actual.f1.f64);
    std::abort();
  }
  assert(visits == expected_visits);
  assert(control_calls[0] == (!(scale > 0.0f) || !std::isfinite(scale) || scale == 1.0f ? 1u : 0u));
  assert(actual.r1.u64 == stack && GetFloat(0x82085654) == 39.0f);
  assert(Load<uint32_t>(memory, 0x82BBC568) == 4 && Load<uint32_t>(memory, 0x82B25EC8) == 3);
  if (scale > 0.0f && std::isfinite(scale) && scale != 1.0f) {
    actual.f1 = input.f1;
    assert(std::memcmp(&actual, &input, sizeof(input)) == 0);
  }
}

int main() {
  constexpr size_t reservation = size_t(1) << 32;
#ifdef _WIN32
  memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, reservation, MEM_RESERVE, PAGE_NOACCESS)); assert(memory);
#else
  memory = static_cast<uint8_t*>(mmap(nullptr, reservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
#endif
  Commit(0x10000, 0x40000); Commit(0x100000, 0x20000);
  Commit(0x82000000, 0x10000); Commit(0x82085000, 0x1000);
  Commit(0x82B20000, 0x170000); Commit(0x845BA000, 0x1000);
  Commit(cg_array, 0x220000); Commit(0x94000000, 0x3000);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  unsigned jump_cases = 0, fov_cases = 0;
  for (unsigned actor = 0; actor < 2; ++actor) for (int level = 0; level < 6; ++level) {
    const float height = kHeights[level];
    for (float z : {0.0f, height - 0.2f, height - 0.05f, height, height + 1.0f, nan, inf, -inf}) {
      for (int gravity : {-800, 0, 800}) {
        JumpCase(actor, level, 0.0f, z, z - 1.0f, gravity, 2000.0f); ++jump_cases;
      }
    }
    for (float start : {nan, inf, -inf, -0.0f}) {
      JumpCase(actor, level, start, 20.0f, 10.0f, 800, nan); ++jump_cases;
    }
    JumpCase(actor, level, 0.0f, 3.0f, 3.0f, 800, -5.0f, true, true); ++jump_cases;
  }
  for (unsigned local = 0; local < 4; ++local) for (unsigned branch = 0; branch < 12; ++branch)
    for (unsigned vm = 0; vm < 2; ++vm) for (float scale : {0.0f, 1.0f, 0.85f, 1.5f})
      for (float floor : {1.0f, 75.0f, nan}) {
        FovCase(local, branch, vm, scale, 1.2f, floor); ++fov_cases;
      }
  for (const auto actor : {0u, 1u, 2u}) for (const int level : {-1, 0, 5, 6}) {
    JumpCase(actor, level, 0, 20, 10, 800, 1000, false, false, false); ++jump_cases;
    JumpCase(actor, level, 0, 20, 10, 800, 1000); ++jump_cases;
  }
  for (float scale : {nan, inf, -inf, -1.0f}) {
    FovCase(0, 0, 0, scale, 1.0f, 1.0f); ++fov_cases;
  }
#ifdef _WIN32
  assert(VirtualFree(memory, 0, MEM_RELEASE));
#else
  assert(munmap(memory, reservation) == 0);
#endif
  std::printf("PASS: %u jump cases x3 readers and %u FOV cases against generated controls\n", jump_cases, fov_cases);
}
