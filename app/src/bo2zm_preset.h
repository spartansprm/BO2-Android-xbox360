#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/ppc.h>

#include "bo2zm_pad.h"

namespace bo2zm::preset {


void DvarRegistered(uint8_t* base, uint32_t name, uint32_t dvar, int kind);


void EveryFrame(PPCContext& ctx, uint8_t* base);


void AfterForceShadows(uint8_t* base);


bool OnScreenNote(uint32_t (&codes)[4]);


bool OnScreenBanner(uint32_t (&codes)[8]);


void ShowBanner(std::string_view text, double delay_s, double seconds);


std::string LoadedMap();


uint64_t MapLoadGeneration();
std::string LoadedLocation();
std::string PresetLabel();


std::string GraphicsOptionsDiagnostic();


uint32_t MipsMinus();


void SetFromMenu(std::string_view value);


bool SetOptionFromMenu(std::string_view option, std::string_view value);


void MapLoading(std::string_view value);

void SelectProfileFromMenu(std::string_view value);


void EnableProfileFromMenu(std::string_view value);

uint16_t OnPad(const bo2zm::pad::Pad& pad);


bool ClientInGame();


void SetFrontEnd(bool on);


bool FrontEnd();
int FrontEndFpsCap();


void SetFrontEndSceneKept(bool kept);


void SetInputBlocked(bool blocked);
bool InputBlocked();
bool FrontEndSceneKept();

}
