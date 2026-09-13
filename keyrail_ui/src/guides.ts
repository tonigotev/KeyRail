// Guide content lives here so writing a tutorial never means touching App.svelte.
//
// Adding screenshots: drop the file in src/assets/guides/, import it at the top
// of this file, and set it as a step's `image`. Vite rewrites the import to the
// correct bundled URL, so this works the same in dev and in a packaged build.
// A step with no image just renders as text, so guides stay usable until the
// screenshots exist.
//
//   import loadUnpacked from "./assets/guides/load-unpacked.png";
//   ...
//   { text: "Click Load unpacked.", image: loadUnpacked, caption: "Top-left of the toolbar" }

export type GuideStep = {
  text: string;
  image?: string;
  caption?: string;
  link?: { label: string; url: string };
  code?: string;
};

export type Guide = {
  id: string;
  title: string;
  summary: string;
  tag: "Browser" | "Discord" | "Daemon" | "Games";
  minutes: number;
  steps: GuideStep[];
};

export const guides: Guide[] = [
  {
    id: "browser-extension",
    title: "Install the browser media bridge",
    summary:
      "Lets the media picker see individual browser tabs instead of one lumped browser entry.",
    tag: "Browser",
    minutes: 3,
    steps: [
      {
        text: "Open Setup and press Stage extension. This copies the extension somewhere permanent, which matters because the browser disables an unpacked extension if its folder ever moves."
      },
      {
        text: "Press Open extensions page. The browser opens on its own extensions screen."
      },
      {
        text: "Turn on Developer mode, top right."
      },
      {
        text: "Click Load unpacked, then pick the staged folder. Setup has a Copy path button so you can paste it straight into the file dialog."
      },
      {
        text: "Play something in a tab, then open the media picker. The tab shows up as its own row with a [tab] marker."
      }
    ]
  },
  {
    id: "vencord-bridge",
    title: "Set up the Discord audio bridge",
    summary:
      "Adds hotkeys that cycle Discord's output and microphone devices without opening Discord settings.",
    tag: "Discord",
    minutes: 10,
    steps: [
      {
        text: "The bridge is a Vencord userplugin, and userplugins only run from a Vencord source build. That means git, Node.js and pnpm have to be present first. Setup checks all three and links the installers for whatever is missing.",
        link: { label: "Node.js downloads", url: "https://nodejs.org/en/download" }
      },
      {
        text: "If only pnpm is missing, the Install pnpm button handles it with npm."
      },
      {
        text: "Press Install Discord bridge. Setup clones Vencord, installs its dependencies, copies the plugin in, builds, and injects. It runs a few minutes on a first install and reports each step as it goes."
      },
      {
        text: "Discord has to be closed while Vencord is injected. Leave the Close Discord option ticked and Setup does it for you."
      },
      {
        text: "Start Discord and enable the plugin.",
        code: "Settings > Vencord > Plugins > OutputDeviceBridge"
      },
      {
        text: "Bind a key to Cycle Discord output or Cycle Discord microphone input, then press it while in a voice channel."
      }
    ]
  },
  {
    id: "fullscreen-overlays",
    title: "Make overlays show over games",
    summary:
      "Why the picker sometimes does not appear over a game, and the two settings that fix it.",
    tag: "Games",
    minutes: 2,
    steps: [
      {
        text: "A game in true exclusive fullscreen owns the display directly, so the desktop compositor is out of the path and no overlay can draw over it. This is not specific to this app: the Discord and Steam overlays hit the same wall."
      },
      {
        text: "Set the game to Borderless. Rocket League: Settings > Video > Window Mode > Borderless. League of Legends: Settings > Video > Window Mode > Borderless."
      },
      {
        text: "Leave fullscreen optimizations enabled for the game's exe. Right-click the exe, Properties, Compatibility, and make sure Disable fullscreen optimizations is unchecked. Turning it off is a common latency tweak, and it breaks every overlay."
      },
      {
        text: "Not sure whether this is the cause? Press Status in the sidebar. If exclusive fullscreen swallowed an overlay, the daemon reports it under overlay visibility."
      }
    ]
  },
  {
    id: "daemon-basics",
    title: "How the daemon detects your keys",
    summary:
      "Background on the Raw Input backend and what it means for anti-cheat and key passthrough.",
    tag: "Daemon",
    minutes: 2,
    steps: [
      {
        text: "Every binding is matched from Raw Input (WM_INPUT). The daemon never calls RegisterHotKey or SetWindowsHookEx, because both register the process as an input path, which is what anti-cheat drivers look for."
      },
      {
        text: "Raw Input only observes, so nothing is ever swallowed. A bound key still reaches the game or app underneath it. Pick bindings that do not collide with in-game keys."
      },
      {
        text: "Press Status to see live Raw Input counters: events seen, matches, dispatches, and ignored held-key repeats. If a binding is not firing, that panel says whether the key was seen at all."
      }
    ]
  },
  {
    id: "rescue-menu",
    title: "Use the rescue menu when the machine freezes",
    summary:
      "A process list and killer that comes up when Task Manager will not: over fullscreen games, during memory thrashing, next to a hung app.",
    tag: "Daemon",
    minutes: 2,
    steps: [
      {
        text: "Press Ctrl+Alt+Shift+End (change it under Settings > Rescue menu). The screen switches to a plain black desktop with a process table, the same trick Ctrl+Alt+Del uses, so it appears over exclusive-fullscreen games too."
      },
      {
        text: "If the daemon is certain which process is freezing the machine (a memory hog, a realtime-priority spinner, a disk storm, a fork bomb) it pauses that process first, so the screen can come back at all. The status line says PAUSED with the reason: Enter kills it, Esc resumes it as if nothing happened."
      },
      {
        text: "If the screen still does not come back within a couple of seconds, press the hotkey again: the paused process is killed outright, without waiting for the menu."
      },
      {
        text: "Rows are ranked by how likely they are the culprit: a HUNG app, a process hammering the disk, one pegging a core, one growing memory or threads. The top row is usually the one to kill. S changes the sort order; R re-sorts."
      },
      {
        text: "Up/Down selects. Enter then Y force-ends the process and waits for it to actually exit. C asks the app to close politely instead. Type / and a few letters to filter by name. PgDn shows every process."
      },
      {
        text: "The bottom line always says what happened. A protected process (anti-cheat, antivirus) or one Windows cannot run without is refused with the reason; a kill the daemon cannot perform without administrator rights says so."
      },
      {
        text: "Esc returns to your normal desktop. If nothing is pressed for three minutes it returns on its own."
      }
    ]
  }
];
