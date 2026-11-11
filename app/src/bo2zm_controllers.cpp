#include "bo2zm_controllers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include "bo2zm_cheats.h"

REXCVAR_DEFINE_BOOL(bo2zm_controller_screen, true, "BO2",
                    "Switch: in the Local lobby a new controller joins through the system's controller screen, "
                    "which a controller outside the lobby brings up by connecting or pressing a button. false = "
                    "the game's own join (each player presses A in the lobby)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::controllers {
namespace {

constexpr uint16_t kStart = 0x0010;
constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;
constexpr int kPlayers = 4;
constexpr int kHandheld = 4;
constexpr int64_t kMs = 1000000;
constexpr int64_t kPressNs = 150 * kMs;
constexpr int64_t kSeenNs = 500 * kMs;
constexpr int64_t kGoneNs = 1000 * kMs;
constexpr int64_t kAfterScreenNs = 600 * kMs;
constexpr int64_t kQuietNs = 1500 * kMs;
constexpr int64_t kReleasedNs = 100 * kMs;
constexpr int64_t kReleaseWaitNs = 3000 * kMs;

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<int> owner{-1};
std::atomic<bool> lobby_open{false};


std::atomic<bool> screen_wanted{false};
std::atomic<int64_t> screen_asked{0};
std::atomic<int64_t> last_held{0};


std::atomic<bool> screen_open{false};
std::atomic<bool> screen_closed{false};
std::atomic<bool> screen_failed{false};
std::atomic<int64_t> quiet_until{0};
std::atomic<int64_t> join_due{0};
std::array<std::atomic<int64_t>, kPlayers> seen{};
std::array<std::atomic<bool>, kPlayers> joined{};
std::array<std::atomic<int64_t>, kPlayers> press_until{};

bool ScreenEnabled() {
#if defined(__SWITCH__) && !defined(BO2_GAME_CAMPAIGN)

  return REXCVAR_GET(bo2zm_controller_screen) && rex::cvar::GetFlagByName("bo2zm_test_start").empty() &&
         !screen_failed.load(std::memory_order_relaxed);
#else
  return false;
#endif
}

bool Connected(int user, int64_t now) {
  return now - seen[user].load(std::memory_order_relaxed) <= kSeenNs;
}

bool InLobby(int user) {
  return user == owner.load(std::memory_order_relaxed) || joined[user].load(std::memory_order_relaxed);
}


bool Gap(int64_t now) {
  const int first = owner.load(std::memory_order_relaxed);
  bool missing = false;
  for (int user = 0; user < kPlayers; ++user) {
    if (user == first) continue;
    if (!Connected(user, now)) missing = true;
    else if (missing) return true;
  }
  return false;
}


std::atomic<bool> handheld_player{true};


int UserForSource(int source, bool handheld) {
  if (source == kHandheld) return handheld ? 0 : -1;
  if (source < 0 || source >= kPlayers) return -1;
  if (!handheld) return source;
  return source + 1 < kPlayers ? source + 1 : -1;
}

#if defined(__SWITCH__)

bool HandheldPlays() {
  return handheld_player.load(std::memory_order_relaxed) && hidGetNpadStyleSet(HidNpadIdType_Handheld) != 0;
}

bool NumberedConnected() {
  for (int source = 0; source < kPlayers; ++source) {
    if (hidGetNpadStyleSet(HidNpadIdType(HidNpadIdType_No1 + source))) return true;
  }
  return false;
}

HidNpadIdType SourceId(int source) {
  return source == kHandheld ? HidNpadIdType_Handheld : HidNpadIdType(HidNpadIdType_No1 + source);
}


void DisconnectOutsiders() {
  const bool handheld = HandheldPlays();
  for (int source = 0; source < kPlayers; ++source) {
    const int user = UserForSource(source, handheld);
    if (user < 0 || InLobby(user) || !hidGetNpadStyleSet(SourceId(source))) {
      continue;
    }
    const Result rc = hidDisconnectNpad(SourceId(source));
    REXLOG_INFO("[controllers] controller {} (system player {}) disconnected before the screen ({:#x})", user,
                source + 1, rc);
  }
}


Result LaunchControllerSupport(const HidLaControllerSupportArg& arg, HidLaControllerSupportResultInfo* info) {
  static_assert(sizeof(HidLaControllerSupportArg) == 0x430 && sizeof(HidLaControllerSupportArgV3) == 0x21c);
  const bool eight_players = hosversionAtLeast(8, 0, 0);
  HidLaControllerSupportArgV3 arg_v3{};
  if (!eight_players) {
    arg_v3.hdr = arg.hdr;
    arg_v3.hdr.player_count_min = std::min<s8>(arg.hdr.player_count_min, 4);
    arg_v3.hdr.player_count_max = std::min<s8>(arg.hdr.player_count_max, 4);
    std::memcpy(arg_v3.identification_color, arg.identification_color, sizeof(arg_v3.identification_color));
    arg_v3.enable_explain_text = arg.enable_explain_text;
    std::memcpy(arg_v3.explain_text, arg.explain_text, sizeof(arg_v3.explain_text));
  }
  const void* arg_data = eight_players ? static_cast<const void*>(&arg) : &arg_v3;
  const size_t arg_size = eight_players ? sizeof(arg) : sizeof(arg_v3);
  u32 version = 0x3;
  if (hosversionAtLeast(11, 0, 0)) version = 0x8;
  else if (hosversionAtLeast(8, 0, 0)) version = 0x7;
  else if (hosversionAtLeast(6, 0, 0)) version = 0x5;
  else if (hosversionAtLeast(3, 0, 0)) version = 0x4;
  HidLaControllerSupportArgPrivate private_arg{};
  private_arg.private_size = sizeof(private_arg);
  private_arg.arg_size = arg_size;
  private_arg.flag1 = 1;
  private_arg.mode = HidLaControllerSupportMode_ShowControllerSupport;
  Result rc = hidGetSupportedNpadStyleSet(&private_arg.npad_style_set);
  HidNpadJoyHoldType hold = HidNpadJoyHoldType_Vertical;
  if (R_SUCCEEDED(rc)) rc = hidGetNpadJoyHoldType(&hold);
  if (R_FAILED(rc)) return rc;
  private_arg.npad_joy_hold_type = hold;
  AppletHolder holder;
  rc = appletCreateLibraryApplet(&holder, AppletId_LibraryAppletController, LibAppletMode_AllForeground);
  if (R_FAILED(rc)) return rc;
  LibAppletArgs common;
  libappletArgsCreate(&common, version);
  rc = libappletArgsPush(&common, &holder);
  if (R_SUCCEEDED(rc)) rc = libappletPushInData(&holder, &private_arg, sizeof(private_arg));
  if (R_SUCCEEDED(rc)) rc = libappletPushInData(&holder, arg_data, arg_size);
  if (R_SUCCEEDED(rc)) rc = libappletStart(&holder);
  if (R_SUCCEEDED(rc)) {
    HidLaControllerSupportResultInfoInternal reply{};
    size_t size = 0;
    libappletPopOutData(&holder, &reply, sizeof(reply), &size);
    if (info) *info = reply.info;
    rc = reply.res;
  }
  appletHolderClose(&holder);
  return rc;
}
#endif

enum class ScreenEnd { kPlayers, kCanceled, kFailed };


ScreenEnd ShowScreen() {
#if defined(__SWITCH__)
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 1;
  arg.hdr.player_count_max = kPlayers;
  arg.hdr.enable_take_over_connection = 1;
  arg.hdr.enable_left_justify = 1;
  arg.hdr.enable_permit_joy_dual = 1;
  arg.hdr.enable_single_mode = 0;
  HidLaControllerSupportResultInfo info{};
  REXLOG_INFO("[controllers] controller screen");
  const Result rc = LaunchControllerSupport(arg, &info);
  if (rc == 0) {
    REXLOG_INFO("[controllers] controller screen closed: {} player(s)", int(info.player_count));
    if (NumberedConnected() && handheld_player.exchange(false, std::memory_order_relaxed)) {
      REXLOG_INFO("[controllers] the screen's player 1 leads the lobby: the attached Joy-Cons no longer play");
    }
    return ScreenEnd::kPlayers;
  }
  if (rc == 2) {
    REXLOG_INFO("[controllers] controller screen closed with B: nobody joins");
    return ScreenEnd::kCanceled;
  }
  REXLOG_WARN("[controllers] controller screen could not open ({:#x}): the game's own join until the next lobby", rc);
#endif
  return ScreenEnd::kFailed;
}


void Join() {
  const int64_t now = Now();
  for (int user = 0; user < kPlayers; ++user) {
    if (InLobby(user) || !Connected(user, now)) {
      continue;
    }
    joined[user].store(true, std::memory_order_relaxed);
    press_until[user].store(now + kPressNs, std::memory_order_relaxed);
    REXLOG_INFO("[controllers] controller {} joins the lobby", user);
  }
}

void AskForScreen() {
  bool none = false;
  if (screen_wanted.compare_exchange_strong(none, true, std::memory_order_relaxed)) {
    screen_asked.store(Now(), std::memory_order_relaxed);
  }
}


void RunScreen() {
  if (screen_open.load(std::memory_order_relaxed) || !screen_wanted.load(std::memory_order_relaxed)) {
    return;
  }
  if (!lobby_open.load(std::memory_order_relaxed)) {
    screen_wanted.store(false, std::memory_order_relaxed);
    return;
  }
  const int64_t now = Now();
  const bool released = now - last_held.load(std::memory_order_relaxed) >= kReleasedNs;
  const bool waited = now - screen_asked.load(std::memory_order_relaxed) >= kReleaseWaitNs;
  if (!released && !waited) {
    return;
  }
  screen_wanted.store(false, std::memory_order_relaxed);
#if defined(__SWITCH__)
  DisconnectOutsiders();
#endif

  static std::thread* thread = nullptr;
  if (thread && thread->joinable()) {
    thread->join();
  }
  screen_open.store(true, std::memory_order_relaxed);
  auto body = []() {
    const ScreenEnd end = ShowScreen();
    if (end == ScreenEnd::kPlayers) {
      screen_closed.store(true, std::memory_order_relaxed);
    } else if (end == ScreenEnd::kFailed) {
      screen_failed.store(true, std::memory_order_relaxed);
    }
    quiet_until.store(Now() + kQuietNs, std::memory_order_relaxed);
    screen_open.store(false, std::memory_order_relaxed);
  };
  try {
    if (thread) {
      *thread = std::thread(body);
    } else {
      thread = new std::thread(body);
    }
  } catch (const std::system_error&) {
    REXLOG_WARN("[controllers] no thread for the controller screen");
    screen_open.store(false, std::memory_order_relaxed);
  }
}


class NpadAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override { devices_ = devices; }


  void DevicesForUser(uint32_t user, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    bool handheld = false;
    bool numbered = false;
    for (const auto& device : devices_) {
      const int source = Source(device.id);
      handheld |= source == kHandheld;
      numbered |= source >= 0 && source < kPlayers;
    }
    if (!numbered) {
      handheld_player.store(true, std::memory_order_relaxed);
    }
    handheld = handheld && handheld_player.load(std::memory_order_relaxed);
    for (const auto& device : devices_) {
      const int source = Source(device.id);
      const int owner_of = source >= 0 ? UserForSource(source, handheld)
                                       : (device.synthetic ? 0 : int(std::min<uint32_t>(device.ordinal, kPlayers)));
      if (owner_of == int(user)) out.push_back(device.id);
    }
  }

 private:

  static int Source(rex::input::DeviceId id) {
    const uint64_t value = uint64_t(id);
    if (((value >> 8) & 0xFFFFFF) != 0x4E5800) return -1;
    return int(value & 0xFF);
  }

  std::vector<rex::input::DeviceInfo> devices_;
};

}

