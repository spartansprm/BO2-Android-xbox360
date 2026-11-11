#include "bo2zm_dlc.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "bo2zm_autoexec.h"

REXCVAR_DEFINE_BOOL(bo2zm_dlc_maps_seen, true, "BO2",
                    "Marks every Zombies map, start location and mode as already seen in each profile when it loads, "
                    "so installed DLC maps show no NEW badge on the globe");

namespace {

constexpr uint32_t kProfileBlocks = 0x836BAE58;
constexpr uint32_t kProfileBlockSize = 2008;
constexpr uint32_t kProfileLoaded = 2000;
constexpr int kControllers = 4;
constexpr const char* kAllSeen = "setprofile unlock_crumbs_zm 16777215";

bool g_applied[kControllers] = {};

}

void bo2zm::dlc::EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(bo2zm_dlc_maps_seen)) [[unlikely]] {
    return;
  }
  for (int controller = 0; controller < kControllers; ++controller) {
    const bool loaded =
        base[kProfileBlocks + uint32_t(controller) * kProfileBlockSize + kProfileLoaded] != 0;
    if (!loaded) {
      g_applied[controller] = false;
      continue;
    }
    if (g_applied[controller]) [[likely]] {
      continue;
    }
    g_applied[controller] = true;
    REXLOG_INFO("[dlc] profile of controller {} loaded: every Zombies map marked as seen (no NEW badges)", controller);
    bo2zm::autoexec::ExecuteCommands(ctx, base, kAllSeen, controller);
  }
}
