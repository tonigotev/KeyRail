# Hotkey To Command

Windows global hotkeys mapped to configurable actions.

This repo contains:

- `hotkey_to_command_cpp/` - C++ hotkey daemon using `RegisterHotKey`, `GetMessage`, a JSON config file, and a local named pipe control channel.
- `hotkey_to_command_ui/` - Tauri + Svelte settings UI for editing bindings and starting/stopping the daemon.
- `hotkey_to_command/` and `hotkey_test.py` - earlier Python prototypes/smoke tests.

## Features

- System-wide Windows hotkeys.
- User-editable JSON config at `%APPDATA%\HotkeyToCommand\config.json`.
- Built-in action for cycling the default audio output device.
- Open-app action for launching executables.
- Advanced command action for shell commands.
- UI controls for editing bindings, saving config, checking daemon status, and stopping the daemon.

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
    }
  ]
}
```

## Daemon Controls

- `Start` in the UI starts `hotkeyd.exe`.
- `Stop` in the UI sends `{ "command": "quit" }` to the daemon.
- `Status` asks the daemon for currently armed bindings.
- `Ctrl+Alt+Q` is still available as a debug quit hotkey while the console daemon is running.
