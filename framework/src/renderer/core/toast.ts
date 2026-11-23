import {h, svg} from "./dom";
import {ICONS} from "./icons";

export type ToastKind = "success" | "error" | "info";

const ICON_BY_KIND: Record<ToastKind, string> = {
    success: ICONS.checkCircle,
    error: ICONS.alertCircle,
    info: ICONS.checkCircle,
};

let layer: HTMLElement | undefined;

export function showToast(message: string, kind: ToastKind = "info", durationMs = 2600): void {
    if (!layer?.isConnected) {
        layer = document.body.appendChild(h("div", "clovernt-toast-layer"));
    }
    const toast = h(
        "div",
        `clovernt-toast clovernt-toast--${kind}`,
        svg(ICON_BY_KIND[kind], "clovernt-toast__icon"),
        h("span", "clovernt-toast__text", message),
    );
    layer.appendChild(toast);
    requestAnimationFrame(() => toast.classList.add("is-visible"));
    setTimeout(() => {
        toast.classList.remove("is-visible");
        toast.addEventListener("transitionend", () => toast.remove(), {once: true});
    }, durationMs);
}
