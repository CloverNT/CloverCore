import {contextBridge, ipcRenderer, webFrame, type IpcRendererEvent} from "electron";

import {fontCss, type FontSettings} from "../shared/fonts";
import {Channel, type RendererEntry} from "../shared/ipc";
import {LogLevel, makeLogger, type LogSink} from "../shared/log";
import {isTrustedOrigin} from "../shared/trust";

declare const MAIN_WORLD_BOOTSTRAP: string;

const sink: LogSink = (level, scope, message) => ipcRenderer.send(Channel.Log, level, scope, message);

const api = {
    log: (scope: string) => makeLogger(sink, scope),
    invoke: (channel: string, ...args: unknown[]) => ipcRenderer.invoke(channel, ...args),
    send: (channel: string, ...args: unknown[]) => ipcRenderer.send(channel, ...args),
    on(channel: string, listener: (...args: unknown[]) => void): () => void {
        const forward = (_event: IpcRendererEvent, ...args: unknown[]) => listener(...args);
        ipcRenderer.on(channel, forward);
        return () => void ipcRenderer.removeListener(channel, forward);
    },
};

const trusted = isTrustedOrigin(location.origin);

if (trusted) {
    (globalThis as Record<string, unknown>).clovernt = api;
    if (process.contextIsolated) {
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
    (error: unknown) => sink(LogLevel.Error, "clovernt-preload", `loading fonts failed: ${String(error)}`),
);

if (trusted) {
    webFrame.executeJavaScript(MAIN_WORLD_BOOTSTRAP).catch((error: unknown) => {
        sink(LogLevel.Error, "clovernt-preload", `main-world bootstrap failed: ${String(error)}`);
    });

    void ipcRenderer.invoke(Channel.RendererEntries).then(
        async (entries: RendererEntry[]) => {
            for (const {name, source} of entries) {
                await webFrame.executeJavaScript(`(function(){\n${source}\n})();`).catch((error: unknown) => {
                    sink(LogLevel.Error, `clovernt:${name}`, `renderer entry failed: ${String(error)}`);
                });
            }
        },
        (error: unknown) => sink(LogLevel.Error, "clovernt-preload", `loading renderer entries failed: ${String(error)}`),
    );
}
