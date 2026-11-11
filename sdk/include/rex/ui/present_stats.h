// SPDX-License-Identifier: GPL-3.0-only
//
// What the presenter really used for the last painted frame, for diagnostics outside the SDK (the BO2 Android

// readers get a recent, possibly torn-between-fields, snapshot. Nothing here changes presentation.
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::ui::present_stats {

// Surface (window) size as connected to the presenter, in surface pixels.
inline std::atomic<uint32_t> surface_width{0};
inline std::atomic<uint32_t> surface_height{0};
// Size of the host render target the guest output is painted into (the swapchain image on Vulkan).
inline std::atomic<uint32_t> host_rt_width{0};
inline std::atomic<uint32_t> host_rt_height{0};
// VkSwapchainKHR imageExtent at its last (re)creation, and how many times it was (re)created.
inline std::atomic<uint32_t> swapchain_width{0};
inline std::atomic<uint32_t> swapchain_height{0};
inline std::atomic<uint64_t> swapchain_creations{0};
// The guest output as the renderer handed it (RefreshGuestOutput) and the aspect it asked to keep.
inline std::atomic<uint32_t> frontbuffer_width{0};
inline std::atomic<uint32_t> frontbuffer_height{0};
inline std::atomic<uint32_t> display_aspect_x{0};
inline std::atomic<uint32_t> display_aspect_y{0};
// Where the guest output landed in the host render target (letterbox/pillarbox bars are outside it).
inline std::atomic<int32_t> output_x{0};
inline std::atomic<int32_t> output_y{0};
inline std::atomic<uint32_t> output_width{0};
inline std::atomic<uint32_t> output_height{0};
inline std::atomic<uint64_t> flows{0};  // paint flows computed with a visible guest output

}  // namespace rex::ui::present_stats
