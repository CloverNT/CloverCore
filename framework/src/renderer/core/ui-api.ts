import type {CloverUiApi, SettingsPageConfig} from "../../shared/settings";
import {uiRegistry} from "./ui-registry";

const PLACEMENTS: ReadonlyArray<unknown> = ["panel", "sidebar"];

function requireFunction(method: string, value: unknown): void {
    if (typeof value !== "function") {
        throw new TypeError(`clovernt.ui.${method}: listener must be a function`);
    }
}

function requireText(value: unknown, field: string): string {
    if (typeof value !== "string" || !value.trim()) {
        throw new TypeError(`clovernt.ui.registerSettingsPage: config.${field} must be a non-empty string`);
    }
    return value.trim();
}

export function createUiApi(): CloverUiApi {
    const registry = uiRegistry();
    return {
        registerSettingsPage(config: SettingsPageConfig) {
            if (typeof config?.render !== "function") {
                throw new TypeError("clovernt.ui.registerSettingsPage: config.render must be a function");
            }
            if (config.placement !== undefined && !PLACEMENTS.includes(config.placement)) {
                throw new TypeError('clovernt.ui.registerSettingsPage: config.placement must be "panel" or "sidebar"');
            }
            const page = {...config, id: requireText(config.id, "id"), title: requireText(config.title, "title")};
            registry.register(page);
            return () => void registry.unregister(page.id);
        },
        unregisterSettingsPage: (id) => registry.unregister(id),
        getSettingsPages: () => registry.pages(),
        onSettingsPagesChange(listener) {
            requireFunction("onSettingsPagesChange", listener);
            return registry.subscribePages(() => listener(registry.pages()));
        },
        isDeveloperMode: () => registry.isDeveloperMode,
        openSettings: (target) => registry.open(target),
        mount(container, options) {
            if (!(container instanceof HTMLElement)) {
                throw new TypeError("clovernt.ui.mount: container must be an element");
            }
            const route = options?.route ?? "overview";
            if (typeof route !== "string") {
                throw new TypeError("clovernt.ui.mount: options.route must be a string");
            }
            return registry.mount(container, route);
        },
        setHost(host, options) {
            if (host !== null && typeof host?.open !== "function") {
                throw new TypeError("clovernt.ui.setHost: host must be null or have an open(target) method");
            }
            const about = options?.about ?? true;
            if (typeof about !== "boolean") {
                throw new TypeError("clovernt.ui.setHost: options.about must be a boolean");
            }
            registry.setHost(host ?? undefined, about);
        },
    };
}
