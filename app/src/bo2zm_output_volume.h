#pragma once
namespace bo2zm::audio {
inline constexpr int OutputVolumePercent(int requested) {
#if defined(__SWITCH__)
  (void)requested;
  return 100;
#else
  return requested < 0 ? 0 : requested > 100 ? 100 : requested;
#endif
}
}
