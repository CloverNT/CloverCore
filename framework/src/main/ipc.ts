import {ipcMain, type IpcMainInvokeEvent} from "electron";

import type {InvokeChannel, InvokeMap} from "../shared/ipc";
import {isTrustedOrigin} from "../shared/trust";

export type Handler<C extends InvokeChannel> = (
    event: IpcMainInvokeEvent,
    ...args: InvokeMap[C]["args"]
) => InvokeMap[C]["result"] | Promise<InvokeMap[C]["result"]>;

export function isTrustedSender(event: Pick<IpcMainInvokeEvent, "senderFrame">): boolean {
    return isTrustedOrigin(event.senderFrame?.origin);
}

export function handle<C extends InvokeChannel>(channel: C, handler: Handler<C>, options: { anyOrigin?: boolean } = {}): void {
    ipcMain.handle(channel, (event, ...args: unknown[]) => {
        if (!options.anyOrigin && !isTrustedSender(event)) {
            throw new Error(`${channel} is not available to ${event.senderFrame?.origin ?? "a closed frame"}`);
        }
        return (handler as (event: IpcMainInvokeEvent, ...args: unknown[]) => unknown)(event, ...args);
    });
}
