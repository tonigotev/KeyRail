#pragma once
// rescue.h -- the rescue menu, as main.cpp sees it.
//
// A resident, deliberately minimal process viewer and killer for when the
// machine is wedged and Task Manager will not come up. It runs on its own
// threads (trigger, sampler, overlay, kill); the main thread only starts and
// stops it and forwards raw keys so the chord has a backup path.

#include "config.h"

#include <windows.h>

#include <string>

// Starts every rescue thread and pre-creates everything the panic path needs.
// `report` gets the multi-line status the daemon prints and the UI shows.
bool startRescue(const RescueSettings& settings, std::wstring* report);
void stopRescue();

// Applies hot-swappable settings after a config reload. Nothing reallocates.
void applyRescueConfig(const RescueSettings& settings);

// Used while the settings UI records a chord, so the rescue hotkey does not
// swallow it. Mirrors WM_KEYRAIL_SUSPEND / RESUME.
void suspendRescueTrigger();
void resumeRescueTrigger();

// Main-thread WM_INPUT path. Returns true when the key was the rescue chord;
// the caller then skips the binding registry for it.
bool rescueObserveRawKey(UINT vk, bool pressed);

// Opens / closes the menu from the control pipe ("trigger" targets).
bool triggerRescue(std::wstring* report);
bool dismissRescue(std::wstring* report);

// Status block for the pipe, including the recent rescue log.
std::wstring describeRescueStatus();
