#pragma once
// rescue_trigger.h -- the two ways the rescue chord reaches the overlay.
//
//   1. RegisterHotKey on a dedicated thread. WM_HOTKEY lands in that thread's
//      own queue, which holds nothing else, so the chord cannot queue behind a
//      config reload or an audio call on the main thread. It also claims the
//      chord: the wedged foreground app never sees it.
//   2. The Raw Input sink the daemon already runs on the main thread. main.cpp
//      hands every key here first; a chord match sets the same event. This is
//      the backup for when RegisterHotKey was refused or dropped.
//
// Both paths do one thing: compare, SetEvent, return. No WH_KEYBOARD_LL: the
// daemon deliberately avoids global hooks because kernel anti-cheat flags them.

#include "hotkey.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace rescue {

// `trigger` is the event the overlay sleeps on. Registers the chord on its own
// thread and reports whether RegisterHotKey succeeded.
bool triggerStart(const std::wstring& hotkey, HANDLE trigger, std::wstring* report);
void triggerStop();

// Re-registers with a new chord. Safe from any thread.
void triggerApply(const std::wstring& hotkey);

// While suspended the chord is unregistered so the settings UI can record it.
void triggerSuspend();
void triggerResume();

// Called from the main thread's WM_INPUT handler for every key. Returns true
// when the key was the rescue chord (fired or not) so the caller stops there.
bool observeRawKey(UINT vk, bool pressed);

// Fires the trigger from code (control pipe). Same event, same path.
void fireTrigger();

// Called on every chord press, on the trigger thread, after the event is set.
// The overlay uses it to escalate a press that arrives while the previous one
// is still fighting the display: must return quickly.
using PressHook = void (*)();
void triggerSetPressHook(PressHook hook);

// QPC stamp taken when the chord was last seen, so the overlay can measure
// trigger-to-first-pixel from the keypress rather than from its own wake-up.
int64_t lastTriggerQpc();

// True when RegisterHotKey currently owns the chord.
bool triggerClaimed();
HotkeyCombo triggerCombo();

} // namespace rescue
