import {createFontsApi} from "../renderer/core/fonts-api";
import {createRuntimeApi} from "../renderer/core/runtime-api";
import {createUiApi} from "../renderer/core/ui-api";

const win = window as unknown as { __clovernt_ipc__?: object; clovernt?: object };
const ipc = win.__clovernt_ipc__ ?? win.clovernt;
Reflect.deleteProperty(win, "__clovernt_ipc__");
win.clovernt = {
    ...ipc,
    ui: createUiApi(),
    runtime: createRuntimeApi(),
    fonts: createFontsApi(),
};
