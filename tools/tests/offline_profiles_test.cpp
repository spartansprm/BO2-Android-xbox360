#include <rex/system/xam/user_profile.h>
#include <rex/system/xam/profile_paths.h>
#include <rex/system/kernel_state.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace rex::system::xam;
namespace fs = std::filesystem;
constexpr uint64_t primary_xuid = 0xB13EBABEBABEBABE;
constexpr uint32_t title_id = 0x415608C3;
constexpr uint32_t blob_id = 0x63E83FFF;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void Write(const fs::path& file, std::string_view contents) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  out << contents;
  out.close();
  Check(bool(out), "fixture write failed");
}
std::string Read(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "expected temporary directory");
    const fs::path root = argv[1];
    rex::system::KernelState kernel(root);
    UserProfile primary, secondary(1);
    primary.set_kernel_state(&kernel);
    secondary.set_kernel_state(&kernel);
    Check(primary.xuid() == primary_xuid && primary.name() == "User", "primary defaults changed");
    Check(secondary.xuid() == primary_xuid + 1 && secondary.name() == "Player 2", "secondary identity unstable");
    Check(primary.type() == 3 && secondary.type() == 1, "secondary advertised an online profile");
    Check(primary.signin_state() == 1 && secondary.signin_state() == 1, "profiles are not local signed-in users");
    primary.set_name("Original");
    secondary.set_name("Second");
    Check(primary.name() == "Original" && secondary.name() == "Second", "renaming a profile renamed the other");
    secondary.set_name("^;\n");
    Check(secondary.name() == "Player 2", "unsafe name did not fall back");
    secondary.set_name("1234567890123456789");
    Check(secondary.name() == "123456789012345", "secondary name exceeds guest buffer");

    primary.AddSetting(std::make_unique<UserProfile::BinarySetting>(blob_id, std::vector<uint8_t>{1,2,3}));
    secondary.AddSetting(std::make_unique<UserProfile::BinarySetting>(blob_id, std::vector<uint8_t>{8,9}));
    const auto first = ProfileDirectory(root, title_id, primary.xuid());
    const auto second = ProfileDirectory(root, title_id, secondary.xuid());
    Check(first != second, "profile paths collide");
    Check(Read(first / "63E83FFF") == std::string("\1\2\3", 3), "secondary overwrote primary settings");
    Check(Read(second / "63E83FFF") == std::string("\10\11", 2), "secondary settings did not persist");
    UserProfile reloaded_first, reloaded_second(1);
    reloaded_first.set_kernel_state(&kernel);
    reloaded_second.set_kernel_state(&kernel);
    auto* a = static_cast<UserProfile::BinarySetting*>(reloaded_first.GetSetting(blob_id));
    auto* b = static_cast<UserProfile::BinarySetting*>(reloaded_second.GetSetting(blob_id));
    Check(a->value == std::vector<uint8_t>({1,2,3}) && b->value == std::vector<uint8_t>({8,9}),
        "reloading profiles mixed their serialized settings");
    secondary.AddSetting(std::make_unique<UserProfile::BinarySetting>(blob_id, std::vector<uint8_t>{7}));
    Check(Read(first / "63E83FFF") == std::string("\1\2\3", 3), "updating secondary damaged primary");


    for (uint32_t index = 0; index < 4; ++index) {
      UserProfile profile(index);
      profile.set_kernel_state(&kernel);
      Check(profile.xuid() == primary_xuid + index, "four-user XUIDs overlap");
      if (index) {
        const auto expected = "Player " + std::to_string(index + 1);
        Check(profile.name() == expected, "four-user default names overlap");
        profile.set_name("^;\n");
        Check(profile.name() == expected, "invalid name aliases another local user");
      }
      const std::vector<uint8_t> marker{uint8_t(index), uint8_t(index + 10)};
      profile.AddSetting(std::make_unique<UserProfile::BinarySetting>(blob_id, marker));
    }
    for (uint32_t index = 0; index < 4; ++index) {
      UserProfile profile(index);
      profile.set_kernel_state(&kernel);
      const auto* saved = static_cast<UserProfile::BinarySetting*>(profile.GetSetting(blob_id));
      Check(saved && saved->value == std::vector<uint8_t>({uint8_t(index), uint8_t(index + 10)}),
            "four-user save reload aliases another profile");
    }

    const auto legacy_root = root / "legacy";
    const auto legacy = legacy_root / "415608C3" / "profile" / "OldName";
    Write(legacy / "63E83FFF", "old-primary");
    auto location = ResolveProfileDirectory(legacy_root, title_id, primary_xuid + 1, primary_xuid);
    Check(fs::exists(legacy) && !fs::exists(location.path), "secondary consumed primary legacy migration");
    location = ResolveProfileDirectory(legacy_root, title_id, primary_xuid, primary_xuid);
    Check(!location.error && Read(location.path / "63E83FFF") == "old-primary", "primary migration broke");
    Check(!fs::exists(legacy), "primary migration did not preserve rename behavior");
    const auto owner_root = root / "owners";
    const auto other_user = ProfileDirectory(owner_root, title_id, primary_xuid + 1);
    Write(other_user / "63E83FFF", "keep-secondary");
    location = ResolveProfileDirectory(owner_root, title_id, primary_xuid, primary_xuid);
    Check(Read(other_user / "63E83FFF") == "keep-secondary" && !fs::exists(location.path),
        "primary stole another XUID directory during migration");
    std::cout << "offline profile identities, real blob isolation/reload, names and legacy ownership passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
