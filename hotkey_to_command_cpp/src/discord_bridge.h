#pragma once

#include <string>

// Starts a localhost WebSocket server used by the Vencord OutputDeviceBridge
// plugin. Safe to call more than once.
void startDiscordBridge(unsigned short port = 8787);

void stopDiscordBridge();

// Sends a command to connected Discord/Vencord clients.
bool sendDiscordBridgeCommand(const std::string& command, std::wstring* report = nullptr);

// Number of connected Vencord clients. Zero means the bridge plugin is not
// installed, not enabled, or Discord is closed, and any Discord device action
// will do nothing.
size_t discordBridgeClientCount();
