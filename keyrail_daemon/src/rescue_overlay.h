#pragma once
// rescue_overlay.h -- the rescue menu window.
//
// Everything is created at start: a private desktop, a window on it (created
// by a thread that switched to that desktop first), a memory DC, a locked DIB
// and a fixed-pitch font. Showing the menu is SwitchDesktop + BitBlt; nothing
// on that path allocates, touches the disk, or creates a window.
//
// Two surfaces exist so there is always a fallback:
//   private  - window on the "KeyRailRescue" desktop. Same mechanism as
//              Ctrl+Alt+Del, which is why it appears over exclusive-fullscreen
//              D3D and over a stuck compositor.
//   fallback - topmost window on the normal desktop, used when the desktop
//              switch is refused (secure desktop up, session isolation) or
//              disabled in config.
// The status line always says which one is on screen.

#include "config.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace rescue {

struct OverlayStatus {
    bool started = false;
    bool privateDesktop = false;      // the private surface exists
    bool visible = false;
    uint32_t shows = 0;
    uint32_t lastTriggerToPixelMicros = 0;
    wchar_t lastTier[24] = {};
};

bool overlayStart(const RescueSettings& settings, std::wstring* report);
void overlayStop();
void overlayApply(const RescueSettings& settings);

// The auto-reset event the trigger paths signal. Valid after overlayStart.
HANDLE overlayTriggerEvent();

bool overlayVisible();
OverlayStatus overlayStatus();

// Closes the menu as Esc would (control pipe). False when it is not open.
bool overlayDismiss();

// The kill worker reports here; the line shows in the status bar. Copies the
// text into fixed storage, so it is safe from any thread.
void overlaySetResult(const wchar_t* text);

} // namespace rescue
