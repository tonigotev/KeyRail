# KeyRail Media Bridge

Browser extension bridge for listing and controlling media inside Brave/Chrome tabs.

## Install for development

1. Start `keyraild.exe`.
2. Open `brave://extensions`.
3. Enable **Developer mode**.
4. Click **Load unpacked**.
5. Select this folder:

   `integrations\browser\mediaTargetBridge` inside your clone

6. Open or refresh media tabs.
7. Start the daemon or run `media_list.exe` to inspect detected targets.

The extension connects to:

`ws://127.0.0.1:8790/browser-media`

## Protocol

Daemon to extension:

- `request_targets`
- `control_media`
- `activate_target`
- `ping`

Extension to daemon:

- `hello`
- `media_targets`
- `targets_changed`
- `response`
- `pong`

Message shape:

```json
{
  "id": "daemon-1",
  "type": "control_media",
  "payload": {
      "targetId": "browser:123:0:media-1",
      "command": "play_pause"
  }
}
```

The extension keeps page interaction shallow by default: it scans media state on
demand and uses site controls for next/previous. If those controls fail, it can
inject `ms-capture.js` into the selected tab/frame as a temporary fallback. That
late helper wraps `MediaSession.setActionHandler` for a short window, then
restores the original function. It is not installed at page start, because some
sites rely on very sensitive player boot code.

The response uses the same `id`:

```json
{
  "id": "daemon-1",
  "type": "response",
  "ok": true,
  "payload": {
    "message": "paused"
  }
}
```

