#pragma once
// daemon_control.h -- the one thing a subsystem may ask of the main thread.

// Starts a fresh copy of this daemon and quits this one cleanly. Used when
// the process has accumulated damage a restart is the only cure for (leaked
// worker threads from a stuck Windows service). The new instance is started
// with --restart so it waits for the single-instance lock instead of giving
// up while the old one is still shutting down.
void requestDaemonRestart(const wchar_t* reason);
