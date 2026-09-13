#pragma once
// rescue_kill.h -- close, kill, verify, and say what actually happened.
//
// TerminateProcess returns success the moment the kill is queued; a process
// with threads stuck in a kernel wait stays alive regardless. So every kill is
// waited on and, when it does not die, diagnosed: protected (anti-cheat / AV),
// critical, or simply unkillable from user mode. Nothing reports silently.

#include "rescue_sampler.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace rescue {

// A process the chord paused because the sampler was sure it was freezing
// the machine. Reversible: Esc resumes it, Enter kills it, a second chord
// press while the menu is still not on screen kills it blind.
struct PauseInfo {
    bool active = false;
    uint32_t pid = 0;
    int64_t createTime = 0;
    Culprit why = Culprit::None;
    wchar_t image[kImageChars] = {};
};

struct KillStatus {
    bool started = false;
    bool elevated = false;          // the daemon's token is elevated
    bool debugPrivilege = false;    // SeDebugPrivilege enabled
};

// Enables SeDebugPrivilege when the token has it and starts the worker thread.
// Never fatal: without the privilege same-user processes are still killable.
bool killStart(std::wstring* report);
void killStop();

// Posts WM_CLOSE to every visible top-level window of the process. PostMessage
// never blocks; a hung app simply never reads it, which the result says.
void requestClose(uint32_t pid, int64_t createTime);

// Terminate, wait up to 3s, diagnose. Refuses the deny-list before trying.
void requestKill(uint32_t pid, int64_t createTime);

// Deny-list check the overlay uses to grey out rows before a request is made.
bool isDenied(uint32_t pid, const wchar_t* image);

// Freezes every thread of the process (NtSuspendProcess). Called on the show
// thread before the desktop switch; one OpenProcess and one kernel call.
bool pauseProcess(const ProcRecord& record, Culprit why);
// Thaws it again, if still paused and not killed. Safe to call when nothing
// is paused.
void resumePaused();
// Blind kill of the paused process: TerminateProcess only, no wait, safe from
// the trigger thread. Returns false when nothing is paused.
bool killPausedNow();
PauseInfo pausedInfo();

KillStatus killStatus();

} // namespace rescue
