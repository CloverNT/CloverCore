import {mkdirSync, realpathSync, statSync} from "node:fs";
import {resolve} from "node:path";

import {shell} from "electron";

import type {OpenTargetRequest} from "../shared/ipc";
import type {CloverBinding} from "./binding";
import {isInside} from "./paths";

function isSafePluginName(name: unknown): name is string {
    return (
        typeof name === "string"
        && name.trim().length > 0
        && name !== "."
        && name !== ".."
        && !/[\\/:\0]/.test(name)
    );
}

export async function openTarget(binding: CloverBinding, request: OpenTargetRequest): Promise<string> {
    if (typeof request !== "object" || request === null) {
        return "无效的目标请求";
    }
    switch (request.kind) {
        case "coreDir":
            return shell.openPath(binding.coreDir);
        case "pluginsDir":
            mkdirSync(binding.pluginsDir, {recursive: true});
            return shell.openPath(binding.pluginsDir);
        case "plugin": {
            if (!isSafePluginName(request.name)) {
                return `无效的插件目录: ${String(request.name ?? "")}`;
            }
            const pluginsDir = resolve(binding.pluginsDir);
            let realPluginsDir: string;
            try {
                realPluginsDir = realpathSync(pluginsDir);
            } catch {
                return `无效的插件目录: ${request.name}`;
            }
            const candidate = resolve(realPluginsDir, request.name);
            if (!isInside(realPluginsDir, candidate)) {
                return `无效的插件目录: ${request.name}`;
            }
            let realTarget: string;
            try {
                realTarget = realpathSync(candidate);
                if (!isInside(realPluginsDir, realTarget) || !statSync(realTarget).isDirectory()) {
                    return `无效的插件目录: ${request.name}`;
                }
            } catch {
                return `无效的插件目录: ${request.name}`;
            }
            return shell.openPath(realTarget);
        }
        case "external": {
            if (typeof request.url !== "string") {
                return "无效的外部链接";
            }
            let url: URL;
            try {
                url = new URL(request.url);
            } catch {
                return "无效的外部链接";
            }
            if (url.protocol !== "https:") {
                return "仅支持打开 HTTPS 外部链接";
            }
            await shell.openExternal(url.href);
            return "";
        }
        default:
            return "不支持的目标类型";
    }
}
