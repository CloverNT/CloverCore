import {readFileSync} from "node:fs";
import {join} from "node:path";

import type {AboutAssets} from "../shared/ipc";

let cached: AboutAssets | undefined;

export function readAboutAssets(): AboutAssets {
    const read = (name: string) => readFileSync(join(__dirname, "assets", name));
    return (cached ??= {
        glb: read("QQ.glb"),
        luts: read("filament_luts.bin"),
        fallbackPng: read("o6a.png"),
        wordmarkPng: read("o_y.png"),
    });
}
