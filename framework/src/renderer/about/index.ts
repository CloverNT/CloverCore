import {Channel, type RuntimeInfo} from "../../shared/ipc";
import {invoke} from "../core/bridge";
import {h, pressable, svg} from "../core/dom";
import {ICONS} from "../core/icons";
import {platformName} from "../core/platform";
import {PROJECT} from "../core/project";
import {showToast} from "../core/toast";
import {uiRegistry} from "../core/ui-registry";
import {HIDDEN_ATTRIBUTE} from "../settings/host";
import {loadBrandAssets} from "./brand-assets";
import {ModelViewer} from "./model/viewer";
import {PolarLight} from "./polar-light";

const MOUNTED_CLASS = "clovernt-about-mounted";
const DEVELOPER_CLICKS = 5;
const DEVELOPER_CLICK_WINDOW_MS = 2600;

function developerModeClicker(): () => void {
    let clicks = 0;
    let resetTimer = 0;
    return () => {
        const registry = uiRegistry();
        if (registry.isDeveloperMode) {
            showToast("你已启用开发者模式，无需点击此按钮", "info", 2000);
            return;
        }
        clearTimeout(resetTimer);
        resetTimer = window.setTimeout(() => (clicks = 0), DEVELOPER_CLICK_WINDOW_MS);
        clicks++;
        if (clicks >= DEVELOPER_CLICKS) {
            clicks = 0;
            registry.setDeveloperMode(true);
            showToast("已启用开发者模式！", "success", 2600);
        } else if (clicks >= 2) {
            showToast(`再点击 ${DEVELOPER_CLICKS - clicks} 次即可启用开发者模式`, "info", 1400);
        }
    };
}

function aboutRow(title: string, tail: Node | Node[]): HTMLElement {
    return h(
        "div",
        "clovernt-about__row",
        h("div", "clovernt-about__row-title", title),
        h("div", "clovernt-about__row-tail", ...[tail].flat()),
    );
}

function environmentGroup(info: RuntimeInfo): Node[] {
    const items = [
        ["Electron", info.versions.electron],
        ["Chromium", info.versions.chrome],
        ["Node.js", info.versions.node],
        ["V8", info.versions.v8],
        ["系统平台", `${platformName(info.platform)} ${info.arch} (${info.osRelease})`],
    ];
    const collapse = h(
        "div",
        "clovernt-about__env-collapse",
        h("div", "clovernt-about__env-inner", h(
            "div",
            "clovernt-about__env-list",
            ...items.map(([label, value]) =>
                h("div", "clovernt-about__env-item", h("span", "clovernt-about__env-label", label), h("span", "clovernt-about__env-val", value)),
            ),
        )),
    );
    const toggle = aboutRow("运行环境组件", [
        h("span", "clovernt-about__val", `Electron ${info.versions.electron} · Node ${info.versions.node}`),
        svg(ICONS.chevronRight, "clovernt-about__chevron"),
    ]);
    toggle.classList.add("is-clickable", "clovernt-about__env-toggle");
    toggle.setAttribute("aria-expanded", "false");
    pressable(toggle, "button", () => {
        const expanded = !toggle.classList.contains("is-expanded");
        toggle.classList.toggle("is-expanded", expanded);
        collapse.classList.toggle("is-expanded", expanded);
        toggle.setAttribute("aria-expanded", String(expanded));
    });
    return [toggle, collapse];
}

function footer(container: HTMLElement): HTMLElement {
    const agreement = container.querySelector<HTMLElement>(".agreement");
    const ntLogo = container.querySelector<HTMLElement>(".nt-logo");
    const ntLogoImage = ntLogo ? getComputedStyle(ntLogo).backgroundImage : "none";
    const logo = h("span", "nt-logo clovernt-about__nt-logo");
    logo.style.backgroundImage = ntLogoImage;
    return h(
        "div",
        "copyright clovernt-about__copyright",
        h(
            "div",
            "clovernt-about__copyright-line",
            h("span", {}, `Copyright © 1999-${new Date().getFullYear()} Tencent. All Rights Reserved.`),
            agreement && pressable(h("span", "agreement clovernt-about__agreement", "服务协议"), "link", () => agreement.click()),
        ),
        ntLogoImage === "none"
            ? h("div", "nt-flag clovernt-about__nt-flag", "基于 QQNT 技术架构")
            : h("div", "nt-flag clovernt-about__nt-flag", h("span", {}, "基于"), logo, h("span", {}, "技术架构")),
    );
}

