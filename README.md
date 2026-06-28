# Hotkey To Command

Windows global hotkeys mapped to configurable actions.

This repo contains:

- `hotkey_to_command_cpp/` - C++ hotkey daemon using `RegisterHotKey`, `GetMessage`, a JSON config file, and a local named pipe control channel.
- `hotkey_to_command_ui/` - Tauri + Svelte settings UI for editing bindings and starting/stopping the daemon.
- `integrations/browser/mediaTargetBridge/` - Brave/Chrome extension that exposes per-tab media targets to the daemon.
- `integrations/vencord/outputDeviceBridge/` - Vencord bridge for Discord audio-device control.
- `hotkey_to_command/` and `hotkey_test.py` - earlier Python prototypes/smoke tests.

## Features

- System-wide Windows hotkeys.
- User-editable JSON config at `%APPDATA%\HotkeyToCommand\config.json`.
- Built-in action for cycling the default audio output device.
- Open-app action for launching executables.
- Advanced command action for shell commands.
- UI controls for editing bindings, saving config, checking daemon status, and stopping the daemon.
- Media picker actions for Windows media sessions and browser tab media targets.
- Browser bridge on `ws://127.0.0.1:8790/browser-media`.

## Requirements

- Windows
- Visual Studio 2022 with C++ build tools
- CMake, or Visual Studio's bundled CMake
- Node.js + npm
- Rust via rustup

## Build The C++ Daemon

From `hotkey_to_command_cpp/`:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

The daemon executable is generated at:

```text
hotkey_to_command_cpp/build/Release/hotkeyd.exe
```

The media diagnostic executable is generated at:

```text
hotkey_to_command_cpp/build/Release/media_list.exe
```

## Run The UI

From `hotkey_to_command_ui/`:

```powershell
npm install
npm run tauri -- dev
```

The UI starts the daemon if needed and talks to it through:

```text
\\.\pipe\hotkeyd-control
```

## Browser Media Bridge

For per-tab browser media control, load the extension in Brave or Chrome:

1. Start the daemon or run `media_list.exe`.
2. Open `brave://extensions`.
3. Enable Developer Mode.
4. Click **Load unpacked**.
5. Select:

```text
integrations/browser/mediaTargetBridge
```

Then test:

```powershell
& "C:\Users\boris\Documents\hotkeys stuff\hotkey_to_command_cpp\build\Release\media_list.exe"
```

With the extension connected, browser targets appear as `kind: browser`.

## Config Example

```json
{
  "version": 1,
  "bindings": [
    {
      "id": "cycle-audio",
      "enabled": true,
      "hotkey": "ctrl+alt+o",
      "action": {
        "type": "builtin",
        "name": "cycle_audio_output"
      }
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
    },
    {
      "id": "media-picker",
      "enabled": true,
      "hotkey": "ctrl+alt+m",
      "action": {
        "type": "builtin",
        "name": "media_picker_open"
      }
    },
    {
      "id": "media-next",
      "enabled": true,
      "hotkey": "media_next",
      "action": {
        "type": "builtin",
        "name": "media_next_contextual"
      }
    },
    {
      "id": "media-play-pause",
      "enabled": true,
      "hotkey": "media_play_pause",
      "action": {
        "type": "builtin",
        "name": "media_play_pause_contextual"
      }
    }
  ]
}
```

## Daemon Controls

- `Start` in the UI starts `hotkeyd.exe`.
- `Stop` in the UI sends `{ "command": "quit" }` to the daemon.
- `Status` asks the daemon for currently armed bindings.
- `Ctrl+Alt+Q` is still available as a debug quit hotkey while the console daemon is running.

## Later Installer Plan

When packaging the app, add an optional "Discord support" setup path:

- Detect Discord Stable/PTB/Canary installs.
- Download or locate a Vencord source checkout.
- Install missing build tools such as Git, Node.js, and pnpm when the user approves.
- Copy `integrations/vencord/outputDeviceBridge` into `Vencord/src/userplugins/outputDeviceBridge`.
- Build and inject Vencord, then restart Discord.
- Verify that the Hotkey To Command bridge is reachable on `127.0.0.1:8787`.
- Show the user a checklist for enabling `OutputDeviceBridge` in Vencord and testing the hotkey.
