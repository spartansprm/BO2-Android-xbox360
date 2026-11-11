#include "bo2zm_debug_options.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "bo2zm_cheats.h"
#include "bo2zm_preset.h"
#include "bo2zm_settings.h"
#include "bo2zm_switch.h"
#include "native/render/bo2zm_native_capture.h"

#if defined(__SWITCH__)
extern "C" const char* RexSwitchLogDir(void);
extern "C" void RexSwitchPerfSetReport(bool on);
#endif

REXCVAR_DEFINE_STRING(bo2zm_max_log_files, "10", "BO2",
                      "Logs kept, one per start: \"5\", \"10\", \"20\" or \"off\" (every log; test tomls). Set here, it "
                      "wins over MAX LOG FILES on the DEBUG page")
    .allowed({"5", "10", "20", "off"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

std::atomic<int> g_textures{0};
std::atomic<int> g_constants{0};
std::atomic<int> g_caches{0};
std::atomic<int> g_safe{0};
std::atomic<int> g_log{0};
std::atomic<int> g_profile{0};
std::atomic<int> g_enabled{0};

bool On() {
  return g_enabled.load(std::memory_order_relaxed) != 0;
}


bool g_level_given = false;
std::string g_given_level = "info";
std::string g_applied_level;
bool g_quiet_start = false;


constexpr const char* kMaxLogNames[] = {"5", "10", "20", "off"};
constexpr int kMaxLogCounts[] = {5, 10, 20, 0};
constexpr int kDefaultMaxLogs = 1;
std::atomic<int> g_max_logs{kDefaultMaxLogs};
int g_logs_kept = kMaxLogCounts[kDefaultMaxLogs];


std::atomic<int> g_purge_tier{0};
std::atomic<bool> g_purge_count{false};
std::string g_purge_label = "COUNTING...";

void SetLevel(const std::string& level) {
  if (level != g_applied_level) {
    g_applied_level = level;
    rex::cvar::SetFlagByName("log_level", level);
  }
}


void ApplyReport() {
#if defined(__SWITCH__)
  RexSwitchPerfSetReport(g_level_given || On());
#endif
}


std::string SavedValue(const std::filesystem::path& file, std::string_view key) {
  const auto trim = [](const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    const size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
  };
  std::ifstream in(file);
  std::string line;
  std::string value;
  while (std::getline(in, line)) {
    const size_t equals = line.find('=');
    if (equals != std::string::npos && trim(line.substr(0, equals)) == key) {
      value = trim(line.substr(equals + 1));
    }
  }
  return value;
}


int LogNumber(const std::string& name) {
  constexpr std::string_view kPrefix = "bo2zm_";
  const auto digits = [](std::string_view text) {
    return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
  };
  if (name.size() <= kPrefix.size() + 4 || name.compare(0, kPrefix.size(), kPrefix) != 0 ||
      name.compare(name.size() - 4, 4, ".log") != 0) {
    return -1;
  }
  const std::string_view rest = std::string_view(name).substr(kPrefix.size(), name.size() - kPrefix.size() - 4);
  const size_t dot = rest.find('.');
  if (!digits(rest.substr(0, dot)) || (dot != std::string_view::npos && !digits(rest.substr(dot + 1))) ||
      rest.substr(0, dot).size() > 6) {
    return -1;
  }
  return std::stoi(std::string(rest.substr(0, dot)));
}


int CurrentLogNumber() {
  if (!rex::cvar::GetFlagByName("log_file").empty()) {
    return -1;
  }
  return LogNumber(rex::CurrentLogFile().filename().string());
}


void PruneLogs(const std::filesystem::path& dir, int keep) {
  std::map<int, std::vector<std::filesystem::path>> by_number;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (const int number = LogNumber(entry.path().filename().string()); number >= 0 && entry.is_regular_file(ec)) {
      by_number[number].push_back(entry.path());
    }
  }
  int kept = 0;
  int removed = 0;
  for (auto it = by_number.rbegin(); it != by_number.rend(); ++it) {
    if (++kept <= keep) {
      continue;
    }
    for (const auto& path : it->second) {
      removed += std::filesystem::remove(path, ec) ? 1 : 0;
    }
  }
  if (removed) {
    REXLOG_INFO("[debug] MAX LOG FILES {}: {} older log files deleted", keep, removed);
  }
}


struct Found {
  std::vector<std::filesystem::path> files;
  uintmax_t bytes = 0;
};

void AddFile(Found& found, const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    found.bytes += std::filesystem::file_size(path, ec);
    found.files.push_back(path);
  }
}


