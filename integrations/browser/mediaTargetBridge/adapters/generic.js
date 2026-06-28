(function () {
  window.htcMediaAdapters = window.htcMediaAdapters || [];

  window.htcMediaAdapters.push({
    name: "generic",
    matches() {
      return true;
    },
    canHandle() {
      return false;
    },
    async run() {
      return false;
    }
  });
})();
