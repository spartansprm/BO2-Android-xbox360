#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct PPCContext;

namespace bo2zm::bots {
struct Context { std::string map, gametype, start; };


namespace preferences {
inline constexpr int kSupportedMaximum = 3;
inline constexpr int kTestMaximum = kSupportedMaximum;
inline int MaximumForHumans(int humans) {
  return humans >= 1 && humans <= 4 ? 4 - humans : 0;
}
inline int ClampToHumans(int requested, int humans) {
  if (requested < 0) return 0;
  const int maximum = MaximumForHumans(humans);
  return requested > maximum ? maximum : requested;
}
inline bool Token(std::string_view token, size_t limit) {
  if (token.empty() || token.size() > limit) return false;
  for (const char c : token)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}
inline std::optional<std::string> Key(const Context& context) {
  if (!context.map.starts_with("zm_") || context.map == "zm_frontend" || context.map.size() <= 3 ||
      !Token(context.map, 48) || !Token(context.gametype, 32) ||
      (!context.start.empty() && !Token(context.start, 48))) return std::nullopt;
  return "bots_count." + context.map + "." + context.gametype + "." +
      (context.start.empty() ? "-" : context.start);
}
inline std::optional<int> Count(std::string_view text) {
  if (text.size() == 1 && text[0] >= '0' && text[0] <= '3') return text[0] - '0';
  return std::nullopt;
}
inline int Resolve(const std::optional<std::string>& per_map,
                   const std::optional<std::string>& fallback, int test_override = -1) {
  if (test_override >= 0 && test_override <= kTestMaximum) return test_override;

  if (per_map) return Count(*per_map).value_or(0);
  return fallback ? Count(*fallback).value_or(0) : 0;
}
inline int RegistrationMaximum(int requested) {
  return requested == kTestMaximum ? kTestMaximum : kSupportedMaximum;
}
}


int HumanCount(const uint8_t* base);


int Humans(const uint8_t* base);
int Requested(const Context& context, int humans = -1);
void SetAvailability(bool available, std::string_view detail);
void RuntimeStatus(std::string_view detail);
bool Command(std::string_view name, std::string_view argument);
void EveryFrame(PPCContext& ctx, uint8_t* base);


bool MigrateLegacySelection(std::string_view selected);
}
