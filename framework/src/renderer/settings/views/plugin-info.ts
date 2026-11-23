import type {PluginItem, PluginKind} from "../../../shared/ipc";
import {avatar, type TagColor, type Tone} from "../../core/components";
import {ICONS} from "../../core/icons";
import type {SettingsStore} from "../store";

export const KIND_STYLE: Record<PluginKind, { icon: string; tag: TagColor }> = {
    native: {icon: ICONS.nativePlugin, tag: "blue"},
    js: {icon: ICONS.jsPlugin, tag: "grey"},
    external: {icon: ICONS.externalPlugin, tag: "purple"},
};

export function kindLabel(plugin: PluginItem): string {
    return plugin.kind === "native" ? "Native" : plugin.kind === "js" ? "JS" : plugin.type;
}

export function pluginAvatar(plugin: PluginItem): HTMLElement {
    return avatar(KIND_STYLE[plugin.kind].icon, plugin.kind, plugin.logoDataUrl);
}

export type PluginState = "on" | "off" | "failed";

export const PLUGIN_STATE: Record<PluginState, { label: string; tone?: Tone }> = {
    on: {label: "已启用", tone: "ok"},
    off: {label: "未启用"},
    failed: {label: "加载失败", tone: "error"},
};

export function pluginState(plugin: PluginItem, store: SettingsStore): PluginState {
    if (plugin.loaded) {
        return "on";
    }
    return store.pluginErrors.has(plugin.id) ? "failed" : "off";
}
