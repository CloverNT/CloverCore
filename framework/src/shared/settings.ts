export type SettingsPageCleanup = () => void;

export type SettingsPageRenderResult = void | HTMLElement | SettingsPageCleanup;

export type SettingsPagePlacement = "panel" | "sidebar";

export interface SettingsPageConfig {
    id: string;
    title: string;
    plugin?: string;
    description?: string;
    icon?: string;
    order?: number;
    placement?: SettingsPagePlacement;

    render(container: HTMLElement): SettingsPageRenderResult | Promise<SettingsPageRenderResult>;
}

export interface CloverPanelHandle {
    readonly title: string;
    readonly canGoBack: boolean;
    readonly route: string;

    back(): void;

    navigate(target: string): void;

    onChange(listener: () => void): () => void;

    dispose(): void;
}

export interface CloverMountOptions {
    route?: string;
}

export interface CloverSettingsHost {
    open(target: string): void;
}

export interface CloverHostOptions {
    about?: boolean;
}

export interface CloverUiApi {
    registerSettingsPage(config: SettingsPageConfig): () => void;

    unregisterSettingsPage(id: string): boolean;

    getSettingsPages(): ReadonlyArray<SettingsPageConfig>;

    onSettingsPagesChange(listener: (pages: ReadonlyArray<SettingsPageConfig>) => void): () => void;

    isDeveloperMode(): boolean;

    mount(container: HTMLElement, options?: CloverMountOptions): CloverPanelHandle;

    setHost(host: CloverSettingsHost | null, options?: CloverHostOptions): void;

    openSettings(target?: string): boolean;
}
