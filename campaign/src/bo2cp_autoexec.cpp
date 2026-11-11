#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_cheats.h"
#include "bo2zm_dvars.h"
#include "bo2zm_extras.h"
#include "bo2zm_heap_track.h"
#include "bo2zm_name.h"
#include "bo2zm_preset.h"
#include "bo2zm_ui.h"

REXCVAR_DEFINE_STRING(bo2cp_autoexec, "", "BO2",
                      "Test helper: console commands run once on the main thread after bo2cp_autoexec_delay_s, for "
                      "example \"map angola\"")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2cp_autoexec_delay_s, 20, "BO2", "Test helper: seconds before bo2cp_autoexec runs")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2cp_autoexec2, "", "BO2",
                      "Test helper: more console commands, run once after bo2cp_autoexec2_delay_s (for example the "
                      "graphics settings once the mission bo2cp_autoexec started has loaded)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2cp_autoexec2_delay_s, 60, "BO2", "Test helper: seconds before bo2cp_autoexec2 runs")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2cp_test_maps, "", "BO2",
                      "Test helper: missions, comma-separated; each start takes the next one as {map} in "
                      "bo2cp_autoexec and bo2cp_autoexec2 (the position is kept in logs/bo2cp_test_map.txt)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2cp_sv_cheats, false, "BO2",
                    "sv_cheats on and writable, so the campaign's cheat commands (god, ufo, give...) work")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


