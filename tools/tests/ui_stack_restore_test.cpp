#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

union PPCRegister { uint64_t u64; uint32_t u32; };
struct PPCContext { PPCRegister r1, r3, r4, r5, r6, r7, r9; uint64_t lr; };

void __imp__sub_824A0BA8(PPCContext&, uint8_t*);
void __imp__sub_824A0BC0(PPCContext&, uint8_t*);
void __imp__sub_82708D68(PPCContext&, uint8_t*);
void __imp__sub_826F2720(PPCContext&, uint8_t*);
void RelocationLog(const char*, uint32_t, uint32_t, uint32_t, uint32_t);
void ErrorLog(const char*, const char*, int, std::string_view);
#define REXLOG_INFO(...) RelocationLog(__VA_ARGS__)
#define REXLOG_ERROR(...) ErrorLog(__VA_ARGS__)
#include "ui_stack_production.inc"

constexpr uint32_t kL = 0x100, kGlobal = 0x300, kError = 0x7000;
constexpr uint32_t kInitialBottom = 0x3000, kDepth = 24, kAllocation = 256;
bool locked = false;
int locks = 0, unlocks = 0, compiles = 0, calls = 0, errors = 0;
int compile_status = 0, call_status = 0;
uint32_t compile_destination = 0, call_destination = 0;
uint32_t result_value = 1, result_type = 1;
bool produce_result = true;
std::string failed_stage, failed_message;
struct Move { uint32_t before, after, count, depth; };
std::vector<Move> moves;
struct Freed { uint32_t address; std::array<uint8_t, kAllocation> poison; };
std::vector<Freed> freed;

void RelocationLog(const char* format, uint32_t before, uint32_t after, uint32_t count, uint32_t depth) {
  assert(locked && std::string_view(format).find("stack relocated") != std::string_view::npos);
  moves.push_back({before, after, count, depth});
}
void ErrorLog(const char*, const char* stage, int status, std::string_view message) {
  assert(locked && status != 0);
  ++errors;
  failed_stage = stage;
  failed_message = message;
}
void __imp__sub_824A0BA8(PPCContext& ctx, uint8_t*) {
  assert(ctx.r3.u32 == kGameLock && !locked);
  assert(ctx.r1.u32 == ((0x9000 - 512) & ~15u));
  locked = true;
  ++locks;
}
void __imp__sub_824A0BC0(PPCContext& ctx, uint8_t* base) {
  assert(ctx.r3.u32 == kGameLock && locked);
  assert(Load32(base, kGlobal + kGlobalSecurityMode) == 2);
  locked = false;
  ++unlocks;
  ctx.r9.u64 = 0xDEADBEEF;
  ctx.lr = 0xBAD;
}

static void Relocate(uint8_t* base, uint32_t destination) {
  if (!destination) return;
  const auto before = Load32(base, kL + kStateStackBottom);
  const auto offset = Load32(base, kL + kStateTop) - before;
  std::memcpy(base + destination, base + before, kAllocation);
  Freed old{before, {}};
  old.poison.fill(0xA5);
  std::memcpy(base + before, old.poison.data(), kAllocation);
  freed.push_back(old);
  Store32(base, kL + kStateStackBottom, destination);
  Store32(base, kL + kStateTop, destination + offset);
}
static void Push(uint8_t* base, uint32_t value, uint32_t type) {
  const auto top = Load32(base, kL + kStateTop);
  Store32(base, top, value);
  Store32(base, top + 4, type);
  Store32(base, kL + kStateTop, top + 8);
}
void __imp__sub_82708D68(PPCContext& ctx, uint8_t* base) {
  assert(locked && ctx.r3.u32 == kL);
  assert(ctx.r4.u32 == kGlobal + kGlobalCompilerSettings);
  assert(ctx.r5.u32 == 0x6000 && ctx.r6.u32 == 7 && ctx.r7.u32 == 0x6100);
  assert(Load32(base, kGlobal + kGlobalSecurityMode) == 0);
  ++compiles;
  Relocate(base, compile_destination);
  if (compile_status == 0) Push(base, 0xF00, 9);
  else Push(base, kError, kTypeString);
  ctx.r3.u64 = uint32_t(compile_status);
}
void __imp__sub_826F2720(PPCContext& ctx, uint8_t* base) {
  assert(locked && ctx.r3.u32 == kL && ctx.r4.u32 == 0 && ctx.r5.u32 == 1 && ctx.r6.u32 == 0);
  assert(Load32(base, kGlobal + kGlobalSecurityMode) == 2);
  ++calls;
  Relocate(base, call_destination);
  Store32(base, kL + kStateTop, Load32(base, kL + kStateTop) - 8);
  if (call_status) Push(base, kError, kTypeString);
  else if (produce_result) Push(base, result_value, result_type);
  ctx.r3.u64 = uint32_t(call_status);
}

