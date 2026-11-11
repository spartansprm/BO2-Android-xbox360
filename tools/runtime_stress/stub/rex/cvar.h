#pragma once

#define REXCVAR_DECLARE(type, name) extern type rexstress_cvar_##name
#define REXCVAR_DEFINE_BOOL(name, default_value, category, description) \
  bool rexstress_cvar_##name = default_value
#define REXCVAR_GET(name) rexstress_cvar_##name
