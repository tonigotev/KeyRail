# KeyRail — handoff

Everything another assistant needs to continue this project. Written 2026-10-26.
Repo: `C:\Users\anton\Documents\KeyRail` (GitHub: `tonigotev/KeyRail`).

---

## 1. What KeyRail is

A Windows hotkey manager: a tray-resident C++ daemon plus a Tauri + Svelte
settings UI, plus a Manifest V3 browser extension for per-tab media control.

```
keyrail_daemon/     C++17 daemon (CMake, MSVC, static CRT). Flat src/, no libs.
keyrail_daemon/stress/   Stressors for testing the rescue menu (not shipped)
keyrail_ui/         Tauri 2 + Svelte settings UI, installer config, updater
integrations/browser/mediaTargetBridge/   Chromium extension (per-tab media)
integrations/vencord/outputDeviceBridge/  Vencord plugin (Discord devices)
rawinput-test/      Standalone Raw Input proof of concept
```

Build targets in `keyrail_daemon/CMakeLists.txt`:

- `keyraild` — the daemon. `/SUBSYSTEM:WINDOWS`, `main()` via `mainCRTStartup`.
- `media_list` — console diagnostic for media targets.
- `rescue_probe` — console dump of the rescue menu's ranked process table.
- `rescue_stress` — synthetic stressors: `spin | balloon | hang | leak | fullscreen`.

The last two do not ship and are not copied into `dist\`.

### Build

```powershell
# daemon only (seconds)
.\build.ps1
# daemon + UI + installers (minutes), and apply the installer over the installed copy
.\build.ps1 -All
# same, plus archive signed artifacts into releases\v<version>\
.\build.ps1 -All -Release
```

CMake lives at
`C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
Direct build: `cmake --build keyrail_daemon\build --config Release`.

---

## 2. Non-negotiable design decisions

These were deliberate and expensive to arrive at. Do not undo them without
asking the owner.

1. **Never use `SetWindowsHookEx` / `WH_KEYBOARD_LL`.** It was removed on
   purpose (commits `a93f753`, `6ecdf36`) because kernel anti-cheat (Vanguard,
   EAC, BattlEye) treats global keyboard hooks as keylogger behaviour. Hotkeys
   use two backends together: `RegisterHotKey` (claims the chord exclusively)
   plus Raw Input `RIDEV_INPUTSINK` (observes, survives when a game owns the
   key). The registry debounces per combo (120 ms) so one press fires once.
2. **Layered fallbacks, never single-path.** Every subsystem degrades: media
   control is direct element → `mediaSession` → DOM adapter → `history.back()`;
   the rescue overlay is private desktop → topmost window; kills are close →
   terminate → diagnose.
3. **Process priority class stays NORMAL.** Per-thread priorities only. Raising
   the class would lift every background thread above the user's foreground app.
4. **Report honestly.** A kill that cannot work says why (protected process,
   critical process, kernel wait) instead of failing silently.

---

## 3. Daemon architecture

`main.cpp` owns the message loop on the main thread: the Raw Input sink window,
`WM_HOTKEY`, `WM_TIMER`, and `WM_APP` control messages from the pipe thread.
Config reload happens here.

Subsystem pattern, used by every module: free functions `startX()` / `stopX()`,
anonymous-namespace globals, its own thread, and a `describeX()` that returns a
status string appended in `getStatus()`.

Threads: main, control pipe, media poll, media overlay, clipboard, browser
bridge (accept + heartbeat + per-client), Discord bridge, and the rescue
module's own set (below). Hotkey actions each run on a detached `std::thread`.

### Control pipe

`\\.\pipe\keyrail-control`, one-line JSON. Commands: `status`, `reload`,
`quit`, `suspend_hotkeys`, `resume_hotkeys`, `media_status`, `discord_status`,
`browser_status`, `clipboard_status`, and `trigger` with a `target`:

