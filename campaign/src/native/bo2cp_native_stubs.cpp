#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include "bo2zm_benchmark.h"
#include "bo2zm_cheats.h"
#include "bo2zm_mode.h"
#include "bo2zm_updates.h"
#include "bo2zm_debug_options.h"
#include "bo2zm_debug_watch.h"
#include "bo2zm_preset.h"
#include "bo2zm_relaunch.h"
#include "bo2zm_switch.h"
#include "bo2zm_volume_writes.h"
#include "native/render/bo2zm_native_render.h"

namespace bo2zm {
bool CampaignAvailable() { return false; }
}

namespace bo2zm::mode {
bool IsZombies() { return false; }
}

namespace bo2zm::benchmark {

std::atomic<uint64_t> g_frames_seen{0};
void AnnotateFrame() { g_frames_seen.fetch_add(1, std::memory_order_relaxed); }
uint64_t FramesSeen() { return g_frames_seen.load(std::memory_order_relaxed); }
int AbArm() { return -1; }
int AbArmCached() { return -1; }
bool IsActive() { return false; }
bool PathTilts(int&, int&) { return false; }
uint32_t PathButtons() { return 0; }
void ApplyOwnPriority() {}
}


namespace bo2zm::cheats {
int LocalPlayerForController(int) { return -1; }
int PrimaryLocalPlayer() { return -1; }
bool IsPrimaryController(int) { return false; }
bool GetPlayerOption(int, std::string_view, int&) { return false; }
bool SetPlayerOption(int, std::string_view, int) { return false; }
bool RunPlayerCommand(int, std::string_view, std::string_view) { return false; }
uint16_t OnPad(const bo2zm::pad::Pad&, int) { return 0; }
int GetCrosshairOverlays(CrosshairOverlay*, int) { return 0; }
void BeginMapLoad() {}
void EndMapLoad() {}
int JumpLevelForState(const uint8_t*, uint32_t) { return 0; }
float FieldOfViewScaleForLocal(int) { return 1.0f; }
bool RunCommand(std::string_view, std::string_view) { return false; }
uint16_t OnPad(const bo2zm::pad::Pad&) { return 0; }
bool ThirdPersonCrosshair() { return false; }
int CrosshairType() { return 0; }
int CrosshairColor() { return 0; }
void SetCrosshairType(int) {}
void SetCrosshairColor(int) {}
int MenuCheats() { return 0; }
void SetMenuCheats(int) {}
bool GodMode() { return false; }
void SetGodMode(bool) {}
int FlyMode() { return kFlyOff; }
void SetFlyMode(int) {}
bool FlyCollision() { return true; }
void SetFlyCollision(bool) {}
bool Combo() { return false; }
void SetCombo(bool) {}
int ComboMode() { return kNoclip; }
void SetComboMode(int) {}
int JumpLevel() { return 0; }
void SetJumpLevel(int) {}
bool HudHidden() { return false; }
void SetHudHidden(bool) {}
bool ThirdPerson() { return false; }
void SetThirdPerson(bool) {}
int ThirdPersonCrosshairSetting() { return 0; }
void SetThirdPersonCrosshair(int) {}
bool FeatherFall() { return false; }
void SetFeatherFall(bool) {}
bool InfiniteAmmo() { return false; }
void SetInfiniteAmmo(bool) {}
bool ExplosiveBullets() { return false; }
void SetExplosiveBullets(bool) {}
bool NoRecoil() { return false; }
void SetNoRecoil(bool) {}
bool ZombiesIgnore() { return false; }
void SetZombiesIgnore(bool) {}
bool StopSpawns() { return false; }
void SetStopSpawns(bool) {}
void GoToRound(int) {}
bool KeepPerksOn() { return false; }
void SetKeepPerks(bool) {}
bool GivePackAPunch() { return false; }
void SetGivePackAPunch(bool) {}
bool CrossMapPerks() { return false; }
void SetCrossMapPerks(bool) {}
int CurrentRound() { return 0; }
bool PlayerSpot(Spot&) { return false; }
int FieldOfView() { return 0; }
void SetFieldOfView(int) {}
int MoveSpeed() { return 0; }
void SetMoveSpeed(int) {}
}


namespace bo2zm::online {
bool RunCommand(std::string_view, std::string_view) { return false; }
}

namespace bo2zm::debug {
void WatchPhysicalPages(const std::vector<uint32_t>&, Watch, const std::vector<uint64_t>*) {}
bool WatchVertices() { return false; }
}

namespace bo2zm::volume_writes {
bool UseMarks() { return false; }
void TakeMarks(uint32_t, uint32_t, std::vector<uint32_t>&) {}
void Missed(uint32_t) {}
}


namespace bo2zm::updates {
void Command(std::string_view) {}
}
