import {AboutPage} from "./about";
import {uiRegistry} from "./core/ui-registry";
import {SettingsHost} from "./settings/host";
import {mountPanel} from "./settings/mount";
import {injectStyles} from "./stylesheet";

interface WindowType {
    windowName: string;
}

async function main(): Promise<void> {
    const registry = uiRegistry();
    registry.setPanelFactory(mountPanel);
    const ensureWindowType = (window as unknown as { waitForWindowTypeEnsure?: () => Promise<WindowType> })
        .waitForWindowTypeEnsure;
    if ((await ensureWindowType?.())?.windowName !== "SettingWindow") {
        return;
    }
    injectStyles();
    const about = new AboutPage();
    let scheduled = false;
    const schedule = (): void => {
        if (!scheduled) {
            scheduled = true;
            requestAnimationFrame(() => {
                scheduled = false;
                host.sync();
                about.sync();
            });
        }
    };
    const host = new SettingsHost(schedule);
    registry.subscribe(schedule);
    new MutationObserver(schedule).observe(document, {childList: true, subtree: true});
    addEventListener("hashchange", () => host.leave());
    schedule();
}

void main();