template <typename Keep>
void AddFiles(Found& found, const std::filesystem::path& dir, bool recurse, Keep keep) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return;
  }
  const auto add = [&](const std::filesystem::directory_entry& entry) {
    if (entry.is_regular_file(ec) && keep(entry.path().filename().string())) {
      AddFile(found, entry.path());
    }
  };
  if (recurse) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
      add(entry);
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      add(entry);
    }
  }
}

bool EndsWith(const std::string& text, std::string_view end) {
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}


Found Collect(int tier) {
  Found found;
  const std::filesystem::path exe = rex::filesystem::GetExecutableFolder();
  if (exe.empty()) {
    return found;
  }
  std::vector<std::filesystem::path> log_dirs = {exe / "logs"};
  if (const auto current = rex::CurrentLogFile().parent_path(); !current.empty() && current != log_dirs[0]) {
    log_dirs.push_back(current);
  }
  const std::filesystem::path user = bo2zm::settings::File().parent_path();
  const bool all = tier == 3;
  if (all || tier == 0) {
    const int current = CurrentLogNumber();
    for (const auto& dir : log_dirs) {
      AddFiles(found, dir, false, [&](const std::string& name) {
        const int number = LogNumber(name);
        return (number >= 0 && number != current) || name == "marks.txt";
      });
    }
    AddFiles(found, exe / "logs" / "rex", false,
             [](const std::string& name) { return EndsWith(name, ".log") && name != "rex_stderr.log"; });
  }
  if (all || tier == 1) {
    AddFiles(found, exe / "captures", false, [](const std::string& name) { return EndsWith(name, ".png"); });
  }
  if (all || tier == 2) {
    AddFiles(found, exe / "d3d_shaders", true, [](const std::string&) { return true; });
    AddFiles(found, exe / "pgo", true, [](const std::string&) { return true; });
    for (const auto& dir : log_dirs) {
      AddFile(found, dir / "bo2zm_prewarm_list.bin");
    }
    if (!user.empty()) {
      AddFiles(found, user, false, [](const std::string& name) {
        return name.rfind("launch_capture_", 0) == 0 && EndsWith(name, ".bin");
      });
    }
    AddFiles(found, exe, false,
             [](const std::string& name) { return name.rfind("pc_profile_", 0) == 0 && EndsWith(name, ".txt"); });
  }
  return found;
}

std::string Amount(size_t files, uintmax_t bytes) {
  const std::string size = bytes >= 1024 * 1024 ? fmt::format("{:.1f} MB", double(bytes) / (1024.0 * 1024.0))
                                                : fmt::format("{} KB", (bytes + 1023) / 1024);
  return fmt::format("{} FILE{} ({})", files, files == 1 ? "" : "S", size);
}


constexpr int kNumPicks = 6;
using Picks = std::array<int, kNumPicks>;
constexpr Picks kPresets[] = {
    {0, 0, 0, 0, 0, 0},
    {1, 0, 0, 0, 0, 0},
    {1, 0, 0, 0, 0, 1},
    {0, 1, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 1},
};
constexpr int kNumPresets = int(std::size(kPresets));

struct SavedPick {
  const char* key;
  const char* names[2];
};
constexpr SavedPick kSaved[kNumPicks] = {
    {"debug_log_detail", {"normal", "detailed"}},
    {"debug_stack_profile", {"off", "on"}},
    {"debug_texture_checks", {"fast", "full"}},
    {"debug_shader_constants", {"fast", "full"}},
    {"debug_geometry_caches", {"on", "off"}},
    {"debug_safe_renderer", {"off", "on"}},
};
std::atomic<int> g_preset{1};
Picks g_custom{};
std::string g_hash_sampling;
bool g_hash_full = false;
int g_marks = 0;
std::atomic<int> g_mark_location{0};
int g_locations = 0;


