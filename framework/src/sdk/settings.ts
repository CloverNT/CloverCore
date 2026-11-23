import type {SettingsPageConfig} from "../shared/settings";
import {cloverntRenderer} from "./runtime";

export function registerSettingsPage(config: SettingsPageConfig): () => void {
    return cloverntRenderer.ui.registerSettingsPage(config);
}

export function unregisterSettingsPage(id: string): boolean {
    return cloverntRenderer.ui.unregisterSettingsPage(id);
}

export function openCloverSettings(target?: string): boolean {
    return cloverntRenderer.ui.openSettings(target);
}
