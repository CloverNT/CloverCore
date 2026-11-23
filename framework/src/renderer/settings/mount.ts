import type {CloverPanelHandle} from "../../shared/settings";
import {injectStyles} from "../stylesheet";
import {SettingsPanel} from "./panel";
import {routesFor, targetOf} from "./targets";

export function mountPanel(container: HTMLElement, target: string): CloverPanelHandle {
    injectStyles();
    const panel = new SettingsPanel(container);
    const listeners = new Set<() => void>();
    let disposed = false;
    panel.onChange = () => {
        for (const listener of [...listeners]) {
            try {
                listener();
            } catch (error) {
                console.error("[CloverNT UI] a panel listener failed:", error);
            }
        }
    };
    container.append(panel.element);
    panel.show(routesFor(target));
    return Object.freeze({
        get title() {
            return panel.header.title;
        },
        get canGoBack() {
            return panel.header.canGoBack;
        },
        get route() {
            return targetOf(panel.current);
        },
        back() {
            if (!disposed) {
                panel.back();
            }
        },
        navigate(next: string) {
            if (typeof next !== "string") {
                throw new TypeError("clovernt.ui.mount: navigate() takes a target string");
            }
            if (!disposed) {
                panel.show(routesFor(next));
                void panel.refresh();
            }
        },
        onChange(listener: () => void) {
            if (typeof listener !== "function") {
                throw new TypeError("clovernt.ui.mount: onChange() takes a function");
            }
            listeners.add(listener);
            return () => void listeners.delete(listener);
        },
        dispose() {
            if (!disposed) {
                disposed = true;
                listeners.clear();
                panel.dispose();
                panel.element.remove();
            }
        },
    });
}
