#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bo2zm::debug {


bool TextureShortcutsOff();
bool ConstantsShortcutOff();
bool CachesOff();


int TextureChecks();
void SetTextureChecks(int full);
int Constants();
void SetConstants(int full);
int Caches();
void SetCaches(int off);
int SafeRenderer();
void SetSafeRenderer(int on);
int LogDetail();
void SetLogDetail(int detailed);
int StackProfile();
void SetStackProfile(int on);


int MarkLocation();
void SetMarkLocation(int on);


void ConfigureStartup(const std::filesystem::path& user_data_root);
void AfterLoggingStarted();


int MaxLogFiles();
void SetMaxLogFiles(int index);


int PurgeTier();
void SetPurgeTier(int tier);
std::string PurgeLabel();


int Enabled();
void SetEnabled(int on);
void Sync();


int Preset();
void SetPreset(int preset);
void LoadCustom();


bool RunCommand(std::string_view name, std::string_view argument);


std::string BuildText();
std::string LogFileName();

}
