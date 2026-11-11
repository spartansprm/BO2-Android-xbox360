#pragma once

#include <filesystem>

namespace bo2cp {

void ApplyPendingRelaunch(const std::filesystem::path& user_dir);
void StartWatchdog(const std::filesystem::path& logs_dir);


void RequestRestart();


bool RestartForScriptChanges();

}
