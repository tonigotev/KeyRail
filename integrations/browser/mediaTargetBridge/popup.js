const statusEl = document.getElementById("status");
const connectButton = document.getElementById("connect");
const scanButton = document.getElementById("scan");

function socketStateName(value) {
  if (value === 0) return "CONNECTING";
  if (value === 1) return "OPEN";
  if (value === 2) return "CLOSING";
  if (value === 3) return "CLOSED";
  return "NONE";
}

async function send(message) {
  return await chrome.runtime.sendMessage(message);
}

async function refresh() {
  const response = await send({ type: "get_status" });
  const status = response?.status || {};
  const rows = [
    `connected: ${Boolean(status.connected)}`,
    `socket: ${socketStateName(status.runtimeSocketState)}`,
    `has socket: ${Boolean(status.runtimeHasSocket)}`,
    `connected at: ${status.connectedAt ? new Date(status.connectedAt).toLocaleTimeString() : "-"}`,
    `last change: ${status.lastConnectionChange ? new Date(status.lastConnectionChange).toLocaleTimeString() : "-"}`,
    `last scan: ${status.lastScanAt ? new Date(status.lastScanAt).toLocaleTimeString() : "-"}`,
    `reconnect delay: ${status.reconnectDelay || "-"}`,
    "",
    "last scan log:",
    JSON.stringify(status.lastScanLog || [], null, 2)
  ];
  statusEl.textContent = rows.join("\n");
}

connectButton.addEventListener("click", async () => {
  statusEl.textContent = "Connecting...";
  await send({ type: "force_connect" });
  setTimeout(refresh, 500);
});

scanButton.addEventListener("click", async () => {
  statusEl.textContent = "Scanning tabs...";
  const response = await send({ type: "force_scan" });
  const targets = response?.targets || [];
  await refresh();
  statusEl.textContent += `\n\nmanual scan targets: ${targets.length}\n${JSON.stringify(targets, null, 2)}`;
});

void refresh();
