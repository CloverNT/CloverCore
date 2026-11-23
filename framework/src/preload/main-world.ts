import {createFontsApi} from "../renderer/core/fonts-api";
import {createRuntimeApi} from "../renderer/core/runtime-api";
import {createUiApi} from "../renderer/core/ui-api";

const win = window as unknown as { __clovernt_ipc__?: object; clovernt?: object };
win.clovernt = {
    ...(win.__clovernt_ipc__ ?? win.clovernt),
    ui: createUiApi(),
    runtime: createRuntimeApi(),
    fonts: createFontsApi(),
};
