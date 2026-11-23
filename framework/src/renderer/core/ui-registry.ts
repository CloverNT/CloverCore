import type {CloverPanelHandle, CloverSettingsHost, SettingsPageConfig} from "../../shared/settings";

const DEVELOPER_MODE_KEY = "clovernt:developer-mode";

export type PanelFactory = (container: HTMLElement, target: string) => CloverPanelHandle;

function notifyAll(listeners: Set<() => void>): void {
    for (const listener of [...listeners]) {
        try {
            listener();
        } catch (error) {
            console.error("[CloverNT UI] a settings listener failed:", error);
        }
    }
}

export class UiRegistry {
    private readonly pageMap = new Map<string, SettingsPageConfig>();
    private readonly listeners = new Set<() => void>();
    private readonly pageListeners = new Set<() => void>();
    private developerMode = localStorage.getItem(DEVELOPER_MODE_KEY) === "1";
    private opener: ((target: string) => void) | undefined;
    private pendingTarget: string | undefined;
    private customHost: CloverSettingsHost | undefined;
    private about = true;
    private panelFactory: PanelFactory | undefined;

    get isDeveloperMode(): boolean {
        return this.developerMode;
    }
    get host(): CloverSettingsHost | undefined {
        return this.customHost;
    }

    get aboutEnabled(): boolean {
        return this.about;
    }

    page(id: string): SettingsPageConfig | undefined {
        return this.pageMap.get(id);
    }

    pages(): SettingsPageConfig[] {
        return [...this.pageMap.values()].sort(
            (a, b) => (a.order ?? 0) - (b.order ?? 0) || a.title.localeCompare(b.title, "zh-CN"),
        );
    }

    panelPages(): SettingsPageConfig[] {
        return this.pages().filter((page) => page.placement !== "sidebar");
    }

    sidebarPages(): SettingsPageConfig[] {
        return this.pages().filter((page) => page.placement === "sidebar");
    }

    pagesForPlugin(pluginName: string): SettingsPageConfig[] {
        return this.pages().filter(
            (page) => page.plugin === pluginName || page.id === pluginName || page.id.startsWith(`${pluginName}:`),
        );
    }

    register(page: SettingsPageConfig): void {
        this.pageMap.set(page.id, page);
        notifyAll(this.pageListeners);
        this.notify();
    }

    unregister(id: string): boolean {
        const removed = this.pageMap.delete(id);
        if (removed) {
            notifyAll(this.pageListeners);
            this.notify();
        }
        return removed;
    }

    setDeveloperMode(enabled: boolean): void {
        this.developerMode = enabled;
        if (enabled) {
            localStorage.setItem(DEVELOPER_MODE_KEY, "1");
        } else {
            localStorage.removeItem(DEVELOPER_MODE_KEY);
        }
        this.notify();
    }

    open(target = "overview"): boolean {
        const opener = this.customHost ? (next: string) => this.customHost!.open(next) : this.opener;
        if (opener) {
            opener(target);
            return true;
        }
        this.pendingTarget = target;
        return false;
    }

    setHost(host: CloverSettingsHost | undefined, about: boolean): void {
        this.customHost = host;
        this.about = about;
        this.notify();
        if (host && this.pendingTarget !== undefined) {
            const target = this.pendingTarget;
            this.pendingTarget = undefined;
            host.open(target);
        }
    }

    setPanelFactory(factory: PanelFactory): void {
        this.panelFactory = factory;
    }

    mount(container: HTMLElement, target: string): CloverPanelHandle {
        if (!this.panelFactory) {
            throw new Error("clovernt.ui.mount: the CloverNT settings UI is not loaded in this page");
        }
        return this.panelFactory(container, target);
    }

    setOpener(opener: (target: string) => void): void {
        this.opener = opener;
        if (this.pendingTarget !== undefined) {
            opener(this.pendingTarget);
            this.pendingTarget = undefined;
        }
    }

    subscribe(listener: () => void): () => void {
        this.listeners.add(listener);
        return () => void this.listeners.delete(listener);
    }

    subscribePages(listener: () => void): () => void {
        this.pageListeners.add(listener);
        return () => void this.pageListeners.delete(listener);
    }

    private notify(): void {
        notifyAll(this.listeners);
    }
}

export function uiRegistry(): UiRegistry {
    const win = window as unknown as { __clovernt_ui__?: UiRegistry };
    return (win.__clovernt_ui__ ??= new UiRegistry());
}
