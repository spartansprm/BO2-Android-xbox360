// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <cstdint>

namespace bo2zm::widescreen {


void PollBoot(uint8_t* base);


void OnViewFov(uint8_t* base, uint32_t local);


uint32_t Placement(uint8_t* base, uint32_t record);


void OnMenuSafeArea(uint8_t* base, const float values[4]);

}
