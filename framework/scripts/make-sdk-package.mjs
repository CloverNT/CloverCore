#!/usr/bin/env node
import {cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync} from "node:fs";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";
import {parseArgs} from "node:util";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");

const {values: args} = parseArgs({
    options: {
        dist: {type: "string", default: join(root, "dist")},
        out: {type: "string", default: join(root, "sdk-package")},
        version: {type: "string", default: "0.0.0-local"},
        "core-version": {type: "string", default: ""},
        commit: {type: "string", default: ""},
        license: {type: "string", default: join(root, "..", "LICENSE")},
    },
});

const semver = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$/;
if (!semver.test(args.version)) {
    throw new Error(`--version "${args.version}" is not a valid semver version`);
}

const dist = resolve(args.dist);
const out = resolve(args.out);
const js = join(dist, "sdk", "index.js");
const dts = join(dist, "sdk", "index.d.ts");
for (const f of [js, dts, resolve(args.license)]) {
    if (!existsSync(f)) {
        throw new Error(`missing ${f} (run \`npm run build\` first?)`);
    }
}
if (/\bfrom\s+['"]\.{1,2}\//.test(readFileSync(dts, "utf8"))) {
    throw new Error(`${dts} still imports relative modules, it would be broken outside of the framework`);
}

rmSync(out, {recursive: true, force: true});
mkdirSync(out, {recursive: true});
cpSync(js, join(out, "index.js"));
cpSync(dts, join(out, "index.d.ts"));
cpSync(resolve(args.license), join(out, "LICENSE"));

const pkg = {
    name: "@clovernt/sdk",
    version: args.version,
    description: "Type definitions and runtime proxy for CloverNT JavaScript plugins (generated from CloverCore).",
    license: "SEE IN LICENSE",
    main: "index.js",
    types: "index.d.ts",
    files: ["index.js", "index.d.ts", "LICENSE"],
    repository: {type: "git", url: "git+https://github.com/CloverNT/CloverCore.git", directory: "framework"},
    peerDependencies: {electron: "*", "@types/node": "*"},
    peerDependenciesMeta: {electron: {optional: true}, "@types/node": {optional: true}},
    cloverCore: {version: args["core-version"] || undefined, commit: args.commit || undefined},
};
writeFileSync(join(out, "package.json"), JSON.stringify(pkg, null, 2) + "\n");

console.log(`@clovernt/sdk ${args.version} -> ${out}`);
