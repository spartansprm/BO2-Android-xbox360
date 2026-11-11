# Android dependencies

The Android sources of this project (`android/`) follow the repository's GPL-3.0 license. The notices for the whole
repository are in [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md). The license texts listed here are packaged
in the APK under `assets/licenses/`.

No Black Ops II executable, patch or resource is bundled in the APK or in this source release: the game library is
generated on your own machine from your own copy of the game.

## Packaged in the APK

- **SDL3**, the SDL contributors: zlib license. Pinned to commit
  `c9a6709bd21750f1ad9597be21abace78c6378c9` at https://github.com/libsdl-org/SDL, fetched by
  `tools/setup_android_diagnostic.py`. The original Java and native sources are used without edits. License:
  `licenses/SDL3.txt`.
- **LLVM libc++ and the compiler runtime**, the LLVM contributors: Apache-2.0 with LLVM exceptions, plus the notices
  of the components it includes. The NDK r27c notice is kept as `licenses/LLVM-NOTICE.txt`. Sources:
  https://android.googlesource.com/toolchain/llvm-project/ and the NDK r27c source package at
  https://github.com/android/ndk/wiki/Unsupported-Downloads.
- **glslang**, The Khronos Group and contributors: BSD-3-Clause, with parts under MIT, Apache-2.0 and the
  GPL-3.0-with-Bison-exception listed in its own `LICENSE.txt`, kept as `licenses/glslang-licenses.txt`. Linked into
  the game library for on-device shader translation
  (`app/src/native/render/bo2zm_shader_translator.cpp`); fetched at configure time from
  https://github.com/KhronosGroup/glslang at the tag `BO2_KHRONOS_TAG` in `android/game/CMakeLists.txt`.
- **SPIRV-Tools** and **SPIRV-Headers**, The Khronos Group and contributors: Apache-2.0 and an MIT-style Khronos
  license. Same use and tag, from https://github.com/KhronosGroup/SPIRV-Tools and
  https://github.com/KhronosGroup/SPIRV-Headers. Licenses: `licenses/SPIRV-Tools-Apache-2.0.txt` and
  `licenses/SPIRV-Headers-MIT.txt`.
- **Vulkan-Headers** (MIT / Apache-2.0, `licenses/Vulkan-Headers-licenses.txt`) and **Vulkan Memory Allocator**
  (MIT, `licenses/VulkanMemoryAllocator-MIT.txt`), pinned in `tools/android_vulkan_deps.json`.
- **XenosRecomp**, hedge-dev and contributors (MIT, `tools/xenosrecomp/translator/LICENSE.md`), with this project's
  changes: also compiled into the game library, so the phone translates shaders with the same code the library is
  built with.
- **The ReXGlue SDK runtime**, with its Xenia portions: BSD-3-Clause. `sdk/LICENSE` is packaged as
  `licenses/ReXGlue-BSD-3-Clause.txt`.
- **FFmpeg** (LGPL-2.1-or-later, built without the GPL and nonfree options) and **libmspack** (LGPL-2.1), linked by
  the runtime: `licenses/FFmpeg-LGPL-2.1.txt` and `licenses/libmspack-LGPL-2.1.txt`. They are built from the upstream
  sources at the commits pinned by ReXGlue SDK v0.10.0; this repository carries no patched copies of them.
- **fmt**, **spdlog**, **SIMDe**, **toml++**, **Dear ImGui**, **o1heap** and the **AES-128** implementation (MIT),
  **xxHash** (BSD-2-Clause), **utfcpp** (Boost Software License) and the SDK's DES and hash implementations: their
  notices are the remaining files under `licenses/`.
- **Gradle Wrapper** 8.14, Gradle Inc. and contributors: Apache-2.0, generated with the official Gradle
  distribution. Source:
  https://github.com/gradle/gradle/tree/v8.14.0/platforms/core-runtime/wrapper-main.

The exact public commits and archive checksums of the runtime dependencies are in `tools/android_runtime_deps.json`
and `tools/android_vulkan_deps.json`.

`libandroid.so`, `liblog.so`, `libvulkan.so` and bionic are provided by the device. The Android Gradle Plugin, the
Android SDK and NDK, CMake and the JDK are installed separately and are not part of this source release or of the APK.

## Code in this repository that follows another implementation

- The STFS block traversal in `StfsPatchVerifier.java` follows this project's GPL-3.0
  `tools/inspect/stfs_extract.py` and the ReXGlue/Xenia implementation in
  `sdk/src/filesystem/devices/stfs_container_device.cpp`.
- `SvodReader.java` follows the same SDK's SVOD addressing and directory layout.

ReXGlue and Xenia keep their BSD-3-Clause notices in the source tree.
