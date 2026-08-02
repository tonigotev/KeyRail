#pragma once

#include "media_sessions.h"

#include <string>
#include <vector>

void startBrowserMediaBridge(unsigned short port = 8790);
void stopBrowserMediaBridge();

std::vector<MediaTarget> listBrowserMediaTargets(std::wstring* report = nullptr);
std::vector<MediaTarget> cachedBrowserMediaTargets(std::wstring* report = nullptr);
bool browserMediaBridgeHasClients();
bool controlBrowserMediaTarget(const std::wstring& targetId, MediaCommand command, std::wstring* report = nullptr);
bool sendSystemMediaKey(MediaCommand command, std::wstring* report = nullptr);
std::wstring describeBrowserBridgeStatus();
