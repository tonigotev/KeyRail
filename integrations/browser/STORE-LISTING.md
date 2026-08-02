# Chrome Web Store listing â€” field by field

Rebuild the upload package with `.\package-extension.ps1` (add `-Version 0.1.1`
for a resubmission; both stores reject an upload whose version has not risen).

Generated graphics live in `integrations\browser\store-assets\`.

---

## Store listing tab

### Description

Paste verbatim. The first paragraph exists because this extension does nothing
on its own â€” reviewers and users both need that stated before anything else, or
it reads as broken.

```text
This extension is the browser half of KeyRail, a Windows app that maps
keyboard shortcuts to system actions. It does nothing on its own â€” install the
desktop app first.

Windows only reports one media session per browser, so a normal media key hits
whichever tab happens to have focus. If you have music in one tab, a video in
another and something autoplaying in a third, you cannot reach the one you want.

This bridge fixes that. It reports each tab that is playing audio or video to the
desktop app, so you can:

â€¢ See every playing tab in a picker and choose one with a hotkey
â€¢ Play, pause and skip that specific tab, even while a game or another app is
  focused
â€¢ Keep controlling the tab you picked, instead of whatever grabbed media focus
  last

How it works

The extension watches which tabs have active media and sends their title, artist
and playback state to the desktop app over a local connection on your own machine
(ws://127.0.0.1:8790). When you press a bound hotkey, the app tells the extension
which tab to act on, and the extension calls play, pause, next or previous on
that page's own media element.

Privacy

Nothing leaves your computer. There are no servers, no accounts, no analytics and
no tracking. The only destination is the KeyRail app running locally.
Page access is used solely to read media playback state and issue playback
commands.

Requires the KeyRail desktop app for Windows:
https://github.com/tonigotev/KeyRail
```

### Category

```text
Productivity
```

Sub-category if prompted: **Workflow & Planning**

### Language

```text
English (United States)
```

---

## Graphic assets

| Field | File | Notes |
| --- | --- | --- |
| Store icon (128Ã—128) | `integrations\browser\store-assets\store-icon-128.png` | Required |
| Screenshot (1280Ã—800) | see below | Required, at least one |
| Small promo tile (440Ã—280) | `integrations\browser\store-assets\promo-small-440x280.png` | Optional |
| Marquee promo tile (1400Ã—560) | `integrations\browser\store-assets\promo-marquee-1400x560.png` | Optional, only used if featured |
| Global promo video | leave empty | |

Promo tiles are generated as 24-bit PNG with no alpha channel, which the store
requires and which `Compress-Archive`-style tooling gets wrong by default.

### Screenshot

Not generated. A screenshot has to show the real product, and the picker
currently has nothing to list because no media is playing. To produce one:

1. Start playback in two or three browser tabs (a music tab and a video tab show
   the point best).
2. Press the media picker hotkey so the overlay appears.
3. Capture it, then say the word and it can be composited onto a clean 1280Ã—800
   backdrop so none of the desktop is exposed.

---

## Additional fields

| Field | Value |
| --- | --- |
| Official URL | `None` â€” needs Search Console domain verification, skip it |
| Homepage URL | `https://github.com/tonigotev/KeyRail` |
| Support URL | `https://github.com/tonigotev/KeyRail/issues` |
| Mature content | Off |

---

## Privacy tab

### Single purpose

```text
Report which browser tabs are playing media to the KeyRail desktop app,
and play, pause or skip the tab the user selects.
```

### Permission justifications

Answer each one specifically. Broad host access is the usual rejection reason, so
do not reuse a generic sentence across fields.

**tabs**

```text
Enumerates open tabs to determine which are currently playing audio or video, so
the desktop app can present them as selectable targets.
```

**scripting**

```text
Reads playback state from a tab and issues play, pause, next and previous on that
page's own media element. This is the only way to control one specific tab rather
than whichever tab currently holds system media focus.
```

**webNavigation**

```text
Detects in-page navigation on single-page sites such as YouTube, so a tab's
reported track stays accurate when the user changes video without a full page
load.
```

**alarms**

```text
Periodically re-establishes the local connection to the desktop app if that app
restarts.
```

**storage**

```text
Caches the last known list of media tabs so the popup can render immediately
instead of rescanning on open.
```

**Host permission `<all_urls>`**

```text
Media can play on any website, and the user chooses which tab to control, so the
set of relevant sites cannot be known in advance. The extension reads only media
playback state â€” title, artist, playing or paused, and which playback controls
the page exposes â€” and does not read or transmit other page content.
```

**Remote code**

```text
No. All code is contained in the package. Nothing is fetched or evaluated from a
remote source.
```

### Data usage

Tick **only** what applies, which is nothing in the collection categories.
Declare all three certifications:

- Not being sold to third parties
- Not being used or transferred for purposes unrelated to the item's single purpose
- Not being used or transferred to determine creditworthiness or for lending

If the form insists on a category, disclose **Website content** and add:

```text
Media metadata (track title, artist, playback state) is sent only to the Hotkey To
Command desktop app over a loopback connection on the same machine
(ws://127.0.0.1:8790). No data is transmitted off the device and none is stored
remotely.
```

---

## Distribution tab

- Visibility: **Public** (or Unlisted while testing)
- Regions: all
- Pricing: free

---

## After it is published

The in-app Setup wizard still walks through staging a folder and Load unpacked,
which stops being how anyone installs this. Once the listing is live, that step
should become an "Install from the store" button opening the listing URL, with
the staging path kept only as a fallback for unpublished builds. Relevant code:
`stage_browser_extension` in `src-tauri/src/setup.rs` and the browser step in
`App.svelte`.

Edge Add-ons takes the same zip: <https://partner.microsoft.com/dashboard/microsoftedge>
