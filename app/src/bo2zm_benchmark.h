#pragma once

#include <cstdint>

namespace bo2zm::benchmark {


bool IsActive();

void Disable();


void AnnotatePlayer(bool present);


uint32_t PathButtons();


bool PathTilts(int& move_percent, int& look_percent);


void AnnotateFrame();

uint64_t FramesSeen();


int AbArm();

int AbArmCached();


void ApplyOwnPriority();

}
