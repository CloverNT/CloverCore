import about from "./about/about.css";
import components from "./core/styles/components.css";
import toast from "./core/styles/toast.css";
import tokens from "./core/styles/tokens.css";
import fonts from "./settings/styles/fonts.css";
import host from "./settings/styles/host.css";
import integration from "./settings/styles/integration.css";
import overview from "./settings/styles/overview.css";
import panel from "./settings/styles/panel.css";
import plugins from "./settings/styles/plugins.css";

const STYLE_ID = "clovernt-ui-styles";

export const css = [tokens, host, components, panel, overview, plugins, fonts, about, toast, integration].join("\n");

export function injectStyles(): void {
    if (!document.getElementById(STYLE_ID)) {
        document.head.append(Object.assign(document.createElement("style"), {id: STYLE_ID, textContent: css}));
    }
}
