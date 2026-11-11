#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bo2zm::launch {

enum class Mode { Campaign = 0, Multiplayer = 1, Zombies = 2 };
enum class Program { Main, Campaign };


std::filesystem::path SharedUserRoot(const std::filesystem::path& runtime_user_dir = {});
void Configure(const std::filesystem::path& runtime_user_dir);


int ReadDefaultModeIndexEarly(const std::filesystem::path& user_root);


std::optional<Mode> ParseTestMode(std::string_view value);
struct StartupTestOptions {
  bool campaign = false;
  std::optional<Mode> mode;
};

StartupTestOptions ReadStartupTestsEarly(const std::filesystem::path& toml);


int GetDefaultModeIndex();
bool SetDefaultModeIndex(int index);
int GetDefaultModeIndex(const std::filesystem::path& user_root);
bool SetDefaultModeIndex(int index, const std::filesystem::path& user_root);

bool MigrateLegacyDefault(bool boot_zombies, std::optional<Mode> test_mode,
                          const std::filesystem::path& user_root);


void RestartCurrentMode();


struct InstalledModes {
  bool campaign = true;
  bool multiplayer = true;
  bool zombies = true;

  bool Has(Mode mode) const;
  int Count() const;

  std::string Text() const;
};


InstalledModes DetectInstalledModes(const std::filesystem::path& game_root, bool campaign_program,
                                    bool main_program = true);


Mode FirstInstalled(Mode wanted, const InstalledModes& installed);


Mode LaunchMode(std::string_view launch_path, const std::vector<uint8_t>& launch_data);


Mode StartupMode(Program program, std::optional<Mode> pending, Mode saved,
                 bool test_campaign = false, std::optional<Mode> test_mode = std::nullopt);

}
