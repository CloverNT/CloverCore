export type CloverPluginKind = "native" | "js" | "external";

export type CloverPluginEntryKind = "main" | "preload" | "renderer";

export interface CloverPluginDependency {
    name: string;
    version?: string;
    optional: boolean;
}

export interface CloverPluginInfo {
    id: string;
    name: string;
    type: string;
    kind: CloverPluginKind;
    version?: string;
    author?: string;
    description?: string;
    passive: boolean;
    loaded: boolean;
    unloadable: boolean;
    logoDataUrl?: string;
    entries: CloverPluginEntryKind[];
    dependencies: CloverPluginDependency[];
}

export interface CloverRuntimeInfo {
    version: string;
    loaderVersion: string;
    qqVersion: string;
    coreDir: string;
    pluginsDir: string;
    platform: string;
    arch: string;
    osRelease: string;
    packetReady: boolean;
    versions: {
        electron: string;
        chrome: string;
        node: string;
        v8: string;
    };
}

export interface CloverPluginActionResult {
    ok: boolean;
    error?: string;
}

export interface CloverRescanResult extends CloverPluginActionResult {
    count: number;
}

export type CloverFolder = "plugins" | "core" | { plugin: string };

export interface CloverFontChoice {
    family: string;
    regular: string;
    bold: string;
    features: Record<string, boolean>;
}

export interface CloverFontSettings {
    ui: CloverFontChoice;
    mono: CloverFontChoice;
}

export type CloverFontKind = keyof CloverFontSettings;

export interface CloverInstalledFace {
    style: string;
    fullName: string;
    postscriptName: string;
    weight: number;
    italic: boolean;
    variable: boolean;
    features: string[];
}

export interface CloverInstalledFont {
    name: string;
    aliases: string[];
    faces: CloverInstalledFace[];
}

export interface CloverRuntimeApi {
    info(): Promise<CloverRuntimeInfo>;

    plugins(): Promise<CloverPluginInfo[]>;

    loadPlugin(id: string): Promise<CloverPluginActionResult>;

    unloadPlugin(id: string): Promise<CloverPluginActionResult>;

    reloadPlugin(id: string): Promise<CloverPluginActionResult>;

    rescan(): Promise<CloverRescanResult>;

    openFolder(target: CloverFolder): Promise<void>;

    onPluginsChange(listener: () => void): () => void;
}

export interface CloverFontsApi {
    get(): Promise<CloverFontSettings>;

    set(kind: CloverFontKind, choice: Pick<CloverFontChoice, "family"> & Partial<CloverFontChoice>): Promise<CloverFontSettings>;

    installed(): Promise<CloverInstalledFont[]>;

    onChange(listener: (fonts: CloverFontSettings) => void): () => void;
}
