const WS_URL = "ws://127.0.0.1:8790/browser-media";
const SCAN_TIMEOUT_MS = 900;
const CONTROL_TIMEOUT_MS = 1500;
const IMAGE_FETCH_TIMEOUT_MS = 900;
const MAX_INLINE_IMAGE_BYTES = 700 * 1024;
const HEARTBEAT_MS = 20000;
const ALARM_NAME = "hotkey-to-command-media-keepalive";
const SKIP_SCHEMES = /^(chrome|edge|brave|opera|vivaldi|about|chrome-extension|moz-extension|devtools|view-source|chrome-search|chrome-untrusted):/i;

let socket;
let reconnectTimer;
let heartbeatTimer;
let reconnectDelay = 1000;
let nextLocalId = 1;
let lastScanLog = [];
let lastTargets = [];
const youtubeWatchStacks = new Map();
const youtubeProgrammaticPrevious = new Map();
const imageDataUrlCache = new Map();

function send(message) {
  if (!socket || socket.readyState !== WebSocket.OPEN) return false;
  socket.send(JSON.stringify(message));
  return true;
}

function responseFor(id, ok, payload = {}, error = undefined) {
  const message = { id, type: "response", ok, payload };
  if (error) message.error = error;
  send(message);
}

function setConnectionState(connected) {
  chrome.storage.session.set({
    connected,
    connectedAt: connected ? Date.now() : undefined,
    reconnectDelay,
    socketState: socket ? socket.readyState : -1,
    lastConnectionChange: Date.now()
  }).catch(() => undefined);
}

function connect() {
  if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) return;
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = undefined;
  }

  try {
    socket = new WebSocket(WS_URL);
  } catch {
    scheduleReconnect();
    return;
  }

  socket.addEventListener("open", () => {
    reconnectDelay = 1000;
    setConnectionState(true);
    startHeartbeat();
    send({
      id: `extension-${nextLocalId++}`,
      type: "hello",
      payload: {
        name: chrome.runtime.getManifest().name,
        version: chrome.runtime.getManifest().version
      }
    });
    void publishTargets();
  });

  socket.addEventListener("message", (event) => {
    void handleDaemonMessage(event.data);
  });
  socket.addEventListener("close", handleDisconnect);
  socket.addEventListener("error", handleDisconnect);
}

function handleDisconnect() {
  stopHeartbeat();
  socket = undefined;
  setConnectionState(false);
  scheduleReconnect();
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = undefined;
    connect();
  }, reconnectDelay);
  reconnectDelay = Math.min(reconnectDelay * 2, 15000);
}

function startHeartbeat() {
  stopHeartbeat();
  heartbeatTimer = setInterval(() => {
    send({ id: `extension-${nextLocalId++}`, type: "ping", payload: { at: Date.now() } });
  }, HEARTBEAT_MS);
}

function stopHeartbeat() {
  if (heartbeatTimer) clearInterval(heartbeatTimer);
  heartbeatTimer = undefined;
}

async function handleDaemonMessage(raw) {
  let message;
  try {
    message = JSON.parse(raw);
  } catch {
    return;
  }

  if (message.type === "request_targets") {
    const result = await collectTargets();
    responseFor(message.id, true, result);
    return;
  }

  if (message.type === "control_media") {
    const result = await controlTarget(message.payload?.targetId, message.payload?.command);
    responseFor(message.id, result.ok, result, result.ok ? undefined : {
      code: result.code || "CONTROL_FAILED",
      message: result.message || "Could not control target"
    });
    if (result.ok) void publishTargets();
    return;
  }

  if (message.type === "activate_target") {
    const result = await activateTarget(message.payload?.targetId, message.payload?.restoreAfterMs);
    responseFor(message.id, result.ok, result, result.ok ? undefined : {
      code: result.code || "ACTIVATE_FAILED",
      message: result.message || "Could not activate target tab"
    });
    return;
  }

  if (message.type === "ping") {
    send({ id: message.id, type: "pong", ok: true, payload: {} });
  }
}

