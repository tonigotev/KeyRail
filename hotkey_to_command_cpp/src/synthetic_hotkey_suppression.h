#pragma once

#include <windows.h>

void suppressNextSyntheticHotkey(UINT vk, DWORD milliseconds = 1000);
bool consumeSuppressedSyntheticHotkey(UINT vk);
