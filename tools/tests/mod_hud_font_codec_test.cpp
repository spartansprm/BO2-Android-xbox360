#include <bit>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <rex/ppc/context.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif
uint8_t* memory;
template<class T> T Load(uint8_t* base, uint32_t at) {
  T value; std::memcpy(&value, base + at, sizeof(value));
  if constexpr (sizeof(T) == 1) return value;
  if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
  if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
  if constexpr (sizeof(T) == 8) return __builtin_bswap64(value);
}
template<class T> void Store(uint8_t* base, uint32_t at, T value) {
  if constexpr (sizeof(T) == 2) value = __builtin_bswap16(value);
  if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
  if constexpr (sizeof(T) == 8) value = __builtin_bswap64(value);
  std::memcpy(base + at, &value, sizeof(value));
}
void Put(uint32_t at, uint32_t value) { Store(memory, at, value); }
void PutFloat(uint32_t at, float value) { Put(at, std::bit_cast<uint32_t>(value)); }
float Float(uint32_t at) { return std::bit_cast<float>(Load<uint32_t>(memory, at)); }
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_GUEST_BARRIER() ((void)0)
#define REX_LOAD_U8(a) Load<uint8_t>(base,uint32_t(a))
#define REX_LOAD_U16(a) Load<uint16_t>(base,uint32_t(a))
#define REX_LOAD_U32(a) Load<uint32_t>(base,uint32_t(a))
#define REX_LOAD_U64(a) Load<uint64_t>(base,uint32_t(a))
#define REX_STORE_U8(a,v) Store<uint8_t>(base,uint32_t(a),uint8_t(v))
#define REX_STORE_U16(a,v) Store<uint16_t>(base,uint32_t(a),uint16_t(v))
#define REX_STORE_U32(a,v) Store<uint32_t>(base,uint32_t(a),uint32_t(v))
#define REX_STORE_U64(a,v) Store<uint64_t>(base,uint32_t(a),uint64_t(v))
#define DEFINE_REX_FUNC(name) void __imp__##name(PPCContext& ctx,uint8_t* base)

void Commit(uint32_t at, size_t size) {
#ifdef _WIN32
  assert(VirtualAlloc(memory + at, size, MEM_COMMIT, PAGE_READWRITE));
#else
  assert(mprotect(memory + at, size, PROT_READ | PROT_WRITE) == 0);
#endif
}
constexpr uint32_t field = 0x10000, old_value = 0x20000, new_value = 0x21000;
constexpr uint32_t decoded = 0x30000, stack = 0x40000, msg = 0x50000, data = 0x60000;
float Roundtrip(float value, uint32_t offset) {
  std::memset(memory + msg, 0, 64);
  Put(msg + 8, data); Put(msg + 16, 256);
  PutFloat(old_value + offset, 1.5f); PutFloat(new_value + offset, value);
  Put(field + 4, offset); Put(field + 8, 4); Put(field + 12, uint32_t(-86));
  PPCContext writer{};
  writer.r1.u64 = stack; writer.r4.u64 = msg; writer.r8.u64 = field;
  writer.r6.u64 = old_value; writer.r7.u64 = new_value;
  memory[stack + 87] = 1; memory[stack + 111] = 0;
  __imp__sub_82425B50(writer, memory);
  assert(writer.r3.u32 == 1 && Load<uint32_t>(memory, msg + 32) == 6);
  const auto bits = memory[data] & 63;
  Put(msg + 28, 0); Put(msg + 32, 0);
  PPCContext reader{};
  reader.r1.u64 = stack; reader.r3.u64 = msg; reader.r5.u64 = old_value;
  reader.r6.u64 = decoded; reader.r7.u64 = field;
  __imp__sub_8241A5F0(reader, memory);
  assert(Load<uint32_t>(memory, msg + 32) == 6);
  const float output = Float(decoded + offset);
  std::printf("font field+%u %.9g -> six-bit %u -> %.9g\n", offset, double(value), unsigned(bits), double(output));
  return output;
}
int main() {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  constexpr size_t size = size_t(1) << 32;
#ifdef _WIN32
  memory = static_cast<uint8_t*>(VirtualAlloc(nullptr,size,MEM_RESERVE,PAGE_NOACCESS)); assert(memory);
#else
  memory = static_cast<uint8_t*>(mmap(nullptr,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)); assert(memory != MAP_FAILED);
#endif
  Commit(0x10000,0x60000); Commit(0x82000000,0x20000); Commit(0x8210E000,0x1000);
  Commit(0x82420000,0x10000);
  PutFloat(0x82000DB0,1); PutFloat(0x82000DB8,0.5f);
  PutFloat(0x82001654,10); PutFloat(0x8200130C,0.1f);
  Store<uint64_t>(memory,0x8200A068,std::bit_cast<uint64_t>(1.0));
  Store<uint64_t>(memory,0x8210E220,std::bit_cast<uint64_t>(1.0e18));
  for (uint32_t offset : {12u,16u}) {
    assert(Roundtrip(1.0f,offset) == 1);
    assert(std::fabs(Roundtrip(1.3f,offset) - 1.3f) < 0.00001f);
    assert(Roundtrip(1.5f,offset) == 1.5f);
    assert(Roundtrip(2.0f,offset) == 2);
    assert(Roundtrip(0.65f,offset) == 7.0f);
    assert(std::fabs(Roundtrip(0.75f,offset) - 7.2f) < 0.00001f);
    assert(Roundtrip(0.5f,offset) > 6);
    assert(std::fabs(Roundtrip(0,offset) - 6.4f) < 0.00001f);
    for (float original : {1.3f,1.5f,2.0f,3.0f,5.0f}) {
      const float safe = std::max(1.0f, float(original * 0.5f));
      assert(std::fabs(Roundtrip(safe,offset) - safe) < 0.00001f);
    }
  }


  PutFloat(decoded + 12,2.5f); PutFloat(decoded + 16,0); Put(decoded + 20,100);
  for (int16_t duration : {-1,0,1000}) {
    Store<uint16_t>(memory,decoded + 82,uint16_t(duration));
    PPCContext interpolation{};
    interpolation.r1.u64 = stack; interpolation.r3.u64 = decoded;
    interpolation.r4.u64 = 1100; interpolation.r5.u64 = decoded + 256;
    __imp__sub_8268EDC0(interpolation,memory);
    assert(Float(decoded + 256) == 2.5f);
    assert(Float(decoded + 16) == 0);
  }
  Roundtrip(2.5f,12); Roundtrip(1.0f,16);
  Put(decoded + 20,100); Store<uint16_t>(memory,decoded + 82,1000);
  for (uint32_t time : {100u,350u,600u,1100u}) {
    PPCContext interpolation{};
    interpolation.r1.u64 = stack; interpolation.r3.u64 = decoded;
    interpolation.r4.u64 = time; interpolation.r5.u64 = decoded + 256;
    __imp__sub_8268EDC0(interpolation,memory);
    assert(Float(decoded + 256) == 1.0f + float(time - 100) / 1000.0f * 1.5f);
  }
  std::puts("PASS actual generated outer writer/field codec/floor/bit transport/reader: both font fields wrap below one; explicit zero is not a wire sentinel");
  std::puts("PASS floored endpoints survive wire transport and stock font animation interpolates the safe decoded endpoints");
  std::puts("PASS stock interpolation ignores dormant/finished from-scale zero without activating a default");
#ifdef _WIN32
  assert(VirtualFree(memory,0,MEM_RELEASE));
#else
  assert(munmap(memory,size) == 0);
#endif
}
