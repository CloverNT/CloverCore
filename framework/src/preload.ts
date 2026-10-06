import {contextBridge, ipcRenderer, webFrame} from "electron";

import {LogLevel} from "./bridge";
import {makeLogger} from "./log";

const sink = (level: number, scope: string, message: string): void =>
    ipcRenderer.send("clovernt:log", level, scope, message);

const api = {
    log: (scope: string) => makeLogger(sink, scope),
    invoke: (channel: string, ...args: unknown[]) => ipcRenderer.invoke(channel, ...args),
    send: (channel: string, ...args: unknown[]) => ipcRenderer.send(channel, ...args),
    on: (channel: string, callback: (...args: unknown[]) => void) =>
        ipcRenderer.on(channel, (_event, ...args) => callback(...args)),
};

(globalThis as Record<string, unknown>).clovernt = api;

try {
    contextBridge.exposeInMainWorld("clovernt", api);
} catch (error) {
    sink(LogLevel.Warning, "clovernt-preload", `contextBridge unavailable: ${String(error)}`);
}

ipcRenderer
    .invoke("clovernt:renderer-entries")
    .then((value) => {
        const entries = (value ?? []) as Array<{ name: string; source: string }>;
        for (const entry of entries) {
            if (!entry.source) {
                continue;
            }
            webFrame
                .executeJavaScript(`(function(){\n${entry.source}\n})();`)
                .catch((error: unknown) =>
                    sink(LogLevel.Error, `clovernt:${entry.name}`, `renderer entry failed: ${String(error)}`),
                );
        }
    })
    .catch(() => {
        /* no renderer entries registered */
    });