```
media_picker_open | media_next | media_previous | media_play_pause
media_picker_confirm | media_picker_cancel
clipboard_quick | clipboard_history
rescue_open | rescue_close | rescue_dump
```

`rescue_dump` renders the rescue menu to `%TEMP%\keyrail-rescue.bmp` without
switching desktops — the fastest way to see the overlay while developing.

PowerShell helper for talking to it:

```powershell
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "keyrail-control", [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(3000); $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
$b = [System.Text.Encoding]::UTF8.GetBytes('{"command":"status"}')
$pipe.Write($b, 0, $b.Length); $pipe.Flush()
$buf = New-Object byte[] 65536; $n = $pipe.Read($buf, 0, $buf.Length)
([System.Text.Encoding]::UTF8.GetString($buf, 0, $n) | ConvertFrom-Json).status
```

### Config

`%APPDATA%\KeyRail\config.json`. snake_case keys. Top-level sections:
`version`, `settings`, `onboarding`, `rescue`, `bindings`.

**Defaults live in three places and must be kept in step:**

1. `keyrail_daemon/src/config.cpp` → `defaultConfigText()` (and `parseRescue`)
2. `keyrail_ui/src-tauri/src/main.rs` → `default_config()`
3. `keyrail_ui/src/App.svelte` → `defaultConfig` + the `withConfigDefaults` merge

Missing keys fall back to struct initialisers in `config.h`; nothing is written
back by the daemon. The UI preserves unknown keys through a save.

Rescue section:

```json
"rescue": {
  "enabled": true,
  "hotkey": "ctrl+alt+shift+end",
  "sample_interval_ms": 1000,
  "row_count": 15,
  "use_private_desktop": true,
  "title_deadline_ms": 50,
  "auto_pause": true,
  "style": "modern",
  "weights": { "hung": 5, "faults": 4, "cpu": 3, "priv": 2, "threads": 1, "io": 2 }
}
```

---

## 4. The rescue menu (the big recent feature)

A process viewer and killer on a global chord that still works when the machine
is wedged and Task Manager will not come up.

**Governing rule — the panic path does no disk I/O, no heap allocation, no lazy
DLL loading, no cross-process sends without a timeout, and no window creation.**
Everything is prepared at startup and `VirtualLock`ed.

Modules in `keyrail_daemon/src/`:

| File | Role |
|---|---|
| `rescue_nt.h/.cpp` | Full `SYSTEM_PROCESS_INFORMATION` / `SYSTEM_THREAD_INFORMATION` layouts (x64, static-asserted), cached ntdll pointers incl. `NtSuspendProcess`/`NtResumeProcess`. |
| `rescue_sampler.h/.cpp` | 1 Hz `NtQuerySystemInformation` walk into locked double buffers; deltas keyed on (pid, create time) and normalised by QPC elapsed; thread-state bucketing; scoring; `findCulprit()`. Also does a deadline-bounded `EnumWindows` pass using only kernel-side calls (`IsHungAppWindow`, `InternalGetWindowText`). |
| `rescue_trigger.h/.cpp` | `RegisterHotKey` on its own thread + the main thread's Raw Input sink as backup. Sets an event; that is all. |
| `rescue_overlay.h/.cpp` | Private desktop (`CreateDesktop` + `SetThreadDesktop` before any window) and a topmost fallback window. Own glyph atlas, locked DIB, two styles (`modern`, `legacy`). Grouping, sections, keyboard + mouse. |
| `rescue_kill.h/.cpp` | Close → terminate → wait 3 s → diagnose. Deny-list, `SeDebugPrivilege`, pause/resume/blind-kill of the auto-paused culprit. Queue holds up to 64 requests so a whole group can be killed. |
| `rescue_meta.h/.cpp` | Tier-2 friendly names from version resources, ≤5 per tick, background I/O priority. Panic path does a hash lookup only. |
| `rescue_residency.h/.cpp` | Gives dwm.exe a hard working-set floor when elevated, so the compositor cannot be paged out. |
| `rescue_log.h` | Fixed ring, no allocation, safe from a TIME_CRITICAL thread. |
| `rescue.h/.cpp` | Lifecycle glue and `describeRescueStatus()`. |