void ApplyHashSampling() {
  const bool full = bo2zm::debug::TextureShortcutsOff();
  if (full == g_hash_full) {
    return;
  }
  g_hash_full = full;
  if (full) {
    g_hash_sampling = rex::cvar::GetFlagByName("bo2zm_native_hash_sampling");
    rex::cvar::SetFlagByName("bo2zm_native_hash_sampling", "0");
  } else {
    rex::cvar::SetFlagByName("bo2zm_native_hash_sampling", g_hash_sampling.empty() ? "8" : g_hash_sampling);
  }
}

std::filesystem::path ProfileFlag() {
#if defined(__SWITCH__)
  return std::filesystem::path(RexSwitchLogDir()) / "profile_stacks.flag";
#else
  return {};
#endif
}


void ApplyProfileFlag() {
  const auto flag = ProfileFlag();
  if (flag.empty()) {
    return;
  }
  const bool want = On() && g_profile.load(std::memory_order_relaxed);
  std::error_code ec;
  if (want == std::filesystem::exists(flag, ec)) {
    return;
  }
  if (want) {
    std::filesystem::create_directories(flag.parent_path(), ec);
    if (std::FILE* f = std::fopen(flag.string().c_str(), "wb")) {
      std::fclose(f);
    }
  } else {
    std::filesystem::remove(flag, ec);
  }
  REXLOG_INFO("[debug] stack profile {} from the next start ({})", want ? "on" : "off", flag.string());
}


void ApplyLog() {
  const std::string full = g_level_given ? g_given_level : "info";
  if (On()) {
    SetLevel(g_log.load(std::memory_order_relaxed) ? "debug" : full);
  } else {
    SetLevel(g_level_given ? g_given_level : "warn");
  }
  ApplyReport();
}

std::atomic<int>* const kPickAtomics[kNumPicks] = {&g_log, &g_profile, &g_textures, &g_constants, &g_caches, &g_safe};

Picks Current() {
  Picks picks{};
  for (int i = 0; i < kNumPicks; ++i) {
    picks[i] = kPickAtomics[i]->load(std::memory_order_relaxed);
  }
  return picks;
}

void ApplyPicks(const Picks& picks) {
  for (int i = 0; i < kNumPicks; ++i) {
    kPickAtomics[i]->store(picks[i] ? 1 : 0, std::memory_order_relaxed);
  }
  bo2zm::debug::Sync();
}

void SaveCustom() {
  for (int i = 0; i < kNumPicks; ++i) {
    bo2zm::settings::Set(kSaved[i].key, kSaved[i].names[g_custom[i] ? 1 : 0]);
  }
}


void SetPick(int index, int value) {
  if (g_preset.load(std::memory_order_relaxed) != 0) {
    g_custom = Current();
    g_preset.store(0, std::memory_order_relaxed);
  }
  g_custom[index] = value ? 1 : 0;
  SaveCustom();
  ApplyPicks(g_custom);
}

std::string Stamp() {
  const std::time_t now = std::time(nullptr);
  char text[32];
  std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", std::localtime(&now));
  return text;
}


void SaveLocation() {
  bo2zm::cheats::Spot spot;
  if (!bo2zm::cheats::PlayerSpot(spot)) {
    REXLOG_INFO("[mark] MARK LOCATION: not in a match");
    return;
  }
  ++g_locations;
  const std::string label = fmt::format("location_{:02}_{}", g_locations, Stamp());
  const std::string map = bo2zm::preset::LoadedMap();
  const std::string location = bo2zm::preset::LoadedLocation();
  const std::string text = fmt::format(
      "LOCATION {}: {} ({}), round {}, feet {:.1f} {:.1f} {:.1f}, camera {:.1f} {:.1f} {:.1f}, looking pitch {:.1f} "
      "yaw {:.1f} roll {:.1f}; preset {}, {} cores, debug {}, build {}, log {}; screenshots captures/{}_1.png to _4.png",
      g_locations, location.empty() ? "?" : location, map, bo2zm::cheats::CurrentRound(), spot.feet[0], spot.feet[1],
      spot.feet[2], spot.camera[0], spot.camera[1], spot.camera[2], spot.angles[0], spot.angles[1], spot.angles[2],
      bo2zm::preset::PresetLabel(), bo2zm::CoresInUse(), On() ? "on" : "off", bo2zm::BuildId(),
      rex::CurrentLogFile().filename().string(), label);

  const std::string again = fmt::format("bo2zm_test_spot = \"{} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}\"",
                                        map.substr(0, map.find(' ')), spot.feet[0], spot.feet[1], spot.feet[2],
                                        spot.angles[0], spot.angles[1]);
  REXLOG_INFO("[mark] ===== {} =====", text);
  REXLOG_INFO("[mark] {}", again);
  const std::filesystem::path marks = rex::CurrentLogFile().parent_path() / "marks.txt";
  std::ofstream file(marks, std::ios::app);
  file << Stamp() << " " << text << "\n    " << again << "\n";
  if (!file) {
    REXLOG_WARN("[mark] could not add the location to {}", marks.string());
  }
  for (int i = 0; i < 4; ++i) {
    bo2zm::capture::RequestCapture(label + "_" + std::to_string(i + 1), std::chrono::milliseconds(700 + 300 * i));
  }
  bo2zm::preset::ShowBanner(fmt::format("LOCATION {} SAVED", g_locations), 2.2, 2.5);
}

}

