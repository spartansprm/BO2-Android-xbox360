#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace bo2zm::native {


bool Active();


bool PortedRendererActive();


bool LogVertexShaderMisses();


bool DebugNoDepth();


bool DebugMagenta();


bool ClearQuads();


uint64_t Swaps();


std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

}
