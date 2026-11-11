#!/usr/bin/env bash

set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$TASK_ROOT"
RUNTIME_CMAKE="${BO2_RUNTIME_CMAKE:-cmake}"
RUNTIME_NINJA="${BO2_RUNTIME_NINJA:-ninja}"
RUNTIME_JOBS="${BO2_BUILD_JOBS:-4}"
RUNTIME_NDK="${ANDROID_NDK_HOME:-${ANDROID_HOME:?Set ANDROID_HOME}/ndk/27.2.12479018}"
python3 tools/setup_android_diagnostic.py
python3 tools/setup_android_runtime.py --vulkan
"$RUNTIME_CMAKE" -S android/runtime -B work/android-runtime -G Ninja \
  "-DCMAKE_MAKE_PROGRAM=$RUNTIME_NINJA" "-DCMAKE_TOOLCHAIN_FILE=$RUNTIME_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
  -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' -DGIT_EXECUTABLE=/nonexistent/git
"$RUNTIME_CMAKE" --build work/android-runtime --target rexruntime -j "$RUNTIME_JOBS"
"$RUNTIME_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug work/android-runtime/lib/librexruntime.so
