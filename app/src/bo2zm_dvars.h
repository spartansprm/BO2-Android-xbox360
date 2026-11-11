#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::dvars {

enum Kind { kBool = 0, kInt = 1, kFloat = 2, kEnum = 3 };

struct Dvar {
  uint32_t pointer = 0;
  int kind = kBool;
};


void Registered(const uint8_t* base, uint32_t name, uint32_t dvar, int kind);


std::optional<Dvar> Find(std::string_view name);


std::string Text(const uint8_t* base, const Dvar& dvar);


bool Matches(const uint8_t* base, const Dvar& dvar, std::string_view value);


void ExecuteIgnoringCheats(PPCContext& ctx, uint8_t* base, std::string_view commands, std::span<const uint32_t> dvars);

}
