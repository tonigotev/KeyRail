<script lang="ts">
  import { onMount } from "svelte";
  import { invoke } from "@tauri-apps/api/core";

  type ActionSpec =
    | { type: "builtin"; name: "cycle_audio_output" | "inspect_focused_app_audio" | "cycle_focused_app_audio_output" | "cycle_discord_output_device" }
    | { type: "open_app"; path: string; args: string[]; show_window: boolean }
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

  const defaultConfig: AppConfig = {
    version: 1,
    bindings: []
  };

  let config: AppConfig = defaultConfig;
  let configPath = "";
  let daemonStatus = "Not checked yet.";
  let message = "";
  let advancedVisible = false;

  $: duplicateHotkeys = findDuplicateHotkeys(config.bindings);
  $: canSave = duplicateHotkeys.size === 0 && config.bindings.every((b) => b.id.trim() && b.hotkey.trim());

  onMount(async () => {
    await loadConfig();
    await ensureDaemonOnLaunch();
    await refreshStatus();
  });

  function touch() {
    config = { ...config, bindings: [...config.bindings] };
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
    try {
      const loaded = await invoke<ConfigEnvelope>("load_config");
      config = loaded.config;
      configPath = loaded.path;
      message = "Config loaded.";
    } catch (error) {
      message = `Could not load config: ${error}`;
    }
  }

  async function saveAndApply() {
    if (!canSave) {
      message = "Fix duplicate or empty hotkeys before saving.";
      return;
    }
    try {
      await invoke("save_config", { config });
      await invoke("ensure_daemon");
      await invoke("send_daemon_command", { command: "reload" });
      message = "Saved and reload requested.";
      await refreshStatus();
    } catch (error) {
      message = `Save/apply failed: ${error}`;
    }
  }

  async function refreshStatus() {
    try {
      const result = await invoke<string>("send_daemon_command", { command: "status" });
      daemonStatus = result;
    } catch (error) {
      daemonStatus = `Daemon is not responding: ${error}`;
    }
  }

  async function ensureDaemonOnLaunch() {
    try {
      await invoke("ensure_daemon");
      message = "Daemon is running.";
    } catch (error) {
      daemonStatus = `Daemon is not running: ${error}`;
    }
  }

  async function startDaemon() {
    try {
      await invoke("ensure_daemon");
      message = "Daemon start requested.";
      await refreshStatus();
    } catch (error) {
      message = `Could not start daemon: ${error}`;
    }
  }

  async function stopDaemon() {
    try {
      await invoke("send_daemon_command", { command: "quit" });
      message = "Daemon stopped.";
      daemonStatus = "Daemon is stopped.";
    } catch (error) {
      message = `Could not stop daemon: ${error}`;
    }
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
  }

  function removeBinding(index: number) {
    config.bindings.splice(index, 1);
    touch();
  }

  function setActionType(binding: BindingSpec, type: string) {
    if (type === "cycle_audio_output") binding.action = { type: "builtin", name: "cycle_audio_output" };
    if (type === "inspect_focused_app_audio") binding.action = { type: "builtin", name: "inspect_focused_app_audio" };
    if (type === "cycle_focused_app_audio_output") binding.action = { type: "builtin", name: "cycle_focused_app_audio_output" };
    if (type === "cycle_discord_output_device") binding.action = { type: "builtin", name: "cycle_discord_output_device" };
    if (type === "open_app") binding.action = { type: "open_app", path: "", args: [], show_window: true };
    if (type === "run_command") binding.action = { type: "run_command", command: "", show_window: false };
    touch();
  }

  function actionSelectValue(action: ActionSpec) {
    if (action.type === "builtin") return action.name;
    return action.type;
  }

  async function chooseApp(binding: BindingSpec) {
    if (binding.action.type !== "open_app") return;
    const path = await invoke<string | null>("select_app_path");
    if (path) {
      binding.action.path = path;
      touch();
    }
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
</script>

<main>
  <section class="topbar">
    <div>
      <h1>Hotkey To Command</h1>
      <p>{configPath || "Loading config path..."}</p>
    </div>
    <div class="actions">
      <button class="ghost" on:click={startDaemon}>Start</button>
      <button class="ghost" on:click={stopDaemon}>Stop</button>
      <button class="ghost" on:click={refreshStatus}>Status</button>
      <button class="primary" disabled={!canSave} on:click={saveAndApply}>Save</button>
    </div>
  </section>

  {#if duplicateHotkeys.size > 0}
    <div class="notice error">Duplicate enabled hotkey: {Array.from(duplicateHotkeys).join(", ")}</div>
  {:else if message}
    <div class="notice">{message}</div>
  {/if}

  <section class="layout">
    <div class="bindings">
      <div class="section-head">
        <h2>Bindings</h2>
        <button class="icon-text" on:click={addBinding}>+ Add</button>
      </div>

      {#each config.bindings as binding, index}
        <article class:disabled={!binding.enabled}>
          <div class="row head">
            <label class="toggle">
              <input type="checkbox" bind:checked={binding.enabled} on:change={touch} />
              <span>{binding.enabled ? "On" : "Off"}</span>
            </label>
            <input class="id" bind:value={binding.id} on:input={touch} aria-label="Binding id" />
            <button class="danger" on:click={() => removeBinding(index)}>Remove</button>
          </div>

          <div class="grid">
            <label>
              <span>Hotkey</span>
              <input
                class:bad={duplicateHotkeys.has(binding.hotkey.toLowerCase())}
                value={binding.hotkey}
                placeholder="Press a combo"
                on:keydown={(event) => captureHotkey(event, binding)}
                on:input={(event) => {
                  binding.hotkey = event.currentTarget.value;
                  touch();
                }}
              />
            </label>

            <label>
              <span>Action</span>
              <select value={actionSelectValue(binding.action)} on:change={(event) => setActionType(binding, event.currentTarget.value)}>
                <option value="open_app">Open app</option>
                <option value="cycle_audio_output">Cycle audio output</option>
                <option value="cycle_focused_app_audio_output">Cycle focused app output</option>
                {#if advancedVisible}
                  <option value="inspect_focused_app_audio">Inspect focused app audio</option>
                  <option value="cycle_discord_output_device">Cycle Discord output</option>
                  <option value="run_command">Run command</option>
                {/if}
              </select>
            </label>
          </div>

          {#if binding.action.type === "open_app"}
            <div class="path-row">
              <label>
                <span>App path</span>
                <input bind:value={binding.action.path} on:input={touch} placeholder="C:\Windows\System32\notepad.exe" />
              </label>
              <button class="ghost" on:click={() => chooseApp(binding)}>Browse</button>
            </div>
            <label>
              <span>Args</span>
              <input
                value={binding.action.args.join(" ")}
                on:input={(event) => {
                  binding.action.args = event.currentTarget.value.split(" ").filter(Boolean);
                  touch();
                }}
                placeholder="Optional arguments"
              />
            </label>
          {:else if binding.action.type === "run_command"}
            <label>
              <span>Command</span>
              <input bind:value={binding.action.command} on:input={touch} placeholder="Advanced shell command" />
            </label>
          {:else}
            <div class="builtin">Built-in action selected.</div>
          {/if}
        </article>
      {/each}
    </div>

    <aside>
      <div class="section-head">
        <h2>Daemon</h2>
        <label class="advanced">
          <input type="checkbox" bind:checked={advancedVisible} />
          Advanced
        </label>
      </div>
      <pre>{daemonStatus}</pre>
    </aside>
  </section>
</main>
