# Hotkey To Command Media Bridge

Browser extension bridge for listing and controlling media inside Brave/Chrome tabs.

## Install for development

1. Start `hotkeyd.exe`.
2. Open `brave://extensions`.
3. Enable **Developer mode**.
4. Click **Load unpacked**.
5. Select this folder:

   `C:\Users\boris\Documents\hotkeys stuff\integrations\browser\mediaTargetBridge`

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

The extension also installs `ms-capture.js` as a static MAIN-world
`document_start` content script. It wraps `MediaSession.setActionHandler` so the
bridge can call captured `nexttrack` / `previoustrack` handlers on the selected
tab before falling back to site buttons or normal YouTube video-only history.

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
