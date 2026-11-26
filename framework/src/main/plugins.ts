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

function dependencies(constraints: unknown, optional: boolean): PluginDependency[] {
    if (constraints === null || typeof constraints !== "object" || Array.isArray(constraints)) {
        return [];
    }
    return Object.entries(constraints as Record<string, unknown>).map(([name, version]) => ({
        name,
        version: typeof version === "string" && version.length > 0 && version !== "*" ? version : undefined,
        optional,
    }));
}

function readManifestSafe(directory: string): Manifest {
    try {
        const parsed: unknown = JSON.parse(readFileSync(join(directory, "clover.json"), "utf8"));
        if (parsed !== null && typeof parsed === "object" && !Array.isArray(parsed)) {
            return parsed as Manifest;
        }
    } catch {
        // Fall back to scan metadata when clover.json is missing or unreadable.
    }
    return {};
}

function toItem(scan: PluginScan, pluginsDir: string): PluginItem {
    const name = scan.name.slice(scan.name.indexOf(":") + 1);
    const directory = join(pluginsDir, name);
    const manifest = readManifestSafe(directory);
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
        entries: (Object.keys(ENTRY_FIELDS) as PluginEntryKind[]).filter((kind) => Boolean(manifest[ENTRY_FIELDS[kind]])),
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
    if (request === null || typeof request !== "object") {
        throw new TypeError("Invalid plugin action request");
    }
    const manager = binding.pluginManager;
    let result: PluginActionResult;
    switch (request.action) {
        case "rescan":
            result = manager.rescanWithResult();
            break;
        case "load":
        case "unload":
        case "reload": {
            if (typeof request.name !== "string" || request.name.length === 0) {
                throw new TypeError(`Invalid plugin name for action '${request.action}'`);
            }
            result =
                request.action === "load"
                    ? manager.loadWithResult(request.name)
                    : request.action === "unload"
                        ? manager.unloadWithResult(request.name)
                        : manager.reloadWithResult(request.name);
            break;
        }
        default:
            throw new TypeError(`Unsupported plugin action: ${String((request as { action?: unknown }).action)}`);
    }
    // Native result objects expose prototype getters, which IPC serialization would drop.
    return {ok: result.ok, error: result.error, count: result.count};
}
