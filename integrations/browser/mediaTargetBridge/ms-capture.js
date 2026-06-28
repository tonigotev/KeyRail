(() => {
  if (window.__hotkeydMedia) return;

  const store = Object.create(null);

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
        return { ok: true, action };
      } catch (error) {
        return { ok: false, reason: "threw", error: String(error), action };
      }
    }
  };

  try {
    Object.defineProperty(window, "__hotkeydMedia", { value: api });
  } catch {
    window.__hotkeydMedia = api;
  }

  const proto =
    (window.MediaSession && window.MediaSession.prototype)
    || (navigator.mediaSession && Object.getPrototypeOf(navigator.mediaSession));

  if (!proto || typeof proto.setActionHandler !== "function") return;
  if (proto.setActionHandler.__hotkeydWrapped) return;

  const original = proto.setActionHandler;
  const wrapped = function (action, handler) {
    try {
      if (handler == null) delete store[action];
      else store[action] = handler;
    } catch {
    }
    return original.call(this, action, handler);
  };

  try {
    Object.defineProperty(wrapped, "__hotkeydWrapped", { value: true });
  } catch {
  }
  proto.setActionHandler = wrapped;
})();
