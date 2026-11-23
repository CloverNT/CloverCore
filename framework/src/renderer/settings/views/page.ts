import {errorMessage} from "../../../shared/errors";
import type {SettingsPageCleanup, SettingsPageConfig} from "../../../shared/settings";
import {alert, tag} from "../../core/components";
import {h} from "../../core/dom";
import type {View} from "./context";

function renderError(config: SettingsPageConfig, error: unknown): HTMLElement {
    const detail = error instanceof Error ? (error.stack ?? error.message) : errorMessage(error);
    return alert({
        tone: "error",
        block: true,
        text: [
            h("div", {}, `渲染「${config.title}」设置页面时发生错误`),
            h("div", "clovernt-page-error__detail", detail),
            h("div", "clovernt-page-error__hint", "这是插件自身的错误，可以把上面的信息反馈给插件作者。"),
        ],
    });
}

export function pageView(config: SettingsPageConfig): View {
    const slot = h("div", "clovernt-slot");
    const element = h(
        "div",
        "clovernt-page",
        (config.description || config.plugin) && h(
            "div",
            "clovernt-pagehead",
            h("div", "clovernt-pagehead__desc", config.description ?? ""),
            config.plugin && h("span", "clovernt-pagehead__owner", "来自", tag(config.plugin, "grey")),
        ),
        slot,
    );
    let cleanup: SettingsPageCleanup | undefined;
    let disposed = false;
    new Promise<Awaited<ReturnType<SettingsPageConfig["render"]>>>((resolve) => resolve(config.render(slot))).then(
        (result) => {
            if (typeof result === "function") {
                if (disposed) {
                    result();
                } else {
                    cleanup = result;
                }
            } else if (result instanceof HTMLElement && !disposed) {
                slot.append(result);
            }
        },
        (error: unknown) => {
            if (!disposed) {
                slot.replaceWith(renderError(config, error));
            }
        },
    );
    return {
        element,
        update: () => undefined,
        dispose() {
            disposed = true;
            try {
                cleanup?.();
            } catch (error) {
                console.error("[CloverNT UI] settings page cleanup threw:", error);
            }
        },
    };
}
