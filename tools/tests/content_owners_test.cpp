#include <rex/filesystem.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

extern std::string content_backup_root;
namespace fs = std::filesystem;
using rex::X_RESULT;
using namespace rex::system;
using namespace rex::system::xam;
constexpr uint64_t kPrimary = 0xB13EBABEBABEBABE, kSecondary = kPrimary + 1;

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void Write(const fs::path& file, const std::string& value) {
  fs::create_directories(file.parent_path());
  std::ofstream stream(file, std::ios::binary);
  stream << value;
  Check(bool(stream), "write failed");
}
std::string Read(const fs::path& file) {
  std::ifstream stream(file, std::ios::binary);
  Check(bool(stream), "read failed");
  return {std::istreambuf_iterator<char>(stream), {}};
}


namespace rex {
std::string path_to_utf8(const fs::path& path) { return path.string(); }
std::u16string path_to_utf16(const fs::path& path) { return path.u16string(); }
fs::path to_path(std::string_view value) { return fs::path(value); }
namespace filesystem {
FILE* OpenFile(const fs::path& file, std::string_view mode) {
  return std::fopen(file.string().c_str(), std::string(mode).c_str());
}
bool CreateEmptyFile(const fs::path& file) {
  if (auto* handle = OpenFile(file, "wb")) return std::fclose(handle) == 0;
  return false;
}
std::vector<FileInfo> ListFiles(const fs::path& root) {
  std::vector<FileInfo> result;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(root, error)) {
    FileInfo info{};
    info.name = entry.path().filename();
    info.type = entry.is_directory() ? FileInfo::Type::kDirectory : FileInfo::Type::kFile;
    if (!entry.is_directory()) info.total_size = entry.file_size();
    result.push_back(info);
  }
  return result;
}
}
namespace thread {
std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex mutex;
  return mutex;
}
}
namespace string {
std::string utf8_lower_ascii(std::string_view text) {
  std::string result(text);
  for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return result;
}
size_t utf8_hash_fnv1a_case(std::string_view text) { return std::hash<std::string>{}(utf8_lower_ascii(text)); }
bool utf8_equal_case(std::string_view a, std::string_view b) { return utf8_lower_ascii(a) == utf8_lower_ascii(b); }
bool utf8_starts_with(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }
}
}

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "expected temporary root");
    const fs::path root = argv[1], backups = root / "backups";
    content_backup_root = backups.string();
    KernelState kernel;
    ContentManager manager(&kernel, root / "saves");
    XCONTENT_AGGREGATE_DATA data{};
    data.device_id = 1;
    data.content_type = XContentType::kSavedGame;
    data.title_id = kCurrentlyRunningTitleId;
    data.set_file_name("shared");
    auto secondary = data;
    secondary.xuid = kSecondary;
    Check(!(data == secondary), "aggregate equality aliases different owners");
    const auto p0 = root / "saves/B13EBABEBABEBABE/415608C3/00000001/shared";
    const auto p1 = root / "saves/B13EBABEBABEBABF/415608C3/00000001/shared";
    const auto b0 = backups / "B13EBABEBABEBABE/shared";
    const auto b1 = backups / "B13EBABEBABEBABF/shared";
    Write(backups / "shared/current/save.bin", "legacy-primary");
    Write(backups / "shared/previous/save.bin", "older-primary");
    Check(manager.CreateContent("host", kPrimary, data) == X_ERROR_SUCCESS, "create primary failed");
    Check(manager.CreateContent("second", kSecondary, data) == X_ERROR_SUCCESS, "create secondary failed");
    Check(manager.GetOpenPackagePath("host") == p0 && manager.GetOpenPackagePath("second") == p1,
          "save paths changed or aliased");
    Write(p0 / "save.bin", "primary-1");
    Write(p1 / "save.bin", "secondary-1");
    Write(p0 / "nested/data.bin", "primary-nested");
    Write(p1 / "nested/data.bin", "secondary-nested");
    Check(manager.WriteContentHeaderFile(kPrimary, data) == X_ERROR_SUCCESS &&
          manager.WriteContentHeaderFile(kSecondary, data) == X_ERROR_SUCCESS, "header writes failed");
    Check(manager.CloseContent("SECOND") == X_ERROR_SUCCESS, "close secondary failed");
    Check(Read(b1 / "current/save.bin") == "secondary-1" && !fs::exists(b1 / "previous"),
          "secondary inherited a legacy primary backup");
    Check(manager.CloseContent("HOST") == X_ERROR_SUCCESS, "close primary failed");
    Check(Read(b0 / "current/save.bin") == "primary-1" && Read(b0 / "previous/save.bin") == "legacy-primary",
          "primary backup import/rotation failed");
    Check(Read(backups / "shared/current/save.bin") == "legacy-primary" &&
          Read(backups / "shared/previous/save.bin") == "older-primary", "legacy recovery copies changed");
    uint32_t license = 0;
    Check(manager.OpenContent("host", kPrimary, data, license) == X_ERROR_SUCCESS, "reopen primary failed");
    Check(manager.OpenContent("second", kSecondary, data, license) == X_ERROR_SUCCESS, "reopen secondary failed");
    Write(p0 / "save.bin", "primary-2");
    Write(p1 / "save.bin", "secondary-2");
    Check(manager.CloseContent("host") == X_ERROR_SUCCESS && manager.CloseContent("second") == X_ERROR_SUCCESS,
          "second backup rotation failed");
    Check(Read(b0 / "current/save.bin") == "primary-2" && Read(b0 / "previous/save.bin") == "primary-1" &&
          Read(b1 / "current/save.bin") == "secondary-2" && Read(b1 / "previous/save.bin") == "secondary-1" &&
          Read(b0 / "current/nested/data.bin") == "primary-nested" &&
          Read(b1 / "current/nested/data.bin") == "secondary-nested", "owners overwrote each other's backups");

    Check(manager.OpenContent("host", kPrimary, data, license) == X_ERROR_SUCCESS, "open host failed");
    Check(manager.DeleteContent(kPrimary, data) == X_ERROR_ACCESS_DENIED, "deleted open primary save");
    Check(manager.DeleteContent(kSecondary, data) == X_ERROR_SUCCESS && !fs::exists(p1) && fs::exists(p0),
          "open primary blocked another owner's delete");
    Check(!fs::exists(root / "saves/B13EBABEBABEBABF/415608C3/Headers/00000001/shared.header"),
          "secondary header was not deleted");


    Check(manager.CreateContent("second", kPrimary, secondary) == X_ERROR_SUCCESS, "explicit owner create failed");
    Write(p1 / "save.bin", "secondary-3");
    Check(manager.CloseContent("second") == X_ERROR_SUCCESS, "explicit owner close failed");
    Check(Read(b1 / "current/save.bin") == "secondary-3" && Read(b0 / "current/save.bin") == "primary-2",
          "backup used caller instead of effective owner");
    Check(manager.OpenContent("second", kPrimary, secondary, license) == X_ERROR_SUCCESS, "explicit owner open failed");
    Check(manager.UnmountContent(kPrimary, secondary) == X_ERROR_SUCCESS &&
          manager.GetOpenPackagePath("second").empty() && manager.GetOpenPackagePath("host") == p0,
          "unmount detached the other owner's package");
    Check(manager.OpenContent("second", kSecondary, data, license) == X_ERROR_SUCCESS, "reopen second failed");
    Check(manager.UnmountAndDeleteContent(kPrimary, secondary) == X_ERROR_SUCCESS &&
          !fs::exists(p1) && manager.GetOpenPackagePath("host") == p0 &&
          manager.GetOpenPackagePath("second").empty(), "unmount/delete crossed owners");
    Check(manager.UnmountContent(kSecondary, data) == X_ERROR_FILE_NOT_FOUND &&
          manager.GetOpenPackagePath("host") == p0, "missing secondary unmounted primary");
    auto alias = data;
    alias.xuid = uint64_t(-1);
    alias.title_id = kernel.title_id();
    alias.device_id = 2;
    Check(manager.IsContentOpen(kPrimary, alias) && !manager.IsContentOpen(kSecondary, alias) &&
          manager.DeleteContent(kPrimary, alias) == X_ERROR_ACCESS_DENIED,
          "resolved owner/title/device aliases bypassed the open-save gate");
    Check(Read(p0 / "save.bin") == "primary-2", "primary save changed during secondary delete");
    Check(manager.CloseContent("host") == X_ERROR_SUCCESS, "final primary close failed");

    auto dlc = data;
    dlc.content_type = XContentType::kMarketplaceContent;
    dlc.xuid = kPrimary;
    Check(manager.CreateContent("dlc", kPrimary, dlc) == X_ERROR_SUCCESS, "shared content create failed");
    dlc.xuid = kSecondary;
    Check(manager.IsContentOpen(kSecondary, dlc) && manager.DeleteContent(kSecondary, dlc) == X_ERROR_ACCESS_DENIED,
          "shared DLC treated as per-user content");
    Check(manager.UnmountContent(kSecondary, dlc) == X_ERROR_SUCCESS &&
          manager.DeleteContent(kSecondary, dlc) == X_ERROR_SUCCESS, "shared content owner resolution changed");
    std::cout << "production content manager: two-owner backups, legacy recovery, delete/unmount isolation and shared DLC passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
