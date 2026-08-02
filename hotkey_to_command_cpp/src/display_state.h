#pragma once

#include <string>

// Exclusive-fullscreen detection.
//
// A game running in true exclusive fullscreen owns the display scanout, so the
// desktop compositor is out of the path and no ordinary window draws over it -
// topmost, layered and no-activate flags make no difference. Our overlays are
// still created and painted correctly, they just never reach the screen, which
// looks like the daemon silently doing nothing.
//
// Windows reports that condition through SHQueryUserNotificationState, the same
// signal it uses to hold back toast notifications. Checking it lets the daemon
// say "the overlay was suppressed" instead of leaving the user guessing.

// True when Windows reports a Direct3D exclusive-fullscreen app is running.
bool exclusiveFullscreenActive();

// Called as an overlay is shown. Records whether it could actually be seen.
// `what` names the overlay for the status text, e.g. L"media picker".
void noteOverlayShown(const std::wstring& what);

// Human-readable line for the daemon status the UI polls. Empty when no overlay
// has been suppressed yet.
std::wstring describeOverlayVisibility();
