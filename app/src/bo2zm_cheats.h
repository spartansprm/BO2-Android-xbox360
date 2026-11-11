#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/ppc.h>

#include "bo2zm_pad.h"
#include "bo2zm_player_cheat_state.h"

namespace bo2zm::cheats {

enum Fly { kFlyOff = 0, kNoclip = 1, kUfo = 2 };


int LocalPlayerForController(int controller);
int PrimaryLocalPlayer();
bool IsPrimaryController(int controller);


bool GetPlayerOption(int local, std::string_view option, int& value);
bool SetPlayerOption(int local, std::string_view option, int value);


bool RunPlayerCommand(int local, std::string_view name, std::string_view argument);
uint16_t OnPad(const bo2zm::pad::Pad& pad, int local);

struct CrosshairOverlay {
  float normalizedRect[4]{};
  bool enabled = false;
  int type = 0;
  int color = 0;
};


int GetCrosshairOverlays(CrosshairOverlay* out, int capacity);


void BeginMapLoad();


void EndMapLoad();


int JumpLevelForState(const uint8_t* base, uint32_t ps);
float FieldOfViewScaleForLocal(int local);


void EveryFrame(PPCContext& ctx, uint8_t* base);


void ServerFrame(PPCContext& ctx, uint8_t* base);


bool RunCommand(std::string_view name, std::string_view argument);


int MenuCheats();
void SetMenuCheats(int on);


uint16_t OnPad(const bo2zm::pad::Pad& pad);


bool GodMode();
void SetGodMode(bool on);
int FlyMode();
void SetFlyMode(int mode);
bool FlyCollision();
void SetFlyCollision(bool on);
bool Combo();
void SetCombo(bool on);
int ComboMode();
void SetComboMode(int mode);
int JumpLevel();
void SetJumpLevel(int level);
bool HudHidden();
void SetHudHidden(bool hidden);
bool ThirdPerson();
void SetThirdPerson(bool on);
int ThirdPersonCrosshairSetting();
void SetThirdPersonCrosshair(int mode);
int CrosshairType();
void SetCrosshairType(int type);
int CrosshairColor();
void SetCrosshairColor(int color);

bool ThirdPersonCrosshair();
bool FeatherFall();
void SetFeatherFall(bool on);
bool InfiniteAmmo();
void SetInfiniteAmmo(bool on);
bool ExplosiveBullets();
void SetExplosiveBullets(bool on);
bool NoRecoil();
void SetNoRecoil(bool on);
bool ZombiesIgnore();
void SetZombiesIgnore(bool on);
bool StopSpawns();
void SetStopSpawns(bool on);

void GoToRound(int round);
bool KeepPerksOn();
void SetKeepPerks(bool on);

bool InMatch();


bool GivePackAPunch();
void SetGivePackAPunch(bool on);
bool CrossMapPerks();
void SetCrossMapPerks(bool on);

int CurrentRound();


struct Spot {
  float feet[3];
  float camera[3];
  float angles[3];
};
bool PlayerSpot(Spot& out);


void HoldAt(const float feet[3], float pitch, float yaw);
void ReleaseHold();
void KillZombiesNow();


int ScriptParentsInUse();


int FieldOfView();
void SetFieldOfView(int level);


struct SyncedPlayer {
  int controller = -1;
  int values[detail::kSyncedCount]{};
};
int LocalSyncedPlayers(SyncedPlayer* out, int capacity);


bool SetRemotePlayer(const uint8_t* base, uint64_t xuid, uint32_t sequence, const int* values, int* client,
                     bool* changed);

bool JoinedRemoteGame();

bool LocalServer(const uint8_t* base);


int RunRemoteCommand(const uint8_t* base, uint64_t xuid, std::string_view name, std::string_view argument,
                     bool pack_a_punch, std::string& why);

bool CatalogText(std::string& text, uint32_t& revision);

void ApplyCatalog(PPCContext& ctx, uint8_t* base, std::string_view text);

std::string WorldStateText();


void ApplyWorldState(std::string_view text);


struct RemoteBlastHit {
  uint64_t xuid;
  float at[3];
};
int TakeRemoteBlasts(RemoteBlastHit* out, int capacity);
void PlayRemoteBlast(const float at[3]);

int MoveSpeed();
void SetMoveSpeed(int level);

}
