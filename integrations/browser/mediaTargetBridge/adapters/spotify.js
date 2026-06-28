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
    name: "spotify",
    matches(location) {
      return location.hostname === "open.spotify.com";
    },
    canHandle(command) {
      return command === "next" || command === "previous";
    },
    async run(command) {
      if (command === "next") {
        return clickFirst(["[data-testid='control-button-skip-forward']", "button[aria-label='Next']"]);
      }
      if (command === "previous") {
        return clickFirst(["[data-testid='control-button-skip-back']", "button[aria-label='Previous']"]);
      }
      return false;
    }
  });
})();