function withTimeout(promise, ms, label) {
  return Promise.race([
    promise,
    new Promise((_, reject) => setTimeout(() => reject(new Error(`${label || "operation"} timeout`)), ms))
  ]);
}

function bytesToBase64(bytes) {
  let binary = "";
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize));
  }
  return btoa(binary);
}

async function imageAsDataUrl(url) {
  if (!url || url.startsWith("data:")) return url || "";
  if (!/^https?:\/\//i.test(url)) return "";
  if (imageDataUrlCache.has(url)) return imageDataUrlCache.get(url);

  try {
    const response = await withTimeout(fetch(url, { credentials: "omit", cache: "force-cache" }), IMAGE_FETCH_TIMEOUT_MS, "image fetch");
    if (!response.ok) throw new Error(`image fetch ${response.status}`);
    const contentType = response.headers.get("content-type") || "image/png";
    if (!contentType.startsWith("image/")) throw new Error(`not an image: ${contentType}`);
    const buffer = await response.arrayBuffer();
    if (buffer.byteLength > MAX_INLINE_IMAGE_BYTES) throw new Error("image too large");
    const dataUrl = `data:${contentType};base64,${bytesToBase64(new Uint8Array(buffer))}`;
    imageDataUrlCache.set(url, dataUrl);
    return dataUrl;
  } catch {
    imageDataUrlCache.set(url, "");
    return "";
  }
}

async function collectTargets() {
  let tabs = [];
  const scanLog = [];

  try {
    tabs = await chrome.tabs.query({});
  } catch (error) {
    const message = `tabs.query failed: ${String(error)}`;
    await storeScan([], [message]);
    return { targets: [], scanLog: [message], ts: Date.now() };
  }

  const settled = await Promise.allSettled(tabs.map((tab) => scanTab(tab)));
  const targets = [];

  for (const result of settled) {
    if (result.status === "fulfilled") {
      if (result.value?.target) targets.push(result.value.target);
      if (result.value?.log) scanLog.push(result.value.log);
    } else {
      scanLog.push(`scan rejected: ${String(result.reason)}`);
    }
  }

  const finalTargets = disambiguateDuplicateTitles(collapseDuplicateTargets(targets));
  await storeScan(finalTargets, scanLog);
  console.table(scanLog.map((line) => ({ scan: line })));
  return { targets: finalTargets, scanLog, ts: Date.now() };
}

async function storeScan(targets, scanLog) {
  lastTargets = targets;
  lastScanLog = scanLog;
  await chrome.storage.session.set({
    lastTargets,
    lastScanLog,
    lastScanAt: Date.now()
  }).catch(() => undefined);
}

async function scanTab(tab) {
  if (!tab.id || !tab.url || SKIP_SCHEMES.test(tab.url)) {
    return { log: `skip: tab ${tab.id || "?"} ${tab.url || ""}` };
  }

  recordYoutubeWatch(tab.id, tab.url);

  const scan = chrome.scripting.executeScript({
    target: { tabId: tab.id, allFrames: true },
    world: "MAIN",
    func: scanFrameMedia
  });

  let frames;
  try {
    frames = await withTimeout(scan, SCAN_TIMEOUT_MS, `scan tab ${tab.id}`);
  } catch (error) {
    return { log: `${String(error.message || error)}: tab ${tab.id} ${safeHost(tab.url)}` };
  }

  if (!Array.isArray(frames)) {
    return { log: `no frame results: tab ${tab.id} ${safeHost(tab.url)}` };
  }

  let best;
  for (const frame of frames) {
    const media = frame?.result;
    if (!media?.hasMedia) continue;
    const candidate = { media, frameId: frame.frameId ?? 0 };
    if (!best || scoreMedia(candidate.media) > scoreMedia(best.media)) best = candidate;
  }

  if (!best) {
    return { log: `no media: tab ${tab.id} ${safeHost(tab.url)} (${tab.title || ""})` };
  }

  const media = best.media;
  const artworkUrl = await imageAsDataUrl(media.artwork || "");
  const favIconUrl = await imageAsDataUrl(tab.favIconUrl || "");
  return {
    log: `target: tab ${tab.id} frame ${best.frameId} ${safeHost(tab.url)} (${media.title || tab.title || ""})`,
    target: {
      id: `browser:${tab.id}:${best.frameId}`,
      kind: "browser_tab",
      tabId: tab.id,
      frameId: best.frameId,
      mediaId: "main-world",
      app: "Browser",
      site: media.site || safeHost(tab.url),
      title: media.title || tab.title || safeHost(tab.url),
      artist: media.artist || "",
      album: media.album || "",
      url: tab.url || media.url || "",
      favIconUrl,
      artworkUrl,
      playing: Boolean(media.isPlaying),
      audible: Boolean(media.isPlaying),
      muted: false,
      documentTitle: media.documentTitle || "",
      tabTitle: tab.title || "",
      mediaSessionSupported: Boolean(media.mediaSessionSupported),
      mediaSessionPlaybackState: media.playbackState || "",
      mediaSessionTitle: media.mediaSessionTitle || "",
      mediaSessionArtist: media.artist || "",
      mediaSessionActions: Array.isArray(media.mediaSessionActions) ? media.mediaSessionActions : [],
      currentTime: Number.isFinite(media.currentTime) ? media.currentTime : 0,
      duration: Number.isFinite(media.duration) ? media.duration : 0,
      canPlayPause: Boolean(media.canPlayPause),
      canNext: Boolean(media.canNext) || canUseTransport(tab.url, "next"),
      canPrevious: Boolean(media.canPrevious) || canUseTransport(tab.url, "previous")
    }
  };
}

function canUseTransport(rawUrl, action) {
  const host = safeHost(rawUrl).toLowerCase();
  if (host.includes("youtube.com") || host.includes("youtu.be")) return true;
  if (host.includes("spotify.com")) return true;
  if (host.includes("soundcloud.com")) return true;
  if (host.includes("music.apple.com")) return true;
  return action === "next" || action === "previous";
}

function scoreMedia(media) {
  return (media.isPlaying ? 8 : 0)
    + (media.title ? 4 : 0)
    + (media.canPlayPause ? 2 : 0)
    + (media.hasElement ? 1 : 0);
}

function scanFrameMedia() {
  try {
    const session = navigator.mediaSession;
    const captured = window.__hotkeydMedia;
    const metadata = session?.metadata;
    const elements = Array.from(document.querySelectorAll("video, audio"));
    const usable = elements.filter((element) => {
      return element.duration || element.currentTime > 0 || element.readyState > 0 || !element.paused;
    });
    const playingElement = usable.find((element) => !element.paused && !element.ended);
    const playbackState = session?.playbackState || (playingElement ? "playing" : usable.length > 0 ? "paused" : "none");
    const hasMedia = Boolean(playingElement) || usable.length > 0 || playbackState === "playing" || playbackState === "paused";

    if (!hasMedia) return { hasMedia: false };

    const first = playingElement || usable[0] || null;
    const title = metadata?.title || document.title || null;
    const artist = metadata?.artist || null;
    const artwork = Array.isArray(metadata?.artwork) && metadata.artwork.length
      ? metadata.artwork[metadata.artwork.length - 1]?.src || ""
      : "";

    return {
      hasMedia: true,
      title: title && artist ? `${title} - ${artist}` : title,
      mediaSessionTitle: metadata?.title || "",
      artist: artist || "",
      album: metadata?.album || "",
      artwork,
      playbackState,
      isPlaying: Boolean(playingElement) || playbackState === "playing",
      canPlayPause: Boolean(first) || playbackState === "playing" || playbackState === "paused",
      hasElement: usable.length > 0,
      elementCount: usable.length,
      currentTime: first && Number.isFinite(first.currentTime) ? first.currentTime : 0,
      duration: first && Number.isFinite(first.duration) ? first.duration : 0,
      documentTitle: document.title || "",
      url: location.href,
      site: location.hostname,
      mediaSessionSupported: Boolean(session),
      mediaSessionActions: captured?.list?.() || [],
      canNext: Boolean(captured?.has?.("nexttrack")),
      canPrevious: Boolean(captured?.has?.("previoustrack"))
    };
  } catch (error) {
    return { hasMedia: false, error: String(error) };
  }
}

function safeHost(url) {
  try {
    return new URL(url).host;
  } catch {
    return url || "?";
  }
}

function collapseDuplicateTargets(targets) {
  const byKey = new Map();
  for (const target of targets) {
    const key = [
      target.tabId,
      normalizedTitle(target.title),
      target.url || "",
      target.duration ? Math.round(target.duration) : 0
    ].join("|");
    const existing = byKey.get(key);
    if (!existing || (target.playing && !existing.playing)) byKey.set(key, target);
  }
  return Array.from(byKey.values());
}

function disambiguateDuplicateTitles(targets) {
  const counts = new Map();
  for (const target of targets) {
    const key = normalizedTitle(target.title);
    counts.set(key, (counts.get(key) || 0) + 1);
  }

  return targets.map((target) => {
    const key = normalizedTitle(target.title);
    if ((counts.get(key) || 0) <= 1) return target;
    const suffix = shortUrlLabel(target.url) || target.tabTitle || `tab ${target.tabId}`;
    return {
      ...target,
      title: `${target.title || target.site || "Media"} (${suffix})`
    };
  });
}

function normalizedTitle(title) {
  return String(title || "").replace(/\s+/g, " ").trim().toLowerCase();
}

function shortUrlLabel(rawUrl) {
  try {
    const url = new URL(rawUrl);
    if (url.hostname.includes("youtube.com") && url.searchParams.get("v")) {
      return `${url.hostname}/watch?v=${url.searchParams.get("v")}`;
    }
    const path = `${url.pathname}${url.search}`.replace(/\/$/, "");
    return `${url.hostname}${path || "/"}`.slice(0, 80);
  } catch {
    return "";
  }
}

function youtubeWatchInfo(rawUrl) {
  try {
    const url = new URL(rawUrl);
    const host = url.hostname.toLowerCase();
    const ok = (host === "youtube.com" || host === "www.youtube.com")
      && url.pathname === "/watch"
      && url.searchParams.has("v");
    if (!ok) return undefined;
    return {
      videoId: url.searchParams.get("v"),
      url: `${url.origin}${url.pathname}?v=${url.searchParams.get("v")}`
    };
  } catch {
    return undefined;
  }
}

function recordYoutubeWatch(tabId, rawUrl) {
  const info = youtubeWatchInfo(rawUrl);
  if (!info || !Number.isInteger(tabId)) return;

  const pending = youtubeProgrammaticPrevious.get(tabId);
  if (pending) {
    if (pending.expiresAt < Date.now()) {
      youtubeProgrammaticPrevious.delete(tabId);
    } else if (info.videoId === pending.fromVideoId || info.videoId === pending.toVideoId) {
      return;
    }
  }

  const stack = youtubeWatchStacks.get(tabId) || [];
  const last = stack[stack.length - 1];
  if (last?.videoId === info.videoId) return;

  stack.push(info);
  if (stack.length > 50) stack.splice(0, stack.length - 50);
  youtubeWatchStacks.set(tabId, stack);
}

async function youtubeSmartPrevious(tabId) {
  let tab;
  try {
    tab = await chrome.tabs.get(tabId);
  } catch (error) {
    return { ok: false, code: "TAB_NOT_FOUND", message: String(error.message || error) };
  }

  const current = youtubeWatchInfo(tab.url || "");
  if (!current) {
    return { ok: false, code: "NOT_YOUTUBE_WATCH", message: "selected tab is not a normal YouTube watch page" };
  }

  recordYoutubeWatch(tabId, tab.url || "");

  const stack = youtubeWatchStacks.get(tabId) || [];
  while (stack.length > 0 && stack[stack.length - 1].videoId === current.videoId) {
    stack.pop();
  }

  const previous = stack[stack.length - 1];
  if (!previous) {
    youtubeWatchStacks.set(tabId, [current]);
    return { ok: false, code: "NO_YOUTUBE_VIDEO_HISTORY", message: "no previous loaded YouTube video in this tab" };
  }

  try {
    youtubeProgrammaticPrevious.set(tabId, {
      fromVideoId: current.videoId,
      toVideoId: previous.videoId,
      expiresAt: Date.now() + 3500
    });
    await chrome.tabs.update(tabId, { url: previous.url });
    youtubeWatchStacks.set(tabId, stack);
    return { ok: true, message: "YouTube previous used video-only history", via: "youtube:video-history" };
  } catch (error) {
    return { ok: false, code: "YOUTUBE_VIDEO_HISTORY_FAILED", message: String(error.message || error) };
  }
}

async function youtubePreviousLastResort(tabId, primaryResult) {
  let tab;
  try {
    tab = await chrome.tabs.get(tabId);
  } catch (error) {
    return primaryResult || { ok: false, code: "TAB_NOT_FOUND", message: String(error.message || error) };
  }

  if (!youtubeWatchInfo(tab.url || "")) {
    return primaryResult || {
      ok: false,
      code: "NOT_YOUTUBE_WATCH",
      message: "previous fallback is only available for normal YouTube watch pages"
    };
  }

  const fallback = await youtubeSmartPrevious(tabId);
  if (fallback.ok && primaryResult) {
    fallback.primaryFailure = {
      code: primaryResult.code,
      reason: primaryResult.reason,
      message: primaryResult.message,
      via: primaryResult.via
    };
  }
  return fallback.ok ? fallback : (primaryResult || fallback);
}

async function controlTarget(targetId, command) {
  const parsed = parseTargetId(targetId);
  if (!parsed) return { ok: false, code: "BAD_TARGET_ID", message: "Invalid browser target id" };

  const action = command === "play_pause" ? "playpause" : command;

  try {
    const selectedResult = await withTimeout(chrome.scripting.executeScript({
      target: { tabId: parsed.tabId, frameIds: [parsed.frameId] },
      world: "MAIN",
      func: controlFrameMedia,
      args: [action]
    }), CONTROL_TIMEOUT_MS, `control tab ${parsed.tabId}`);

    const first = Array.isArray(selectedResult) ? selectedResult[0]?.result : undefined;
    if (first?.ok || (action !== "next" && action !== "previous")) {
      return first || { ok: false, code: "NO_RESULT", message: "No control result" };
    }

    const allFrameResult = await withTimeout(chrome.scripting.executeScript({
      target: { tabId: parsed.tabId, allFrames: true },
      world: "MAIN",
      func: controlFrameMedia,
      args: [action]
    }), CONTROL_TIMEOUT_MS, `control all frames ${parsed.tabId}`);

    const hits = Array.isArray(allFrameResult) ? allFrameResult.map((item) => item.result).filter(Boolean) : [];
    const result = hits.find((item) => item.ok) || first || hits[0] || { ok: false, code: "NO_RESULT", message: "No control result" };
    if (!result.ok && action === "previous") {
      return await youtubePreviousLastResort(parsed.tabId, result);
    }
    return result;
  } catch (error) {
    return { ok: false, code: "CONTROL_ERROR", message: String(error.message || error) };
  }
}

async function activateTarget(targetId, restoreAfterMs = 650) {
  const parsed = parseTargetId(targetId);
  if (!parsed) return { ok: false, code: "BAD_TARGET_ID", message: "Invalid browser target id" };

  let targetTab;
  try {
    targetTab = await chrome.tabs.get(parsed.tabId);
  } catch (error) {
    return { ok: false, code: "TAB_NOT_FOUND", message: String(error.message || error) };
  }

  const previousTabs = await chrome.tabs.query({ active: true, lastFocusedWindow: true }).catch(() => []);
  const previous = previousTabs[0] && previousTabs[0].id !== parsed.tabId
    ? { tabId: previousTabs[0].id, windowId: previousTabs[0].windowId }
    : undefined;

  try {
    await chrome.windows.update(targetTab.windowId, { focused: true });
    await chrome.tabs.update(parsed.tabId, { active: true });
  } catch (error) {
    return { ok: false, code: "ACTIVATE_FAILED", message: String(error.message || error) };
  }

  const delay = Number.isFinite(restoreAfterMs) ? Math.max(0, restoreAfterMs) : 650;
  if (previous && delay > 0) {
    setTimeout(() => {
      chrome.tabs.update(previous.tabId, { active: true })
        .then(() => chrome.windows.update(previous.windowId, { focused: true }))
        .catch(() => undefined);
    }, delay);
  }

  return {
    ok: true,
    message: "target tab activated",
    previousTabId: previous?.tabId,
    previousWindowId: previous?.windowId
  };
}

function controlFrameMedia(action) {
  try {
    function roots() {
      const seen = new Set();
      const queue = [document];
      const out = [];

      for (let i = 0; i < queue.length; ++i) {
        const root = queue[i];
        if (!root || seen.has(root)) continue;
        seen.add(root);
        out.push(root);

        const nodes = root.querySelectorAll ? root.querySelectorAll("*") : [];
        for (const node of nodes) {
          if (node.shadowRoot && !seen.has(node.shadowRoot)) queue.push(node.shadowRoot);
        }
      }

      return out;
    }

    function deepQueryAll(selector) {
      const found = [];
      for (const root of roots()) {
        try {
          found.push(...root.querySelectorAll(selector));
        } catch {
        }
      }
      return found;
    }

    function clickableFor(element) {
      return element?.closest?.("button, [role='button'], a, tp-yt-paper-icon-button")
        || element;
    }

    function elementText(element) {
      if (!element) return "";
      return [
        element.getAttribute?.("aria-label"),
        element.getAttribute?.("title"),
        element.getAttribute?.("data-testid"),
        element.getAttribute?.("data-test-id"),
        element.getAttribute?.("id"),
        element.getAttribute?.("class"),
        element.textContent
      ].filter(Boolean).join(" ").toLowerCase();
    }

    function disabledReason(element) {
      const clickable = clickableFor(element);
      if (!clickable) return "absent";
      const disabled = clickable.disabled
        || clickable.getAttribute?.("disabled") !== null
        || clickable.getAttribute?.("aria-disabled") === "true"
        || clickable.classList?.contains("disabled");
      if (disabled) return "disabled";
      return "";
    }

    function clickElement(element) {
      const clickable = clickableFor(element);
      const reason = disabledReason(clickable);
      if (reason) return { ok: false, reason, element: elementText(clickable) };

      for (const type of ["pointerdown", "mousedown", "mouseup", "click"]) {
        clickable.dispatchEvent(new MouseEvent(type, {
          bubbles: true,
          cancelable: true,
          composed: true,
          view: window
        }));
      }
      clickable.click();
      return { ok: true, via: elementText(clickable) || clickable.tagName.toLowerCase() };
    }

    function siteAdapters() {
      return [
        {
          name: "youtube-music",
          matches: (host) => host.includes("music.youtube.com"),
          next: ["tp-yt-paper-icon-button.next-button", ".next-button", "[title*='Next' i]", "[aria-label*='Next' i]"],
          previous: ["tp-yt-paper-icon-button.previous-button", ".previous-button", "[title*='Previous' i]", "[aria-label*='Previous' i]"]
        },
        {
          name: "youtube",
          matches: (host) => host.includes("youtube.com") || host.includes("youtu.be"),
          next: [".ytp-next-button", "a.ytp-next-button", "button[aria-label*='Next' i]", "[aria-label*='Next' i]"],
          previous: [".ytp-prev-button", "a.ytp-prev-button", "button[aria-label*='Previous' i]", "[aria-label*='Previous' i]"]
        },
        {
          name: "spotify",
          matches: (host) => host.includes("spotify.com"),
          next: ["[data-testid='control-button-skip-forward']", "[data-testid*='skip-forward' i]", "[aria-label*='Next' i]"],
          previous: ["[data-testid='control-button-skip-back']", "[data-testid*='skip-back' i]", "[aria-label*='Previous' i]"]
        },
        {
          name: "soundcloud",
          matches: (host) => host.includes("soundcloud.com"),
          next: [".skipControl__next", "[title*='Next' i]", "[aria-label*='Next' i]"],
          previous: [".skipControl__previous", "[title*='Previous' i]", "[aria-label*='Previous' i]"]
        }
      ];
    }

    function trySelectors(selectors) {
      let disabled;

      for (const selector of selectors) {
        const elements = deepQueryAll(selector);
        for (const element of elements) {
          const result = clickElement(element);
          if (result.ok) return { ok: true, via: selector, detail: result.via };
          if (result.reason === "disabled") disabled = { selector, detail: result.element };
        }
      }

      if (disabled) return { ok: false, reason: "disabled", ...disabled };
      return { ok: false, reason: "absent" };
    }

    function controlYoutube(kind) {
      const selectors = kind === "next"
        ? [".ytp-next-button", "a.ytp-next-button"]
        : [".ytp-prev-button", "a.ytp-prev-button"];

      let sawButton = false;
      let disabled;
      for (const selector of selectors) {
        for (const element of deepQueryAll(selector)) {
          sawButton = true;
          const reason = disabledReason(element);
          if (reason === "disabled") disabled = selector;
          if (reason && kind !== "previous") continue;

          const href = element.href || element.getAttribute?.("href");
          if (href && href !== "#" && href !== location.href) {
            location.href = new URL(href, location.href).href;
            return { ok: true, message: `${kind} navigated`, via: `youtube:${selector}:href` };
          }

          const clicked = clickElement(element);
          if (clicked.ok) return { ok: true, message: `${kind} clicked`, via: `youtube:${selector}:click` };
        }
      }

      if (disabled) return { ok: false, code: "TRANSPORT_DISABLED", reason: "disabled", message: `${kind} is disabled on youtube`, via: `youtube:${disabled}` };
      if (sawButton) return { ok: false, code: "TRANSPORT_ABSENT", reason: "absent", message: `${kind} youtube button had no usable href/click target` };
      return undefined;
    }

    function genericSelectors(kind) {
      return kind === "next"
        ? [
          "[aria-label*='next' i]",
          "[aria-label*='skip forward' i]",
          "[title*='next' i]",
          "[title*='skip forward' i]",
          "[data-testid*='next' i]",
          "[data-testid*='skip-forward' i]"
        ]
        : [
          "[aria-label*='previous' i]",
          "[aria-label*='prev' i]",
          "[aria-label*='skip back' i]",
          "[title*='previous' i]",
          "[title*='prev' i]",
          "[title*='skip back' i]",
          "[data-testid*='previous' i]",
          "[data-testid*='prev' i]",
          "[data-testid*='skip-back' i]"
        ];
    }

    function controlTrack(kind) {
      const host = location.hostname.toLowerCase();
      const adapter = siteAdapters().find((candidate) => candidate.matches(host));

      if (adapter?.name === "youtube") {
        const youtubeResult = controlYoutube(kind);
        if (youtubeResult) return youtubeResult;
      }

      if (adapter) {
        const result = trySelectors(adapter[kind]);
        if (result.ok) return { ok: true, message: `${kind} clicked`, via: `${adapter.name}:${result.via}` };
        if (result.reason === "disabled") {
          return { ok: false, code: "TRANSPORT_DISABLED", reason: "disabled", message: `${kind} is disabled on ${adapter.name}`, via: `${adapter.name}:${result.selector || ""}` };
        }
      }

      const fallback = trySelectors(genericSelectors(kind));
      if (fallback.ok) return { ok: true, message: `${kind} clicked`, via: `generic:${fallback.via}` };

      const reason = "absent";
      return {
        ok: false,
        code: "TRANSPORT_ABSENT",
        reason,
        message: adapter
          ? `${kind} button not found for ${adapter.name}; adapter needs a selector update`
          : `${kind} button not found for ${host || "this site"}`
      };
    }

    const media = Array.from(document.querySelectorAll("video, audio"));
    const target = media.find((element) => !element.paused && !element.ended) || media[0] || null;

    if (action === "next" || action === "previous") {
      const handlerAction = action === "next" ? "nexttrack" : "previoustrack";
      const captured = window.__hotkeydMedia;
      if (captured?.has?.(handlerAction)) {
        const result = captured.invoke(handlerAction);
        if (result?.ok) return { ok: true, message: `${action} handler invoked`, via: `handler:${handlerAction}` };
      }
      return controlTrack(action);
    }

    if (action === "playpause") {
      if (!target) return { ok: false, message: "No media element" };
      if (target.paused) target.play();
      else target.pause();
      return { ok: true, message: "play/pause toggled" };
    }

    if (action === "play") {
      if (!target) return { ok: false, message: "No media element" };
      target.play();
      return { ok: true, message: "play sent" };
    }

    if (action === "pause") {
      if (!target) return { ok: false, message: "No media element" };
      target.pause();
      return { ok: true, message: "pause sent" };
    }

    return { ok: false, message: `Unsupported action: ${action}` };
  } catch (error) {
    return { ok: false, message: String(error) };
  }
}

function parseTargetId(targetId) {
  if (typeof targetId !== "string") return undefined;
  const parts = targetId.split(":");
  if (parts.length < 3 || parts[0] !== "browser") return undefined;
  const tabId = Number(parts[1]);
  const frameId = Number(parts[2]);
  if (!Number.isInteger(tabId) || !Number.isInteger(frameId)) return undefined;
  return { tabId, frameId };
}

async function publishTargets() {
  const result = await collectTargets();
  send({ type: "media_targets", payload: result });
}

function wakeAndPublish() {
  connect();
  if (socket?.readyState === WebSocket.OPEN) void publishTargets();
}

function handlePossibleYoutubeNavigation(tabId, url) {
  recordYoutubeWatch(tabId, url || "");
  wakeAndPublish();
}

async function popupStatus() {
  const state = await chrome.storage.session.get([
    "connected",
    "connectedAt",
    "lastConnectionChange",
    "lastScanAt",
    "lastScanLog",
    "lastTargets",
    "reconnectDelay",
    "socketState"
  ]);
  return {
    ...state,
    runtimeSocketState: socket ? socket.readyState : -1,
    runtimeHasSocket: Boolean(socket)
  };
}

chrome.runtime.onStartup.addListener(connect);
chrome.runtime.onInstalled.addListener(connect);
chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type === "media_state_changed") return false;

  if (message?.type === "force_connect") {
    wakeAndPublish();
    sendResponse({ ok: true });
    return false;
  }

  if (message?.type === "force_scan") {
    collectTargets()
      .then((result) => sendResponse({ ok: true, ...result }))
      .catch((error) => sendResponse({ ok: false, error: String(error), targets: [], scanLog: [] }));
    return true;
  }

  if (message?.type === "get_status") {
    popupStatus().then((status) => sendResponse({ ok: true, status }));
    return true;
  }

  return false;
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
  if (changeInfo.url || tab?.url) handlePossibleYoutubeNavigation(tabId, changeInfo.url || tab.url);
  else wakeAndPublish();
});
chrome.tabs.onRemoved.addListener((tabId) => {
  youtubeWatchStacks.delete(tabId);
  youtubeProgrammaticPrevious.delete(tabId);
  wakeAndPublish();
});
chrome.tabs.onActivated.addListener(wakeAndPublish);

chrome.webNavigation.onHistoryStateUpdated.addListener((details) => {
  if (details.frameId === 0) handlePossibleYoutubeNavigation(details.tabId, details.url);
});

chrome.webNavigation.onCommitted.addListener((details) => {
  if (details.frameId === 0) handlePossibleYoutubeNavigation(details.tabId, details.url);
});

chrome.alarms.create(ALARM_NAME, { periodInMinutes: 1 });
chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === ALARM_NAME) wakeAndPublish();
});

connect();
