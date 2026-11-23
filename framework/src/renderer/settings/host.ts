import {Channel} from "../../shared/ipc";
import type {SettingsPageConfig} from "../../shared/settings";
import {invoke} from "../core/bridge";
import {h, svg} from "../core/dom";
import {ICONS} from "../core/icons";
import {uiRegistry} from "../core/ui-registry";
import {SettingsPanel, type PanelHeader} from "./panel";
import {routesFor} from "./targets";

export const HIDDEN_ATTRIBUTE = "data-clovernt-hidden";

const QICON_COLOR_VARIABLE = "--340fd034";
const LEAVE_TIMEOUT_MS = 1000;

function adoptScope(native: Element | null | undefined, node: Element): void {
    for (const attribute of native?.attributes ?? []) {
        if (attribute.name.startsWith("data-v-")) {
            node.setAttribute(attribute.name, attribute.value);
        }
    }
}

function endsWith(parent: Element, nodes: Element[]): boolean {
    const offset = parent.children.length - nodes.length;
    return offset >= 0 && nodes.every((node, index) => parent.children[offset + index] === node);
}

interface NavItem {
    readonly page?: string;
    readonly element: HTMLElement;
    readonly icon: HTMLElement;
    readonly name: HTMLElement;
}

function navItem(icon: HTMLElement, title: string, onClick: () => void, page?: string): NavItem {
    icon.style.setProperty(QICON_COLOR_VARIABLE, "inherit");
    const name = h("div", "name", title);
    return {page, icon, name, element: h("div", {class: "nav-item clovernt-nav-item", onClick}, icon, name)};
}

function pageIcon(page: SettingsPageConfig): HTMLElement {
    const markup = page.icon?.trim() ?? "";
    if (markup.startsWith("<svg")) {
        return svg(markup, "q-svg-icon q-icon icon");
    }
    if (markup) {
        return h("span", "q-svg-icon q-icon icon clovernt-nav-item__text", markup.slice(0, 2));
    }
    return svg(ICONS.externalPlugin, "q-svg-icon q-icon icon");
}

export class SettingsHost {
    private active = false;
    private navBar: HTMLElement | undefined;
    private main: HTMLElement | undefined;
    private panel: SettingsPanel | undefined;
    private windowWidthEnsured = false;
    private sidebarKey = "";
    private pageItems: NavItem[] = [];
    private leaving: { href: string; page: Element | null; timer: number } | undefined;

    private readonly divider = h("div", {class: "clovernt-nav-divider", role: "separator"});
    private readonly cloverItem = navItem(svg(ICONS.navClover, "q-svg-icon q-icon icon"), "CloverNT", () => this.open("overview"));
    private readonly title = h("div", "setting-title clovernt-setting-title");
    private readonly scrollView = h("div", "q-scroll-view scroll-view--show-scrollbar clovernt-scroll-view");
    private readonly content = h("div", "setting-main__content", this.scrollView);
    private readonly wrapper = h("div", "wrapper layout-center clovernt-host-wrapper", this.content);

    constructor(private readonly onVisibilityChange: () => void) {
        const registry = uiRegistry();
        this.renderPageItems();
        registry.subscribe(() => this.renderPageItems());
        registry.setOpener((target) => this.open(target));
    }

    sync(): void {
        const navBar = document.querySelector<HTMLElement>(".setting-layout .setting-tab .nav-bar");
        const main = document.querySelector<HTMLElement>(".setting-layout .setting-main");
        if (!navBar || !main) {
            return;
        }
        if (uiRegistry().host) {
            this.detach(navBar, main);
            return;
        }
        if (navBar !== this.navBar) {
            this.navBar?.removeEventListener("click", this.onNativeNavClick, true);
            navBar.addEventListener("click", this.onNativeNavClick, true);
            this.navBar = navBar;
        }
        this.main = main;
        const navNodes = [this.divider, ...[this.cloverItem, ...this.pageItems].map((item) => item.element)];
        if (!endsWith(navBar, navNodes)) {
            this.attachNav(navBar, navNodes);
        }
        if (this.wrapper.parentElement !== main) {
            this.attachMain(main);
        }
        this.applyVisibility();
        if (this.leaving && (location.href !== this.leaving.href || this.leaving.page?.isConnected === false)) {
            this.deactivate();
        }
        if (!this.windowWidthEnsured) {
            this.windowWidthEnsured = true;
            void invoke(Channel.EnsureWindowSize);
        }
    }

    open(target: string): void {
        const panel = this.ensurePanel();
        panel.show(routesFor(target));
        void panel.refresh();
        this.stopLeaving();
        this.setActive(true);
    }

    deactivate(): void {
        this.stopLeaving();
        this.setActive(false);
    }

    leave(): void {
        if (this.active && !this.leaving) {
            this.leaving = {
                href: location.href,
                page: this.nativePage(),
                timer: window.setTimeout(() => this.deactivate(), LEAVE_TIMEOUT_MS),
            };
        }
    }

