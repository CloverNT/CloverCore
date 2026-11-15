import {execFileSync} from "node:child_process";
import {cpSync, writeFileSync} from "node:fs";
import {createRequire} from "node:module";
import {dirname, join} from "node:path";

import {defineConfig} from "tsup";

const tsc = join(dirname(createRequire(import.meta.url).resolve("typescript/package.json")), "bin", "tsc");

export default defineConfig({
    entry: {
        "clovernt-preload": "src/preload.ts",
        "sdk/index": "src/sdk.ts",
    },
    format: ["cjs"],
    outExtension: () => ({js: ".js"}),
    target: "node22",
    platform: "node",
    external: ["electron"],
    clean: true,
    treeshake: true,
    onSuccess: async () => {
        execFileSync(process.execPath, [tsc, "-p", "tsconfig.sdk.json"], {stdio: "inherit"});
        cpSync("src/clovernt.gen.d.ts", "dist/sdk/clovernt.gen.d.ts");
        writeFileSync("dist/sdk/index.d.ts", 'export * from "./sdk";\n');
    },
});
