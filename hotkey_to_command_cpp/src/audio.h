#pragma once
#include <string>

// Switch the default render device to the next active output.
// Returns true on success; if outName is non-null, it receives the new
// device's friendly name.
bool cycleAudioDevice(std::wstring* outName = nullptr);

// Switch the default capture device to the next active microphone/input.
// Returns true on success; if outName is non-null, it receives the new
// device's friendly name.
bool cycleMicrophoneDevice(std::wstring* outName = nullptr);

// Mute/unmute the current default capture endpoint.
bool setDefaultMicrophoneMute(bool muted, std::wstring* report = nullptr);
bool getDefaultMicrophoneMute(bool* muted, std::wstring* report = nullptr);