bool bo2zm::debug::TextureShortcutsOff() {
  return On() && (g_textures.load(std::memory_order_relaxed) || g_safe.load(std::memory_order_relaxed));
}
bool bo2zm::debug::ConstantsShortcutOff() {
  return On() && (g_constants.load(std::memory_order_relaxed) || g_safe.load(std::memory_order_relaxed));
}
bool bo2zm::debug::CachesOff() {
  return On() && (g_caches.load(std::memory_order_relaxed) || g_safe.load(std::memory_order_relaxed));
}

void bo2zm::debug::ConfigureStartup(const std::filesystem::path& user_data_root) {
  const std::filesystem::path settings = user_data_root / "bo2zm_settings.txt";
  g_level_given = rex::cvar::GetFlagSource("log_level") != rex::cvar::Source::kDefault;
  g_given_level = rex::cvar::GetFlagByName("log_level");
  const bool debug_on = SavedValue(settings, "debug") == "on";
  g_quiet_start = !g_level_given && !debug_on;

  const std::string max_logs = rex::cvar::GetFlagSource("bo2zm_max_log_files") != rex::cvar::Source::kDefault
                                   ? std::string(REXCVAR_GET(bo2zm_max_log_files))
                                   : SavedValue(settings, "max_log_files");
  for (int i = 0; i < int(std::size(kMaxLogNames)); ++i) {
    if (max_logs == kMaxLogNames[i]) {
      g_max_logs.store(i, std::memory_order_relaxed);
    }
  }
  g_logs_kept = kMaxLogCounts[g_max_logs.load(std::memory_order_relaxed)];
  if (g_level_given) {
    g_applied_level = g_given_level;
  } else {
    SetLevel(debug_on ? "info" : "warn");
  }
#if defined(__SWITCH__)

  RexSwitchPerfSetReport(!g_quiet_start && !(g_level_given && g_given_level == "off"));
#endif
}

void bo2zm::debug::AfterLoggingStarted() {
  if (g_quiet_start) {

    REXLOG_WARN("[debug] quiet log: DEBUG is off, so only warnings and errors are written (build {}). Extras > DEBUG "
                "on gives the full log.", bo2zm::BuildId());
  }
  if (g_logs_kept > 0 && CurrentLogNumber() >= 0) {
    PruneLogs(rex::CurrentLogFile().parent_path(), g_logs_kept);
  }
}

