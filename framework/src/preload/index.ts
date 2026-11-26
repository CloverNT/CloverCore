import {contextBridge, ipcRenderer, webFrame, type IpcRendererEvent} from "electron";

import {errorMessage, unwrapError} from "../shared/errors";
import {fontCss, type FontSettings} from "../shared/fonts";
import {Channel, type RendererEntry} from "../shared/ipc";
import {LogLevel, makeLogger, type LogSink} from "../shared/log";
import {isTrustedOrigin} from "../shared/trust";

declare const MAIN_WORLD_BOOTSTRAP: string;

const sink: LogSink = (level, scope, message) => ipcRenderer.send(Channel.Log, level, scope, message);

const ALLOWED_CLOVER_INVOKE = new Set<string>([
    Channel.GetRuntimeInfo,
    Channel.GetAboutAssets,
    Channel.GetPlugins,
    Channel.PluginAction,
    Channel.OpenTarget,
    Channel.EnsureWindowSize,
    Channel.GetGuard,
    Channel.SetGuard,
    Channel.GetFonts,
    Channel.SetFonts,
    Channel.ListSystemFonts,
]);

const ALLOWED_CLOVER_EVENTS = new Set<string>([
    Channel.PluginsChanged,
    Channel.GuardChanged,
    Channel.FontsChanged,
]);

const BLOCKED_INTERNAL_CHANNEL = /^(?:ELECTRON_|electron[:.]|IPC_UP_|IPC_DOWN_|___!|nodeIKernel)/i;

function assertAllowedChannel(channel: unknown, mode: "invoke" | "send" | "on"): asserts channel is string {
    if (typeof channel !== "string" || channel.length === 0 || channel.length > 256 || /[\x00-\x1f]/.test(channel)) {
        throw new Error(`Invalid IPC channel: ${String(channel)}`);
    }
    if (BLOCKED_INTERNAL_CHANNEL.test(channel)) {
        throw new Error(`IPC channel is not allowed: ${channel}`);
    }
    if (channel.startsWith("clovernt:")) {
        const allowed = mode === "invoke"
            ? ALLOWED_CLOVER_INVOKE.has(channel)
            : mode === "on" && ALLOWED_CLOVER_EVENTS.has(channel);
        if (!allowed) {
            throw new Error(`IPC channel is not allowed: ${channel}`);
        }
    }
}

const api = {
    log: (scope: string) => makeLogger(sink, scope),
    invoke: (channel: string, ...args: unknown[]) => {
        assertAllowedChannel(channel, "invoke");
        return ipcRenderer.invoke(channel, ...args).catch((error: unknown) => {
            throw unwrapError(error);
        });
    },
    send: (channel: string, ...args: unknown[]) => {
        assertAllowedChannel(channel, "send");
        ipcRenderer.send(channel, ...args);
    },
    on(channel: string, listener: (...args: unknown[]) => void): () => void {
        assertAllowedChannel(channel, "on");
        if (typeof listener !== "function") {
            throw new TypeError("IPC listener must be a function");
        }
        const forward = (_event: IpcRendererEvent, ...args: unknown[]) => listener(...args);
        ipcRenderer.on(channel, forward);
        return () => void ipcRenderer.removeListener(channel, forward);
    },
};

const trusted = isTrustedOrigin(location.origin);
const isTopFrame = typeof process.isMainFrame === "boolean"
    ? process.isMainFrame
    : webFrame.top !== null && webFrame.top.routingId === webFrame.routingId;

if (trusted) {
    (globalThis as Record<string, unknown>).clovernt = api;
    if (isTopFrame && process.contextIsolated) {
        contextBridge.exposeInMainWorld("__clovernt_ipc__", api);
    }
}

let fontSheet: string | undefined;
let fontsBroadcast = false;

function applyFonts(fonts: FontSettings): void {
    if (fontSheet !== undefined) {
        webFrame.removeInsertedCSS(fontSheet);
    }
    const css = fontCss(fonts);
    fontSheet = css ? webFrame.insertCSS(css) : undefined;
}

ipcRenderer.on(Channel.FontsChanged, (_event, fonts: FontSettings) => {
    fontsBroadcast = true;
    applyFonts(fonts);
});
void ipcRenderer.invoke(Channel.GetFonts).then(
    (fonts: FontSettings) => {
        if (!fontsBroadcast) {
            applyFonts(fonts);
        }
    },
    (error: unknown) => sink(LogLevel.Error, "clovernt-preload", `loading fonts failed: ${errorMessage(error)}`),
);

if (trusted && isTopFrame) {
    const bootstrapReady = webFrame.executeJavaScript(MAIN_WORLD_BOOTSTRAP).catch((error: unknown) => {
        sink(LogLevel.Error, "clovernt-preload", `main-world bootstrap failed: ${errorMessage(error)}`);
    });

    void ipcRenderer.invoke(Channel.RendererEntries).then(
        async (entries: RendererEntry[]) => {
            await bootstrapReady;
            for (const {name, source} of entries) {
                await webFrame.executeJavaScript(`(function(){\n${source}\n})();`).catch((error: unknown) => {
                    sink(LogLevel.Error, `clovernt:${name}`, `renderer entry failed: ${errorMessage(error)}`);
                });
            }
        },
        (error: unknown) => sink(LogLevel.Error, "clovernt-preload", `loading renderer entries failed: ${errorMessage(error)}`),
    );
}
