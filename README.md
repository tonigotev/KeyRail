# Hotkey To Command

Windows hotkeys that keep working while a game is focused.

Bind a key to switch your audio output, pick which browser tab the media keys
control, swap Discord's microphone, paste from clipboard history, or run
anything else — without the daemon registering itself as an input path that
anti-cheat drivers block.

## Why this exists

Most hotkey tools call `RegisterHotKey` or install a `WH_KEYBOARD_LL` hook. Both
register the process as part of the input pipeline, which is exactly what
kernel-level anti-cheat looks for, and both tend to stop working the moment a
game takes the foreground.

This daemon uses **Raw Input only**. A message-only window registers the HID
keyboard usage with `RIDEV_INPUTSINK`, so `WM_INPUT` keeps arriving while another
app owns the foreground, and `GetRawInputData` supplies the key, the make/break
flag and the source device for every event. Modifier state is tracked from those
same events, so combos match without polling the keyboard.

Two consequences worth knowing up front:

- **Nothing is ever swallowed.** Raw Input observes; it cannot consume. A bound
  key still reaches the focused app, so pick combos that do not collide with
  in-game keys.
- **Injected keys come back.** Keys the daemon synthesizes with `SendInput` are
  reported to it as well, so those are marked and ignored to stop an action
  retriggering its own binding.

`rawinput-test/` is a 200-line standalone version of the same technique. Run it,
focus a game, and watch whether your key presses still print.

## Install

Grab the installer from [Releases](https://github.com/tonigotev/Custom-hotkey-software/releases).
It is a per-user install, so there is no UAC prompt, and it puts everything under
`%LOCALAPPDATA%\Hotkey To Command`.

The installer is unsigned, so SmartScreen will warn on first run. **More info →
Run anyway**, or build from source below if you would rather not.

On first launch the app opens **Setup**, which walks through the two optional
integrations. Neither is required — the app works without both.

## What it does

**Audio**

- Cycle the default output device, or the microphone
- Cycle the output device of just the focused app
- Push-to-talk and push-to-mute, with an optional on-screen indicator

**Media**

- A picker overlay listing everything currently playing, including individual
  browser tabs, so next/previous hit the tab you chose rather than whichever one
  grabbed media focus
- Contextual play/pause, next and previous

**Clipboard**

- Clipboard history with a picker overlay, text and images

**Windows**

- Close or force-kill the focused app
- Launch apps, run scripts, run shell commands

Bindings live in `%APPDATA%\HotkeyToCommand\config.json` and can be edited by
hand or through the UI. Presets let you keep several sets and switch between
them.

## Optional integrations

Both are set up from **Setup** inside the app, and both tell you in the bindings
list when something needs them.

| Integration | Unlocks | Needs |
| --- | --- | --- |
| Browser media bridge | Individual browser tabs as media targets | A Chromium browser |
| Discord audio bridge | Switching Discord's own input/output devices | git, Node.js, pnpm — Setup installs them if missing |

Without the browser bridge the media picker still works, but sees one lumped
browser entry and falls back to system media keys. Without the Discord bridge the
two `cycle_discord_*` actions do nothing, and the focused-app audio cycle works
for every app except Discord.

## Build from source

Requires Windows, Visual Studio 2022 with the C++ workload, Node.js, and Rust via
rustup.

```bash
.\build.ps1
```

Builds just the daemon, a few seconds. Add `-All` to also build the settings UI
and the installers, which takes minutes. `build.cmd` is a double-clickable
wrapper if you would rather not touch the PowerShell execution policy.

The script stops a running daemon through its control pipe first — a running
`hotkeyd.exe` locks its own file — and starts it again afterwards. Output lands
in `dist\`:

```text
dist/hotkeyd.exe                the daemon
dist/media_list.exe             media diagnostic tool
dist/hotkey_to_command_ui.exe   settings UI            (-All)
dist/*-setup.exe, dist/*.msi    installers             (-All)
```

The UI looks for `hotkeyd.exe` beside its own executable before falling back to
the build tree, so `dist\` works as a folder you can copy elsewhere and run.

For UI development with hot reload:

```bash
cd hotkey_to_command_ui
npm install
npm run tauri -- dev
```

## Layout

```text
hotkey_to_command_cpp/     C++ daemon: Raw Input, actions, overlays, control pipe
hotkey_to_command_ui/      Tauri + Svelte settings UI and installer config
integrations/browser/      Chromium extension exposing per-tab media targets
integrations/vencord/      Vencord plugin for Discord device switching
rawinput-test/             Standalone Raw Input proof of concept
```

The UI talks to the daemon over a named pipe at `\\.\pipe\hotkeyd-control` using
one-line JSON commands (`status`, `reload`, `quit`, `trigger`, and friends). The
browser extension connects to `ws://127.0.0.1:8790/browser-media`; the Vencord
plugin to `ws://127.0.0.1:8787`. Both protocols are documented in
`integrations/browser/INTEGRATING.txt`.

## Config example

```json
{
  "version": 1,
  "bindings": [
    {
      "id": "cycle-audio",
      "enabled": true,
      "hotkey": "ctrl+alt+o",
      "action": { "type": "builtin", "name": "cycle_audio_output" }
    },
    {
      "id": "media-picker",
      "enabled": true,
      "hotkey": "f6",
      "action": { "type": "builtin", "name": "media_picker_open" }
    },
    {
      "id": "open-notepad",
      "enabled": true,
      "hotkey": "ctrl+alt+t",
      "action": {
        "type": "open_app",
        "path": "C:\\Windows\\System32\\notepad.exe",
        "args": [],
        "show_window": true
      }
    }
  ]
}
```

## Troubleshooting

**A binding does nothing.** Press **Status** in the sidebar. The Raw Input
counters show events seen, matches and dispatches — if events are climbing but
matches are not, the key is being seen and the combo is not matching.

**Overlays do not appear over a game.** A game in true exclusive fullscreen owns
the display, and no overlay can draw over it — the Discord and Steam overlays hit
the same wall. Set the game to borderless, and leave "Disable fullscreen
optimizations" unchecked on its exe. Status reports when an overlay was
suppressed for this reason.

**Hotkeys do not fire in an app running as administrator.** Windows blocks a
standard-rights process from seeing input destined for an elevated one. Settings
has a "Restart daemon as administrator" option.