static void RunCase(uint32_t compile_move, uint32_t call_move, int compile_error = 0, int run_error = 0,
                    uint32_t value = 1, uint32_t type = 1, bool output = true, uint32_t attempts = 0) {
  std::array<uint8_t, 0x10000> memory{};
  auto* base = memory.data();
  locked = false;
  locks = unlocks = compiles = calls = errors = 0;
  compile_destination = compile_move; call_destination = call_move;
  compile_status = compile_error; call_status = run_error;
  result_value = value; result_type = type; produce_result = output;
  g_attempts = attempts;
  moves.clear(); freed.clear(); failed_stage.clear(); failed_message.clear();
  const auto old_count = g_stack_relocations;
  Store32(base, kL + kStateGlobal, kGlobal);
  Store32(base, kGlobal + kGlobalSecurityMode, 2);
  Store32(base, kL + kStateStackBottom, kInitialBottom);
  Store32(base, kL + kStateTop, kInitialBottom + kDepth);
  std::array<uint8_t, kDepth> retained{};
  for (uint32_t i = 0; i < kDepth; ++i) retained[i] = uint8_t(i + 1);
  std::memcpy(base + kInitialBottom, retained.data(), retained.size());
  std::strcpy(reinterpret_cast<char*>(base + kError), "fixture guest error");
  PPCContext ctx;
  std::memset(&ctx, 0x6B, sizeof(ctx));
  ctx.r1.u64 = 0x9000;
  const auto saved = ctx;
  const bool installed = RunLua(ctx, base, kL, 0x6000, 7, 0x6100);
  assert(installed == (!compile_error && !run_error && output && type == 1 && value != 0));
  const auto bottom = Load32(base, kL + kStateStackBottom);
  assert(Load32(base, kL + kStateTop) == bottom + kDepth);
  assert(std::memcmp(&ctx, &saved, sizeof(ctx)) == 0);
  assert(!locked && locks == 1 && unlocks == 1 && compiles == 1 && calls == !compile_error);
  assert(std::memcmp(base + bottom, retained.data(), retained.size()) == 0);
  const bool changed = bottom != kInitialBottom;
  assert(moves.size() == size_t(changed) && g_stack_relocations == old_count + changed);
  if (changed) {
    const auto& log = moves.front();
    assert(log.before == kInitialBottom && log.after == bottom && log.count == old_count + 1);
    assert(log.depth == kDepth / 8);
  }
  const bool report_error = (compile_error || run_error) && attempts <= 3;
  assert(errors == report_error);
  if (report_error) {
    assert(failed_stage == (compile_error ? "compile" : "run"));
    assert(failed_message == "fixture guest error");
  }


  Push(base, 0xBEEF, 3);
  for (const auto& old : freed) {
    assert(std::memcmp(base + old.address, old.poison.data(), kAllocation) == 0);
  }
}

int main() {
  RunCase(0, 0);
  for (auto moved : {0x1000u, 0x5000u}) {
    RunCase(moved, 0);
    RunCase(0, moved);
    RunCase(moved, 0, 7);
    RunCase(0, moved, 0, 5);
    RunCase(moved, 0, 0, 0, 0);
    RunCase(0, moved, 0, 0, 1, 3);
    RunCase(0, moved, 0, 0, 1, 1, false);
    RunCase(0, moved, 0, 5, 1, 1, true, 4);
  }
  RunCase(0x1000, 0x5000);
  std::cout << "RunLua: 18 relocation/result/error/lock/context cases passed\n";
}