void WrapInput(rex::RuntimeConfig& config) {
#if defined(__SWITCH__) && !defined(BO2_GAME_CAMPAIGN)
  if (!config.input_factory) {
    return;
  }
  auto base = config.input_factory;
  config.input_factory = [base](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {

      static_cast<rex::input::InputSystem*>(system.get())->SetDeviceAssignment(std::make_unique<NpadAssignment>());
    }
    return system;
  };
#else
  (void)config;
#endif
}

PadEdit OnPad(const bo2zm::pad::Pad& pad) {
  PadEdit edit;
#if defined(BO2_GAME_CAMPAIGN)
  return edit;
#endif
  const int user = int(pad.user);
  if (user < 0 || user >= kPlayers) {
    return edit;
  }
  const int64_t now = Now();
  const bool arrived = now - seen[user].exchange(now, std::memory_order_relaxed) > kSeenNs;
  if (pad.buttons) {
    last_held.store(now, std::memory_order_relaxed);
  }
  if (now < press_until[user].load(std::memory_order_relaxed)) {
    edit.add = kA;
  }
  if (screen_open.load(std::memory_order_relaxed)) {
    edit.hide = pad.buttons;
    return edit;
  }
  if (bo2zm::cheats::InMatch()) {
    return edit;
  }
  int first = owner.load(std::memory_order_relaxed);
  const bool pressed = (pad.buttons & ~pad.before) != 0;
  if (!lobby_open.load(std::memory_order_relaxed)) {
    if (first < 0 && pressed && owner.compare_exchange_strong(first, user)) {
      REXLOG_INFO("[controllers] controller {} owns the menus (player 1)", user);
      first = user;
    }

    if (first >= 0 && user != first) {
      edit.hide = pad.buttons;
    }
    return edit;
  }
  if (user == first) {
    return edit;
  }
  if (joined[user].load(std::memory_order_relaxed)) {
    if (pad.Pressed(kB)) {
      joined[user].store(false, std::memory_order_relaxed);
    } else if (pad.Pressed(kA) && ScreenEnabled() && Gap(now)) {
      edit.hide = kA;
      AskForScreen();
    }
    return edit;
  }
  if (!ScreenEnabled()) {
    if (pad.Pressed(kA) || pad.Pressed(kStart)) {
      joined[user].store(true, std::memory_order_relaxed);
    }
    return edit;
  }

  edit.hide = pad.buttons;
  if ((pressed || arrived) && now >= quiet_until.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[controllers] controller {} {}: the controller screen", user, arrived ? "connected" : "pressed");
    AskForScreen();
  }
  return edit;
}

