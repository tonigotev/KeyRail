(function () {
  window.htcMediaAdapters = window.htcMediaAdapters || [];

  function clickFirst(selectors) {
    for (const selector of selectors) {
      const element = document.querySelector(selector);
      if (element) {
        element.click();
        return true;
      }
    }
    return false;
  }

  window.htcMediaAdapters.push({
    name: "youtube",
    matches(location) {
      return /(^|\.)youtube\.com$/.test(location.hostname) && !location.hostname.startsWith("music.");
    },
    canHandle(command) {
      return command === "next" || command === "previous";
    },
    async run(command) {
      if (command === "next") {
        return clickFirst([".ytp-next-button", "a.ytp-next-button"]);
      }
      if (command === "previous") {
        return clickFirst([".ytp-prev-button", "a.ytp-prev-button"]);
      }
      return false;
    }
  });
})();
