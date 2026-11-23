import {execFileSync} from "node:child_process";
import {cpSync, rmSync, writeFileSync} from "node:fs";
import {createRequire} from "node:module";
import {dirname, join} from "node:path";

import {build} from "esbuild";
import {defineConfig, type Options} from "tsup";

const tsc = join(dirname(createRequire(import.meta.url).resolve("typescript/package.json")), "bin", "tsc");

export default defineConfig(async (): Promise<Options[]> => {
    rmSync("dist", {recursive: true, force: true});
    const bootstrap = await build({
        entryPoints: ["src/preload/main-world.ts"],
        bundle: true,
        platform: "browser",
        target: "chrome128",
        format: "iife",
        write: false,
    });

    return [
        {
            entry: {
                "clovernt-main": "src/main/index.ts",
                "clovernt-preload": "src/preload/index.ts",
                "sdk/index": "src/sdk/index.ts",
            },
            define: {MAIN_WORLD_BOOTSTRAP: JSON.stringify(bootstrap.outputFiles[0].text)},
            format: ["cjs"],
            outExtension: () => ({js: ".js"}),
            target: "node22",
            platform: "node",
            external: ["electron"],
            clean: false,
            treeshake: true,
            onSuccess: async () => {
                execFileSync(process.execPath, [tsc, "-p", "tsconfig.sdk.json"], {stdio: "inherit"});
                cpSync("src/clovernt.gen.d.ts", "dist/sdk/clovernt.gen.d.ts");
                cpSync("assets", "dist/assets", {recursive: true});
                writeFileSync("dist/sdk/index.d.ts", 'export * from "./sdk/index";\n');
            },
        },
        {
            entry: {"clovernt-renderer": "src/renderer/index.ts"},
            format: ["iife"],
            outExtension: () => ({js: ".js"}),
            target: "chrome128",
            platform: "browser",
            loader: {".css": "text"},
            clean: false,
            treeshake: true,
        },
    ];
});
