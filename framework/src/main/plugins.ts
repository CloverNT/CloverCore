import {readFileSync} from "node:fs";
import {join} from "node:path";

import type {PluginScan} from "../clovernt.gen";
import type {
    PluginActionRequest,
    PluginActionResult,
    PluginDependency,
    PluginEntryKind,
    PluginItem,
    PluginKind,
} from "../shared/ipc";
import type {CloverBinding} from "./binding";
import {readPluginLogo} from "./plugin-logo";

interface Manifest {
    logo?: unknown;
    entry?: string;
    preloadEntry?: string;
    rendererEntry?: string;
    dependencies?: Record<string, string>;
    optionalDependencies?: Record<string, string>;
}

const ENTRY_FIELDS = {
    main: "entry",
    preload: "preloadEntry",
    renderer: "rendererEntry",
} as const satisfies Record<PluginEntryKind, keyof Manifest>;

function kindOf(type: string): PluginKind {
    return type === "clover" ? "native" : type === "cloverjs" ? "js" : "external";
}

function dependencies(constraints: Record<string, string> = {}, optional: boolean): PluginDependency[] {
    return Object.entries(constraints).map(([name, version]) => ({
        name,
        version: version && version !== "*" ? version : undefined,
        optional,
    }));
}

function toItem(scan: PluginScan, pluginsDir: string): PluginItem {
    const name = scan.name.slice(scan.name.indexOf(":") + 1);
    const directory = join(pluginsDir, name);
    const manifest = JSON.parse(readFileSync(join(directory, "clover.json"), "utf8")) as Manifest;
    return {
        id: scan.name,
        name,
        type: scan.type,
        kind: kindOf(scan.type),
        version: scan.version,
        author: scan.author,
        description: scan.description,
        passive: scan.passive,
        loaded: scan.loaded,
        unloadable: scan.unloadable,
        logoDataUrl: typeof manifest.logo === "string" ? readPluginLogo(directory, manifest.logo) : undefined,
        entries: (Object.keys(ENTRY_FIELDS) as PluginEntryKind[]).filter((kind) => manifest[ENTRY_FIELDS[kind]]),
        dependencies: [
            ...dependencies(manifest.dependencies, false),
            ...dependencies(manifest.optionalDependencies, true),
        ],
    };
}

export function collectPlugins(binding: CloverBinding): PluginItem[] {
    return binding.pluginManager
        .scan()
        .filter((scan) => !scan.builtin)
        .map((scan) => toItem(scan, binding.pluginsDir))
        .sort((a, b) => Number(b.loaded) - Number(a.loaded) || a.name.localeCompare(b.name, "zh-CN"));
}

export function runPluginAction(binding: CloverBinding, request: PluginActionRequest): PluginActionResult {
    const manager = binding.pluginManager;
    const result =
        request.action === "rescan"
            ? manager.rescanWithResult()
            : manager[`${request.action}WithResult`](request.name);
    // Native result objects expose prototype getters, which IPC serialization would drop.
    return {ok: result.ok, error: result.error, count: result.count};
}