void EveryFrame() {
  if (bo2zm::cheats::InMatch()) {
    lobby_open.store(false, std::memory_order_relaxed);
    return;
  }

  int64_t latest = 0;
  for (const auto& read : seen) latest = std::max(latest, read.load(std::memory_order_relaxed));
  for (int user = 0; user < kPlayers; ++user) {
    if (joined[user].load(std::memory_order_relaxed) && latest - seen[user].load(std::memory_order_relaxed) > kGoneNs) {
      joined[user].store(false, std::memory_order_relaxed);
      REXLOG_INFO("[controllers] controller {} left", user);
    }
  }
  RunScreen();
  if (screen_closed.exchange(false, std::memory_order_relaxed) && lobby_open.load(std::memory_order_relaxed)) {
    join_due.store(Now() + kAfterScreenNs, std::memory_order_relaxed);
  }
  const int64_t due = join_due.load(std::memory_order_relaxed);
  if (due && Now() >= due && lobby_open.load(std::memory_order_relaxed)) {
    join_due.store(0, std::memory_order_relaxed);
    Join();
  }
}

bool RunCommand(std::string_view name, std::string_view argument) {
  if (name != "bo2zm_local_lobby") {
    return false;
  }

  const bool open = !argument.empty() && argument[0] == '1';
  if (open) {
    int leader = -1;
    const auto space = argument.find(' ');
    if (space != std::string_view::npos) {
      std::from_chars(argument.data() + space + 1, argument.data() + argument.size(), leader);
    }
    if (leader >= 0 && leader < kPlayers && owner.exchange(leader, std::memory_order_relaxed) != leader) {
      REXLOG_INFO("[controllers] controller {} leads the Local lobby (player 1)", leader);
    }
  }
  if (open != lobby_open.exchange(open, std::memory_order_relaxed)) {
    for (auto& player : joined) player.store(false, std::memory_order_relaxed);
    join_due.store(0, std::memory_order_relaxed);
    if (open) {
      screen_failed.store(false, std::memory_order_relaxed);

      quiet_until.store(Now() + kQuietNs, std::memory_order_relaxed);
    }
    REXLOG_INFO("[controllers] Local lobby {}", open ? "open" : "closed");
  }
  return true;
}

}
