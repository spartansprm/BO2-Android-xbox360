#pragma once

#include <cstdint>

namespace bo2zm::reflection_demand {


constexpr uint32_t kAddress = 0x07C5A000;


void AnnotateRead();
void AnnotateCopy();
void AnnotateSwap();


bool MeasureVisibility();


bool VisibilityChecked();


void AnnotateVisible(bool measured);

void AnnotateHidden();


void AnnotateWitness(bool with_samples);

}
