#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace bo2zm::native {


bool IsActive();


uint32_t ProgressRing();


bool WaitProgressRing(uint32_t seen, std::chrono::microseconds limit);


void WarnProgressRing();


uint64_t SwapsNative();


std::unique_ptr<rex::system::IGraphicsSystem> CreateSystemGraphics();

}
