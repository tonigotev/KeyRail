# Privacy Policy for Hotkey To Command Media Bridge

Effective date: August 2, 2026

Hotkey To Command Media Bridge is a browser extension for the Hotkey To Command Windows desktop application. Its purpose is to let the desktop app detect and control media playback in individual browser tabs selected by the user.

## Data handled by the extension

The extension handles limited browser tab and media playback information so it can show controllable media tabs in the local desktop app.

This may include:

- Tab URL and domain
- Tab title
- Tab favicon or media artwork
- Media title and artist when exposed by the page
- Playback state, such as playing or paused
- Available media controls, such as play, pause, next, or previous
- Temporary extension connection and scan status

This information may fall under Chrome Web Store categories such as web history and website content because it includes tab URLs, page titles, and media-related page information.

## How the data is used

The extension uses this information only to:

- Detect which browser tabs are currently playing or exposing media
- Send the list of media tabs to the Hotkey To Command desktop app running on the same computer
- Run the user-requested media command, such as play, pause, next, or previous, in the selected tab
- Keep the extension connected to the local desktop app
- Display extension connection and scan status in the extension popup

## Where the data is sent

The extension sends media tab information only to the Hotkey To Command desktop app on the same computer using the local WebSocket address `ws://127.0.0.1:8790`.

The extension does not send this data to any remote server controlled by us.

## Data storage

The extension may temporarily store connection state, scan status, and the last known list of detected media tabs using Chrome extension storage. This is used so the popup and background service worker can recover state while the browser is running.

The extension does not store passwords, authentication tokens, payment information, health information, personal messages, or precise location data.

## Data sharing and sale

We do not sell user data.

We do not transfer user data to third parties except as necessary for the extension's single purpose of communicating with the local Hotkey To Command desktop app on the user's own computer.

We do not use user data for advertising, creditworthiness, lending, analytics, tracking, or unrelated purposes.

## Remote code

The extension does not load or execute remote JavaScript or WebAssembly code. All executable extension code is included in the extension package.

## User control and deletion

Users can stop the extension from handling browser media data by disabling or uninstalling the extension in the browser's extension settings.

Temporary extension state can be cleared by removing the extension or clearing extension site/app data through the browser.

## Changes to this policy

We may update this privacy policy when the extension's behavior changes. The latest version will be available in this repository.

## Contact

