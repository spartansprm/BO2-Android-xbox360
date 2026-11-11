#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only


import os, sys
root = sys.argv[1] if len(sys.argv) > 1 else "."
gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen")
os.makedirs(gen, exist_ok=True)
render = os.path.join(root, "app/src/native/render")
T = open(os.path.join(render, "bo2zm_native_targets.cpp")).read()
D = open(os.path.join(render, "bo2zm_native_draws.cpp")).read()
H = open(os.path.join(render, "bo2zm_native_draws.h")).read()
def func(src, sig):
    i = src.rfind("\n", 0, src.index(sig)) + 1
    k = src.index("{", i); depth = 0
    while True:
        c = src[k]
        if c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0: return src[i:k + 1]
        k += 1
tf = ["bool ClearAsDraw(const DrawClear& b) override", "bool GuestClearsInPassOn()", "void GuestClearBarrier()",
      "bool DeferGuestClear(", "bool HasDeferredWork(", "void TakeDeferredClears(const std::array<NativeImage*, 5>& images",
      "void RecordDeferredClear(const OwnedImage& image, const DeferredClear& d)", "void RefreshVolumesEager()",
      "bool BuildVolume("]
df = ["int ActivePassSlot(", "bool CanClearInActivePass(", "bool ClearInActivePass("]
open(os.path.join(gen, "tfuncs.inc"), "w").write("\n".join(func(T, f) for f in tf))
open(os.path.join(gen, "dfuncs.inc"), "w").write("\n".join(func(D, f) for f in df))
a = D.index("    std::array<bool, 5> clears_fused{};\n    std::array<VkClearValue, 5> clear_values{};\n    // bo2zm_native_guest_clears_in_pass")
b = D.index("    const VkRenderPass pass = PassOf(formats, mode_load);", a)
a2 = D.index("    VkRenderPassBeginInfo start{};", b)
c = D.index("    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(", a2)
a3 = D.index("    pass_area_height_ = height_pass;")
b3 = D.index("    // The shadow map (bo2zm_native_shadow_scale)", a3)
open(os.path.join(gen, "beginpass.inc"), "w").write(
    D[a:b] + "\n    VkFramebuffer framebuffer{}; VkRenderPass pass{}; VkCommandBuffer cmd{};\n" + D[a2:c] + D[a3:b3])
a = H.index("struct NativeImage {"); b = H.index("};", a) + 2
open(os.path.join(gen, "nativeimage.inc"), "w").write(H[a:b])
print("extracted into", gen)
