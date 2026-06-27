#pragma once

#include <string>

// Starts a localhost WebSocket server used by the Vencord OutputDeviceBridge
// plugin. Safe to call more than once.
void startDiscordBridge(unsigned short port = 8787);

void stopDiscordBridge();

// Sends a command to connected Discord/Vencord clients.
bool sendDiscordBridgeCommand(const std::string& command, std::wstring* report = nullptr);
