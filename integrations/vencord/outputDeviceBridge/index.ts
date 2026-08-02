import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { OptionType } from "@utils/types";
import { findByPropsLazy, findStoreLazy } from "@webpack";

const log = new Logger("OutputDeviceBridge");

const MediaEngineStore = findStoreLazy("MediaEngineStore");
const AudioActions = findByPropsLazy("setOutputDevice", "setInputDevice");

const settings = definePluginSettings({
    port: {
        type: OptionType.NUMBER,
        description: "Localhost port your controller app's WebSocket server listens on",
        default: 8787,
        restartNeeded: true,
    },
    includeDefault: {
        type: OptionType.BOOLEAN,
        description: "Include the Default / Communications entries when cycling",
        default: false,
    },
});

function outputDevices(): Record<string, { name?: string; id?: string; }> {
    return MediaEngineStore.getOutputDevices?.() ?? {};
}

function inputDevices(): Record<string, { name?: string; id?: string; }> {
    return MediaEngineStore.getInputDevices?.() ?? {};
}

function orderedOutputIds(): string[] {
    let ids = Object.keys(outputDevices());
    if (!settings.store.includeDefault) {
        ids = ids.filter(id => id !== "default" && id !== "communications");
    }
    return ids.sort();
}

function orderedInputIds(): string[] {
    let ids = Object.keys(inputDevices());
    if (!settings.store.includeDefault) {
        ids = ids.filter(id => id !== "default" && id !== "communications");
    }
    return ids.sort();
}

function outputName(id: string): string {
    return outputDevices()[id]?.name ?? id;
}

function inputName(id: string): string {
    return inputDevices()[id]?.name ?? id;
}

function setOutput(id: string): string {
    if (AudioActions?.setOutputDevice) {
        AudioActions.setOutputDevice(id);
        return "action";
    }

    MediaEngineStore.getMediaEngine().setOutputDevice(id);
    return "engine";
}

function setInput(id: string): string {
    if (AudioActions?.setInputDevice) {
        AudioActions.setInputDevice(id);
        return "action";
    }

    MediaEngineStore.getMediaEngine().setInputDevice(id);
    return "engine";
}

function cycleOutput(dir: 1 | -1 = 1): string | null {
    const ids = orderedOutputIds();
    if (!ids.length) {
        log.warn("no output devices found");
        return null;
    }

    const current = MediaEngineStore.getOutputDeviceId?.();
    let index = ids.indexOf(current);
    if (index === -1) index = 0;

    const next = ids[(index + dir + ids.length) % ids.length];
    const method = setOutput(next);
    log.info(`cycled output ${dir > 0 ? "next" : "previous"} to ${outputName(next)} via ${method}`);
    return next;
}

function cycleInput(dir: 1 | -1 = 1): string | null {
    const ids = orderedInputIds();
    if (!ids.length) {
        log.warn("no input devices found");
        return null;
    }

    const current = MediaEngineStore.getInputDeviceId?.();
    let index = ids.indexOf(current);
    if (index === -1) index = 0;

    const next = ids[(index + dir + ids.length) % ids.length];
    const method = setInput(next);
    log.info(`cycled input ${dir > 0 ? "next" : "previous"} to ${inputName(next)} via ${method}`);
    return next;
}

function handle(raw: string): { id: string | null; name?: string; kind?: "input" | "output"; devices?: Array<{ id: string; name: string; kind: "input" | "output"; }>; } {
    let command = raw.trim();
    let id: string | undefined;

    try {
        const json = JSON.parse(raw);
        command = json.cmd ?? json.command ?? command;
        id = json.id;
    } catch {
        // Plain-text command.
    }

    let target: string | null = null;
    let kind: "input" | "output" = "output";
    switch (command) {
        case "cycle_output_device":
        case "next":
            target = cycleOutput(1);
            break;
        case "prev":
            target = cycleOutput(-1);
            break;
        case "cycle_input_device":
        case "next_input":
            kind = "input";
            target = cycleInput(1);
            break;
        case "prev_input":
            kind = "input";
            target = cycleInput(-1);
            break;
        case "set":
            if (id) {
                setOutput(id);
                target = id;
            }
            break;
        case "set_input":
            kind = "input";
            if (id) {
                setInput(id);
                target = id;
            }
            break;
        case "list":
            return {
                id: null,
                devices: orderedOutputIds().map(deviceId => ({ id: deviceId, name: outputName(deviceId), kind: "output" })),
            };
        case "list_input":
            return {
                id: null,
                kind: "input",
                devices: orderedInputIds().map(deviceId => ({ id: deviceId, name: inputName(deviceId), kind: "input" })),
            };
        default:
            log.warn("unknown command:", command);
    }

    return { id: target, kind, name: target ? (kind === "input" ? inputName(target) : outputName(target)) : undefined };
}

let ws: WebSocket | null = null;
let reconnect: number | null = null;
let stopped = false;

function scheduleReconnect() {
    if (stopped || reconnect != null) return;
    reconnect = window.setTimeout(() => {
        reconnect = null;
        connect();
    }, 3000);
}

function connect() {
    if (stopped) return;

    try {
        ws = new WebSocket(`ws://127.0.0.1:${settings.store.port}`);
        ws.addEventListener("open", () => log.info("connected to controller"));
        ws.addEventListener("message", event => {
            const result = handle(String(event.data));
            try {
                ws?.send(JSON.stringify({ event: "switched", ...result }));
            } catch {}
        });
        ws.addEventListener("close", scheduleReconnect);
        ws.addEventListener("error", () => ws?.close());
    } catch (error) {
        log.error("connect failed", error);
        scheduleReconnect();
    }
}

export default definePlugin({
    name: "OutputDeviceBridge",
    description: "Connects to KeyRail and lets it cycle/set Discord's output and input devices.",
    authors: [{ name: "Antonio", id: 0n }],
    settings,

    start() {
        stopped = false;
        connect();
    },

    stop() {
        stopped = true;
        if (reconnect != null) {
            clearTimeout(reconnect);
            reconnect = null;
        }
        ws?.close();
        ws = null;
    },
});
