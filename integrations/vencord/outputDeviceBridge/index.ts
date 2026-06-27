import { definePluginSettings } from "@api/Settings";
import { Logger } from "@utils/Logger";
import definePlugin, { OptionType } from "@utils/types";
import { findByPropsLazy, findStoreLazy } from "@webpack";

const log = new Logger("OutputDeviceBridge");

const MediaEngineStore = findStoreLazy("MediaEngineStore");
const AudioActions = findByPropsLazy("setOutputDevice");

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

function orderedOutputIds(): string[] {
    let ids = Object.keys(outputDevices());
    if (!settings.store.includeDefault) {
        ids = ids.filter(id => id !== "default" && id !== "communications");
    }
    return ids.sort();
}

function deviceName(id: string): string {
    return outputDevices()[id]?.name ?? id;
}

function setOutput(id: string): string {
    if (AudioActions?.setOutputDevice) {
        AudioActions.setOutputDevice(id);
        return "action";
    }

    MediaEngineStore.getMediaEngine().setOutputDevice(id);
    return "engine";
}

function cycle(dir: 1 | -1 = 1): string | null {
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
    log.info(`cycled ${dir > 0 ? "next" : "previous"} to ${deviceName(next)} via ${method}`);
    return next;
}

function handle(raw: string): { id: string | null; name?: string; devices?: Array<{ id: string; name: string; }>; } {
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
    switch (command) {
        case "cycle_output_device":
        case "next":
            target = cycle(1);
            break;
        case "prev":
            target = cycle(-1);
            break;
        case "set":
            if (id) {
                setOutput(id);
                target = id;
            }
            break;
        case "list":
            return {
                id: null,
                devices: orderedOutputIds().map(deviceId => ({ id: deviceId, name: deviceName(deviceId) })),
            };
        default:
            log.warn("unknown command:", command);
    }

    return { id: target, name: target ? deviceName(target) : undefined };
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
    description: "Connects to Hotkey To Command and lets it cycle/set Discord's output device.",
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
