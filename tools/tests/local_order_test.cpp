#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

constexpr uint32_t manager_address = 0x837F6A28;
constexpr uint32_t primary_address = 0x82976AC8;
constexpr uint32_t modes_address = 0x837F6A80;
constexpr uint32_t stack_address = 0x100000;
uint8_t* memory;
std::array<uint32_t, 4> runtime_owner, connection_owner;
std::vector<std::array<uint32_t, 2>> swaps;
unsigned original_calls, reports;
uint32_t reported_controller;
PPCContext original_result{};

uint32_t Word(uint32_t address) {
  uint32_t value;
  std::memcpy(&value, memory + address, 4);
  return __builtin_bswap32(value);
}
void Store(uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(memory + address, &value, 4);
}
void Commit(uint32_t address) {
  const auto page = address & ~uint32_t(4095);
#ifdef _WIN32
  assert(VirtualAlloc(memory + page, 4096, MEM_COMMIT, PAGE_READWRITE));
#else
  assert(mprotect(memory + page, 4096, PROT_READ | PROT_WRITE) == 0);
#endif
}

extern "C" void __imp__sub_82404358(PPCContext& ctx, uint8_t*) {


  ++original_calls;
  ctx.r7.u64 = 0x12345678;
  ctx.f1.f64 = 2.25;
  ctx.lr = 0x82404474;
  original_result = ctx;
}
void RecordLog(const char*, uint32_t controller) {
  ++reports;
  reported_controller = controller;
}
#define REX_HOOK_RAW(name) extern "C" REX_FUNC(name)
#define REXLOG_INFO(...) RecordLog(__VA_ARGS__)

namespace bo2zm::guest {
template <uint32_t Address> void Call(PPCContext& ctx, uint8_t* base) {
  static_assert(Address == 0x82404188);
  assert(base == memory && original_calls == 1);
  assert(ctx.r1.u64 == original_result.r1.u64);
  const auto a = ctx.r3.u32, b = ctx.r4.u32;
  assert(a < 4 && b < 4);
  swaps.push_back({a, b});

  std::array<uint8_t, 20> saved;
  auto* first = base + manager_address + a * 20;
  auto* second = base + manager_address + b * 20;
  std::memcpy(saved.data(), first, 20);
  std::memcpy(first, second, 20);
  std::memcpy(second, saved.data(), 20);
  Store(manager_address + a * 20 + 4, a);
  Store(manager_address + b * 20 + 4, b);
  std::swap(runtime_owner[a], runtime_owner[b]);
  std::swap(connection_owner[a], connection_owner[b]);
  const auto primary = Word(primary_address);
  if (primary == a) Store(primary_address, b);
  else if (primary == b) Store(primary_address, a);
  ctx.r3.u64 = 0xfedcba;
  ctx.r7.u64 = 99;
  ctx.f1.f64 = 9.5;
  ctx.lr = 0x82404318;
}
}


PPCContext Prepare(const std::vector<uint32_t>& controllers, uint32_t primary) {
  original_calls = reports = 0;
  swaps.clear();
  Store(primary_address, primary);
  Store(modes_address, 4);
  Store(stack_address + 88, 0x824332F8);
  for (uint32_t local = 0; local < 4; ++local) {
    const bool active = local < controllers.size();
    const auto controller = active ? controllers[local] : 0;
    const auto record = manager_address + local * 20;
    Store(record, active ? 0x11 : 0);
    Store(record + 4, local);
    Store(record + 8, controller);
    Store(record + 12, 100 + controller);
    Store(record + 16, 200 + controller);
    runtime_owner[local] = 1000 + controller;
    connection_owner[local] = 2000 + controller;
  }
  PPCContext ctx{};
  ctx.r1.u64 = stack_address;
  ctx.r3.u64 = 15;
  ctx.lr = 0x82271620;
  return ctx;
}
void Run(PPCContext& ctx) {
  sub_82404358(ctx, memory);
  assert(original_calls == 1);
  assert(std::memcmp(&ctx, &original_result, sizeof(ctx)) == 0);
}

int main() {
  constexpr size_t reservation = size_t(1) << 32;
#ifdef _WIN32
  memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, reservation, MEM_RESERVE, PAGE_NOACCESS));
  assert(memory);
#else
  memory = static_cast<uint8_t*>(mmap(nullptr, reservation, PROT_NONE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
#endif


  for (const auto address : {manager_address, primary_address, modes_address, stack_address}) Commit(address);
  unsigned cases = 0;
  for (unsigned mask = 1; mask < 16; ++mask) {
    std::vector<uint32_t> controllers;
    for (unsigned c = 0; c < 4; ++c) if (mask & (1u << c)) controllers.push_back(c);
    for (unsigned primary = 0; primary < controllers.size(); ++primary) {
      for (const auto caller : {0x824332F8u, 0x82298D68u, 0x8227746Cu}) {
        auto ctx = Prepare(controllers, primary);
        if (caller == 0x8227746C) ctx.lr = caller;
        else Store(stack_address + 88, caller);
        Run(ctx);
        auto expected = controllers;
        std::rotate(expected.begin(), expected.begin() + primary, expected.begin() + primary + 1);
        assert(Word(primary_address) == 0);
        assert(swaps.size() == primary && reports == (primary ? 1u : 0u));
        if (primary) assert(reported_controller == controllers[primary]);
        for (uint32_t local = 0; local < expected.size(); ++local) {
          const auto record = manager_address + local * 20;
          assert(Word(record) == 0x11 && Word(record + 4) == local);
          assert(Word(record + 8) == expected[local]);
          assert(Word(record + 12) == 100 + expected[local]);
          assert(Word(record + 16) == 200 + expected[local]);
          assert(runtime_owner[local] == 1000 + expected[local]);
          assert(connection_owner[local] == 2000 + expected[local]);
        }
        ++cases;
      }
    }
  }
  for (unsigned invalid = 0; invalid < 11; ++invalid) {
    auto ctx = Prepare({0, 1, 2}, 2);
    switch (invalid) {
      case 0: ctx.lr = 0x1234; break;
      case 1: Store(stack_address + 88, 0x826110FC); break;
      case 2: Store(modes_address, 2); break;
      case 3: Store(primary_address, 4); break;
      case 4: Store(primary_address, 0xFFFFFFFF); break;
      case 5: Store(manager_address + 20 + 4, 3); break;
      case 6: Store(manager_address + 20, 0); break;
      case 7: Store(manager_address + 20 + 8, 0); break;
      case 8: Store(manager_address + 20 + 8, 4); break;
      case 9: Store(manager_address + 40, 0); break;
      case 10: ctx.r1.u64 = 0xFFFFFFFC; break;
    }
    std::array<uint8_t, 80> before;
    std::memcpy(before.data(), memory + manager_address, before.size());
    const auto before_primary = Word(primary_address);
    Run(ctx);
    assert(swaps.empty() && reports == 0);
    assert(std::memcmp(before.data(), memory + manager_address, before.size()) == 0);
    assert(Word(primary_address) == before_primary);
  }
#ifdef _WIN32
  assert(VirtualFree(memory, 0, MEM_RELEASE));
#else
  assert(munmap(memory, reservation) == 0);
#endif
  std::printf("PASS: %u primary/controller/setup combinations and 11 rejection cases; context retained\n", cases);
}
