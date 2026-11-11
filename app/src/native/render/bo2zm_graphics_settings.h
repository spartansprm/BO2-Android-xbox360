#pragma once

#include <cstdint>

namespace bo2zm::settings {


struct Postprocess {
  bool active = false;
  float brightness = 0.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float vibrance = 0.0f;
  float temperature = 0.0f;
  float gamma = 1.0f;
  float tint_r = 1.0f;
  float tint_g = 1.0f;
  float tint_b = 1.0f;
  float tint = 0.0f;
  float vignette = 0.0f;
  float lines = 0.0f;
};


Postprocess ReadPostprocess();


uint64_t VersionPostprocess();


bool AntialiasingFxaa();


int GlowSky();


int TreatmentVisual();


void WatchSettingsInLive();


void ApplySettingsGraphics();


void RequestBootClocks();


void HideSettingsNoEffect();

}
