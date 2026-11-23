import type {FontSettings, SystemFontFamily} from "./fonts";
import type {GuardSetRequest, GuardSetResult} from "./guard";
import type {
    CloverFontSettings,
    CloverPluginDependency,
    CloverPluginEntryKind,
    CloverPluginInfo,
    CloverPluginKind,
    CloverRuntimeInfo,
} from "./public";

export const Channel = {
    GetRuntimeInfo: "clovernt:ui:get-runtime-info",
    GetAboutAssets: "clovernt:ui:get-about-assets",
    GetPlugins: "clovernt:ui:get-plugins",
    PluginAction: "clovernt:ui:plugin-action",
    OpenTarget: "clovernt:ui:open-target",
    EnsureWindowSize: "clovernt:ui:ensure-window-size",
    GetGuard: "clovernt:ui:get-guard",
    SetGuard: "clovernt:ui:set-guard",
    GetFonts: "clovernt:ui:get-fonts",
    SetFonts: "clovernt:ui:set-fonts",
    ListSystemFonts: "clovernt:ui:list-system-fonts",
    PluginsChanged: "clovernt:ui:plugins-changed",
    GuardChanged: "clovernt:ui:guard-changed",
    FontsChanged: "clovernt:ui:fonts-changed",
    // Handled by the native runtime.
    Log: "clovernt:log",
    RendererEntries: "clovernt:renderer-entries",
} as const;

export type PluginKind = CloverPluginKind;

export type PluginEntryKind = CloverPluginEntryKind;

export type PluginDependency = CloverPluginDependency;

export type PluginItem = CloverPluginInfo;

export type RuntimeInfo = CloverRuntimeInfo;

export interface AboutAssets {
    glb: Uint8Array;
    luts: Uint8Array;
    fallbackPng: Uint8Array;
    wordmarkPng: Uint8Array;
}

export type PluginActionRequest =
    | { action: "load" | "unload" | "reload"; name: string }
    | { action: "rescan" };

export interface PluginActionResult {
    ok: boolean;
    error?: string;
    count: number;
}

export type OpenTargetRequest =
    | { kind: "coreDir" }
    | { kind: "pluginsDir" }
    | { kind: "plugin"; name: string }
    | { kind: "external"; url: string };

export interface RendererEntry {
    name: string;
    source: string;
}

export interface InvokeMap {
    [Channel.GetRuntimeInfo]: { args: []; result: RuntimeInfo };
    [Channel.GetAboutAssets]: { args: []; result: AboutAssets };
    [Channel.GetPlugins]: { args: []; result: PluginItem[] };
    [Channel.PluginAction]: { args: [PluginActionRequest]; result: PluginActionResult };
    [Channel.OpenTarget]: { args: [OpenTargetRequest]; result: string };
    [Channel.EnsureWindowSize]: { args: []; result: void };
    [Channel.GetGuard]: { args: []; result: number | null };
    [Channel.SetGuard]: { args: [GuardSetRequest]; result: GuardSetResult };
    [Channel.GetFonts]: { args: []; result: FontSettings };
    [Channel.SetFonts]: { args: [Partial<CloverFontSettings>]; result: FontSettings };
    [Channel.ListSystemFonts]: { args: []; result: SystemFontFamily[] };
    [Channel.RendererEntries]: { args: []; result: RendererEntry[] };
}

export type InvokeChannel = keyof InvokeMap;

export interface EventMap {
    [Channel.PluginsChanged]: [];
    [Channel.GuardChanged]: [lockedMask: number | null];
    [Channel.FontsChanged]: [fonts: FontSettings];
}