/** One mount of the About page into QQ's about container. */
class AboutStage {
    readonly element: HTMLElement;
    private readonly polarLight: PolarLight;
    private model: ModelViewer | undefined;
    private disposed = false;

    constructor(readonly container: HTMLElement) {
        const qqVersion = container.querySelector(".version")?.textContent?.trim() ?? "";
        const polarCanvas = h("canvas", "clovernt-about__polar-canvas");
        const fallback = h("img", {class: "clovernt-about__fallback-img", alt: "QQ", draggable: false});
        const wordmark = h("img", {class: "clovernt-about__wordmark-img", alt: "QQ", draggable: false});
        const modelCanvas = h("canvas", "clovernt-about__model-canvas");
        const modelStage = h("div", "clovernt-about__model-stage");
        const card = h("div", "setting-item-group clovernt-about__card");

        this.element = h(
            "div",
            "clovernt-about",
            polarCanvas,
            h(
                "div",
                "q-scroll-view scroll-view--show-scrollbar clovernt-about__scroll",
                h(
                    "div",
                    "clovernt-about__content",
                    h(
                        "div",
                        "clovernt-about__main",
                        h(
                            "div",
                            "clovernt-about__hero",
                            h("div", "clovernt-about__model-wrap", fallback, h("div", "clovernt-about__model-canvas-wrap", modelCanvas), modelStage),
                            h("div", "clovernt-about__wordmark", wordmark),
                            h("div", "clovernt-about__version", qqVersion),
                        ),
                        card,
                    ),
                    footer(container),
                ),
            ),
        );
        this.polarLight = new PolarLight(polarCanvas);

        void invoke(Channel.GetRuntimeInfo).then((info) => {
            if (this.disposed) {
                return;
            }
            const coreVersion = h("button", {class: "clovernt-about__ver-btn", type: "button", onClick: developerModeClicker()}, info.version);
            card.append(
                aboutRow("CloverCore", coreVersion),
                aboutRow("CloverLoader", h("span", "clovernt-about__val", info.loaderVersion)),
                aboutRow("开源许可协议", h("span", "clovernt-about__val", PROJECT.license)),
                ...environmentGroup(info),
            );
        });
        void loadBrandAssets().then((assets) => {
            if (this.disposed) {
                return;
            }
            fallback.src = assets.fallbackUrl;
            wordmark.src = assets.wordmarkUrl;
            try {
                this.model = new ModelViewer(modelCanvas, modelStage, assets.model, assets.luts);
                fallback.classList.add("is-hidden");
                this.model.loop.resume();
            } catch (error) {
                console.warn("[CloverNT UI] 3D logo unavailable, showing the image instead:", error);
            }
        });
    }

    sync(): void {
        for (const child of this.container.children) {
            child.toggleAttribute(HIDDEN_ATTRIBUTE, child !== this.element);
        }
        this.container.classList.add(MOUNTED_CLASS);
        this.polarLight.loop.resume();
        this.model?.loop.resume();
    }

    dispose(): void {
        this.disposed = true;
        this.polarLight.loop.stop();
        this.model?.dispose();
        this.model = undefined;
        this.element.remove();
        for (const child of this.container.children) {
            child.removeAttribute(HIDDEN_ATTRIBUTE);
        }
        this.container.classList.remove(MOUNTED_CLASS);
    }
}

export class AboutPage {
    private stage: AboutStage | undefined;

    sync(): void {
        if (!uiRegistry().aboutEnabled) {
            this.stage?.dispose();
            this.stage = undefined;
            return;
        }
        const container = document.querySelector<HTMLElement>(".setting-main .about-container");
        if (!container) {
            this.stage?.dispose();
            this.stage = undefined;
            return;
        }
        if (this.stage?.container !== container) {
            this.stage?.dispose();
            this.stage = new AboutStage(container);
            container.append(this.stage.element);
        }
        this.stage.sync();
    }
}
