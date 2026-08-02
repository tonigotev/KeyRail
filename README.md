# KeyRail

Windows makes a handful of everyday things needlessly painful, and nobody had
built the tool I wanted. So I built it.

Switching your audio output takes three clicks through a settings panel. The
media keys hit whichever browser tab grabbed focus last, not the one you are
actually listening to. Changing Discord's microphone means opening Discord.
Windows has a clipboard history, but you cannot bind it to the key you want.
None of these are hard problems. They were just left unsolved.

This is a Windows daemon that binds a key to any of them, plus the plumbing to
make those bindings work in places other hotkey tools give up on.

## Install

Download the installer from
[Releases](https://github.com/tonigotev/KeyRail/releases). It is a
per-user install with no UAC prompt.

The installer is unsigned, so SmartScreen will warn on first run: **More info â†’
Run anyway**. Build it yourself from source if you would rather not take my word
for it.

On first launch the app opens **Setup**, which walks through two optional
integrations. Neither is required.

## Features

**Audio**

- Cycle the default output device or the default microphone
- Cycle the output device of just the focused app, leaving everything else alone
- Cycle the focused app's microphone
- Switch Discord's own input and output devices without opening Discord
- Push-to-talk and push-to-mute, with an optional on-screen mic indicator

**Media**

- A picker overlay listing everything currently playing, including individual
  browser tabs, with artwork
- Next, previous and play/pause aimed at the target you picked, not whatever
  Windows decided owns the media keys
- Contextual controls that follow the focused app when you have not picked one
- Per-tab control in Chrome, Brave, Edge and other Chromium browsers, with
  adapters for YouTube, YouTube Music and Spotify

**Clipboard**

- Clipboard history with a picker overlay, for text and images
- A quick picker for the last few entries
- Navigate, confirm and cancel all bindable

**Windows and apps**

- Close the focused app, or force-kill it when it stops responding
- Launch applications with arguments
- Run scripts through Python, PowerShell, Node, AutoHotkey or any interpreter
- Run arbitrary shell commands

**The app itself**

- Two hotkey modes: everything global, or a launcher key that wakes the bindings
  for a few seconds so ordinary keys can be used without permanently claiming
  them
- Presets, so you can keep several sets of bindings and switch between them
- Start with Windows, optionally elevated so hotkeys work inside apps running as
  administrator
- Four themes, a live log, and a guides section
- Config is plain JSON at `%APPDATA%\KeyRail\config.json`, editable by
  hand if you prefer

## How keys are detected

Two backends run at once, because neither is enough on its own.

**`RegisterHotKey` is primary.** It genuinely claims the combo: Windows hands
the key to us and the focused app never sees it. That is what you want from a
hotkey. Press F6 to open the media picker and F6 does not also leak into
whatever you were typing in.

**Raw Input is the fallback.** A message-only window registers the HID keyboard
usage with `RIDEV_INPUTSINK`, so `WM_INPUT` keeps arriving even when normal
hotkey delivery does not happen â€” most visibly when a foreground application has
already claimed the key, which is common in games running kernel-level
anti-cheat. Raw Input can only observe, never consume, so a key caught this way
still reaches the app underneath. That is the trade: it works where the primary
cannot, at the cost of not being exclusive.

Both are armed for every binding, and a per-combo debounce means a press seen by
both fires the action exactly once. **Status** in the sidebar shows which
bindings were claimed and live Raw Input counters, so when something does not
fire you can tell whether the key was even seen.

`rawinput-test/` is a small standalone version of the fallback, useful for
checking whether a device is still observable while a particular app is focused.

## Optional integrations

Both are set up from **Setup** in the app, and the bindings list says when
something needs one.

| Integration | Unlocks | Needs |
| --- | --- | --- |
| Browser media bridge | Individual browser tabs as media targets | [Chrome Web Store extension](https://chromewebstore.google.com/detail/jejgnmfgjblnacclebdhmonhljfcbfdo) |
| Discord audio bridge | Switching Discord's own devices | git, Node.js, pnpm â€” Setup installs them if missing |

Without the browser bridge the media picker still works, it just sees one lumped
browser entry and falls back to system media keys. Without the Discord bridge the
two Discord device actions do nothing, and the focused-app audio cycle works for
every app except Discord.

## Build from source

Needs Windows, Visual Studio 2022 with the C++ workload, Node.js, and Rust via
rustup.

```bash
.\build.ps1
```

Builds the daemon in a few seconds. `-All` also builds the settings UI and the
installers, which takes minutes. `build.cmd` is a double-clickable wrapper if you
would rather not touch the PowerShell execution policy.

The script stops a running daemon over its control pipe first â€” a running
`keyraild.exe` locks its own file â€” and restarts it afterwards. Output lands in
`dist\`:

```text
dist/keyraild.exe                the daemon
dist/media_list.exe             media diagnostic tool
dist/keyrail_ui.exe   settings UI            (-All)
dist/*-setup.exe, dist/*.msi    installers             (-All)
```

For UI development with hot reload:

```bash
cd keyrail_ui
npm install
npm run tauri -- dev
```

## Layout

```text
keyrail_daemon/     C++ daemon: detection, actions, overlays, control pipe
keyrail_ui/      Tauri + Svelte settings UI and installer config
integrations/browser/      Chromium extension exposing per-tab media targets
integrations/vencord/      Vencord plugin for Discord device switching
rawinput-test/             Standalone Raw Input proof of concept
```

The UI talks to the daemon over a named pipe at `\\.\pipe\keyrail-control` with
one-line JSON commands. The browser extension connects to
`ws://127.0.0.1:8790/browser-media`, the Vencord plugin to `ws://127.0.0.1:8787`.

Building something on top? `integrations/browser/INTEGRATING.txt` documents the
extension's WebSocket protocol â€” you can drive it from your own app without
writing a browser extension.

## Troubleshooting

**A binding does nothing.** Press **Status**. If Raw Input events are climbing
but matches are not, the key is being seen and the combo is not matching. If the
binding says "raw input only" rather than "claimed", something else on the system
already owns that combo.

**Overlays do not appear over a game.** A game in true exclusive fullscreen owns
the display and nothing can draw over it â€” the Discord and Steam overlays hit the
same wall. Set the game to borderless and leave "Disable fullscreen
optimizations" unchecked on its exe. Status reports when an overlay was
suppressed for this reason.

**Hotkeys do not fire in an app running as administrator.** Windows blocks a
standard-rights process from seeing input meant for an elevated one. Settings has
a "Restart daemon as administrator" option.