    private detach(navBar: HTMLElement, main: HTMLElement): void {
        this.stopLeaving();
        this.active = false;
        if (this.panel) {
            this.panel.dispose();
            this.panel.element.remove();
            this.panel = undefined;
        }
        for (const node of [this.divider, this.cloverItem.element, ...this.pageItems.map((item) => item.element), this.title, this.wrapper]) {
            node.remove();
        }
        navBar.removeAttribute("data-clovernt-active");
        for (const child of main.children) {
            child.removeAttribute(HIDDEN_ATTRIBUTE);
        }
    }

    private stopLeaving(): void {
        if (this.leaving) {
            clearTimeout(this.leaving.timer);
            this.leaving = undefined;
        }
    }

    private nativePage(): Element | null {
        return this.main?.querySelector(":scope > .wrapper:not(.clovernt-host-wrapper) .q-scroll-view > *") ?? null;
    }

    private readonly onNativeNavClick = (event: MouseEvent): void => {
        const item = (event.target as Element).closest(".nav-item");
        if (!item || item.classList.contains("clovernt-nav-item")) {
            return;
        }
        if (item.classList.contains("nav-item-active") && !this.leaving) {
            this.deactivate();
        } else {
            this.leave();
        }
    };

    private ensurePanel(): SettingsPanel {
        if (!this.panel) {
            const panel = new SettingsPanel(this.scrollView);
            panel.onChange = () => {
                this.renderTitle(panel.header);
                this.applyVisibility();
            };
            this.scrollView.append(panel.element);
            this.panel = panel;
        }
        return this.panel;
    }

    private renderPageItems(): void {
        const pages = uiRegistry().sidebarPages();
        const key = JSON.stringify(pages.map(({id, title, icon}) => [id, title, icon]));
        if (key === this.sidebarKey) {
            return;
        }
        this.sidebarKey = key;
        for (const item of this.pageItems) {
            item.element.remove();
        }
        this.pageItems = pages.map((page) => navItem(pageIcon(page), page.title, () => this.open(page.id), page.id));
        if (this.navBar) {
            this.sync();
        }
    }

    private setActive(active: boolean): void {
        if (this.active !== active) {
            this.active = active;
            this.applyVisibility();
            this.onVisibilityChange();
        }
    }

    private attachNav(navBar: HTMLElement, navNodes: HTMLElement[]): void {
        const nativeItem = navBar.querySelector(".nav-item:not(.clovernt-nav-item)");
        for (const item of [this.cloverItem, ...this.pageItems]) {
            adoptScope(nativeItem, item.element);
            adoptScope(nativeItem?.querySelector(".icon"), item.icon);
            adoptScope(nativeItem?.querySelector(".name"), item.name);
            item.element.classList.toggle("appearance", nativeItem?.classList.contains("appearance") ?? false);
        }
        navBar.append(...navNodes);
    }

    private attachMain(main: HTMLElement): void {
        const nativeWrapper = main.querySelector(":scope > .wrapper:not(.clovernt-host-wrapper)");
        const nativeContent = nativeWrapper?.querySelector(":scope > .setting-main__content");
        adoptScope(main.querySelector(":scope > .setting-title:not(.clovernt-setting-title)"), this.title);
        adoptScope(nativeWrapper ?? main, this.wrapper);
        adoptScope(nativeContent ?? main, this.content);
        adoptScope(nativeContent?.querySelector(":scope > .q-scroll-view") ?? main, this.scrollView);
        main.append(this.title, this.wrapper);
        this.renderTitle(this.panel?.header ?? {title: "CloverNT", canGoBack: false});
    }

    private renderTitle({title, canGoBack}: PanelHeader): void {
        const nativeTitle = this.main?.querySelector(":scope > .setting-title:not(.clovernt-setting-title)");
        const nativeIconArea = nativeTitle?.querySelector(".icon-area") ?? nativeTitle;
        const scoped = <E extends Element>(node: E): E => {
            adoptScope(nativeIconArea, node);
            return node;
        };
        this.title.classList.toggle("can-hover", canGoBack);
        if (canGoBack) {
            const backIcon = scoped(svg(ICONS.arrowLeft, "q-icon back-icon"));
            this.title.replaceChildren(scoped(h(
                "div",
                {class: "setting-title__clickable", onClick: () => this.panel?.back()},
                scoped(h("div", "icon-area", backIcon)),
                scoped(h("span", {}, title)),
            )));
        } else {
            this.title.replaceChildren(scoped(h("div", "icon-area")), ` ${title}`);
        }
    }

    private applyVisibility(): void {
        if (!this.navBar || !this.main) {
            return;
        }
        const root = this.panel?.root;
        const rootPage = root?.view === "page" ? root.id : undefined;
        this.navBar.toggleAttribute("data-clovernt-active", this.active);
        this.cloverItem.element.classList.toggle("nav-item-active", this.active && rootPage === undefined);
        for (const item of this.pageItems) {
            item.element.classList.toggle("nav-item-active", this.active && item.page === rootPage);
        }
        for (const child of this.main.children) {
            if (child === this.title || child === this.wrapper) {
                child.toggleAttribute(HIDDEN_ATTRIBUTE, !this.active);
            } else if (child.matches(".setting-title, .wrapper")) {
                child.toggleAttribute(HIDDEN_ATTRIBUTE, this.active);
            }
        }
    }
}
