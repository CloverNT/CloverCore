import {BrowserWindow, screen, webContents, type WebContents} from "electron";

import type {EventMap} from "../shared/ipc";
import {isTrustedOrigin} from "../shared/trust";

const COMFORTABLE_WIDTH = 920;
const TARGET_WIDTH = 980;
const SCREEN_MARGIN = 12;

const resizedWindows = new WeakSet<BrowserWindow>();

export function ensureSettingsWindowWidth(sender: WebContents): void {
    const win = BrowserWindow.fromWebContents(sender);
    if (!win || resizedWindows.has(win) || win.isMaximized() || win.isFullScreen()) {
        return;
    }
    resizedWindows.add(win);

    const bounds = win.getBounds();
    const [minWidth] = win.getMinimumSize();
    if (bounds.width > Math.max(minWidth + 24, COMFORTABLE_WIDTH)) {
        return;
    }
    const {workArea} = screen.getDisplayMatching(bounds);
    const width = Math.min(TARGET_WIDTH, workArea.width - 40);
    if (width <= bounds.width) {
        return;
    }
    const centeredX = bounds.x - Math.round((width - bounds.width) / 2);
    const maxX = workArea.x + workArea.width - width - SCREEN_MARGIN;
    win.setBounds({...bounds, x: Math.max(workArea.x + SCREEN_MARGIN, Math.min(centeredX, maxX)), width});
}

export function broadcast<E extends keyof EventMap>(channel: E, ...args: EventMap[E]): void {
    for (const contents of webContents.getAllWebContents()) {
        if (!contents.isDestroyed() && isTrustedOrigin(contents.mainFrame?.origin)) {
            contents.send(channel, ...args);
        }
    }
}

export function broadcastToAll<E extends keyof EventMap>(channel: E, ...args: EventMap[E]): void {
    for (const contents of webContents.getAllWebContents()) {
        if (!contents.isDestroyed()) {
            contents.send(channel, ...args);
        }
    }
}
