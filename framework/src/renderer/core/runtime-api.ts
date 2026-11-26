import {errorMessage} from "../../shared/errors";
import {Channel, type OpenTargetRequest, type PluginActionRequest} from "../../shared/ipc";
import type {CloverFolder, CloverPluginActionResult, CloverRuntimeApi} from "../../shared/public";
import {invoke, subscribe} from "./bridge";

function requireId(method: string, id: unknown): string {
    if (typeof id !== "string" || !id.trim()) {
        throw new TypeError(`clovernt.runtime.${method}: id must be a non-empty string`);
    }
    return id.trim();
}

function folderRequest(target: CloverFolder): OpenTargetRequest {
    if (target === "plugins") {
        return {kind: "pluginsDir"};
    }
    if (target === "core") {
        return {kind: "coreDir"};
    }
    const plugin = (target as { plugin?: unknown } | null)?.plugin;
    if (typeof plugin === "string" && plugin.trim()) {
        return {kind: "plugin", name: plugin.trim()};
    }
    throw new TypeError('clovernt.runtime.openFolder: target must be "plugins", "core" or { plugin: name }');
}

async function act(request: PluginActionRequest): Promise<CloverPluginActionResult & { count: number }> {
    const {ok, error, count} = await invoke(Channel.PluginAction, request);
    return error ? {ok, error: errorMessage(error), count} : {ok, count};
}

export function createRuntimeApi(): CloverRuntimeApi {
    const action = async (method: string, action: "load" | "unload" | "reload", id: unknown) => {
        const {ok, error} = await act({action, name: requireId(method, id)});
        return error ? {ok, error} : {ok};
    };
    return Object.freeze({
        info: () => invoke(Channel.GetRuntimeInfo),
        plugins: () => invoke(Channel.GetPlugins),
        loadPlugin: (id: string) => action("loadPlugin", "load", id),
        unloadPlugin: (id: string) => action("unloadPlugin", "unload", id),
        reloadPlugin: (id: string) => action("reloadPlugin", "reload", id),
        rescan: () => act({action: "rescan"}),
        async openFolder(target: CloverFolder) {
            const error = await invoke(Channel.OpenTarget, folderRequest(target));
            if (error) {
                throw new Error(errorMessage(error));
            }
        },
        onPluginsChange(listener: () => void) {
            if (typeof listener !== "function") {
                throw new TypeError("clovernt.runtime.onPluginsChange: listener must be a function");
            }
            return subscribe(Channel.PluginsChanged, () => listener());
        },
    });
}
