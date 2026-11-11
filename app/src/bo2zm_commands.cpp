#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_cheats.h"
#include "bo2zm_controllers.h"
#include "bo2zm_bots.h"
#include "bo2zm_extras.h"
#include "bo2zm_name.h"
#include "bo2zm_online.h"
#include "bo2zm_preset.h"
#include "bo2zm_updates.h"
#include "bo2zm_script_manager.h"

REX_EXTERN(__imp__sub_82402E78);

namespace {

constexpr uint32_t kMaxText = 16 * 1024;
constexpr std::string_view kPrefix = "bo2zm_";

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

bool RunPlayerScoped(std::string_view argument) {


  if (argument.size() < 3 || argument[0] < '0' || argument[0] > '3' || !IsSpace(argument[1])) return true;
  for (const char c : argument) {
    if (c == ';' || c == '"' || (static_cast<unsigned char>(c) < 32 && c != '\t')) return true;
  }
  const int controller = argument[0] - '0';
  argument.remove_prefix(2);
  while (!argument.empty() && IsSpace(argument.front())) argument.remove_prefix(1);
  const auto space = argument.find_first_of(" \t");
  const auto name = argument.substr(0, space);
  auto value = space == std::string_view::npos ? std::string_view{} : argument.substr(space + 1);
  while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
  while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
  if (!name.starts_with(kPrefix) || name == "bo2zm_player") return true;
  const int local = bo2zm::cheats::LocalPlayerForController(controller);
  if (local < 0 || local >= 4) {
    REXLOG_WARN("[commands] controller {} has no active local player", controller);
    return true;
  }
  if (!bo2zm::extras::RunPlayerCommand(local, name, value))
    REXLOG_WARN("[commands] unsupported player command: {}", name);
  return true;
}


bool RunOurs(PPCContext& ctx, uint8_t* base, std::string_view command) {
  const size_t space = command.find_first_of(" \t");
  const std::string_view name = command.substr(0, space);
  std::string_view argument = space == std::string_view::npos ? std::string_view() : command.substr(space + 1);
  while (!argument.empty() && IsSpace(argument.front())) {
    argument.remove_prefix(1);
  }
  while (!argument.empty() && IsSpace(argument.back())) {
    argument.remove_suffix(1);
  }
  if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"') {
    argument = argument.substr(1, argument.size() - 2);
  }
  if (name == "bo2zm_player") return RunPlayerScoped(argument);
  if (name == "bo2zm_log") {
    REXLOG_INFO("[lua] {}", argument);
    return true;
  }
  if (bo2zm::bots::Command(name, argument)) {
    bo2zm::bots::EveryFrame(ctx, base);
    return true;
  }
  if (bo2zm::scripts::Command(name, argument)) {
    bo2zm::scripts::Publish(ctx, base);
    return true;
  }
  if (name == "bo2zm_update") {
    bo2zm::updates::Command(argument);
    return true;
  }
  if (name == "bo2zm_preset") {
    bo2zm::preset::SetFromMenu(argument);
    return true;
  }
  if (name == "bo2zm_input_block") {
    bo2zm::preset::SetInputBlocked(argument == "1");
    return true;
  }
  if (name == "bo2zm_profile_map") {
    bo2zm::preset::MapLoading(argument);
    return true;
  }
  if (name == "bo2zm_profile") {
    bo2zm::preset::SelectProfileFromMenu(argument);
    return true;
  }
  if (name == "bo2zm_profile_on") {
    bo2zm::preset::EnableProfileFromMenu(argument);
    return true;
  }
  if (constexpr std::string_view kGfx = "bo2zm_gfx_";
      name.substr(0, kGfx.size()) == kGfx && bo2zm::preset::SetOptionFromMenu(name.substr(kGfx.size()), argument)) {
    return true;
  }
  if (bo2zm::controllers::RunCommand(name, argument) || bo2zm::extras::RunCommand(name, argument) ||
      bo2zm::name::RunCommand(name, argument) || bo2zm::online::RunCommand(name, argument)) {
    return true;
  }
  if (name.substr(0, kPrefix.size()) == kPrefix) {
    REXLOG_WARN("[commands] unknown command: {}", command);
    return true;
  }
  return false;
}

}

REX_HOOK_RAW(sub_82402E78) {
  const uint32_t text = ctx.r6.u32;
  char* p = nullptr;
  std::vector<char> original;
  if (text >= 0x10000) {
    p = reinterpret_cast<char*>(base + text);
    const size_t length = strnlen(p, kMaxText);
    if (std::string_view(p, length).find(kPrefix) != std::string_view::npos) {
      original.assign(p, p + length);
      size_t start = 0;
      while (start < length) {
        size_t end = start;
        bool quoted = false;
        while (end < length && (quoted || (p[end] != ';' && p[end] != '\n'))) {
          quoted ^= p[end] == '"';
          ++end;
        }
        size_t first = start;
        while (first < end && IsSpace(p[first])) {
          ++first;
        }
        const std::string_view command(p + first, end - first);
        if (command.substr(0, kPrefix.size()) == kPrefix && RunOurs(ctx, base, command)) {
          std::memset(p + first, ' ', end - first);
        }
        start = end + 1;
      }
    }
  }
  __imp__sub_82402E78(ctx, base);
  if (!original.empty()) {
    std::memcpy(p, original.data(), original.size());
  }
}
