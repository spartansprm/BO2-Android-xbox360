#pragma once

#include <filesystem>

#include "bo2zm_launch_options.h"

namespace bo2zm {


void ApplyPendingRelaunch(const std::filesystem::path& user_dir);


bool CampaignAvailable();


const launch::InstalledModes& ModesInstalled();


void HandOverEarly();


void RequestRestart();


bool RestartForScriptChanges();

}
