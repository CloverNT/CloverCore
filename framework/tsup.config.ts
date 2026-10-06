import {defineConfig} from "tsup";

export default defineConfig({
    entry: {
        "clovernt-preload": "src/preload.ts",
        "sdk/index": "src/sdk.ts",
    },
    format: ["cjs"],
    outExtension: () => ({js: ".js"}),
    dts: {entry: {"sdk/index": "src/sdk.ts"}},
    target: "node22",
    platform: "node",
    external: ["electron"],
    clean: true,
    treeshake: true,
});