Threads: trigger (TIME_CRITICAL), sampler (HIGHEST), show (TIME_CRITICAL, pumps
no messages), one per overlay surface, kill worker, metadata worker (LOWEST +
background mode).

### Measured behaviour (do not re-derive, these cost real time)

- Idle: trigger → first pixel **16–34 ms**; sampler tick ~4–8 ms for ~400
  processes / 7,000 threads.
- Under a real 36 GB thrash on a 32 GB machine: our own path stays under 10 ms,
  but `SwitchDesktop` and the final `BitBlt` waited **25 s and 15 s** on Windows
  paging its own display stack back in. Pinning dwm helped once (285 ms), not
  reliably.
- **Auto-pause is what actually fixed it**: suspending the culprit before the
  desktop switch took the menu from 25–40 s to **2.7 s** under the same thrash.
  100 ms is not reachable from user mode; a kernel driver could, but that was
  ruled out (signing + anti-cheat exposure).
- Culprit detection lesson: **the memory hog itself barely hard-faults — its
  victims do.** The rule is "the system is hard-faulting heavily AND one process
  holds a dominant slice of RAM while churning it". A thread-state "looks hung"
  heuristic was tried and removed: Qt/Electron/Chromium idle in a wait it
  misread, so every idle browser looked hung. `IsHungAppWindow` only.

### Testing it

```powershell
cd keyrail_daemon\build\Release
.\rescue_probe.exe --ticks 3 --rows 15     # ranked table + CULPRIT verdict
.\rescue_stress.exe spin --threads 1
.\rescue_stress.exe balloon --mb 36000     # thrashes the machine; watchdog it
.\rescue_stress.exe hang
.\rescue_stress.exe leak --rate 40
.\rescue_stress.exe fullscreen             # exclusive D3D11, for the overlay test
```

Always arm a watchdog before a balloon run:
`Start-Process powershell -ArgumentList "-Command","Start-Sleep 180; taskkill /IM rescue_stress.exe /F"`.

---

## 5. Performance instrumentation (newest work, and the open question)

`perf_watch.h/.cpp`. Every periodic operation is wrapped in a `perf::Scope`,
which tallies call count / average / max per operation and logs anything over
250 ms to `%APPDATA%\KeyRail\perf.log` with a timestamp. A heartbeat thread
wakes every 250 ms and records when the machine stopped scheduling it at all,
stating whether KeyRail was mid-operation. Both appear in `status`.

Fixed because of it (commit `d4fb7a5`), daemon steady cost **13% of a core →
2.2%**:

- Media poll ran at 1 Hz forever. Now 1 s only while media is in use, 5 s idle,
  backing off to 15 s then 30 s after repeated empty polls.
- Album art was re-fetched and re-decoded every poll per session. Now cached on
  (appId | title | artist).
- The thread-leak check took a system-wide Toolhelp snapshot every 10 s. It now
  reads the rescue sampler's existing data: 7 ms → 0.

**OPEN ISSUE — the reason the instrumentation exists.** The owner reports the
whole PC hard-lagging for 5–10 seconds roughly every 20 minutes, noticeable
while gaming, and suspects keyraild. That has **not** been attributed yet. Next
step: after it happens, read

```
%APPDATA%\KeyRail\perf.log
```

and the `where the daemon's time goes` / `slow operations` blocks in `status`.
If a heartbeat line says "nothing in KeyRail was running", the stall is not
ours and the search should move elsewhere (this machine also has a sick Windows
media broker and leaky `Wcmsvc` / `gamingservices`).

### Earlier related fix (commits `b02f696`, `ab4b7fb`)

