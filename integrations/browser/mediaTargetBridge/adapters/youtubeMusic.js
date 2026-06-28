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
    name: "youtube-music",
    matches(location) {
      return location.hostname === "music.youtube.com";
    },
    canHandle(command) {
      return command === "next" || command === "previous";
    },
    async run(command) {
      if (command === "next") {
        return clickFirst(["tp-yt-paper-icon-button.next-button", ".next-button", "[title='Next']"]);
      }
      if (command === "previous") {
        return clickFirst(["tp-yt-paper-icon-button.previous-button", ".previous-button", "[title='Previous']"]);
      }
      return false;
    }
  });
})();
