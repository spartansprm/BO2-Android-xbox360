#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bo2zm::settings {


std::filesystem::path File();


std::optional<std::string> Get(std::string_view key);

std::optional<std::string> GetAt(const std::filesystem::path& path, std::string_view key);
bool SetAt(const std::filesystem::path& path, std::string_view key, std::string_view value);
bool SetManyAt(const std::filesystem::path& path,
               const std::vector<std::pair<std::string, std::string>>& values);


bool Set(std::string_view key, std::string_view value);


bool SetMany(const std::vector<std::pair<std::string, std::string>>& values);

}
