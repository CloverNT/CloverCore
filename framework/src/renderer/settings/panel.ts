import {Channel} from "../../shared/ipc";
import {subscribe} from "../core/bridge";
import {h} from "../core/dom";
import {uiRegistry} from "../core/ui-registry";
import {SettingsStore} from "./store";
import type {Route, View, ViewContext} from "./views/context";
import {developerView} from "./views/developer";
import {fontsView} from "./views/fonts";
import {overviewView} from "./views/overview";
import {pageView} from "./views/page";
import {pluginsView} from "./views/plugins";

export interface PanelHeader {
    title: string;
    canGoBack: boolean;
}

interface Entry {
    readonly route: Route;
    view?: View;
    scrollTop: number;
}

const OVERVIEW: Route = {view: "overview"};
const TITLES = {overview: "CloverNT", plugins: "插件管理", developer: "开发者选项", fonts: "字体"} as const;

export class SettingsPanel implements ViewContext {
    readonly element = h("div", "clovernt-panel");
    readonly store = new SettingsStore();
    onChange: () => void = () => undefined;

    private stack: Entry[] = [];
    private readonly teardown: Array<() => void>;
    constructor(private readonly scroller: HTMLElement) {
        this.teardown = [
            this.store.subscribe(() => this.top?.view?.update()),
            uiRegistry().subscribe(() => this.onRegistryChange()),
            subscribe(Channel.PluginsChanged, () => void this.refresh()),
            subscribe(Channel.GuardChanged, (mask) => this.store.setGuardMask(mask)),
            subscribe(Channel.FontsChanged, (fonts) => this.store.setFonts(fonts)),
        ];
        void this.refresh();
    }

    get root(): Route {
        return this.stack[0]?.route ?? OVERVIEW;
    }

    get current(): Route {
        return this.top?.route ?? OVERVIEW;
    }

    get header(): PanelHeader {
        return {title: this.titleOf(this.top?.route ?? OVERVIEW), canGoBack: this.stack.length > 1};
    }

    refresh(): Promise<void> {
        return this.store.refresh().catch(() => undefined);
    }

    show(routes: Route[]): void {
        this.clear();
        this.stack = routes.filter((route) => this.isAvailable(route)).map((route) => ({route, scrollTop: 0}));
        if (this.stack.length === 0) {
            this.stack.push({route: OVERVIEW, scrollTop: 0});
        }
        this.render(0);
    }

    readonly push = (route: Route): void => {
        const current = this.top;
        if (current && this.isAvailable(route)) {
            current.scrollTop = this.scroller.scrollTop;
            this.stack.push({route, scrollTop: 0});
            this.render(0);
        }
    };

    back(): void {
        if (this.stack.length > 1) {
            this.stack.pop()!.view?.dispose?.();
            this.render(this.top!.scrollTop);
        }
    }

    dispose(): void {
        this.clear();
        for (const fn of this.teardown) {
            fn();
        }
    }

    private get top(): Entry | undefined {
        return this.stack.at(-1);
    }

    private titleOf(route: Route): string {
        return route.view === "page" ? (uiRegistry().page(route.id)?.title ?? "") : TITLES[route.view];
    }

    private isAvailable(route: Route): boolean {
        switch (route.view) {
            case "developer":
                return uiRegistry().isDeveloperMode;
            case "page":
                return uiRegistry().page(route.id) !== undefined;
            default:
                return true;
        }
    }

    private createView(route: Route): View {
        const view = this.viewFor(route);
        view.element.setAttribute("data-clovernt-page", route.view);
        if (route.view === "page") {
            view.element.setAttribute("data-clovernt-page-id", route.id);
        }
        return view;
    }

    private viewFor(route: Route): View {
        switch (route.view) {
            case "overview":
                return overviewView(this);
            case "plugins":
                return pluginsView(this, route.focus);
            case "developer":
                return developerView(this);
            case "fonts":
                return fontsView(this);
            case "page":
                return pageView(uiRegistry().page(route.id)!);
        }
    }

    private render(scrollTop: number): void {
        const entry = this.top!;
        if (entry.view) {
            entry.view.update();
        } else {
            entry.view = this.createView(entry.route);
        }
        this.element.replaceChildren(entry.view.element);
        this.scroller.scrollTop = scrollTop;
        this.onChange();
    }

    private clear(): void {
        for (const {view} of this.stack) {
            view?.dispose?.();
        }
        this.stack = [];
    }

    private onRegistryChange(): void {
        const root = this.stack[0]?.route;
        if (root?.view === "page" && uiRegistry().page(root.id)?.placement !== "sidebar") {
            this.show([OVERVIEW]);
            return;
        }
        const index = this.stack.findIndex(({route}) => !this.isAvailable(route));
        if (index > 0) {
            for (const {view} of this.stack.splice(index)) {
                view?.dispose?.();
            }
            this.render(this.top!.scrollTop);
        } else {
            this.top?.view?.update();
        }
    }
}