int bo2zm::debug::MaxLogFiles() {
  return g_max_logs.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetMaxLogFiles(int index) {
  if (index >= 0 && index < int(std::size(kMaxLogCounts))) {
    g_max_logs.store(index, std::memory_order_relaxed);
  }
}

int bo2zm::debug::PurgeTier() {
  return g_purge_tier.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetPurgeTier(int tier) {
  if (tier >= 0 && tier <= 3) {
    g_purge_tier.store(tier, std::memory_order_relaxed);
    g_purge_count.store(true, std::memory_order_relaxed);
  }
}
std::string bo2zm::debug::PurgeLabel() {
  if (g_purge_count.exchange(false, std::memory_order_relaxed)) {
    const Found found = Collect(g_purge_tier.load(std::memory_order_relaxed));
    g_purge_label = found.files.empty() ? "NOTHING TO DELETE" : "DELETE " + Amount(found.files.size(), found.bytes);
  }
  return g_purge_label;
}

int bo2zm::debug::Enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetEnabled(int on) {
  g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);


  if (on) {
    Sync();
    REXLOG_INFO("[debug] DEBUG on: the debug picks apply");
  } else {
    REXLOG_INFO("[debug] DEBUG off: normal play");
    Sync();
  }
}
void bo2zm::debug::Sync() {
  ApplyHashSampling();
  ApplyLog();
  ApplyProfileFlag();
}

int bo2zm::debug::Preset() {
  return g_preset.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetPreset(int preset) {
  if (preset < 0 || preset > kNumPresets) {
    return;
  }
  g_preset.store(preset, std::memory_order_relaxed);
  ApplyPicks(preset == 0 ? g_custom : kPresets[preset - 1]);
}
void bo2zm::debug::LoadCustom() {
  for (int i = 0; i < kNumPicks; ++i) {
    if (const auto saved = bo2zm::settings::Get(kSaved[i].key)) {
      g_custom[i] = *saved == kSaved[i].names[1];
    }
  }
}

int bo2zm::debug::TextureChecks() {
  return g_textures.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetTextureChecks(int full) {
  SetPick(2, full);
}
int bo2zm::debug::Constants() {
  return g_constants.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetConstants(int full) {
  SetPick(3, full);
}
int bo2zm::debug::Caches() {
  return g_caches.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetCaches(int off) {
  SetPick(4, off);
}
int bo2zm::debug::SafeRenderer() {
  return g_safe.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetSafeRenderer(int on) {
  SetPick(5, on);
}
int bo2zm::debug::LogDetail() {
  return g_log.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetLogDetail(int detailed) {
  SetPick(0, detailed);
}
int bo2zm::debug::StackProfile() {
  return g_profile.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetStackProfile(int on) {
  SetPick(1, on);
}
int bo2zm::debug::MarkLocation() {
  return g_mark_location.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetMarkLocation(int on) {
  g_mark_location.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool bo2zm::debug::RunCommand(std::string_view name, std::string_view argument) {
  if (name == "bo2zm_mark") {
    ++g_marks;
    const std::string label = "mark_" + Stamp();
    REXLOG_INFO("[debug] ===== MARK {}: the player marked this moment (round {}); screenshot captures/{}.png =====",
                g_marks, bo2zm::cheats::CurrentRound(), label);
    bo2zm::capture::RequestCapture(label);
    return true;
  }
  if (name == "bo2zm_mark_here") {
    SaveLocation();
    return true;
  }
  if (name == "bo2zm_screenshot") {
    const std::string label = "shot_" + Stamp();
    REXLOG_INFO("[debug] screenshot captures/{}.png", label);
    bo2zm::capture::RequestCapture(label);
    return true;
  }
  if (name == "bo2zm_purge_scan") {
    g_purge_count.store(true, std::memory_order_relaxed);
    return true;
  }
  if (name == "bo2zm_purge") {
    const int tier = g_purge_tier.load(std::memory_order_relaxed);
    const Found found = Collect(tier);
    size_t deleted = 0;
    uintmax_t bytes = 0;
    std::error_code ec;
    for (const auto& path : found.files) {
      const uintmax_t size = std::filesystem::file_size(path, ec);
      if (std::filesystem::remove(path, ec)) {
        ++deleted;
        bytes += size;
      }
    }
    g_purge_label = found.files.empty() ? "NOTHING TO DELETE" : "DELETED " + Amount(deleted, bytes);
    if (deleted < found.files.size()) {
      g_purge_label += fmt::format(" ({} COULD NOT GO)", found.files.size() - deleted);
    }
    REXLOG_WARN("[debug] PURGE DEBUG INFO, tier {}: {} of {} files deleted ({} bytes)", tier, deleted,
                found.files.size(), bytes);
    return true;
  }
  if (name == "bo2zm_debug_reset") {
    SetPreset(1);
    REXLOG_INFO("[debug] every debug option back to normal (preset BASIC; CUSTOM keeps its picks)");
    return true;
  }
  (void)argument;
  return false;
}

std::string bo2zm::debug::BuildText() {
  return bo2zm::BuildId();
}

std::string bo2zm::debug::LogFileName() {
  return rex::CurrentLogFile().filename().string();
}
