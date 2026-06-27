<script lang="ts">
  import { onMount } from "svelte";
  import { invoke } from "@tauri-apps/api/core";
  import { getCurrentWindow } from "@tauri-apps/api/window";

  type ActionSpec =
    | { type: "builtin"; name: "cycle_audio_output" | "cycle_microphone_input" | "inspect_focused_app_audio" | "cycle_focused_app_audio_output" | "cycle_focused_app_microphone_input" | "cycle_discord_output_device" | "cycle_discord_microphone_input" | "close_focused_app" | "kill_focused_app"; strong_close?: boolean }
    | { type: "open_app"; path: string; args: string[]; show_window: boolean }
    | { type: "run_script"; path: string; args: string[]; working_dir: string; interpreter: string; show_window: boolean }
    | { type: "run_command"; command: string; show_window: boolean };

  type BindingSpec = {
    id: string;
    enabled: boolean;
    hotkey: string;
    action: ActionSpec;
  };

  type AppConfig = {
    version: number;
    bindings: BindingSpec[];
  };

  type ConfigEnvelope = {
    path: string;
    config: AppConfig;
  };

  type Toast = {
    id: number;
    type: "success" | "error" | "info";
    text: string;
  };

  type DaemonView = {
    state: "running" | "stopped" | "warning" | "checking";
    label: string;
    detail: string;
    lines: string[];
  };

  const defaultConfig: AppConfig = {
    version: 1,
    bindings: []
  };

  let config: AppConfig = defaultConfig;
  let configPath = "";
  let daemonStatus = "Not checked yet.";
  let advancedVisible = false;
  let busyAction = "";
  let toasts: Toast[] = [];
  let toastId = 1;
  let statusTimer: ReturnType<typeof setInterval> | undefined;
  let eventLog: string[] = [];
  let advancedPulse = false;
  let advancedPulseTimer: ReturnType<typeof setTimeout> | undefined;

  $: duplicateHotkeys = findDuplicateHotkeys(config.bindings);
  $: canSave = duplicateHotkeys.size === 0 && config.bindings.every((b) => b.id.trim() && b.hotkey.trim());
  $: daemonView = parseDaemonStatus(daemonStatus);
  $: armedBindings = config.bindings.filter((binding) => binding.enabled && binding.hotkey.trim()).length;

  onMount(() => {
    void (async () => {
      await loadConfig().catch(() => undefined);
      await ensureDaemonOnLaunch();
      await refreshStatus();
    })();
    statusTimer = setInterval(refreshStatusQuietly, 5000);
    return () => {
      if (statusTimer) clearInterval(statusTimer);
    };
  });

  function touch() {
    config = { ...config, bindings: [...config.bindings] };
  }

  function logEvent(text: string) {
    const time = new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    eventLog = [`${time} ${text}`, ...eventLog].slice(0, 8);
  }

  function findDuplicateHotkeys(bindings: BindingSpec[]) {
    const seen = new Map<string, number>();
    const dupes = new Set<string>();
    for (const binding of bindings) {
      if (!binding.enabled || !binding.hotkey.trim()) continue;
      const key = binding.hotkey.trim().toLowerCase();
      seen.set(key, (seen.get(key) ?? 0) + 1);
      if ((seen.get(key) ?? 0) > 1) dupes.add(key);
    }
    return dupes;
  }

  async function loadConfig() {
    await runWithFeedback("load", "Config loaded.", async () => {
      const loaded = await invoke<ConfigEnvelope>("load_config");
      config = loaded.config;
      configPath = loaded.path;
      logEvent("loaded config");
    }, false);
  }

  async function saveAndApply() {
    if (!canSave) {
      pushToast("Fix duplicate or empty hotkeys before saving.", "error");
      return;
    }
    await runWithFeedback("save", "Saved and reloaded.", async () => {
      await invoke("save_config", { config });
      await invoke("ensure_daemon");
      await invoke("send_daemon_command", { command: "reload" });
      await refreshStatus();
      logEvent("saved and reloaded");
    });
  }

  async function refreshStatus() {
    await runWithFeedback("status", "Status updated.", refreshStatusQuietly, false);
  }

  async function refreshStatusQuietly() {
    try {
      daemonStatus = await invoke<string>("send_daemon_command", { command: "status" });
    } catch (error) {
      daemonStatus = `Daemon is not responding: ${error}`;
    }
  }

  async function ensureDaemonOnLaunch() {
    await runWithFeedback("launch", "Daemon is running.", async () => {
      await invoke("ensure_daemon");
    }, false).catch((error) => {
      daemonStatus = `Daemon is not running: ${error}`;
    });
  }

  async function startDaemon() {
    await runWithFeedback("start", "Daemon started.", async () => {
      await invoke("ensure_daemon");
      await refreshStatus();
      logEvent("daemon started");
    });
  }

  async function restartDaemon() {
    await runWithFeedback("restart", "Daemon restarted.", async () => {
      try {
        await invoke("send_daemon_command", { command: "quit" });
      } catch {
        // The daemon may already be down; starting it is the important part.
      }
      await invoke("ensure_daemon");
      await invoke("send_daemon_command", { command: "reload" });
      await refreshStatus();
      logEvent("daemon restarted");
    });
  }

  async function stopDaemon() {
    await runWithFeedback("stop", "Daemon stopped.", async () => {
      await invoke("send_daemon_command", { command: "quit" });
      daemonStatus = "Daemon is stopped.";
      logEvent("daemon stopped");
    });
  }

  function addBinding() {
    config.bindings = [
      ...config.bindings,
      {
        id: `binding-${config.bindings.length + 1}`,
        enabled: true,
        hotkey: "",
        action: { type: "open_app", path: "", args: [], show_window: true }
      }
    ];
    touch();
    logEvent("added binding");
  }

  function removeBinding(index: number) {
    const removed = config.bindings[index]?.id || "binding";
    config.bindings.splice(index, 1);
    touch();
    logEvent(`removed ${removed}`);
  }

  function setActionType(binding: BindingSpec, type: string) {
    if (type === "cycle_audio_output") binding.action = { type: "builtin", name: "cycle_audio_output" };
    if (type === "cycle_microphone_input") binding.action = { type: "builtin", name: "cycle_microphone_input" };
    if (type === "inspect_focused_app_audio") binding.action = { type: "builtin", name: "inspect_focused_app_audio" };
    if (type === "cycle_focused_app_audio_output") binding.action = { type: "builtin", name: "cycle_focused_app_audio_output" };
    if (type === "cycle_focused_app_microphone_input") binding.action = { type: "builtin", name: "cycle_focused_app_microphone_input" };
    if (type === "cycle_discord_output_device") binding.action = { type: "builtin", name: "cycle_discord_output_device" };
    if (type === "cycle_discord_microphone_input") binding.action = { type: "builtin", name: "cycle_discord_microphone_input" };
    if (type === "close_focused_app") binding.action = { type: "builtin", name: "close_focused_app", strong_close: false };
    if (type === "kill_focused_app") binding.action = { type: "builtin", name: "kill_focused_app" };
    if (type === "open_app") binding.action = { type: "open_app", path: "", args: [], show_window: true };
    if (type === "run_script") binding.action = { type: "run_script", path: "", args: [], working_dir: "", interpreter: "auto", show_window: false };
    if (type === "run_command") binding.action = { type: "run_command", command: "", show_window: false };
    touch();
  }

  function actionSelectValue(action: ActionSpec) {
    if (action.type === "builtin") return action.name;
    return action.type;
  }

  const knownInterpreters = new Set([
    "auto",
    "direct",
    "py.exe",
    "powershell.exe",
    "cmd.exe",
    "node.exe",
    "wscript.exe",
    "AutoHotkey.exe"
  ]);

  function interpreterSelectValue(action: Extract<ActionSpec, { type: "run_script" }>) {
    return knownInterpreters.has(action.interpreter) ? action.interpreter : "custom";
  }

  function setInterpreterMode(action: Extract<ActionSpec, { type: "run_script" }>, mode: string) {
    if (mode === "custom") {
      if (knownInterpreters.has(action.interpreter)) action.interpreter = "";
    } else {
      action.interpreter = mode;
    }
    touch();
  }

  function toggleAdvanced() {
    advancedVisible = !advancedVisible;
    pushToast(
      advancedVisible
        ? "Developer actions are now visible in the action dropdowns."
        : "Developer actions are hidden again.",
      "info"
    );
    advancedPulse = true;
    if (advancedPulseTimer) clearTimeout(advancedPulseTimer);
    advancedPulseTimer = setTimeout(() => {
      advancedPulse = false;
    }, 2000);
  }

  async function chooseApp(binding: BindingSpec) {
    if (binding.action.type !== "open_app") return;
    await runWithFeedback("browse", "", async () => {
      const path = await invoke<string | null>("select_app_path");
      if (path) {
        binding.action.path = path;
        touch();
      }
    }, false);
  }

  async function chooseScript(binding: BindingSpec) {
    if (binding.action.type !== "run_script") return;
    await runWithFeedback("browse", "", async () => {
      const path = await invoke<string | null>("select_script_path");
      if (path) {
        binding.action.path = path;
        touch();
      }
    }, false);
  }

  function captureHotkey(event: KeyboardEvent, binding: BindingSpec) {
    event.preventDefault();
    const parts: string[] = [];
    if (event.ctrlKey) parts.push("ctrl");
    if (event.altKey) parts.push("alt");
    if (event.shiftKey) parts.push("shift");
    if (event.metaKey) parts.push("win");

    const key = normalizeKey(event.key);
    if (!key) return;

    binding.hotkey = [...parts, key].join("+");
    touch();
  }

  function hotkeyParts(hotkey: string) {
    return hotkey.split("+").map((part) => part.trim()).filter(Boolean);
  }

  function actionLabel(action: ActionSpec) {
    if (action.type === "open_app") return "Run application";
    if (action.type === "run_script") return "Run script";
    if (action.type === "run_command") return "Run command";
    if (action.name === "cycle_audio_output") return "Cycle audio output";
    if (action.name === "cycle_microphone_input") return "Cycle microphone input";
    if (action.name === "cycle_focused_app_audio_output") return "Focused app output";
    if (action.name === "cycle_focused_app_microphone_input") return "Focused app microphone";
    if (action.name === "inspect_focused_app_audio") return "Inspect focused app audio";
    if (action.name === "cycle_discord_output_device") return "Cycle Discord output";
    if (action.name === "close_focused_app") return "Close focused app";
    if (action.name === "kill_focused_app") return "Kill focused app";
    return "Cycle Discord microphone input";
  }

  function actionMeta(binding: BindingSpec) {
    if (!binding.enabled) return "disabled";
    if (binding.action.type === "open_app") return "application action";
    if (binding.action.type === "run_script") return "script action";
    if (binding.action.type === "run_command") return "command action";
    return "built-in action";
  }

  function duplicateOwner(binding: BindingSpec) {
    const key = binding.hotkey.trim().toLowerCase();
    if (!binding.enabled || !key || !duplicateHotkeys.has(key)) return "";
    const owner = config.bindings.find((candidate) => candidate !== binding && candidate.enabled && candidate.hotkey.trim().toLowerCase() === key);
    return owner?.id || "another binding";
  }

  function normalizeKey(key: string) {
    const lower = key.toLowerCase();
    if (["control", "shift", "alt", "meta"].includes(lower)) return "";
    if (lower === " ") return "space";
    if (lower === "arrowup") return "up";
    if (lower === "arrowdown") return "down";
    if (lower === "arrowleft") return "left";
    if (lower === "arrowright") return "right";
    if (lower === "escape") return "esc";
    if (lower === "pageup" || lower === "pagedown") return lower;
    if (lower.length === 1) return lower;
    return lower;
  }

  async function runWithFeedback(
    action: string,
    successText: string,
    task: () => Promise<void>,
    notifySuccess = true
  ) {
    busyAction = action;
    try {
      await task();
      if (notifySuccess && successText) pushToast(successText, "success");
    } catch (error) {
      pushToast(String(error), "error");
      throw error;
    } finally {
      busyAction = "";
    }
  }

  function pushToast(text: string, type: Toast["type"] = "info") {
    const id = toastId++;
    toasts = [...toasts, { id, type, text }];
    setTimeout(() => {
      toasts = toasts.filter((toast) => toast.id !== id);
    }, type === "error" ? 4200 : 2600);
  }

  function parseDaemonStatus(text: string): DaemonView {
    const trimmed = text.trim();
    const lines = trimmed ? trimmed.split(/\r?\n/).map((line) => line.trim()).filter(Boolean) : ["No status yet."];
    const lower = trimmed.toLowerCase();
    if (!trimmed || lower.includes("not checked")) {
      return {
        state: "checking",
        label: "Checking",
        detail: "Waiting for the daemon status.",
        lines
      };
    }
    if (lower.includes("not responding") || lower.includes("not running") || lower.includes("stopped")) {
      return {
        state: "stopped",
        label: "Stopped",
        detail: lines[0] ?? "Daemon is stopped.",
        lines
      };
    }
    if (lower.includes("error") || lower.includes("failed")) {
      return {
        state: "warning",
        label: "Needs attention",
        detail: lines[0] ?? "Daemon reported a problem.",
        lines
      };
    }
    return {
      state: "running",
      label: "Running",
      detail: summarizeStatus(lines),
      lines
    };
  }

  function summarizeStatus(lines: string[]) {
    const bindingLines = lines.filter((line) => line.includes("=>") || line.toLowerCase().includes("hotkey"));
    if (bindingLines.length === 1) return "1 binding is armed.";
    if (bindingLines.length > 1) return `${bindingLines.length} bindings are armed.`;
    return lines[0] ?? "Ready.";
  }

  function isBusy(action: string) {
    return busyAction === action;
  }

  async function minimizeWindow() {
    await getCurrentWindow().minimize();
  }

  async function toggleMaximizeWindow() {
    await getCurrentWindow().toggleMaximize();
  }

  async function closeWindow() {
    await getCurrentWindow().close();
  }

  async function startWindowDrag(event: PointerEvent) {
    if (event.button !== 0) return;
    if ((event.target as HTMLElement).closest("button")) return;
    await getCurrentWindow().startDragging();
  }
