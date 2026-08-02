(() => {
  const TTL_MS = 30000;

  if (window.__keyraildMedia?.renew) {
    window.__keyraildMedia.renew(TTL_MS);
    return;
  }

  const store = Object.create(null);
  let uninstallTimer;

  const proto =
    (window.MediaSession && window.MediaSession.prototype)
    || (navigator.mediaSession && Object.getPrototypeOf(navigator.mediaSession));

  if (!proto || typeof proto.setActionHandler !== "function") return;
  if (proto.setActionHandler.__keyraildWrapped) return;

  const original = proto.setActionHandler;

  function renew(ms = TTL_MS) {
    if (uninstallTimer) clearTimeout(uninstallTimer);
    uninstallTimer = setTimeout(uninstall, ms);
  }

  function uninstall() {
    if (uninstallTimer) clearTimeout(uninstallTimer);
    uninstallTimer = undefined;

    try {
      if (proto.setActionHandler === wrapped) proto.setActionHandler = original;
    } catch {
    }

    try {
      delete window.__keyraildMedia;
    } catch {
      window.__keyraildMedia = undefined;
    }
  }

  const api = {
    has(action) {
      return typeof store[action] === "function";
    },
    list() {
      return Object.keys(store);
    },
    invoke(action) {
      const fn = store[action];
      if (typeof fn !== "function") return { ok: false, reason: "no-handler", action };
      try {
        fn({ action });
        renew();
        return { ok: true, action };
      } catch (error) {
        return { ok: false, reason: "threw", error: String(error), action };
      }
    },
    renew,
    uninstall
  };

  const wrapped = function (action, handler) {
    try {
      if (handler == null) delete store[action];
      else store[action] = handler;
    } catch {
    }
    return original.call(this, action, handler);
  };

  try {
    Object.defineProperty(wrapped, "__keyraildWrapped", { value: true });
  } catch {
  }

  try {
    Object.defineProperty(window, "__keyraildMedia", {
      configurable: true,
      value: api
    });
  } catch {
    window.__keyraildMedia = api;
  }

  proto.setActionHandler = wrapped;
  renew();
})();