#if defined(__SWITCH__)
constexpr bool kDirectCampaignDefault = true;
#else
constexpr bool kDirectCampaignDefault = false;
#endif
REXCVAR_DEFINE_BOOL(bo2cp_direct_campaign, kDirectCampaignDefault, "BO2",
                    "The front end starts on the campaign's page: START on the press-start screen is pressed for the "
                    "primary controller, the glasses intro is skipped (the way the game returns from a mission), and "
                    "Back on the mode list stays there. On by default on the Switch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82367760);
REX_EXTERN(__imp__sub_822239E8);
REX_EXTERN(__imp__sub_823CFDA0);
REX_EXTERN(__imp__sub_823CFE40);

bool bo2zm::cheats::InMatch() {
  return false;
}

bool bo2zm::autoexec::CaptureRenderDoc() {
  REXLOG_WARN("[renderdoc] no RenderDoc captures in the campaign");
  return false;
}

void bo2zm::autoexec::ExecuteCommands(PPCContext& ctx, uint8_t* base, std::string_view commands, int controller) {
  if (commands.size() > 2000) {
    REXLOG_WARN("[autoexec] commands too long ({} bytes): skipped", commands.size());
    return;
  }
  const auto saved = ctx;
  const uint32_t text = (ctx.r1.u32 - 4096) & ~uint32_t(0xF);
  std::memcpy(base + text, commands.data(), commands.size());
  base[text + commands.size()] = 0;
  ctx.r1.u64 = (text - 256) & ~uint64_t(0xF);
  ctx.r3.u64 = 0;
  ctx.r4.u64 = uint64_t(controller);
  ctx.r5.u64 = text;
  __imp__sub_82367760(ctx, base);
  ctx = saved;
}

namespace {


std::string NextTestMap() {
  std::vector<std::string> maps;
  const std::string& list = REXCVAR_GET(bo2cp_test_maps);
  for (size_t at = 0; at < list.size();) {
    const size_t comma = std::min(list.find(',', at), list.size());
    if (comma > at) {
      maps.push_back(list.substr(at, comma - at));
    }
    at = comma + 1;
  }
  if (maps.empty()) {
    return {};
  }
  std::error_code ec;
  const auto dir = rex::filesystem::GetExecutableFolder() / "logs";
  std::filesystem::create_directories(dir, ec);
  const auto file = (dir / "bo2cp_test_map.txt").string();
  unsigned position = 0;
  if (FILE* f = std::fopen(file.c_str(), "rb")) {
    if (std::fscanf(f, "%u", &position) != 1) {
      position = 0;
    }
    std::fclose(f);
  }
  const std::string& map = maps[position % maps.size()];
  if (FILE* f = std::fopen(file.c_str(), "wb")) {
    std::fprintf(f, "%u\nlast: %s\n", position + 1, map.c_str());
    std::fclose(f);
  }
  REXLOG_INFO("[autoexec] test mission {} of {}: {}", position % maps.size() + 1, maps.size(), map);
  return map;
}


std::atomic<uint32_t> g_mapname{0};

bool FrontEndLoaded(const uint8_t* base) {
  const uint32_t dvar = g_mapname.load(std::memory_order_relaxed);
  if (!dvar) {
    return false;
  }
  uint32_t text;
  std::memcpy(&text, base + dvar + 0x18, 4);
  text = __builtin_bswap32(text);
  if (text < 0x40000000u) {
    return false;
  }
  return std::strncmp(reinterpret_cast<const char*>(base + text), "frontend", 9) == 0;
}


std::string WithMap(std::string commands, const std::string& map) {
  for (size_t at; (at = commands.find("{map}")) != std::string::npos;) {
    commands.replace(at, 5, map);
  }
  return commands;
}

}


struct Timed {
  std::string commands;
  int delay_s;
  bool done = commands.empty();

  void Run(PPCContext& ctx, uint8_t* base, std::chrono::steady_clock::duration since_start) {
    if (done || since_start < std::chrono::seconds(delay_s)) {
      return;
    }
    done = true;
    REXLOG_INFO("[autoexec] running: {}", commands);
    bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
  }
};

REX_HOOK_RAW(sub_822239E8) {
  bo2zm::extras::PaceFrame();
  __imp__sub_822239E8(ctx, base);
  bo2zm::heap_track::EveryFrame();
  static bool direct_set = false;
  if (!direct_set) {
    direct_set = true;
    bo2zm::autoexec::ExecuteCommands(ctx, base,
                                     REXCVAR_GET(bo2cp_direct_campaign) ? "set bo2zm_cp_direct_ui 1"
                                                                        : "set bo2zm_cp_direct_ui 0");
  }

  static int backdrop_shown = -1;
  if (const int kept = bo2zm::preset::FrontEndSceneKept() ? 1 : 0; kept != backdrop_shown) {
    backdrop_shown = kept;
    bo2zm::autoexec::ExecuteCommands(ctx, base, kept ? "set bo2zm_cp_backdrop_ui 1" : "set bo2zm_cp_backdrop_ui 0");
  }
  bo2zm::preset::SetFrontEnd(FrontEndLoaded(base));
  bo2zm::preset::EveryFrame(ctx, base);
  bo2zm::ui::EveryFrame(ctx, base);
  bo2zm::extras::EveryFrame(ctx, base);
  bo2zm::name::EveryFrame(ctx, base);
  static const std::string map = NextTestMap();
  static Timed first{WithMap(REXCVAR_GET(bo2cp_autoexec), map), REXCVAR_GET(bo2cp_autoexec_delay_s)};
  static Timed second{WithMap(REXCVAR_GET(bo2cp_autoexec2), map), REXCVAR_GET(bo2cp_autoexec2_delay_s)};
  if (first.done && second.done) [[likely]] {
    return;
  }
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  const auto since_start = Clock::now() - start;
  first.Run(ctx, base, since_start);
  second.Run(ctx, base, since_start);
}


REX_HOOK_RAW(sub_823CFDA0) {
  if (REXCVAR_GET(bo2cp_sv_cheats) && ctx.r4.u32 == 1 && ctx.r3.u32 &&
      std::strcmp(reinterpret_cast<const char*>(base + ctx.r3.u32), "sv_cheats") == 0) {
    REXLOG_INFO("[cheats] sv_cheats registered with flags {:X}: on, flags {:X}", ctx.r5.u32,
                ctx.r5.u32 & ~uint32_t(0x50));
    ctx.r5.u64 = ctx.r5.u32 & ~uint32_t(0x50);
    ctx.r6.u64 = (ctx.r6.u64 & 0x00FFFFFFFFFFFFFFull) | (uint64_t(1) << 56);
  }
  const uint32_t name = ctx.r3.u32;
  const bool mapname = name && std::strcmp(reinterpret_cast<const char*>(base + name), "mapname") == 0;
  __imp__sub_823CFDA0(ctx, base);
  if (mapname && ctx.r3.u32) {
    g_mapname.store(ctx.r3.u32, std::memory_order_relaxed);
    REXLOG_INFO("[preset] mapname dvar at {:08X}", ctx.r3.u32);
  }
}


REX_HOOK_RAW(sub_823CFE40) {
  const uint32_t name = ctx.r3.u32;
  __imp__sub_823CFE40(ctx, base);
  bo2zm::dvars::Registered(base, name, ctx.r3.u32, bo2zm::dvars::kBool);
  bo2zm::preset::DvarRegistered(base, name, ctx.r3.u32, 0);
}
