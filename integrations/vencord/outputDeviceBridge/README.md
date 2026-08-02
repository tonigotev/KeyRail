# OutputDeviceBridge Vencord Userplugin

This plugin connects Discord/Vencord to the KeyRail daemon.

## Install

Use a Vencord source checkout with userplugins enabled, then copy this folder to:

```text
Vencord/src/userplugins/outputDeviceBridge/
```

Build and inject Vencord:

```powershell
pnpm build
taskkill /f /im discord.exe
pnpm inject
```

Relaunch Discord, then enable `OutputDeviceBridge` in:

```text
Settings -> Vencord -> Plugins
```

## Test

Start KeyRail. The daemon listens on:

```text
ws://127.0.0.1:8787
```

In KeyRail, bind a hotkey to one of:

```text
Cycle Discord output
Cycle Discord microphone input
```

When pressed, the daemon sends one of:

```text
cycle_output_device
cycle_input_device
```

The plugin receives it and calls Discord's own `setOutputDevice` or `setInputDevice` path.

Supported bridge commands:

```text
cycle_output_device
cycle_input_device
next
prev
next_input
prev_input
list
list_input
{"cmd":"set","id":"output-device-id"}
{"cmd":"set_input","id":"input-device-id"}
```

## Notes

- This is a Discord-specific adapter. Normal apps should keep using Windows per-app routing.
- If Discord or Vencord changes internal module names, the plugin may need updates.
- If Discord cannot connect, check the Vencord console for CSP or WebSocket errors.
