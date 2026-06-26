#pragma once
#include <string>

// Switch the default render device to the next active output.
// Returns true on success; if outName is non-null, it receives the new
// device's friendly name.
bool cycleAudioDevice(std::wstring* outName = nullptr);
