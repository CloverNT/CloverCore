import {mkdirSync} from "node:fs";
import {resolve} from "node:path";

import {shell} from "electron";

import type {OpenTargetRequest} from "../shared/ipc";
import type {CloverBinding} from "./binding";
import {isInside} from "./paths";

export async function openTarget(binding: CloverBinding, request: OpenTargetRequest): Promise<string> {
    switch (request.kind) {
        case "coreDir":
            return shell.openPath(binding.coreDir);
        case "pluginsDir":
            mkdirSync(binding.pluginsDir, {recursive: true});
            return shell.openPath(binding.pluginsDir);
        case "plugin": {
            const pluginsDir = resolve(binding.pluginsDir);
            const directory = resolve(pluginsDir, request.name);
            return isInside(pluginsDir, directory) ? shell.openPath(directory) : `无效的插件目录: ${request.name}`;
        }
        case "external": {
            const url = new URL(request.url);
            if (url.protocol !== "https:") {
                return "仅支持打开 HTTPS 外部链接";
            }
            await shell.openExternal(url.href);
            return "";
        }
    }
}
