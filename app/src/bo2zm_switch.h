#pragma once

namespace bo2zm {


void InitSwitchServices();


const char* BuildId();


int ProcessCores();


int CpuCoresSetting();
void SetCpuCoresSetting(int cores);

int CoresInUse();

bool CoresSwitchedOff();


bool HasRefreshSignal();

bool WaitForNextRefresh(int timeout_ms);

}