The daemon was found at **29,000 threads / 1.7 GB** after a day. Every
`GlobalSystemMediaTransportControlsSessionManager::RequestAsync()` to a hung
Windows media broker leaked a worker thread that never returned; 27,000 of them
left 31 million kernel RPC messages (26 GB of paged pool). The guard: watch our
own thread count, pause media queries for 10 minutes when they start leaking,
and if 500+ stuck threads have already accumulated, restart the daemon
(`requestDaemonRestart` in `daemon_control.h`; the new copy is launched with
`--restart` so it waits for the single-instance mutex and inherits elevation).

---

## 6. Release process

Version lives in **three** files, all must match:
`keyrail_ui/src-tauri/tauri.conf.json`, `keyrail_ui/package.json`,
`keyrail_ui/src-tauri/Cargo.toml`.

Updater signing key: `%USERPROFILE%\.tauri\keyrail.key` + `keyrail.pass`.
Without them the build still produces installers but no `.sig` / `latest.json`,
and existing installs cannot auto-update. The public key in `tauri.conf.json`
must match `keyrail.key.pub`. **Never commit the key.**

`.\build.ps1 -All -Release` then publish: push, tag `v<version>`, create the
GitHub release, upload **`KeyRail_<version>_x64-setup.exe` and `latest.json`**
from `releases\v<version>\`. The updater endpoint is
`https://github.com/tonigotev/KeyRail/releases/latest/download/latest.json`, so
a release without `latest.json` attached makes every client report "update
check failed".

### Current state (2026-10-26)

- Version **0.1.5**, branch `main`, **5 commits ahead of `origin/main`, unpushed**.
- Published on GitHub: **v0.1.3 is the newest**, and v0.1.2/v0.1.3 were uploaded
  **without `latest.json`** — that is why clients say "update check failed".
  v0.1.4 and v0.1.5 exist only locally in `releases\`.
- `releases\v0.1.5\` is signed but was built **before** commits `7fb4715` and
  `d4fb7a5`, so rebuild before publishing.
- The installed copy (`%LOCALAPPDATA%\KeyRail\`) runs a build newer than any
  release, installed by hand during debugging.

---

## 7. Gotchas that cost time here

- **The running daemon locks its own exe.** Stop it (`quit` over the pipe, then
  `Stop-Process`) before copying a new build over `%LOCALAPPDATA%\KeyRail\keyraild.exe`.
- **`build.ps1` restarts the daemon non-elevated.** For elevation run
  `schtasks /Run /TN KeyRailDaemonElevated`. Elevation is arranged entirely by
  the UI (a `/RL HIGHEST` scheduled task); the daemon has no self-elevation.
- **Shared sources break sibling targets.** `media_sessions.cpp` is in both
  `keyraild` and `media_list`; `rescue_sampler.cpp` is in `keyraild`,
  `media_list` and `rescue_probe`. Adding a dependency to a shared file means
  adding that `.cpp` to every target that uses it (and `media_list_main.cpp`
  carries a no-op `requestDaemonRestart`).
- **Do not let a script write `\n` into a C++ string literal.** Python helper
  scripts that patch source must escape it as `\\n`, or MSVC reports
  "newline in constant". This happened repeatedly.
- `keyrail_daemon/build/` is **tracked in git** despite `.gitignore`, so every
  rebuild shows ~70 modified files. `git rm -r --cached keyrail_daemon/build`
  would fix it; the owner has not decided.
- `keyrail_ui/src/guides.ts` still claims the daemon "never calls
  RegisterHotKey" — stale since the Raw Input rewrite. Worth correcting.

---

## 8. How the owner likes to work

- Wants the reasoning and the trade-offs, not just code. Two options → say both,
  then recommend one.
- Layered fallbacks over single-path solutions, every time.
- Do not widen scope. The rescue menu is a rescue tool, not a Task Manager
  clone; adding a feature "because Task Manager has it" is the signal to stop.
- Be direct, skip reassurance, and never claim something works that has not been
  verified — measure instead.
- No planning documents in the repo. Code comments explain *why*, in the voice
  of the surrounding code.
- Commit messages: plain prose explaining the why, ending with
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
