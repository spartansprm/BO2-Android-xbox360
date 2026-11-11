#pragma once

#include <filesystem>
#if defined(__ANDROID__)
#include <rex/android.h>
#else
#include <rex/filesystem.h>
#endif

namespace bo2zm::native::cache {

inline std::filesystem::path UserFolder() {
#if defined(__ANDROID__)


  return rex::GetAndroidPrivateFilesDir() / "game-user";
#else
  return rex::filesystem::GetExecutableFolder();
#endif
}

inline std::filesystem::path Folder() {
#ifndef BO2ZM_NATIVE_CACHE_FOLDER
#define BO2ZM_NATIVE_CACHE_FOLDER "cache"
#endif
  return UserFolder() / BO2ZM_NATIVE_CACHE_FOLDER;
}

inline std::filesystem::path PipelineFile() {
  return Folder() / "bo2zm_native_pipelines.bin";
}

}
