#pragma once

#include <string>

std::wstring focusedAppExeName();

// Builds a diagnostic report that identifies the foreground app and matches it
// against current Windows audio sessions grouped by output device.
bool describeFocusedAppAudio(std::wstring* outReport);

// Experimental: asks Windows to persist the focused app's preferred output
// endpoint to the next active render device.
bool cycleFocusedAppAudioOutput(std::wstring* outReport);
