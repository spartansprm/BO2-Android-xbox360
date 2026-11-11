#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


template <typename... Args> void TestLog(const Args&...) {}
#define REXLOG_INFO(...) TestLog(__VA_ARGS__)
struct PPCContext {};
namespace bo2zm::mode {
bool zombies = true;
bool IsZombies() { return zombies; }
}
namespace bo2zm::settings {
std::map<std::string, std::string> saved;
void SetMany(const std::vector<std::pair<std::string, std::string>>& values) {
  for (const auto& value : values) saved[value.first] = value.second;
}
}
namespace bo2zm::autoexec {
std::string commands;
void ExecuteCommands(PPCContext&, uint8_t*, const std::string& value) { commands = value; }
}


void ExpectUI(int profile, int on) {
  PPCContext ctx;
  PublishProfileUI(ctx, nullptr);
  assert(bo2zm::autoexec::commands == "set bo2zm_profile_ui " + std::to_string(profile) +
         "; set bo2zm_profile_on_ui " + std::to_string(on));
}

int main() {
  struct MapCase { const char* loading; int location; const char* label; };
  const MapCase maps[] = {
      {"zm_transit zstandard town", 0, "TOWN"},
      {"zm_transit zgrief farm", 1, "FARM"},
      {"zm_transit zstandard transit", 2, "BUS DEPOT"},
      {"zm_transit zclassic town", 3, "TRANZIT"},
      {"zm_transit_dr zcleansed diner", 4, "DINER"},
      {"zm_nuked zstandard nuked", 5, "NUKETOWN"},
      {"zm_highrise zclassic rooftop", 6, "DIE RISE"},
      {"zm_prison zclassic prison", 7, "MOB OF THE DEAD"},
      {"zm_buried zclassic processing", 8, "BURIED"},
      {"zm_tomb zclassic tomb", 9, "ORIGINS"},
  };
  g_index = kCustom;
  for (int o = 0; o < kNumOptions; ++o) g_custom[o] = kOptions[o].levels - 1;
  g_in_match = true;


  for (const auto& map : maps) {
    g_profiles.fill(Profile{});
    g_selected = 0;
    MapLoading(map.loading);
    assert(g_location == map.location);
    assert(std::string(ProfileLabel(map.location + 1)) == map.label);
    assert(ActiveProfile() == 0 && EditedProfile() == 0);
    ExpectUI(0, 0);
    EnableProfileFromMenu("1");
    assert(ActiveProfile() == map.location + 1 && EditedProfile() == map.location + 1);
    assert(g_profiles[map.location].own);
    assert(PresetOf(ActiveProfile()) == kCustom);
    for (int o = 0; o < kNumOptions; ++o)
      assert(LevelIn(ActiveProfile(), o) == kOptions[o].levels - 1);
    assert(bo2zm::settings::saved.at(std::string("profile_") + kLocations[map.location].key) == "on");
    ExpectUI(map.location + 1, 1);
    EnableProfileFromMenu("0");
    assert(ActiveProfile() == 0 && EditedProfile() == 0);
    ExpectUI(0, 0);
  }
  std::cout << "All 10 locations: GLOBAL -> enabled map profile -> GLOBAL, UI and saved settings passed\n";

  MapLoading("zm_buried zclassic processing");
  EnableProfileFromMenu("1");
  g_profiles[8].preset = kHighest;
  EnableProfileFromMenu("0");
  EnableProfileFromMenu("2");
  assert(ActiveProfile() == 9 && PresetOf(9) == kHighest);

  g_selected = 1;
  SelectProfileFromMenu("2");
  assert(g_selected == 1 && EditedProfile() == 9);
  ExpectUI(9, 1);

  using rex::cvar::Source;
  struct StartupCase { Source source; bool forced; };
  const StartupCase startup[] = {
      {Source::kDefault, false}, {Source::kConfig, false}, {Source::kRuntime, false},
      {Source::kEnvironment, true}, {Source::kCommandLine, true},
  };


  for (const auto& test : startup) {
    for (const bool benchmark : {false, true}) {
      const bool forced = benchmark || test.forced;
      assert(ForceGlobalAtStartup(test.source, benchmark) == forced);
      g_force_global = ForceGlobalAtStartup(test.source, benchmark);
      g_index = kDefault;
      g_profiles[8].enabled = true;
      g_profiles[8].preset = kHighest;
      MapLoading("zm_buried zclassic processing");
      assert(ActiveProfile() == (forced ? 0 : 9));
      assert(EditedProfile() == (forced ? 0 : 9));
      assert(PresetOf(ActiveProfile()) == (forced ? kDefault : kHighest));
      ExpectUI(forced ? 0 : 9, forced ? 0 : 1);

      MapLoading("zm_buried zclassic processing");
      assert(g_force_global == forced);
      g_pending = false;
      EnableProfileFromMenu("1");
      assert(!g_force_global && ActiveProfile() == 9 && EditedProfile() == 9);
      assert(PresetOf(ActiveProfile()) == kHighest);
      assert(g_pending == forced);
      ExpectUI(9, 1);
      MapLoading("zm_buried zclassic processing");
      assert(!g_force_global && ActiveProfile() == 9);


      g_profiles[8] = Profile{};
      g_force_global = ForceGlobalAtStartup(test.source, benchmark);
      ExpectUI(0, 0);
      EnableProfileFromMenu("1");
      assert(!g_force_global && g_profiles[8].own && ActiveProfile() == 9);
      assert(PresetOf(9) == kDefault);
      ExpectUI(9, 1);
    }
  }

  g_force_global = true;
  EnableProfileFromMenu("2");
  assert(!g_force_global && g_profiles[8].enabled && ActiveProfile() == 9);
  g_force_global = true;
  EnableProfileFromMenu("0");
  assert(!g_force_global && !g_profiles[8].enabled && ActiveProfile() == 0);
  ExpectUI(0, 0);
  EnableProfileFromMenu("1");
  ExpectUI(9, 1);
  g_in_match = false;
  g_selected = 9;
  g_force_global = true;
  assert(EditedProfile() == 0);
  ExpectUI(9, 0);
  g_pending = false;
  EnableProfileFromMenu("1");
  assert(!g_force_global && g_pending && EditedProfile() == 9);
  ExpectUI(9, 1);
  g_in_match = true;
  g_selected = 1;
  std::cout << "All cvar sources and benchmark baselines: startup, OFF UI and interactive release passed\n";


  for (const auto mode : {"Multiplayer", "Campaign"}) {
    bo2zm::mode::zombies = false;
    assert(ActiveProfile() == 0 && EditedProfile() == 0);
    ExpectUI(0, 2);
    g_force_global = true;
    EnableProfileFromMenu("0");
    SelectProfileFromMenu("2");
    assert(g_force_global && g_profiles[8].enabled && g_selected == 1);
    g_in_match = false;
    g_selected = 9;
    assert(EditedProfile() == 0);
    ExpectUI(0, 2);
    EnableProfileFromMenu("0");
    SelectProfileFromMenu("2");
    assert(g_force_global && g_profiles[8].enabled && g_selected == 9);
    g_in_match = true;
    g_selected = 1;
    MapLoading("zm_buried zclassic processing");
    assert(g_location == -1);
    std::cout << mode << ": GLOBAL/N/A and ignored profile writes passed\n";
    bo2zm::mode::zombies = true;
    g_force_global = false;
    MapLoading("zm_buried zclassic processing");
    ExpectUI(9, 1);
  }
  for (const auto loading : {"", "mp_hijacked dm", "zm_unknown zclassic town", "zm_transit zstandard unknown"}) {
    MapLoading(loading);
    assert(g_location == -1 && ActiveProfile() == 0);
    ExpectUI(0, 2);
    const auto saved = bo2zm::settings::saved;
    g_force_global = true;
    EnableProfileFromMenu("1");
    assert(g_force_global && bo2zm::settings::saved == saved);
    g_force_global = false;
  }
  g_in_match = false;
  SelectProfileFromMenu("1");
  ExpectUI(1, 0);
  EnableProfileFromMenu("1");
  assert(EditedProfile() == 1);
  ExpectUI(1, 1);
  SelectProfileFromMenu("0");
  assert(EditedProfile() == 0);
  ExpectUI(0, 2);
  g_force_global = true;
  EnableProfileFromMenu("1");
  assert(g_force_global && EditedProfile() == 0);
  std::cout << "Unknown maps, saved profile reuse and front-end selection passed\n";
}
