# OutputDeviceBridge Vencord Userplugin

This plugin connects Discord/Vencord to the Hotkey To Command daemon.

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

Start Hotkey To Command. The daemon listens on:

```text
ws://127.0.0.1:8787
```

In Hotkey To Command, bind a hotkey to:

```text
Cycle Discord output
```

When pressed, the daemon sends:

```text
cycle_output_device
```

The plugin receives it and calls Discord's own `setOutputDevice` path.

## Notes

- This is a Discord-specific adapter. Normal apps should keep using Windows per-app routing.
- If Discord or Vencord changes internal module names, the plugin may need updates.
- If Discord cannot connect, check the Vencord console for CSP or WebSocket errors.
