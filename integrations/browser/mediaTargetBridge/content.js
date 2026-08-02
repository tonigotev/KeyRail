(function () {
  if (window.__keyRailMediaBridgeLoaded) return;
  window.__keyRailMediaBridgeLoaded = true;

  const MEDIA_ID_ATTR = "data-hotkey-to-command-media-id";
  let nextMediaId = 1;

  function adapters() {
    return Array.isArray(window.htcMediaAdapters) ? window.htcMediaAdapters : [];
  }

  function adapterFor(command) {
    return adapters().find((adapter) => {
      try {
        return adapter.matches?.(location) && adapter.canHandle?.(command);
      } catch {
        return false;
      }
    });
  }

  function ensureMediaId(element) {
    let id = element.getAttribute(MEDIA_ID_ATTR);
    if (!id) {
      id = `media-${Date.now()}-${nextMediaId++}`;
      element.setAttribute(MEDIA_ID_ATTR, id);
    }
    return id;
  }

  function mediaElements() {
    const found = new Set();

    function collect(root) {
      if (!root?.querySelectorAll) return;
      for (const element of root.querySelectorAll("video, audio")) {
        found.add(element);
      }
      for (const element of root.querySelectorAll("*")) {
        if (element.shadowRoot) collect(element.shadowRoot);
      }
    }

    collect(document);
    return Array.from(found);
  }

  function mediaSessionInfo() {
    const session = navigator.mediaSession;
    const metadata = session?.metadata;
    const artwork = Array.isArray(metadata?.artwork) && metadata.artwork.length
      ? metadata.artwork[metadata.artwork.length - 1]?.src || ""
      : "";
    return {
      supported: Boolean(session),
      playbackState: session?.playbackState || "none",
      title: metadata?.title || "",
      artist: metadata?.artist || "",
      album: metadata?.album || "",
      artwork
    };
  }

  function bestTitle(element, fallback, sessionInfo) {
    if (sessionInfo?.title) {
      return sessionInfo.artist ? `${sessionInfo.title} - ${sessionInfo.artist}` : sessionInfo.title;
    }
    const aria = element.getAttribute("aria-label");
    const title = element.getAttribute("title");
    const nearby = element.closest("[aria-label], [title]");
    return aria || title || nearby?.getAttribute("aria-label") || nearby?.getAttribute("title") || document.title || fallback || location.hostname;
  }

  function scanMedia(payload) {
    const canNext = Boolean(adapterFor("next"));
    const canPrevious = Boolean(adapterFor("previous"));
    const sessionInfo = mediaSessionInfo();
    const elements = mediaElements();

    const targets = elements
      .filter((element) => {
        const hasSource = Boolean(element.currentSrc || element.src || element.querySelector("source"));
        const hasDuration = Number.isFinite(element.duration) && element.duration > 0;
        const hasProgress = Number.isFinite(element.currentTime) && element.currentTime > 0;
        const sessionSaysActive = sessionInfo.playbackState === "playing" || sessionInfo.playbackState === "paused";
        return hasSource || hasDuration || hasProgress || !element.paused || sessionSaysActive;
      })
      .map((element) => {
        const mediaId = ensureMediaId(element);
        const tabId = payload.tabId;
        const frameId = payload.frameId;
        const playing = sessionInfo.playbackState === "playing" || (!element.paused && !element.ended);
        const audible = playing && !element.muted && element.volume > 0;
        const url = payload.tabUrl || location.href;

        return {
          id: `browser:${tabId}:${frameId}:${mediaId}`,
          kind: "browser_tab",
          tabId,
          frameId,
          mediaId,
          app: "Browser",
          site: location.hostname,
          title: bestTitle(element, payload.tabTitle, sessionInfo),
          url,
          playing,
          audible,
          muted: element.muted,
          documentTitle: document.title || "",
          tabTitle: payload.tabTitle || "",
          mediaSessionSupported: sessionInfo.supported,
          mediaSessionPlaybackState: sessionInfo.playbackState,
          mediaSessionTitle: sessionInfo.title,
          mediaSessionArtist: sessionInfo.artist,
          artworkUrl: sessionInfo.artwork,
          currentTime: Number.isFinite(element.currentTime) ? element.currentTime : 0,
          duration: Number.isFinite(element.duration) ? element.duration : 0,
          canPlayPause: true,
          canNext,
          canPrevious
        };
      });

    if (targets.length > 0) return targets;

    if (sessionInfo.playbackState === "playing" || sessionInfo.playbackState === "paused") {
      return [{
        id: `browser:${payload.tabId}:${payload.frameId}:media-session`,
        kind: "browser_tab",
        tabId: payload.tabId,
        frameId: payload.frameId,
        mediaId: "media-session",
        app: "Browser",
        site: location.hostname,
        title: sessionInfo.title || document.title || payload.tabTitle || location.hostname,
        url: payload.tabUrl || location.href,
        playing: sessionInfo.playbackState === "playing",
        audible: sessionInfo.playbackState === "playing",
        muted: false,
        documentTitle: document.title || "",
        tabTitle: payload.tabTitle || "",
        mediaSessionSupported: sessionInfo.supported,
        mediaSessionPlaybackState: sessionInfo.playbackState,
        mediaSessionTitle: sessionInfo.title,
        mediaSessionArtist: sessionInfo.artist,
        artworkUrl: sessionInfo.artwork,
        currentTime: 0,
        duration: 0,
        canPlayPause: false,
        canNext,
        canPrevious
      }];
    }

    return [];
  }

  async function controlMedia(payload) {
    const element = document.querySelector(`[${MEDIA_ID_ATTR}="${CSS.escape(payload.mediaId)}"]`);
    const command = payload.command;

    if (command === "next" || command === "previous") {
      const adapter = adapterFor(command);
      if (!adapter) return { ok: false, code: "UNSUPPORTED_COMMAND", message: `${command} is not supported on this site yet` };
      const ok = await adapter.run(command);
      return { ok, message: ok ? `${command} clicked` : `${command} failed` };
    }

    if (!element) {
      return { ok: false, code: "TARGET_NOT_FOUND", message: "Media element no longer exists" };
    }

    if (command === "play_pause") {
      if (element.paused) await element.play();
      else element.pause();
      notifyChanged();
      return { ok: true, message: element.paused ? "paused" : "playing" };
    }

    if (command === "play") {
      await element.play();
      notifyChanged();
      return { ok: true, message: "playing" };
    }

    if (command === "pause") {
      element.pause();
      notifyChanged();
      return { ok: true, message: "paused" };
    }

    return { ok: false, code: "UNKNOWN_COMMAND", message: `Unknown command: ${command}` };
  }

  function notifyChanged() {
    try {
      chrome.runtime.sendMessage({ type: "media_state_changed" });
    } catch {
    }
  }

  for (const eventName of ["play", "pause", "ended", "volumechange", "loadedmetadata"]) {
    document.addEventListener(eventName, notifyChanged, true);
  }

  chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (message?.type === "scan_media") {
      sendResponse({ targets: scanMedia(message.payload || {}) });
      return false;
    }

    if (message?.type === "control_media") {
      controlMedia(message.payload || {})
        .then(sendResponse)
        .catch((error) => sendResponse({ ok: false, code: "CONTROL_ERROR", message: String(error) }));
      return true;
    }

    return false;
  });
})();
