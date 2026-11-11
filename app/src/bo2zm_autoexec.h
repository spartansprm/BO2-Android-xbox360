#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::autoexec {


void ExecuteCommands(PPCContext& ctx, uint8_t* base, std::string_view commands, int controller = 0);


bool CaptureRenderDoc();


void ApplyVolume();

}
