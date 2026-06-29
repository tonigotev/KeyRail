<script lang="ts">
  import { onMount, tick } from "svelte";
  import { invoke } from "@tauri-apps/api/core";
  import { getCurrentWindow } from "@tauri-apps/api/window";

  type ActionSpec =
    | { type: "builtin"; name: "cycle_audio_output" | "cycle_microphone_input" | "inspect_focused_app_audio" | "cycle_focused_app_audio_output" | "cycle_focused_app_microphone_input" | "cycle_discord_output_device" | "cycle_discord_microphone_input" | "media_picker_open" | "media_picker_next" | "media_picker_previous" | "media_picker_confirm" | "media_picker_cancel" | "media_selected_play_pause" | "media_selected_next" | "media_selected_previous" | "media_next_contextual" | "media_previous_contextual" | "media_play_pause_contextual" | "inspect_media_sessions" | "close_focused_app" | "kill_focused_app"; strong_close?: boolean }
    | { type: "open_app"; path: string; args: string[]; show_window: boolean }
    | { type: "run_script"; path: string; args: string[]; working_dir: string; interpreter: string; show_window: boolean }
    | { type: "run_command"; command: string; show_window: boolean };

  type BindingSpec = {
    id: string;
    enabled: boolean;
    hotkey: string;
    action: ActionSpec;
  };

  type AppSettings = {
    hotkey_mode: "global" | "command";
    command_hotkey: string;
    command_timeout_ms: number;
  };

  type OnboardingState = {
    completed: boolean;
    version: number;
    seen_media_picker_extension: boolean;
    seen_discord_bridge: boolean;
  };

  type AppConfig = {
    version: number;
    settings: AppSettings;
    onboarding: OnboardingState;
    bindings: BindingSpec[];
  };

  type ConfigEnvelope = {
    path: string;
    config: AppConfig;
    active_preset?: string | null;
  };

  type PresetInfo = {
    name: string;
    path: string;
    active: boolean;
  };

  type DeleteTarget =
    | { type: "binding"; index: number; name: string }
    | { type: "media-group"; count: number }
    | { type: "preset"; name: string };

  type Toast = {
    id: number;
    type: "success" | "error" | "info";
    text: string;
  };

  type LogEntry = {
    id: number;
    time: string;
    level: "ok" | "info" | "warn" | "error";
    text: string;
    detail?: string;
  };

  type DaemonView = {
    state: "running" | "stopped" | "warning" | "checking";
    label: string;
    detail: string;
    lines: string[];
  };

  type BrowserBridgeStatus = {
    connected: boolean;
    clients: number;
    targets: number;
    detail: string;
  };

  type StartupStatus = {
    enabled: boolean;
    path: string | null;
    detail: string;
  };

  type AddItem = {
    id: string;
    category: string;
    icon: string;
    name: string;
    description: string;
    actionType: string;
    count?: number;
    advanced?: boolean;
  };

  type ThemeName = "current" | "dark" | "cream" | "barbie";

  const themes: { name: ThemeName; label: string; colors: string[] }[] = [
    { name: "current", label: "Current", colors: ["#13222d", "#0f8f8e", "#f6f7f8"] },
    { name: "dark", label: "Dark", colors: ["#090d14", "#4f8cff", "#151a22"] },
    { name: "cream", label: "Cream", colors: ["#f7ead5", "#b7672f", "#fffaf0"] },
    { name: "barbie", label: "Barbie", colors: ["#ff4fa3", "#8f2cff", "#fff0f8"] }
  ];

  const defaultConfig: AppConfig = {
    version: 1,
    settings: {
      hotkey_mode: "global",
      command_hotkey: "ctrl+alt+space",
      command_timeout_ms: 4000
    },
    onboarding: {
      completed: false,
      version: 1,
      seen_media_picker_extension: false,
      seen_discord_bridge: false
    },
    bindings: []
  };

  let config: AppConfig = defaultConfig;
  let configPath = "";
  let activePreset = "";
  let presets: PresetInfo[] = [];
  let daemonStatus = "Not checked yet.";
  let advancedVisible = false;
  let busyAction = "";
  let toasts: Toast[] = [];
  let toastId = 1;
  let logId = 1;
  let statusTimer: ReturnType<typeof setInterval> | undefined;
  let eventLog: LogEntry[] = [];
  let simpleLog = true;
  let lastDaemonIssue = "";
  let advancedPulse = false;
  let advancedPulseTimer: ReturnType<typeof setTimeout> | undefined;
  let theme: ThemeName = "current";
  let searchQuery = "";
  let activePage: "bindings" | "settings" = "bindings";
  let onboardingOpen = false;
  let onboardingStep = 0;
  let onboardingAnimationKey = 0;
  let onboardingDirection: "forward" | "back" = "forward";
  let onboardingStarter: "cycle_audio_output" | "media_picker_bundle" | "open_app" | "" = "";
  let onboardingBindingId = "";
  let onboardingCreatedBindingIds: string[] = [];
  let onboardingTestStartEvent = "";
  let onboardingTestState: "idle" | "listening" | "success" | "timeout" | "skipped" = "idle";
  let onboardingDiscordOptIn = false;
  let addChooserOpen = false;
  let addQuery = "";
  let addSelectedIndex = 0;
  let addSearchInput: HTMLInputElement | undefined;
  let highlightedBindingId = "";
  let recordingBindingId = "";
  let highlightTimer: ReturnType<typeof setTimeout> | undefined;
  let pendingDelete: DeleteTarget | null = null;
  let mediaPickerIntroOpen = false;
  let daemonStatusCheckingVisual = false;
  let browserBridgePulse = false;
  let browserBridgeCheckingVisual = false;
  let browserBridgePulseTimer: ReturnType<typeof setTimeout> | undefined;
  let browserBridge: BrowserBridgeStatus = {
    connected: false,
    clients: 0,
    targets: 0,
    detail: "Not checked yet."
  };
  let startupStatus: StartupStatus = {
    enabled: false,
    path: null,
    detail: "Not checked yet."
  };

  $: duplicateHotkeys = findDuplicateHotkeys(config.bindings);
  $: canSave = duplicateHotkeys.size === 0 && config.bindings.every((b) => b.id.trim() && b.hotkey.trim());
  $: daemonView = parseDaemonStatus(daemonStatus);
  $: displayedDaemonView = daemonStatusCheckingVisual
    ? { ...daemonView, state: "checking" as const, label: "Checking", detail: "Refreshing daemon status..." }
    : daemonView;
  $: daemonPowerText = busyAction === "start"
    ? "Starting..."
    : busyAction === "stop"
      ? "Stopping..."
      : daemonView.state === "stopped"
        ? "Start"
        : "Stop";
  $: armedBindings = config.bindings.filter((binding) => binding.enabled && binding.hotkey.trim()).length;
  $: bindingRows = config.bindings.map((binding, index) => ({ binding, index }));
  $: visibleRows = bindingRows.filter(({ binding }) => bindingMatchesSearch(binding, searchQuery));
  $: visibleMediaRows = visibleRows.filter(({ binding }) => isMediaAction(binding.action));
  $: visibleRegularRows = visibleRows.filter(({ binding }) => !isMediaAction(binding.action));
  $: visibleCount = visibleRegularRows.length + visibleMediaRows.length;
  $: onboardingSteps = buildOnboardingSteps();
  $: onboardingCurrent = onboardingSteps[Math.min(onboardingStep, Math.max(0, onboardingSteps.length - 1))] ?? "welcome";
  $: onboardingProgressLabel = `Step ${Math.min(onboardingStep + 1, onboardingSteps.length)} of ${onboardingSteps.length}`;
  $: onboardingCanGoNext = canAdvanceOnboarding();
  $: onboardingTestBinding = config.bindings.find((binding) => binding.id === onboardingBindingId);
  $: addItems = buildAddItems(advancedVisible);
  $: filteredAddItems = addItems.filter((item) => addItemMatches(item, addQuery));
  $: if (addSelectedIndex >= filteredAddItems.length) addSelectedIndex = Math.max(0, filteredAddItems.length - 1);

  onMount(() => {
    const savedTheme = localStorage.getItem("hotkey-ui-theme") as ThemeName | null;
    if (savedTheme && themes.some((item) => item.name === savedTheme)) theme = savedTheme;
    simpleLog = localStorage.getItem("hotkey-log-mode") !== "detailed";
    void (async () => {
      await loadConfig().catch(() => undefined);
      await refreshPresets();
      await ensureDaemonOnLaunch();
      await refreshStatus();
      await refreshBrowserBridge(false);
      await refreshStartupStatus();
      if (!config.onboarding.completed) {
        onboardingOpen = true;
        await enableStartupForOnboardingDefault();
      }
    })();
    statusTimer = setInterval(() => {
      void refreshStatusQuietly();
      void refreshBrowserBridgeQuietly();
    }, 5000);
    return () => {
      if (statusTimer) clearInterval(statusTimer);
    };
  });

  function setTheme(name: ThemeName) {
    theme = name;
    localStorage.setItem("hotkey-ui-theme", name);
    pushToast(`${themes.find((item) => item.name === name)?.label ?? "Theme"} theme applied.`, "info");
    logEvent(`theme set to ${name}`);
  }

  function setPage(page: "bindings" | "settings") {
    activePage = page;
  }

  function touch() {
    config = { ...config, bindings: [...config.bindings] };
  }

  function touchSettings() {
    config = { ...config, settings: { ...config.settings } };
  }

  function setSimpleLog(value: boolean) {
    simpleLog = value;
    localStorage.setItem("hotkey-log-mode", value ? "simple" : "detailed");
  }

  function clearLog() {
    eventLog = [];
  }

  function resetOnboardingForDev() {
    config.onboarding = {
      ...config.onboarding,
      completed: false,
      version: 1
    };
    onboardingStep = 0;
    onboardingAnimationKey += 1;
    onboardingDirection = "forward";
    onboardingOpen = true;
    touchOnboarding();
    void saveConfigOnly();
    pushToast("Onboarding reset.", "info");
  }

  function previewOnboarding() {
    onboardingStep = 0;
    onboardingAnimationKey += 1;
    onboardingDirection = "forward";
    onboardingStarter = "";
    onboardingBindingId = "";
    onboardingCreatedBindingIds = [];
    onboardingTestState = "idle";
    onboardingDiscordOptIn = false;
    onboardingOpen = true;
  }

  function touchOnboarding() {
    config = { ...config, onboarding: { ...config.onboarding } };
  }

  function maybeShowMediaPickerIntro() {
    if (onboardingOpen) return;
    if (browserBridge.connected) return;
    if (config.onboarding.seen_media_picker_extension) return;
    mediaPickerIntroOpen = true;
    config.onboarding.seen_media_picker_extension = true;
    touchOnboarding();
    void saveConfigOnly();
  }

  function closeMediaPickerIntro() {
    mediaPickerIntroOpen = false;
  }

  function logEvent(text: string, level: LogEntry["level"] = "info", detail = "") {
    const time = new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    eventLog = [{ id: logId++, time, level, text, detail }, ...eventLog].slice(0, 40);
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
      config = withConfigDefaults(loaded.config);
      configPath = loaded.path;
      activePreset = loaded.active_preset ?? "";
      logEvent("loaded config");
    }, false);
  }

  function withConfigDefaults(value: AppConfig): AppConfig {
    return {
      ...defaultConfig,
      ...value,
      settings: {
        ...defaultConfig.settings,
        ...(value.settings ?? {})
      },
      onboarding: {
        ...defaultConfig.onboarding,
        ...(value.onboarding ?? {})
      },
      bindings: value.bindings ?? []
    };
  }

  async function saveConfigOnly() {
    await invoke("save_config", { config });
  }

  async function refreshPresets() {
    try {
      presets = await invoke<PresetInfo[]>("list_presets");
      const active = presets.find((preset) => preset.active);
      if (active) activePreset = active.name;
      else if (activePreset && !presets.some((preset) => preset.name === activePreset)) activePreset = "";
    } catch (error) {
      logEvent("preset list failed", "error", String(error));
    }
  }

  function cleanPresetConfig(): AppConfig {
    return withConfigDefaults({
      ...defaultConfig,
      settings: { ...config.settings },
      onboarding: {
        ...config.onboarding,
        completed: true
      },
      bindings: []
    });
  }

  async function createCleanPreset() {
    await runWithFeedback("preset-create", "Preset created.", async () => {
      await saveConfigOnly();
      const nextConfig = cleanPresetConfig();
      const created = await invoke<PresetInfo>("create_preset", { config: nextConfig });
      config = nextConfig;
      activePreset = created.name;
      await refreshPresets();
      await invoke("ensure_daemon");
      await invoke("send_daemon_command", { command: "reload" });
      await refreshStatus();
      logEvent(`created ${created.name}`, "ok", created.path);
    });
  }

  async function switchPreset(name: string) {
    if (!name || name === activePreset) return;
    if (!canSave) {
      pushToast("Fix duplicate or empty hotkeys before switching presets.", "error");
      return;
    }
    await runWithFeedback("preset-switch", `Loaded ${name}.`, async () => {
      await saveConfigOnly();
      const loaded = await invoke<ConfigEnvelope>("switch_preset", { name });
      config = withConfigDefaults(loaded.config);
      configPath = loaded.path;
      activePreset = loaded.active_preset ?? name;
      await refreshPresets();
      await invoke("ensure_daemon");
      await invoke("send_daemon_command", { command: "reload" });
      await refreshStatus();
      logEvent(`switched to ${name}`, "ok");
    });
  }

  function requestDeletePreset() {
    if (!activePreset) {
      pushToast("No preset selected.", "info");
      return;
    }
    pendingDelete = { type: "preset", name: activePreset };
  }

  async function deletePresetByName(name: string) {
    if (!name) return;
    await runWithFeedback("preset-delete", `${name} deleted.`, async () => {
      const deletedName = name;
      const loaded = await invoke<ConfigEnvelope | null>("delete_preset", { name: deletedName });
      if (loaded) {
        config = withConfigDefaults(loaded.config);
        configPath = loaded.path;
        activePreset = loaded.active_preset ?? "";
      } else {
        activePreset = "";
      }
      await refreshPresets();
      await invoke("ensure_daemon");
      await invoke("send_daemon_command", { command: "reload" });
      await refreshStatus();
      logEvent(`deleted ${deletedName}`, "warn");
    });
  }

  function deleteTitle(target: DeleteTarget) {
    if (target.type === "binding") return `Delete ${target.name}?`;
    if (target.type === "media-group") return "Delete Media picker controls?";
    return `Delete ${target.name}?`;
  }

  function deleteMessage(target: DeleteTarget) {
    if (target.type === "binding") return "This removes the binding from the current config. You can recreate it later, but the hotkey and action details will be gone.";
    if (target.type === "media-group") return `This deletes all ${target.count} bindings in this group.`;
    return "This deletes the preset folder and its JSON file. If this is the active preset, the app will fall back to a clean main config.";
  }

  function deleteButtonText(target: DeleteTarget) {
    if (target.type === "media-group") return "Delete group";
    if (target.type === "preset") return "Delete preset";
    return "Delete binding";
  }

  async function confirmPendingDelete() {
    if (!pendingDelete) return;
    const target = pendingDelete;
    pendingDelete = null;
    if (target.type === "binding") {
      removeBinding(target.index);
    } else if (target.type === "media-group") {
      removeMediaPickerGroup();
    } else {
      await deletePresetByName(target.name);
    }
  }

  function cancelPendingDelete() {
    pendingDelete = null;
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

  async function refreshStatusFromCard() {
    daemonStatusCheckingVisual = true;
    try {
      await Promise.all([
        refreshStatusQuietly(),
        wait(650)
      ]);
      logEvent("status checked");
    } finally {
      daemonStatusCheckingVisual = false;
    }
  }

  async function refreshStatusQuietly() {
    try {
      const status = await invoke<string>("send_daemon_command", { command: "status" });
      daemonStatus = status;
      const issue = daemonIssue(status);
      if (issue && issue !== lastDaemonIssue) {
        logEvent(issue, issue.toLowerCase().includes("stopped") ? "warn" : "error", status);
        lastDaemonIssue = issue;
      } else if (!issue && lastDaemonIssue) {
        logEvent("daemon recovered", "ok", status);
        lastDaemonIssue = "";
      }
    } catch (error) {
      const message = `Daemon is not responding: ${error}`;
      daemonStatus = message;
      if (message !== lastDaemonIssue) {
        logEvent("daemon is not responding", "error", String(error));
        lastDaemonIssue = message;
      }
    }
  }

  async function ensureDaemonOnLaunch() {
    await runWithFeedback("launch", "Daemon is running.", async () => {
      await invoke("ensure_daemon");
    }, false).catch((error) => {
      daemonStatus = `Daemon is not running: ${error}`;
      logEvent("daemon launch failed", "error", String(error));
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
      try {
        await invoke("send_daemon_command", { command: "quit" });
      } catch (error) {
        logEvent("daemon stop confirmed by disconnect", "info", String(error));
      }
      daemonStatus = "Daemon is stopped.";
      logEvent("daemon stopped");
    });
  }

  async function toggleDaemonPower() {
    if (daemonView.state === "stopped") {
      await startDaemon();
    } else {
      await stopDaemon();
    }
  }

  function setCardHoverDirection(event: PointerEvent) {
    const card = event.currentTarget as HTMLElement;
    const rect = card.getBoundingClientRect();
    card.dataset.hoverFrom = event.clientY < rect.top + rect.height / 2 ? "top" : "bottom";
  }

  function wait(milliseconds: number) {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
  }

  async function refreshBrowserBridge(showVisual = true) {
    const wasConnected = browserBridge.connected;
    if (showVisual) browserBridgeCheckingVisual = true;
    try {
      await Promise.all([
        runWithFeedback("browser-status", "Browser bridge checked.", refreshBrowserBridgeQuietly, false),
        showVisual ? wait(1000) : Promise.resolve()
      ]);
      if (showVisual && (browserBridge.connected || browserBridge.connected !== wasConnected)) {
        browserBridgePulse = true;
        if (browserBridgePulseTimer) clearTimeout(browserBridgePulseTimer);
        browserBridgePulseTimer = setTimeout(() => {
          browserBridgePulse = false;
        }, 900);
      }
    } finally {
      browserBridgeCheckingVisual = false;
    }
  }

  async function refreshBrowserBridgeQuietly() {
    try {
      browserBridge = await invoke<BrowserBridgeStatus>("browser_bridge_status");
    } catch (error) {
      browserBridge = {
        ...browserBridge,
        connected: false,
        clients: 0,
        targets: 0,
        detail: `Could not check browser bridge: ${error}`
      };
    }
  }

  async function refreshStartupStatus() {
    try {
      startupStatus = await invoke<StartupStatus>("daemon_startup_status");
    } catch (error) {
      startupStatus = {
        enabled: false,
        path: null,
        detail: String(error)
      };
    }
  }

  async function setDaemonStartup(enabled: boolean) {
    await runWithFeedback("startup", enabled ? "Daemon will start with Windows." : "Daemon startup disabled.", async () => {
      startupStatus = await invoke<StartupStatus>("set_daemon_startup", { enabled });
      logEvent(enabled ? "enabled startup" : "disabled startup", "info", startupStatus.detail);
    });
  }

  async function enableStartupForOnboardingDefault() {
    if (startupStatus.enabled) return;
    try {
      startupStatus = await invoke<StartupStatus>("set_daemon_startup", { enabled: true });
      logEvent("enabled startup for onboarding default", "info", startupStatus.detail);
    } catch (error) {
      startupStatus = {
        enabled: false,
        path: null,
        detail: `Startup could not be enabled automatically: ${error}`
      };
    }
  }

  function buildOnboardingSteps() {
    const steps = ["welcome", "startup", "mode", "binding"] as string[];
    if (onboardingBindingId) steps.push("test");
    if (onboardingStarter === "media_picker_bundle") steps.push("browser");
    if (onboardingDiscordOptIn) steps.push("discord");
    steps.push("finish");
    return steps;
  }

  function canAdvanceOnboarding() {
    if (onboardingCurrent === "binding") {
      if (!onboardingStarter) return true;
      if (!onboardingBindingId) return true;
      if (!onboardingTestBinding?.hotkey.trim()) return false;
      return !duplicateOwner(onboardingTestBinding);
    }
    return true;
  }

  async function completeOnboarding() {
    config.onboarding = {
      ...config.onboarding,
      completed: true,
      version: 1
    };
    touchOnboarding();
    await saveConfigOnly();
    onboardingOpen = false;
    logEvent("onboarding completed", "ok");
    await ensureDaemonOnLaunch();
    await saveAndApply();
  }

  async function skipOnboarding() {
    cleanupIncompleteOnboardingBindings();
    config.onboarding = {
      ...config.onboarding,
      completed: true,
      version: 1
    };
    touchOnboarding();
    await saveConfigOnly();
    onboardingOpen = false;
    logEvent("onboarding skipped", "info");
    await ensureDaemonOnLaunch();
  }

  async function nextOnboardingStep() {
    if (!onboardingCanGoNext) return;
    if (onboardingCurrent === "binding" && onboardingStarter && !onboardingBindingId) {
      await createOnboardingStarter();
      return;
    }
    if (onboardingCurrent === "test" && onboardingTestState === "idle") {
      await startOnboardingHotkeyTest();
      return;
    }
    if (onboardingCurrent === "finish") {
      await completeOnboarding();
      return;
    }
    if (onboardingCurrent === "browser") {
      config.onboarding.seen_media_picker_extension = true;
      touchOnboarding();
      void saveConfigOnly();
    }
    if (onboardingCurrent === "discord") {
      config.onboarding.seen_discord_bridge = true;
      touchOnboarding();
      void saveConfigOnly();
    }
    moveOnboardingStep(Math.min(onboardingStep + 1, onboardingSteps.length - 1), "forward");
  }

  function previousOnboardingStep() {
    moveOnboardingStep(Math.max(0, onboardingStep - 1), "back");
  }

  function moveOnboardingStep(step: number, direction: "forward" | "back") {
    if (step === onboardingStep) return;
    onboardingDirection = direction;
    onboardingStep = step;
    onboardingAnimationKey += 1;
  }

  async function createOnboardingStarter() {
    if (!onboardingStarter) {
      moveOnboardingStep(Math.min(onboardingStep + 1, onboardingSteps.length - 1), "forward");
      return;
    }

    const beforeIds = new Set(config.bindings.map((binding) => binding.id));
    if (onboardingStarter === "media_picker_bundle") {
      await addMediaPickerBundle();
    } else {
      const binding: BindingSpec = {
        id: uniqueBindingId(defaultIdForAction(onboardingStarter)),
        enabled: true,
        hotkey: "",
        action: onboardingStarter === "open_app"
          ? { type: "open_app", path: "C:\\Windows\\System32\\notepad.exe", args: [], show_window: true }
          : actionForType(onboardingStarter)
      };
      config.bindings = [...config.bindings, binding];
      touch();
      await focusNewBinding(binding.id);
    }

    onboardingCreatedBindingIds = config.bindings
      .map((binding) => binding.id)
      .filter((id) => !beforeIds.has(id));
    onboardingBindingId = onboardingCreatedBindingIds[0] ?? "";
    await tick();
  }

  function cleanupIncompleteOnboardingBindings() {
    if (!onboardingCreatedBindingIds.length) return;
    config.bindings = config.bindings.filter((binding) => {
      if (!onboardingCreatedBindingIds.includes(binding.id)) return true;
      return !!binding.hotkey.trim();
    });
    touch();
  }

  function captureOnboardingHotkey(event: KeyboardEvent) {
    if (!onboardingTestBinding) return;
    captureHotkey(event, onboardingTestBinding);
    onboardingTestState = "idle";
  }

  function lastHotkeyEvent(status: string) {
    return status.split(/\r?\n/).find((line) => line.trim().startsWith("last hotkey:")) ?? "";
  }

  async function startOnboardingHotkeyTest() {
    if (!onboardingTestBinding?.hotkey.trim()) return;
    onboardingTestState = "listening";
    await saveAndApply();
    onboardingTestStartEvent = lastHotkeyEvent(daemonStatus);
    const startedAt = Date.now();
    while (onboardingOpen && onboardingCurrent === "test" && Date.now() - startedAt < 6500) {
      await wait(700);
      await refreshStatusQuietly();
      const currentEvent = lastHotkeyEvent(daemonStatus);
      if (currentEvent && currentEvent !== onboardingTestStartEvent && currentEvent.includes(onboardingTestBinding.id)) {
        onboardingTestState = "success";
        return;
      }
    }
    if (onboardingTestState === "listening") onboardingTestState = "timeout";
  }

  function skipOnboardingTest() {
    onboardingTestState = "skipped";
    moveOnboardingStep(Math.min(onboardingStep + 1, onboardingSteps.length - 1), "forward");
  }

  function onboardingPrimaryText() {
    if (onboardingCurrent === "finish") return "Start using HotkeyToCommand";
    if (onboardingCurrent === "binding" && onboardingStarter && !onboardingBindingId) return "Create binding";
    if (onboardingCurrent === "test" && onboardingTestState === "idle") return "Start test";
    return "Next";
  }

  async function openExtensionsPage(browser: "brave" | "chrome" | "edge") {
    const label = browser === "brave" ? "Brave" : browser === "chrome" ? "Chrome" : "Edge";
    await runWithFeedback(`extensions-${browser}`, `${label} extension page opened.`, async () => {
      await invoke("open_browser_extensions_page", { browser });
      logEvent(`opened ${label} extension page`);
    });
  }

  function buildAddItems(showAdvanced: boolean): AddItem[] {
    const items: AddItem[] = [
      { id: "open-app", category: "WINDOW & SYSTEM", icon: "APP", name: "Run application", description: "Open an app or file with optional arguments.", actionType: "open_app" },
      { id: "run-script", category: "WINDOW & SYSTEM", icon: "SCR", name: "Run script", description: "Run Python, PowerShell, Node, AutoHotkey, or an executable.", actionType: "run_script" },
      { id: "close-focused-app", category: "WINDOW & SYSTEM", icon: "WIN", name: "Close focused app", description: "Ask the active app to close, like Alt+F4.", actionType: "close_focused_app" },
      { id: "kill-focused-app", category: "WINDOW & SYSTEM", icon: "KILL", name: "Kill focused app", description: "Force-end the active app process.", actionType: "kill_focused_app" },
      { id: "cycle-audio-output", category: "AUDIO", icon: "OUT", name: "Cycle audio output", description: "Switch the default output device.", actionType: "cycle_audio_output" },
      { id: "cycle-microphone-input", category: "AUDIO", icon: "MIC", name: "Cycle microphone input", description: "Switch the default microphone.", actionType: "cycle_microphone_input" },
      { id: "focused-app-output", category: "AUDIO", icon: "APP", name: "Focused app output", description: "Switch output for the app currently in focus.", actionType: "cycle_focused_app_audio_output" },
      { id: "focused-app-microphone", category: "AUDIO", icon: "MIC", name: "Focused app microphone", description: "Switch microphone for the app currently in focus.", actionType: "cycle_focused_app_microphone_input" },
      { id: "open-media-picker", category: "MEDIA", icon: "PICK", name: "Open media picker", description: "Choose a media target to control.", actionType: "media_picker_open" },
      { id: "media-next", category: "MEDIA", icon: "NEXT", name: "Media next / picker next", description: "Skip media or move the picker selection forward.", actionType: "media_next_contextual" },
      { id: "media-previous", category: "MEDIA", icon: "PREV", name: "Media previous / picker previous", description: "Go back or move the picker selection backward.", actionType: "media_previous_contextual" },
      { id: "media-play-pause", category: "MEDIA", icon: "PLAY", name: "Media play/pause / picker confirm", description: "Toggle playback or confirm the picker target.", actionType: "media_play_pause_contextual" },
      { id: "media-picker-controls", category: "PRESETS", icon: "SET", name: "Media picker controls", description: "Add the full media picker hotkey group.", actionType: "media_picker_bundle", count: 5 },
    ];

    if (showAdvanced) {
      items.push(
        { id: "inspect-focused-app-audio", category: "ADVANCED", icon: "DBG", name: "Inspect focused app audio", description: "Show focused app audio sessions and devices.", actionType: "inspect_focused_app_audio", advanced: true },
        { id: "inspect-media-sessions", category: "ADVANCED", icon: "DBG", name: "Inspect media sessions", description: "Show raw media session targets.", actionType: "inspect_media_sessions", advanced: true },
        { id: "cycle-discord-output", category: "ADVANCED", icon: "DISC", name: "Cycle Discord output", description: "Use the Discord bridge output switcher.", actionType: "cycle_discord_output_device", advanced: true },
        { id: "cycle-discord-microphone", category: "ADVANCED", icon: "DISC", name: "Cycle Discord microphone", description: "Use the Discord bridge microphone switcher.", actionType: "cycle_discord_microphone_input", advanced: true },
        { id: "run-command", category: "ADVANCED", icon: "CMD", name: "Run command", description: "Run a raw shell command.", actionType: "run_command", advanced: true }
      );
    }

    return items;
  }

  function addItemMatches(item: AddItem, query: string) {
    const needle = query.trim().toLowerCase();
    if (!needle) return true;
    return [item.name, item.description, item.category, item.actionType].join(" ").toLowerCase().includes(needle);
  }

  async function openAddChooser() {
    addChooserOpen = true;
    addQuery = "";
    addSelectedIndex = 0;
    await tick();
    addSearchInput?.focus();
  }

  function closeAddChooser() {
    addChooserOpen = false;
  }

  function addChooserKeydown(event: KeyboardEvent) {
    if (event.key === "Escape") {
      event.preventDefault();
      closeAddChooser();
      return;
    }
    if (!filteredAddItems.length) return;
    if (event.key === "ArrowDown") {
      event.preventDefault();
      addSelectedIndex = (addSelectedIndex + 1) % filteredAddItems.length;
      return;
    }
    if (event.key === "ArrowUp") {
      event.preventDefault();
      addSelectedIndex = (addSelectedIndex - 1 + filteredAddItems.length) % filteredAddItems.length;
      return;
    }
    if (event.key === "Enter") {
      event.preventDefault();
      createFromAddItem(filteredAddItems[addSelectedIndex]);
    }
  }

  function actionForType(type: string): ActionSpec {
    if (type === "cycle_audio_output") return { type: "builtin", name: "cycle_audio_output" };
    if (type === "cycle_microphone_input") return { type: "builtin", name: "cycle_microphone_input" };
    if (type === "inspect_focused_app_audio") return { type: "builtin", name: "inspect_focused_app_audio" };
    if (type === "cycle_focused_app_audio_output") return { type: "builtin", name: "cycle_focused_app_audio_output" };
    if (type === "cycle_focused_app_microphone_input") return { type: "builtin", name: "cycle_focused_app_microphone_input" };
    if (type === "cycle_discord_output_device") return { type: "builtin", name: "cycle_discord_output_device" };
    if (type === "cycle_discord_microphone_input") return { type: "builtin", name: "cycle_discord_microphone_input" };
    if (type === "media_picker_open") return { type: "builtin", name: "media_picker_open" };
    if (type === "media_next_contextual") return { type: "builtin", name: "media_next_contextual" };
    if (type === "media_previous_contextual") return { type: "builtin", name: "media_previous_contextual" };
    if (type === "media_play_pause_contextual") return { type: "builtin", name: "media_play_pause_contextual" };
    if (type === "inspect_media_sessions") return { type: "builtin", name: "inspect_media_sessions" };
    if (type === "close_focused_app") return { type: "builtin", name: "close_focused_app", strong_close: false };
    if (type === "kill_focused_app") return { type: "builtin", name: "kill_focused_app" };
    if (type === "run_script") return { type: "run_script", path: "", args: [], working_dir: "", interpreter: "auto", show_window: false };
    if (type === "run_command") return { type: "run_command", command: "", show_window: false };
    return { type: "open_app", path: "", args: [], show_window: true };
  }

  function defaultIdForAction(type: string) {
    return type.replace(/^cycle_/, "cycle-").replace(/_/g, "-").replace("-audio-output", "-audio");
  }

  async function createFromAddItem(item: AddItem) {
    closeAddChooser();
    if (item.actionType === "media_picker_bundle") {
      await addMediaPickerBundle();
      return;
    }

    const binding: BindingSpec = {
      id: uniqueBindingId(defaultIdForAction(item.actionType)),
      enabled: true,
      hotkey: "",
      action: actionForType(item.actionType)
    };
    config.bindings = [...config.bindings, binding];
    touch();
    logEvent(`added ${item.name}`, "ok");
    await focusNewBinding(binding.id);
  }

  async function focusNewBinding(id: string) {
    highlightedBindingId = id;
    if (highlightTimer) clearTimeout(highlightTimer);
    highlightTimer = setTimeout(() => {
      highlightedBindingId = "";
    }, 1400);
    await tick();
    const escaped = window.CSS?.escape ? CSS.escape(id) : id.replace(/"/g, '\\"');
    const button = document.querySelector<HTMLButtonElement>(`[data-hotkey-id="${escaped}"]`);
    button?.scrollIntoView({ block: "center", behavior: "smooth" });
    setTimeout(() => button?.focus(), 180);
  }

  function addBinding() {
    void openAddChooser();
  }

  function removeMediaPickerGroup() {
    const removed = config.bindings.filter((binding) => isMediaAction(binding.action)).length;
    config.bindings = config.bindings.filter((binding) => !isMediaAction(binding.action));
    touch();
    logEvent(`removed media picker controls (${removed})`, "warn");
  }

  async function addMediaPickerBundle() {
    const definitions = mediaPickerBundleDefinitions();
    const existingActions = new Set(config.bindings.map((binding) => builtinName(binding.action)).filter(Boolean));
    const added: BindingSpec[] = [];

    for (const definition of definitions) {
      if (existingActions.has(definition.name)) continue;
      added.push({
        id: uniqueBindingId(definition.id),
        enabled: true,
        hotkey: definition.hotkey,
        action: { type: "builtin", name: definition.name }
      });
      existingActions.add(definition.name);
    }

    if (!added.length) {
      pushToast("Media picker controls already exist.", "info");
      return;
    }

    config.bindings = [...config.bindings, ...added];
    touch();
    pushToast(`Media picker controls created with ${added.length} hotkeys.`, "success");
    logEvent("created media picker controls", "ok");
    await focusNewBinding(added[0].id);
    maybeShowMediaPickerIntro();
  }

  function legacyAddBinding() {
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

  function builtinName(action: ActionSpec) {
    return action.type === "builtin" ? action.name : "";
  }

  function uniqueBindingId(base: string, skipIndex = -1) {
    const used = new Set(config.bindings.map((binding, index) => index === skipIndex ? "" : binding.id.trim().toLowerCase()).filter(Boolean));
    let candidate = base;
    let suffix = 2;
    while (used.has(candidate.toLowerCase())) {
      candidate = `${base}-${suffix}`;
      suffix += 1;
    }
    return candidate;
  }

  function mediaPickerBundleDefinitions() {
    return [
      { id: "media-picker", hotkey: "ctrl+alt+m", name: "media_picker_open" },
      { id: "media-next", hotkey: "media_next", name: "media_next_contextual" },
      { id: "media-previous", hotkey: "media_previous", name: "media_previous_contextual" },
      { id: "media-play-pause", hotkey: "media_play_pause", name: "media_play_pause_contextual" },
      { id: "media-picker-cancel", hotkey: "media_stop", name: "media_picker_cancel" }
    ] as const;
  }

  function createMediaPickerBundle(index: number) {
    const existingActions = new Set(config.bindings.map((binding, bindingIndex) => bindingIndex === index ? "" : builtinName(binding.action)).filter(Boolean));
    const definitions = mediaPickerBundleDefinitions();
    const current = config.bindings[index];
    if (!current) return;

    current.id = uniqueBindingId(definitions[0].id, index);
    if (!current.hotkey.trim()) current.hotkey = definitions[0].hotkey;
    current.enabled = true;
    current.action = { type: "builtin", name: definitions[0].name };

    const added: BindingSpec[] = [];
    for (const definition of definitions.slice(1)) {
      if (existingActions.has(definition.name)) continue;
      added.push({
        id: uniqueBindingId(definition.id),
        enabled: true,
        hotkey: definition.hotkey,
        action: { type: "builtin", name: definition.name }
      });
      existingActions.add(definition.name);
    }

    config.bindings.splice(index + 1, 0, ...added);
    touch();
    pushToast(`Media picker controls created${added.length ? ` with ${added.length + 1} hotkeys.` : "."}`, "success");
    logEvent("created media picker controls");
    maybeShowMediaPickerIntro();
  }

  function mediaLabel(action: ActionSpec) {
    if (action.type !== "builtin") return "";
    if (action.name === "media_picker_open") return "Open picker";
    if (action.name === "media_next_contextual") return "Next / picker next";
    if (action.name === "media_previous_contextual") return "Previous / picker previous";
    if (action.name === "media_play_pause_contextual") return "Play/pause / confirm";
    if (action.name === "media_picker_cancel") return "Cancel picker";
    if (action.name === "media_picker_next") return "Picker next";
    if (action.name === "media_picker_previous") return "Picker previous";
    if (action.name === "media_picker_confirm") return "Picker confirm";
    if (action.name === "media_selected_play_pause") return "Selected play/pause";
    if (action.name === "media_selected_next") return "Selected next";
    if (action.name === "media_selected_previous") return "Selected previous";
    if (action.name === "inspect_media_sessions") return "Inspect media";
    return "";
  }

  function isMediaAction(action: ActionSpec) {
    return action.type === "builtin" && !!mediaLabel(action);
  }

  function bindingSearchText(binding: BindingSpec) {
    return [
      binding.id,
      binding.hotkey,
      actionLabel(binding.action),
      mediaLabel(binding.action),
      actionMeta(binding)
    ].join(" ").toLowerCase();
  }

  function bindingMatchesSearch(binding: BindingSpec, query: string) {
    const needle = query.trim().toLowerCase();
    if (!needle) return true;
    return bindingSearchText(binding).includes(needle);
  }

  function setActionType(binding: BindingSpec, type: string, index: number) {
    if (type === "media_picker_bundle") {
      createMediaPickerBundle(index);
      return;
    }
    if (type === "cycle_audio_output") binding.action = { type: "builtin", name: "cycle_audio_output" };
    if (type === "cycle_microphone_input") binding.action = { type: "builtin", name: "cycle_microphone_input" };
    if (type === "inspect_focused_app_audio") binding.action = { type: "builtin", name: "inspect_focused_app_audio" };
    if (type === "cycle_focused_app_audio_output") binding.action = { type: "builtin", name: "cycle_focused_app_audio_output" };
    if (type === "cycle_focused_app_microphone_input") binding.action = { type: "builtin", name: "cycle_focused_app_microphone_input" };
    if (type === "cycle_discord_output_device") binding.action = { type: "builtin", name: "cycle_discord_output_device" };
    if (type === "cycle_discord_microphone_input") binding.action = { type: "builtin", name: "cycle_discord_microphone_input" };
    if (type === "media_picker_open") binding.action = { type: "builtin", name: "media_picker_open" };
    if (type === "media_picker_next") binding.action = { type: "builtin", name: "media_picker_next" };
    if (type === "media_picker_previous") binding.action = { type: "builtin", name: "media_picker_previous" };
    if (type === "media_picker_confirm") binding.action = { type: "builtin", name: "media_picker_confirm" };
    if (type === "media_picker_cancel") binding.action = { type: "builtin", name: "media_picker_cancel" };
    if (type === "media_selected_play_pause") binding.action = { type: "builtin", name: "media_selected_play_pause" };
    if (type === "media_selected_next") binding.action = { type: "builtin", name: "media_selected_next" };
    if (type === "media_selected_previous") binding.action = { type: "builtin", name: "media_selected_previous" };
    if (type === "media_next_contextual") binding.action = { type: "builtin", name: "media_next_contextual" };
    if (type === "media_previous_contextual") binding.action = { type: "builtin", name: "media_previous_contextual" };
    if (type === "media_play_pause_contextual") binding.action = { type: "builtin", name: "media_play_pause_contextual" };
    if (type === "inspect_media_sessions") binding.action = { type: "builtin", name: "inspect_media_sessions" };
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

  function setHotkeyMode(mode: "global" | "command") {
    config.settings.hotkey_mode = mode;
    touchSettings();
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
    if (event.key === "Escape") {
      recordingBindingId = "";
      return;
    }
    const parts: string[] = [];
    if (event.ctrlKey) parts.push("ctrl");
    if (event.altKey) parts.push("alt");
    if (event.shiftKey) parts.push("shift");
    if (event.metaKey) parts.push("win");

    const key = normalizeKey(event.key);
    if (!key) return;

    binding.hotkey = [...parts, key].join("+");
    recordingBindingId = "";
    touch();
  }

  function startHotkeyCapture(binding: BindingSpec) {
    recordingBindingId = binding.id;
  }

  function captureCommandHotkey(event: KeyboardEvent) {
    event.preventDefault();
    const parts: string[] = [];
    if (event.ctrlKey) parts.push("ctrl");
    if (event.altKey) parts.push("alt");
    if (event.shiftKey) parts.push("shift");
    if (event.metaKey) parts.push("win");

    const key = normalizeKey(event.key);
    if (!key) return;

    config.settings.command_hotkey = [...parts, key].join("+");
    touchSettings();
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
    if (action.name === "cycle_discord_microphone_input") return "Cycle Discord microphone input";
    if (action.name === "media_picker_open") return "Open media picker";
    if (action.name === "media_picker_next") return "Picker next target";
    if (action.name === "media_picker_previous") return "Picker previous target";
    if (action.name === "media_picker_confirm") return "Picker confirm target";
    if (action.name === "media_picker_cancel") return "Picker cancel";
    if (action.name === "media_selected_play_pause") return "Selected media play/pause";
    if (action.name === "media_selected_next") return "Selected media next";
    if (action.name === "media_selected_previous") return "Selected media previous";
    if (action.name === "media_next_contextual") return "Media next / picker next";
    if (action.name === "media_previous_contextual") return "Media previous / picker previous";
    if (action.name === "media_play_pause_contextual") return "Media play/pause / picker confirm";
    if (action.name === "inspect_media_sessions") return "Inspect media sessions";
    if (action.name === "close_focused_app") return "Close focused app";
    if (action.name === "kill_focused_app") return "Kill focused app";
    return action.name;
  }

  function clearSearch() {
    searchQuery = "";
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
    if (lower === "mediatracknext") return "media_next";
    if (lower === "mediatrackprevious") return "media_previous";
    if (lower === "mediaplaypause") return "media_play_pause";
    if (lower === "mediastop") return "media_stop";
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
      logEvent(`${action} failed`, "error", String(error));
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

  function daemonIssue(text: string) {
    const trimmed = text.trim();
    const lower = trimmed.toLowerCase();
    if (!trimmed || lower.includes("not checked")) return "";
    const firstLine = trimmed.split(/\r?\n/).map((line) => line.trim()).find(Boolean) ?? "";
    if (lower.includes("not responding")) return "daemon is not responding";
    if (lower.includes("not running") || lower.includes("stopped")) return "daemon stopped";
    if (lower.includes("error") || lower.includes("failed")) return firstLine || "daemon reported an error";
    return "";
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

<main data-theme={theme}>
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
        <button
          type="button"
          class:running={displayedDaemonView.state === "running"}
          class:stopped={displayedDaemonView.state === "stopped"}
          class:warning={displayedDaemonView.state === "warning"}
          class:checking={displayedDaemonView.state === "checking"}
          class="daemon-card"
          disabled={daemonStatusCheckingVisual || (!!busyAction && !isBusy("status"))}
          aria-label="Check daemon status"
          title="Check daemon status"
          on:click={refreshStatusFromCard}
        >
          <div class="daemon-top">
            <span class="status-dot"></span>
            <div>
              <strong>{displayedDaemonView.label}</strong>
              <p>{armedBindings} {armedBindings === 1 ? "binding" : "bindings"} armed</p>
            </div>
          </div>
          <p class="daemon-detail">{displayedDaemonView.detail}</p>
        </button>

        <div class="daemon-actions">
          <button class="ghost dark" disabled={!!busyAction} on:click={restartDaemon}>{isBusy("restart") ? "Restarting..." : "Restart"}</button>
          <button class:stop={daemonView.state !== "stopped"} class:primary-soft={daemonView.state === "stopped"} disabled={!!busyAction} on:click={toggleDaemonPower}>{daemonPowerText}</button>
        </div>
      </section>

      <nav class="side-nav" aria-label="Main sections">
        <button class:active={activePage === "bindings"} on:click={() => setPage("bindings")}>Bindings</button>
        <button class:active={activePage === "settings"} on:click={() => setPage("settings")}>Settings</button>
      </nav>

      <section class="log-panel">
        <div class="log-head">
          <h2>Live log</h2>
          <div class="log-actions">
            <button class:active={!simpleLog} class="log-mode" on:click={() => setSimpleLog(!simpleLog)}>
              {simpleLog ? "Simple" : "Detailed"}
            </button>
            <button class="log-clear" disabled={!eventLog.length} on:click={clearLog}>Clear</button>
          </div>
        </div>
        <div class="log-box">
          {#if eventLog.length}
            {#each eventLog as item}
              <div class={`log-entry ${item.level}`}>
                <span>{item.time}</span>
                <strong>{item.text}</strong>
                {#if !simpleLog && item.detail}
                  <pre>{item.detail}</pre>
                {/if}
              </div>
            {/each}
          {:else}
            <div>No activity yet.</div>
          {/if}
        </div>
      </section>

      <section class="config-panel">
        <h2>Config - v{config.version}</h2>
        <p>{configPath || "%APPDATA%\\HotkeyToCommand\\config.json"}</p>
        <div class="preset-box">
          <label>
            <span>Preset</span>
            <select
              value={activePreset}
              disabled={!!busyAction || presets.length === 0}
              on:change={(event) => switchPreset(event.currentTarget.value)}
            >
              {#if presets.length === 0}
                <option value="">Main config</option>
              {:else}
                {#each presets as preset}
                  <option value={preset.name}>{preset.name}</option>
                {/each}
              {/if}
            </select>
          </label>
          <div class="preset-actions">
            <button class="ghost" disabled={!!busyAction} on:click={createCleanPreset}>
              {isBusy("preset-create") ? "Creating..." : "New preset"}
            </button>
            <button class="danger" disabled={!!busyAction || !activePreset} on:click={requestDeletePreset}>
              {isBusy("preset-delete") ? "Deleting..." : "Delete"}
            </button>
          </div>
        </div>
        <label class="advanced">
          <input type="checkbox" checked={advancedVisible} on:change={toggleAdvanced} />
          Advanced mode
        </label>
      </section>

      <section class="theme-panel">
        <h2>Theme</h2>
        <div class="theme-grid">
          {#each themes as item}
            <button
              class:active={theme === item.name}
              class="theme-choice"
              aria-pressed={theme === item.name}
              on:click={() => setTheme(item.name)}
            >
              <span class="swatches">
                {#each item.colors as color}
                  <i style={`background: ${color}`}></i>
                {/each}
              </span>
              <span>{item.label}</span>
            </button>
          {/each}
        </div>
      </section>
    </aside>

    <section class="content">
      {#if activePage === "bindings"}
      <div class="toolbar">
        <div>
          <h2>Bindings</h2>
          <p>{config.bindings.length} {config.bindings.length === 1 ? "binding" : "bindings"} - {armedBindings} armed</p>
        </div>
        <div class="actions">
          <button class="ghost" disabled={!!busyAction} on:click={previewOnboarding}>Preview setup</button>
          <button class="primary" disabled={!canSave || !!busyAction} on:click={saveAndApply}>{isBusy("save") ? "Saving..." : "Save"}</button>
          <button class="add" disabled={!!busyAction} on:click={addBinding}>+ Add</button>
        </div>
      </div>

      <div class="search-row">
        <label class="search-box">
          <span>Search bindings</span>
          <input bind:value={searchQuery} placeholder="Name, hotkey, action..." />
        </label>
        <div class="search-count">
          {#if searchQuery.trim()}
            {visibleCount} shown
            <button class="clear-search" on:click={clearSearch}>Clear</button>
          {:else}
            Search by binding name or action
          {/if}
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
        {#if visibleMediaRows.length}
          <article class:highlight={highlightedBindingId === "media-picker"} class="media-group" on:pointerenter={setCardHoverDirection}>
            <div class="media-group-head">
              <div>
                <h3>Media picker controls</h3>
                <p>{visibleMediaRows.length} controls grouped to keep the binding list short.</p>
              </div>
              <div class="media-group-actions">
                <span>{visibleMediaRows.filter(({ binding }) => binding.enabled).length} on</span>
                <button class="danger group-delete" disabled={!!busyAction} on:click={() => pendingDelete = { type: "media-group", count: visibleMediaRows.length }}>Delete</button>
              </div>
            </div>

            <div class="media-control-list">
              {#each visibleMediaRows as { binding, index }}
                {@const duplicate = duplicateOwner(binding)}
                <div class:highlight={binding.id === highlightedBindingId} class:invalid={!!duplicate} class:disabled={!binding.enabled} class="media-control-row">
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
                    <button
                      data-hotkey-id={binding.id}
                      class:bad={!!duplicate}
                      class:listening={recordingBindingId === binding.id}
                      class="bind-button"
                      type="button"
                      on:click={() => startHotkeyCapture(binding)}
                      on:keydown={(event) => recordingBindingId === binding.id && captureHotkey(event, binding)}
                      on:blur={() => {
                        if (recordingBindingId === binding.id) recordingBindingId = "";
                      }}
                    >
                      {recordingBindingId === binding.id ? "Listening..." : "Bind"}
                    </button>
                  </div>

                  <div class="media-control-action">
                    <strong>{mediaLabel(binding.action)}</strong>
                    {#if duplicate}
                      <span>Duplicate of {binding.hotkey} already bound to {duplicate}.</span>
                    {:else}
                      <span>{binding.enabled ? "armed" : "off"}</span>
                    {/if}
                  </div>

                  <button class="danger square" disabled={!!busyAction} aria-label="Delete binding" title="Delete binding" on:click={() => pendingDelete = { type: "binding", index, name: binding.id || "binding" }}>Delete</button>
                </div>
              {/each}
            </div>
          </article>
        {/if}

        {#if visibleCount === 0}
          <div class="empty-results">
            <strong>No bindings found</strong>
            <p>Try a different name, hotkey, or action.</p>
          </div>
        {/if}

        {#each visibleRegularRows as { binding, index }}
          {@const duplicate = duplicateOwner(binding)}
          <article class:highlight={binding.id === highlightedBindingId} class:disabled={!binding.enabled} class:invalid={!!duplicate} on:pointerenter={setCardHoverDirection}>
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
                  <button
                    data-hotkey-id={binding.id}
                    class:bad={!!duplicate}
                    class:listening={recordingBindingId === binding.id}
                    class="bind-button"
                    type="button"
                    on:click={() => startHotkeyCapture(binding)}
                    on:keydown={(event) => recordingBindingId === binding.id && captureHotkey(event, binding)}
                    on:blur={() => {
                      if (recordingBindingId === binding.id) recordingBindingId = "";
                    }}
                  >
                    {recordingBindingId === binding.id ? "Listening..." : "Bind"}
                  </button>
                </div>
              </div>

              <div class="binding-action">
                <select class:advanced-pulse={advancedPulse} value={actionSelectValue(binding.action)} on:change={(event) => setActionType(binding, event.currentTarget.value, index)}>
                  <option value="open_app">Run application</option>
                  <option value="run_script">Run script</option>
                  <option value="cycle_audio_output">Cycle audio output</option>
                  <option value="cycle_microphone_input">Cycle microphone input</option>
                  <option value="cycle_focused_app_audio_output">Focused app output</option>
                  <option value="cycle_focused_app_microphone_input">Focused app microphone</option>
                  <option value="media_picker_bundle">Media picker controls</option>
                  <option value="close_focused_app">Close focused app</option>
                  <option value="kill_focused_app">Kill focused app</option>
                  {#if advancedVisible}
                    <option value="inspect_focused_app_audio">Inspect focused app audio</option>
                    <option value="inspect_media_sessions">Inspect media sessions</option>
                    <option value="media_picker_open">Open media picker</option>
                    <option value="media_next_contextual">Media next / picker next</option>
                    <option value="media_previous_contextual">Media previous / picker previous</option>
                    <option value="media_play_pause_contextual">Media play/pause / picker confirm</option>
                    <option value="media_picker_next">Picker next target</option>
                    <option value="media_picker_previous">Picker previous target</option>
                    <option value="media_picker_confirm">Picker confirm target</option>
                    <option value="media_picker_cancel">Picker cancel</option>
                    <option value="media_selected_play_pause">Selected media play/pause</option>
                    <option value="media_selected_next">Selected media next</option>
                    <option value="media_selected_previous">Selected media previous</option>
                    <option value="cycle_discord_output_device">Cycle Discord output</option>
                    <option value="cycle_discord_microphone_input">Cycle Discord microphone input</option>
                    <option value="run_command">Run command</option>
                  {/if}
                </select>

                <button class="danger square" disabled={!!busyAction} aria-label="Delete binding" title="Delete binding" on:click={() => pendingDelete = { type: "binding", index, name: binding.id || "binding" }}>Delete</button>
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
      {:else}
        <div class="toolbar settings-toolbar">
          <div>
            <h2>Settings</h2>
            <p>Control how the daemon listens for hotkeys.</p>
          </div>
          <div class="actions">
            <button class="ghost" disabled={!!busyAction} on:click={previewOnboarding}>Preview setup</button>
            <button class="primary" disabled={!canSave || !!busyAction} on:click={saveAndApply}>{isBusy("save") ? "Saving..." : "Save"}</button>
          </div>
        </div>

        <div class="settings-grid">
          <article class="settings-card wide" on:pointerenter={setCardHoverDirection}>
            <div class="settings-card-head">
              <div>
                <h3>Hotkey Listening</h3>
                <p>Choose whether bindings are always global or only active after a launcher hotkey.</p>
              </div>
              <span>{config.settings.hotkey_mode === "command" ? "Command mode" : "Global mode"}</span>
            </div>

            <div class="mode-options">
              <button class:active={config.settings.hotkey_mode === "global"} on:click={() => setHotkeyMode("global")}>
                <strong>
                  Global hotkeys
                  <span class="info-dot" aria-hidden="true">
                    i
                    <span class="tooltip">Use this when you want every shortcut to work instantly from anywhere. It is fastest, but it can steal keys from apps and games.</span>
                  </span>
                </strong>
                <span>Every enabled binding is registered all the time.</span>
              </button>
              <button class:active={config.settings.hotkey_mode === "command"} on:click={() => setHotkeyMode("command")}>
                <strong>
                  Command mode
                  <span class="info-dot" aria-hidden="true">
                    i
                    <span class="tooltip">Use this when shortcuts like F6, F9, or media keys conflict with other apps. Only the launcher key is always active; your other bindings wake up briefly after it.</span>
                  </span>
                </strong>
                <span>Only the launcher is global. Press it, then press the binding hotkey.</span>
              </button>
            </div>
          </article>

          <article class:muted={config.settings.hotkey_mode !== "command"} class="settings-card command-settings-card" on:pointerenter={setCardHoverDirection}>
            <div class="settings-card-head">
              <div>
                <h3>Command Mode</h3>
                <p>The launcher wakes the daemon for a short window, then it goes quiet again.</p>
              </div>
            </div>

            <div class="settings-fields">
              <label>
                <span>Launcher hotkey</span>
                <input
                  value={config.settings.command_hotkey}
                  placeholder="ctrl+alt+space"
                  on:keydown={captureCommandHotkey}
                  on:input={(event) => {
                    config.settings.command_hotkey = event.currentTarget.value;
                    touchSettings();
                  }}
                />
              </label>

              <label>
                <span>Listening window</span>
                <select
                  value={String(config.settings.command_timeout_ms)}
                  on:change={(event) => {
                    config.settings.command_timeout_ms = Number(event.currentTarget.value);
                    touchSettings();
                  }}
                >
                  <option value="2000">2 seconds</option>
                  <option value="4000">4 seconds</option>
                  <option value="6000">6 seconds</option>
                  <option value="10000">10 seconds</option>
                </select>
              </label>
            </div>

            <div class="settings-note">
              In command mode, a binding like F6 will not be captured until you press the launcher first.
            </div>
          </article>

          <article class="settings-card" on:pointerenter={setCardHoverDirection}>
            <div class="settings-card-head">
              <div>
                <h3>Windows Startup</h3>
                <p>Start the daemon in the background when you sign in to Windows.</p>
              </div>
              <span>{startupStatus.enabled ? "Enabled" : "Off"}</span>
            </div>

            <div class="bridge-status">
              <span class:online={startupStatus.enabled} class="bridge-dot"></span>
              <div>
                <strong>{startupStatus.enabled ? "Daemon starts with Windows" : "Daemon starts when the app opens"}</strong>
                <p>{startupStatus.enabled ? "Your hotkeys can work immediately after login." : "Open this app once after login to start listening for hotkeys."}</p>
              </div>
            </div>

            <label class="inline-check">
              <input
                type="checkbox"
                checked={startupStatus.enabled}
                disabled={!!busyAction}
                on:change={(event) => setDaemonStartup(event.currentTarget.checked)}
              />
              <span>Launch daemon at Windows sign-in</span>
            </label>

            <div class="settings-note">
              {startupStatus.detail}
            </div>
          </article>

          {#if advancedVisible}
            <article class="settings-card" on:pointerenter={setCardHoverDirection}>
              <div class="settings-card-head">
                <div>
                  <h3>Developer Tools</h3>
                  <p>Testing shortcuts for setup and onboarding.</p>
                </div>
                <span>Advanced</span>
              </div>
              <button class="ghost" on:click={resetOnboardingForDev}>Re-run onboarding</button>
            </article>
          {/if}

          <article class:checking={browserBridgeCheckingVisual} class:connected={browserBridge.connected} class:pulse={browserBridgePulse} class="settings-card bridge-card wide" on:pointerenter={setCardHoverDirection}>
            <div class="settings-card-head">
              <div>
                <h3>Browser Media Picker</h3>
                <p>Optional extension for controlling media inside browser tabs.</p>
              </div>
              <span>Optional</span>
            </div>

            <div class="bridge-status">
              <span class:online={browserBridge.connected} class="bridge-dot"></span>
              <div>
                {#if browserBridge.connected}
                  <strong>Connected</strong>
                  <p>{browserBridge.clients} {browserBridge.clients === 1 ? "browser" : "browsers"} connected, {browserBridge.targets} {browserBridge.targets === 1 ? "media tab" : "media tabs"} visible.</p>
                {:else}
                  <strong>{browserBridgeCheckingVisual ? "Checking connection" : "Not connected"}</strong>
                  <p>Without the extension, media tabs like YouTube, YouTube Music, Spotify, and similar sites are grouped under one browser entry, so per-tab control is limited.</p>
                {/if}
              </div>
            </div>

            {#if !browserBridge.connected}
              <div class="setup-steps">
                <div><span>1</span> Choose your browser and install the media picker extension.</div>
                <div><span>2</span> Refresh any browser tab that is playing media.</div>
                <div><span>3</span> Press Check to confirm it connected.</div>
              </div>
            {/if}

            <div class="bridge-actions">
              <button class:checking={browserBridgeCheckingVisual} class="ghost bridge-check" disabled={!!busyAction || browserBridgeCheckingVisual} on:click={() => refreshBrowserBridge()}>
                Check
              </button>
              <button class="primary-soft" disabled={!!busyAction} on:click={() => openExtensionsPage("brave")}>
                {isBusy("extensions-brave") ? "Opening..." : "Brave"}
              </button>
              <button class="ghost" disabled={!!busyAction} on:click={() => openExtensionsPage("chrome")}>
                {isBusy("extensions-chrome") ? "Opening..." : "Chrome"}
              </button>
              <button class="ghost" disabled={!!busyAction} on:click={() => openExtensionsPage("edge")}>
                {isBusy("extensions-edge") ? "Opening..." : "Edge"}
              </button>
            </div>

            <div class="settings-note">
              The media picker still works without the extension, but media tabs are grouped as one browser target instead of separate playable tabs.
            </div>
          </article>
        </div>
      {/if}
    </section>
  </section>

  {#if addChooserOpen}
    <div class="modal-backdrop" role="presentation" on:click={closeAddChooser}>
      <div class="add-modal" role="dialog" aria-modal="true" aria-label="Add a binding" tabindex="-1" on:click|stopPropagation on:keydown={addChooserKeydown}>
        <label class="search-box add-search">
          <span>Add a binding</span>
          <input bind:this={addSearchInput} bind:value={addQuery} placeholder="Search actions or presets..." />
        </label>

        <div class="add-list">
          {#if filteredAddItems.length}
            {#each filteredAddItems as item, itemIndex}
              {#if itemIndex === 0 || filteredAddItems[itemIndex - 1].category !== item.category}
                <div class="add-category">{item.category}</div>
              {/if}
              <button
                class:selected={itemIndex === addSelectedIndex}
                class="add-option"
                on:mouseenter={() => addSelectedIndex = itemIndex}
                on:click={() => createFromAddItem(item)}
              >
                <span class="add-icon">{item.icon}</span>
                <span>
                  <strong>{item.name}</strong>
                  <small>{item.description}</small>
                </span>
                {#if item.count}
                  <em>{item.count} bindings</em>
                {/if}
              </button>
            {/each}
          {:else}
            <div class="add-empty">
              <strong>No matches</strong>
              <p>Try audio, media, app, script, or close.</p>
            </div>
          {/if}
        </div>
      </div>
    </div>
  {/if}

  {#if onboardingOpen}
    <div class="modal-backdrop onboarding-backdrop" role="presentation">
      <div class="onboarding-modal" role="dialog" aria-modal="true" aria-label="HotkeyToCommand setup" tabindex="-1" on:keydown={(event) => {
        if (event.key === "Enter" && onboardingCanGoNext) void nextOnboardingStep();
        if (event.key === "Escape" && confirm("Skip setup? You can change everything later in Settings.")) void skipOnboarding();
      }}>
        <div class="onboarding-head">
          <span>Setup</span>
          <strong>{onboardingProgressLabel}</strong>
        </div>

        {#key onboardingAnimationKey}
        <div class={`onboarding-step-frame ${onboardingDirection}`}>
        {#if onboardingCurrent === "welcome"}
          <section class="onboarding-step">
            <h2>Turn any hotkey into an action</h2>
            <p>HotkeyToCommand listens for your shortcuts and runs the action you choose. This takes about a minute, and you can skip anytime and do it later in Settings.</p>
          </section>
        {:else if onboardingCurrent === "startup"}
          <section class="onboarding-step">
            <h2>Start with Windows</h2>
            <p>Start HotkeyToCommand automatically when you log in, so your hotkeys always work.</p>
            <label class="onboarding-toggle-row">
              <span class="switch" aria-label="Launch daemon at Windows sign-in">
                <input
                  type="checkbox"
                  checked={startupStatus.enabled}
                  disabled={!!busyAction}
                  on:change={(event) => setDaemonStartup(event.currentTarget.checked)}
                />
                <span></span>
              </span>
              <span class="onboarding-toggle-copy">
                <strong>Launch daemon at Windows sign-in</strong>
                <small>Recommended, so your hotkeys are ready right after login. You can change this later in Settings.</small>
              </span>
            </label>
          </section>
        {:else if onboardingCurrent === "mode"}
          <section class="onboarding-step">
            <h2>Choose how hotkeys listen</h2>
            <p>This changes how the whole app behaves.</p>
            <div class="onboarding-choice-grid">
              <button class:active={config.settings.hotkey_mode === "global"} on:click={() => setHotkeyMode("global")}>
                <strong>Global hotkeys</strong>
                <span>Key combos fire actions anytime.</span>
              </button>
              <button class:active={config.settings.hotkey_mode === "command"} on:click={() => setHotkeyMode("command")}>
                <strong>Command mode</strong>
                <span>Only the launcher is global. Press it, then press the binding hotkey.</span>
              </button>
            </div>
          </section>
        {:else if onboardingCurrent === "binding"}
          <section class="onboarding-step">
            <h2>Create your first binding</h2>
            <p>Pick a starter action, then press the hotkey you want to use.</p>
            {#if !onboardingBindingId}
              <div class="onboarding-choice-grid three">
                <button class:active={onboardingStarter === "cycle_audio_output"} on:click={() => onboardingStarter = "cycle_audio_output"}>
                  <strong>Cycle audio output</strong>
                  <span>Switch speakers/headphones with one hotkey.</span>
                </button>
                <button class:active={onboardingStarter === "media_picker_bundle"} on:click={() => onboardingStarter = "media_picker_bundle"}>
                  <strong>Media picker controls</strong>
                  <span>Add the full media control hotkey group.</span>
                </button>
                <button class:active={onboardingStarter === "open_app"} on:click={() => onboardingStarter = "open_app"}>
                  <strong>Run app / script</strong>
                  <span>Create a customizable launcher binding. You can pick the exact path after setup.</span>
                </button>
              </div>
              <button class="ghost onboarding-skip-inline" on:click={() => { onboardingStarter = ""; moveOnboardingStep(Math.min(onboardingStep + 1, onboardingSteps.length - 1), "forward"); }}>Skip - I'll add bindings later</button>
            {:else}
              <label class="onboarding-hotkey">
                <span>Press your hotkey</span>
                <input
                  value={onboardingTestBinding?.hotkey ?? ""}
                  placeholder="Press your hotkey..."
                  on:keydown={captureOnboardingHotkey}
                  on:input={(event) => {
                    if (onboardingTestBinding) {
                      onboardingTestBinding.hotkey = event.currentTarget.value;
                      touch();
                    }
                  }}
                />
              </label>
              <p class="onboarding-note">Created: {onboardingTestBinding?.id ?? "binding"}</p>
              {#if onboardingTestBinding && duplicateOwner(onboardingTestBinding)}
                <div class="inline-warning">Duplicate of {onboardingTestBinding.hotkey} already bound to {duplicateOwner(onboardingTestBinding)}.</div>
              {/if}
            {/if}
            <label class="inline-check onboarding-discord-opt">
              <input type="checkbox" bind:checked={onboardingDiscordOptIn} />
              <span>Show advanced Discord/Vencord bridge setup</span>
            </label>
          </section>
        {:else if onboardingCurrent === "test"}
          <section class="onboarding-step">
            <h2>Test it</h2>
            <p>
              {#if config.settings.hotkey_mode === "command"}
                Press <strong>{config.settings.command_hotkey}</strong>, then <strong>{onboardingTestBinding?.hotkey || "your hotkey"}</strong>. The daemon is listening.
              {:else}
                Press <strong>{onboardingTestBinding?.hotkey || "your hotkey"}</strong> now. The daemon is listening.
              {/if}
            </p>
            <div class={`test-state ${onboardingTestState}`}>
              {#if onboardingTestState === "success"}
                <strong>Got it - your hotkey works.</strong>
                <span>The daemon saw the shortcut and ran the binding.</span>
              {:else if onboardingTestState === "timeout"}
                <strong>Still waiting.</strong>
                <span>Try again, or skip the test and adjust the hotkey later.</span>
              {:else if onboardingTestState === "listening"}
                <strong>Listening...</strong>
                <span>Press the shortcut in this window or anywhere else.</span>
              {:else}
                <strong>Ready to test.</strong>
                <span>Press Next to arm and start listening for the hotkey.</span>
              {/if}
            </div>
            <button class="ghost onboarding-skip-inline" on:click={skipOnboardingTest}>Skip test</button>
          </section>
        {:else if onboardingCurrent === "browser"}
          <section class="onboarding-step">
            <h2>Optional browser extension</h2>
            <p>Without the extension, your browser shows up as one app. With it, individual YouTube, YouTube Music, Spotify, and similar tabs appear separately so you can control the exact one you want.</p>
            <div class="bridge-actions intro-actions">
              <button class="primary-soft" disabled={!!busyAction} on:click={() => openExtensionsPage("brave")}>Brave</button>
              <button class="ghost" disabled={!!busyAction} on:click={() => openExtensionsPage("chrome")}>Chrome</button>
              <button class="ghost" disabled={!!busyAction} on:click={() => openExtensionsPage("edge")}>Edge</button>
            </div>
          </section>
        {:else if onboardingCurrent === "discord"}
          <section class="onboarding-step">
            <h2>Advanced Discord bridge</h2>
            <p>Discord-specific audio and microphone switching needs the Vencord bridge. One-click setup is not in this app yet, so this stays optional and manual for now.</p>
            <div class="settings-note">TODO hook: connect this to a future Vencord bridge setup routine. If your bridge is already installed, Discord controls will work through the daemon bridge.</div>
          </section>
        {:else}
          <section class="onboarding-step">
            <h2>Ready</h2>
            <p>HotkeyToCommand is ready to use.</p>
            <div class="finish-list">
              <div><strong>Startup</strong><span>{startupStatus.enabled ? "Enabled" : "Off"}</span></div>
              <div><strong>Listening mode</strong><span>{config.settings.hotkey_mode === "global" ? "Global hotkeys" : "Command mode"}</span></div>
              <div><strong>Bindings</strong><span>{onboardingCreatedBindingIds.length ? `${onboardingCreatedBindingIds.length} created` : "Skipped for now"}</span></div>
            </div>
            <p class="onboarding-note">You can change startup later in Settings. Your config lives in %APPDATA%\HotkeyToCommand for backup or multi-PC setup.</p>
          </section>
        {/if}
        </div>
        {/key}

        <div class="onboarding-footer">
          <button class="ghost linkish" disabled={!!busyAction} on:click={() => skipOnboarding()}>Skip setup</button>
          <div class="onboarding-dots" aria-label={onboardingProgressLabel}>
            {#each onboardingSteps as _, stepIndex}
              <span class:active={stepIndex === onboardingStep}></span>
            {/each}
          </div>
          <div class="onboarding-nav">
            <button class="ghost" disabled={onboardingStep === 0 || !!busyAction} on:click={previousOnboardingStep}>Back</button>
            <button class="primary" disabled={!onboardingCanGoNext || !!busyAction} on:click={() => nextOnboardingStep()}>
              {onboardingPrimaryText()}
            </button>
          </div>
        </div>
      </div>
    </div>
  {/if}

  {#if pendingDelete}
    <div class="modal-backdrop" role="presentation" on:click={cancelPendingDelete}>
      <div class="confirm-modal" role="dialog" aria-modal="true" aria-label={deleteTitle(pendingDelete)} tabindex="-1" on:click|stopPropagation on:keydown={(event) => {
        if (event.key === "Escape") cancelPendingDelete();
        if (event.key === "Enter") void confirmPendingDelete();
      }}>
        <h2>{deleteTitle(pendingDelete)}</h2>
        <p>{deleteMessage(pendingDelete)}</p>
        <div class="confirm-actions">
          <button class="ghost" on:click={cancelPendingDelete}>Cancel</button>
          <button class="danger" disabled={!!busyAction} on:click={confirmPendingDelete}>
            {pendingDelete.type === "preset" && isBusy("preset-delete") ? "Deleting..." : deleteButtonText(pendingDelete)}
          </button>
        </div>
      </div>
    </div>
  {/if}

  {#if mediaPickerIntroOpen}
    <div class="modal-backdrop" role="presentation" on:click={closeMediaPickerIntro}>
      <div class="confirm-modal media-intro-modal" role="dialog" aria-modal="true" aria-label="Browser tab support for Media Picker" tabindex="-1" on:click|stopPropagation on:keydown={(event) => {
        if (event.key === "Escape") closeMediaPickerIntro();
      }}>
        <span class="intro-kicker">Optional upgrade</span>
        <h2>Show browser tabs in Media Picker</h2>
        <p>
          Media Picker works now, but without the extension, individual browser tabs playing media, like YouTube, YouTube Music, Spotify, and similar sites, show up as one browser target. That limits choosing and controlling a specific tab. Install the extension to show those tabs separately.
        </p>
        <div class="setup-steps intro-steps">
          <div><span>1</span> Install the extension in your browser.</div>
          <div><span>2</span> Refresh any tab that is playing media.</div>
          <div><span>3</span> Press Check in Settings when you want to confirm it connected.</div>
        </div>
        <div class="bridge-actions intro-actions">
          <button class="primary-soft" disabled={!!busyAction} on:click={() => openExtensionsPage("brave")}>
            {isBusy("extensions-brave") ? "Opening..." : "Brave"}
          </button>
          <button class="ghost" disabled={!!busyAction} on:click={() => openExtensionsPage("chrome")}>
            {isBusy("extensions-chrome") ? "Opening..." : "Chrome"}
          </button>
          <button class="ghost" disabled={!!busyAction} on:click={() => openExtensionsPage("edge")}>
            {isBusy("extensions-edge") ? "Opening..." : "Edge"}
          </button>
          <button class="ghost" on:click={closeMediaPickerIntro}>Later</button>
        </div>
      </div>
    </div>
  {/if}
</main>
