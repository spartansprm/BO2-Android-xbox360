# Third-party notices

Black Ops II para Android is licensed under the GNU General Public License v3.0 (`LICENSE`). That license covers
everything this project wrote: `app/`, `android/`, `campaign/`, `tools/` and this project's changes inside `sdk/`.
The port is built on the projects and libraries below. Their code keeps its own copyright and license notices,
exactly as they are, and every one of those licenses is compatible with GPL-3.0.

This file covers the repository. The notices for what is packaged inside the APK are in
[`android/THIRD_PARTY_NOTICES.md`](android/THIRD_PARTY_NOTICES.md), and the license texts themselves are shipped in
the APK under `assets/licenses/`.

## Projects this port is derived from

| Project | Used for | License |
|---|---|---|
| [bo2-360-nx](https://github.com/AlvaroEPena/bo2-360-nx) by Álvaro E. Peña | The port this one is derived from: the static recompilation of Black Ops II for Xbox 360, the engine modules in `app/src/` and the campaign in `campaign/`. That project targets Nintendo Switch and Windows; this repository is the Android port | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 (commit `c94f5eb`) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | `sdk/`: code generator, Xbox 360 kernel and runtime, GPU command processing. See `sdk/README.md` for which files are the official release and which are changed | BSD-3-Clause (`sdk/LICENSE`) |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev and contributors | `tools/xenosrecomp/translator/`: the Xenos microcode to HLSL translator. It is also compiled into the game library, so the phone translates shaders with the same code the library is built with | MIT (`tools/xenosrecomp/translator/LICENSE.md`) |

Parts of `app/` and `tools/` are derived from GPL-3.0 code, so GPL-3.0 would apply to them in any case. The upstream
code in this repository keeps its own license: the ReXGlue SDK with its Xenia portions under BSD-3-Clause
(`sdk/LICENSE`) and the translator under MIT (`tools/xenosrecomp/translator/LICENSE.md`).

## Libraries

`sdk/thirdparty/` is not versioned in this repository (see `.gitignore`): `tools/fetch_thirdparty.py` restores it from
the submodules of ReXGlue SDK v0.10.0, at the commits that release pins. The Android build additionally fetches the
dependencies pinned by commit and checksum in `tools/android_runtime_deps.json` and `tools/android_vulkan_deps.json`,
and the Khronos tag `BO2_KHRONOS_TAG` in `android/game/CMakeLists.txt`.

| Library | Used for | License |
|---|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: the WMV3 decoder for movies and the VP8 decoder for the loading videos) | runtime | LGPL-2.1-or-later (configured without the GPL parts) |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression) | runtime | LGPL-2.1 |
| [glslang](https://github.com/KhronosGroup/glslang) | On-device shader translation, linked into the game library | BSD-3-Clause, with parts under MIT, Apache-2.0 and others; see the full text in `assets/licenses/glslang-licenses.txt` |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | On-device shader translation and validation | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | renderer | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | renderer | MIT |
| [SDL3](https://github.com/libsdl-org/SDL) | Window, input and audio on Android | zlib |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | runtime | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | runtime | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | recompiled code | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | configuration | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | SDK overlay | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | runtime allocator | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | runtime | BSL-1.0 |
| AES-128 by LuoPeng, and the SDK's DES and hash implementations | Xbox 360 content decryption | MIT, with the notices in `assets/licenses/Crypto-*.txt` |
| [CLI11](https://github.com/CLIUtils/CLI11) | Host build tools only; not in the APK | BSD-3-Clause |
| LLVM libc++ and the compiler runtime, from the Android NDK | runtime | Apache-2.0 with LLVM exceptions |

### FFmpeg and libmspack (LGPL)

Both are built from the upstream sources at the commits pinned by ReXGlue SDK v0.10.0 and fetched by
`tools/fetch_thirdparty.py`; this repository does not carry patched copies of them. Their notices travel in the APK as
`assets/licenses/FFmpeg-LGPL-2.1.txt` and `assets/licenses/libmspack-LGPL-2.1.txt`. If you change either library,
LGPL-2.1 requires you to publish those changes along with the binary you distribute.

## Build-time tools (not shipped)

The shader library is built on your own machine from your own game data, with the translator above and
[DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) (University of Illinois/NCSA Open Source
License, with LLVM's license terms), fetched by `tools/setup_dxc.py`.

## Game data

This repository contains no data from Black Ops II: no executables, no patches, no resources and no shader libraries.
Everything of that kind is produced on your own machine from your own copy of the game and stays there; the
directories that hold it are excluded in `.gitignore`.

## Trademarks

Call of Duty and Call of Duty: Black Ops II are trademarks of Activision Publishing, Inc. Xbox 360 is a trademark of
Microsoft. Android is a trademark of Google LLC. This project is not affiliated with or endorsed by any of them.