</script>

<main>
  <header class="titlebar" role="presentation" data-tauri-drag-region on:pointerdown={startWindowDrag}>
    <div class="window-title" data-tauri-drag-region>
      <span></span>
      <p data-tauri-drag-region>Hotkey To Command</p>
    </div>
    <div class="window-controls">
      <button aria-label="Minimize" on:click={minimizeWindow}>
        <span></span>
      </button>
      <button aria-label="Maximize" on:click={toggleMaximizeWindow}>
        <span></span>
      </button>
      <button class="close-window" aria-label="Close" on:click={closeWindow}>
        <span></span>
      </button>
    </div>
  </header>

  <section class="shell">
    <aside class="sidebar">
      <section class="daemon-panel">
        <h2>Daemon</h2>
        <div class:running={daemonView.state === "running"} class:stopped={daemonView.state === "stopped"} class:warning={daemonView.state === "warning"} class:checking={daemonView.state === "checking"} class="daemon-card">
          <div class="daemon-top">
            <span class="status-dot"></span>
            <div>
              <strong>{daemonView.label}</strong>
              <p>{armedBindings} {armedBindings === 1 ? "binding" : "bindings"} armed</p>
            </div>
          </div>
          <p class="daemon-detail">{daemonView.detail}</p>
        </div>

        <div class="daemon-actions">
          <button class="ghost dark" disabled={!!busyAction} on:click={restartDaemon}>{isBusy("restart") ? "Restarting..." : "Restart"}</button>
          <button class="stop" disabled={!!busyAction} on:click={stopDaemon}>{isBusy("stop") ? "Stopping..." : "Stop"}</button>
        </div>
      </section>

      <section class="log-panel">
        <h2>Live log</h2>
        <div class="log-box">
          {#if eventLog.length}
            {#each eventLog as item}
              <div>{item}</div>
            {/each}
          {:else}
            <div>No activity yet.</div>
          {/if}
        </div>
      </section>

      <section class="config-panel">
        <h2>Config - v{config.version}</h2>
        <p>{configPath || "%APPDATA%\\HotkeyToCommand\\config.json"}</p>
        <label class="advanced">
          <input type="checkbox" checked={advancedVisible} on:change={toggleAdvanced} />
          Advanced mode
        </label>
      </section>
    </aside>

    <section class="content">
      <div class="toolbar">
        <div>
          <h2>Bindings</h2>
          <p>{config.bindings.length} {config.bindings.length === 1 ? "binding" : "bindings"} - {armedBindings} armed</p>
        </div>
        <div class="actions">
          <button class="ghost" disabled={!!busyAction} on:click={startDaemon}>{isBusy("start") ? "Starting..." : "Start"}</button>
          <button class="ghost" disabled={!!busyAction} on:click={refreshStatus}>{isBusy("status") ? "Checking..." : "Status"}</button>
          <button class="primary" disabled={!canSave || !!busyAction} on:click={saveAndApply}>{isBusy("save") ? "Saving..." : "Save"}</button>
          <button class="add" disabled={!!busyAction} on:click={addBinding}>+ Add</button>
        </div>
      </div>

      {#if duplicateHotkeys.size > 0}
        <div class="notice error">Duplicate enabled hotkey: {Array.from(duplicateHotkeys).join(", ")}</div>
      {/if}

      <div class="toasts" aria-live="polite">
        {#each toasts as toast}
          <div class:error={toast.type === "error"} class:success={toast.type === "success"} class:info={toast.type === "info"} class="toast">
            {toast.text}
          </div>
        {/each}
      </div>

      <div class="binding-list">
        {#each config.bindings as binding, index}
          {@const duplicate = duplicateOwner(binding)}
          <article class:disabled={!binding.enabled} class:invalid={!!duplicate}>
            <div class="binding-main">
              <div class="binding-left">
                <label class="switch" aria-label="Binding enabled">
                  <input type="checkbox" bind:checked={binding.enabled} on:change={touch} />
                  <span></span>
                </label>

                <input class="id" bind:value={binding.id} on:input={touch} aria-label="Binding id" />

                <div class="hotkey-editor">
                  {#if hotkeyParts(binding.hotkey).length}
                    {#each hotkeyParts(binding.hotkey) as part, partIndex}
                      <span class:bad={!!duplicate} class="key-chip">{part}</span>
                      {#if partIndex < hotkeyParts(binding.hotkey).length - 1}
                        <span class="plus">+</span>
                      {/if}
                    {/each}
                  {:else}
                    <span class="empty-hotkey">No hotkey</span>
                  {/if}
                  <input
                    class:bad={!!duplicate}
                    value={binding.hotkey}
                    placeholder="Record"
                    on:keydown={(event) => captureHotkey(event, binding)}
                    on:input={(event) => {
                      binding.hotkey = event.currentTarget.value;
                      touch();
                    }}
                  />
                </div>
              </div>

              <div class="binding-action">
                <select class:advanced-pulse={advancedPulse} value={actionSelectValue(binding.action)} on:change={(event) => setActionType(binding, event.currentTarget.value)}>
                  <option value="open_app">Run application</option>
                  <option value="run_script">Run script</option>
                  <option value="cycle_audio_output">Cycle audio output</option>
                  <option value="cycle_microphone_input">Cycle microphone input</option>
                  <option value="cycle_focused_app_audio_output">Focused app output</option>
                  <option value="cycle_focused_app_microphone_input">Focused app microphone</option>
                  <option value="close_focused_app">Close focused app</option>
                  <option value="kill_focused_app">Kill focused app</option>
                  {#if advancedVisible}
                    <option value="inspect_focused_app_audio">Inspect focused app audio</option>
                    <option value="cycle_discord_output_device">Cycle Discord output</option>
                    <option value="cycle_discord_microphone_input">Cycle Discord microphone input</option>
                    <option value="run_command">Run command</option>
                  {/if}
                </select>

                <button class="danger square" disabled={!!busyAction} aria-label="Remove binding" title="Remove binding" on:click={() => removeBinding(index)}>Del</button>
              </div>
            </div>

            {#if duplicate}
              <div class="inline-warning">Duplicate of {binding.hotkey} already bound to {duplicate}.</div>
            {:else}
              <div class="binding-meta">{actionMeta(binding)} - {binding.enabled ? "armed" : "off"}</div>
            {/if}

            {#if binding.action.type === "open_app"}
              <div class="action-details two-col">
                <label>
                  <span>Application path</span>
                  <input bind:value={binding.action.path} on:input={touch} placeholder="C:\Windows\System32\notepad.exe" />
                </label>
                <label>
                  <span>Arguments</span>
                  <input
                    value={binding.action.args.join(" ")}
                    on:input={(event) => {
                      binding.action.args = event.currentTarget.value.split(" ").filter(Boolean);
                      touch();
                    }}
                    placeholder="Optional arguments"
                  />
                </label>
                <button class="ghost browse" disabled={!!busyAction} on:click={() => chooseApp(binding)}>{isBusy("browse") ? "Opening..." : "Browse"}</button>
              </div>
            {:else if binding.action.type === "run_script"}
              <div class="action-details script-details">
                <label>
                  <span>Script path</span>
                  <input bind:value={binding.action.path} on:input={touch} placeholder="C:\Scripts\thing.py" />
                </label>
                <button class="ghost browse" disabled={!!busyAction} on:click={() => chooseScript(binding)}>{isBusy("browse") ? "Opening..." : "Browse"}</button>
                <label>
                  <span>Arguments</span>
                  <input
                    value={binding.action.args.join(" ")}
                    on:input={(event) => {
                      binding.action.args = event.currentTarget.value.split(" ").filter(Boolean);
                      touch();
                    }}
                    placeholder="Optional arguments"
                  />
                </label>
                <label>
                  <span>Working directory</span>
                  <input bind:value={binding.action.working_dir} on:input={touch} placeholder="Optional" />
                </label>
                <label>
                  <span>Interpreter</span>
                  <select value={interpreterSelectValue(binding.action)} on:change={(event) => setInterpreterMode(binding.action, event.currentTarget.value)}>
                    <option value="auto">Auto detect</option>
                    <option value="direct">Compiled program (.exe)</option>
                    <option value="py.exe">Python</option>
                    <option value="powershell.exe">PowerShell</option>
                    <option value="cmd.exe">Command Prompt</option>
                    <option value="node.exe">Node.js</option>
                    <option value="wscript.exe">Windows Script Host</option>
                    <option value="AutoHotkey.exe">AutoHotkey</option>
                    <option value="custom">Custom path</option>
                  </select>
                </label>
                {#if interpreterSelectValue(binding.action) === "custom"}
                  <label>
                    <span>Custom interpreter</span>
                    <input bind:value={binding.action.interpreter} on:input={touch} placeholder="C:\Path\to\interpreter.exe" />
                  </label>
                {/if}
                <label class="inline-check">
                  <input type="checkbox" bind:checked={binding.action.show_window} on:change={touch} />
                  <span>Show window</span>
                </label>
              </div>
            {:else if binding.action.type === "run_command"}
              <div class="action-details">
                <label>
                  <span>Command</span>
                  <input bind:value={binding.action.command} on:input={touch} placeholder="Advanced shell command" />
                </label>
              </div>
            {:else if binding.action.type === "builtin" && binding.action.name === "close_focused_app"}
              <div class="builtin builtin-close">
                <span>Asks the focused window to close, like Alt+F4. The app can still save or cancel.</span>
                <label class="inline-check">
                  <input
                    type="checkbox"
                    checked={binding.action.strong_close ?? false}
                    on:change={(event) => {
                      binding.action.strong_close = event.currentTarget.checked;
                      touch();
                    }}
                  />
                  <span>{binding.action.strong_close ? "Stronger close request" : "Weaker close request"}</span>
                </label>
              </div>
            {:else if binding.action.type === "builtin" && binding.action.name === "kill_focused_app"}
              <div class="builtin builtin-kill">
                Force-ends the focused app process. Unsaved work can be lost, and protected Windows processes are blocked.
              </div>
            {:else}
              <div class="builtin">{actionLabel(binding.action)}</div>
            {/if}
          </article>
        {/each}
      </div>
    </section>
  </section>
</main>
