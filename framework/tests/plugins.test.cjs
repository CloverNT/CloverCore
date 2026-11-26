const {after, before, test} = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const {tmpdir} = require("node:os");
const {join, resolve} = require("node:path");
const {buildSync} = require("esbuild");

// Small 1x1 test images. Compare the entire payload to catch MIME or binary encoding regressions.
const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mP4//8/AAX+Av4zEpUUAAAAAElFTkSuQmCC", "base64");
const jpeg = Buffer.from("/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAFAEBAAAAAAAAAAAAAAAAAAAAAP/EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAMAwEAAhEDEQA/AL+AD//Z", "base64");
const webp = Buffer.from("UklGRiQAAABXRUJQVlA4IBgAAAAwAQCdASoBAAEAAUAmJaQAA3AA/vz0AAA=", "base64");

let scratch;
let collectPlugins;
let runPluginAction;
before(() => {
    scratch = fs.mkdtempSync(join(tmpdir(), "clover-plugins-"));
    const bundle = join(scratch, "main.cjs");
    buildSync({
        stdin: {
            resolveDir: resolve(__dirname, "../src/main"),
            contents: 'export {collectPlugins, runPluginAction} from "./plugins";'
        },
        platform: "node", bundle: true, outfile: bundle, format: "cjs",
    });
    ({collectPlugins, runPluginAction} = require(bundle));
});
after(() => fs.rmSync(scratch, {recursive: true, force: true}));

/** A plugins directory with one JS plugin; `item(manifest)` rewrites its clover.json and collects it. */
function fixture() {
    const pluginsDir = fs.mkdtempSync(join(scratch, "plugins-"));
    const root = join(pluginsDir, "Greeting");
    fs.mkdirSync(root);
    const scan = [
        {name: "cloverjs:Greeting", type: "cloverjs", loaded: true, unloadable: true, builtin: false, passive: false},
        {name: "CloverCore", type: "core", loaded: true, unloadable: false, builtin: true, passive: false},
    ];
    const binding = {pluginsDir, pluginManager: {scan: () => scan}};
    const item = (extra = {}) => {
        fs.writeFileSync(join(root, "clover.json"), JSON.stringify({name: "Greeting", type: "js", entry: "main.js", ...extra}));
        const plugins = collectPlugins(binding);
        assert.equal(plugins.length, 1, "builtin plugins are not listed");
        return plugins[0];
    };
    return {root, item};
}

test("manifest fields the native scan lacks are merged into the plugin item", () => {
    const {item} = fixture();
    const plugin = item({
        rendererEntry: "renderer.js",
        dependencies: {Base: "*", Other: "^1.2.0"},
        optionalDependencies: {Extra: ">=2"},
    });
    assert.equal(plugin.id, "cloverjs:Greeting");
    assert.equal(plugin.name, "Greeting");
    assert.equal(plugin.kind, "js");
    assert.deepEqual(plugin.entries, ["main", "renderer"]);
    assert.deepEqual(plugin.dependencies, [
        {name: "Base", version: undefined, optional: false},
        {name: "Other", version: "^1.2.0", optional: false},
        {name: "Extra", version: ">=2", optional: true},
    ]);
});

test("a relative raster logo produces a MIME-matched data URL", () => {
    const {root, item} = fixture();
    fs.mkdirSync(join(root, "assets"));
    for (const [ext, mime, bytes] of [["PNG", "png", png], ["jpg", "jpeg", jpeg], ["jpeg", "jpeg", jpeg], ["webp", "webp", webp]]) {
        const name = "assets/icon." + ext;
        fs.writeFileSync(join(root, name), bytes);
        assert.equal(item({logo: "./" + name}).logoDataUrl, "data:image/" + mime + ";base64," + bytes.toString("base64"));
    }
});

test("missing, invalid, non-raster or oversized logos leave the plugin without one", () => {
    const {root, item} = fixture();
    fs.writeFileSync(join(root, "fake.png"), "<svg xmlns='http://www.w3.org/2000/svg'/>");
    fs.writeFileSync(join(root, "wrong.jpeg"), png);
    fs.writeFileSync(join(root, "too-large.png"), Buffer.concat([png, Buffer.alloc(256 * 1024 + 1 - png.length)]));
    fs.writeFileSync(join(root, "empty.png"), "");
    fs.writeFileSync(join(root, "forbidden.svg"), png);
    fs.mkdirSync(join(root, "directory.png"));
    for (const invalid of [undefined, null, "", {}, 3, "missing.png", "fake.png", "wrong.jpeg", "too-large.png", "empty.png", "directory.png", "forbidden.svg"]) {
        assert.equal(item({logo: invalid}).logoDataUrl, undefined, JSON.stringify(invalid));
    }
});

test("paths that leave the plugin directory are rejected before any file access", (t) => {
    const {root, item} = fixture();
    const realpath = t.mock.method(fs, "realpathSync");
    for (const invalid of ["../other/icon.png", "assets/../icon.png", "..\\other\\icon.png", join(root, "icon.png"), "/tmp/icon.png", "https://example.invalid/icon.png", "file:///icon.png", "data:image/png;base64,icon.png", "\\\\server\\share\\icon.png", "C:\\icon.png", "C:icon.png", "icon:stream.png"]) {
        assert.equal(item({logo: invalid}).logoDataUrl, undefined, invalid);
    }
    assert.equal(realpath.mock.callCount(), 0);
});

test("directory links cannot redirect logo reads", () => {
    const {root, item} = fixture();
    const other = fs.mkdtempSync(join(scratch, "sibling-"));
    fs.writeFileSync(join(other, "icon.png"), png);
    fs.symlinkSync(other, join(root, "redirect"), process.platform === "win32" ? "junction" : "dir");
    assert.equal(item({logo: "redirect/icon.png"}).logoDataUrl, undefined);
});

test("file symlinks cannot redirect logo reads", (t) => {
    const {root, item} = fixture();
    const other = join(scratch, "other.png");
    fs.writeFileSync(other, png);
    try {
        fs.symlinkSync(other, join(root, "icon.png"), "file");
    } catch (error) {
        if (process.platform === "win32" && error.code === "EPERM") return t.skip("File symlink creation requires Windows Developer Mode or privilege");
        throw error;
    }
    assert.equal(item({logo: "icon.png"}).logoDataUrl, undefined);
});

test("missing or malformed clover.json falls back to scan metadata without failing collectPlugins", () => {
    const pluginsDir = fs.mkdtempSync(join(scratch, "plugins-broken-"));
    const brokenRoot = join(pluginsDir, "Broken");
    fs.mkdirSync(brokenRoot);
    fs.writeFileSync(join(brokenRoot, "clover.json"), "{not-valid-json");

    const binding = {
        pluginsDir,
        pluginManager: {
            scan: () => [
                {
                    name: "cloverjs:Broken",
                    type: "cloverjs",
                    loaded: false,
                    unloadable: true,
                    builtin: false,
                    passive: false
                },
                {
                    name: "ext:NoDiskManifest",
                    type: "ext",
                    loaded: true,
                    unloadable: true,
                    builtin: false,
                    passive: false
                },
            ],
        },
    };

    const items = collectPlugins(binding);
    assert.equal(items.length, 2);
    assert.deepEqual(items.map((p) => ({id: p.id, entries: p.entries, dependencies: p.dependencies})), [
        {id: "ext:NoDiskManifest", entries: [], dependencies: []},
        {id: "cloverjs:Broken", entries: [], dependencies: []},
    ]);
});

test("runPluginAction validates action kind and plugin name", () => {
    const calls = [];
    const binding = {
        pluginsDir: scratch,
        pluginManager: {
            loadWithResult: (name) => (calls.push(["load", name]), {ok: true, error: "", count: 0}),
            unloadWithResult: (name) => (calls.push(["unload", name]), {ok: true, error: "", count: 0}),
            reloadWithResult: (name) => (calls.push(["reload", name]), {ok: true, error: "", count: 0}),
            rescanWithResult: () => (calls.push(["rescan"]), {ok: true, error: "", count: 2}),
        },
    };

    assert.deepEqual(runPluginAction(binding, {action: "rescan"}), {ok: true, error: "", count: 2});
    assert.deepEqual(runPluginAction(binding, {action: "reload", name: "cloverjs:Greeting"}), {
        ok: true,
        error: "",
        count: 0
    });
    assert.deepEqual(calls, [["rescan"], ["reload", "cloverjs:Greeting"]]);

    assert.throws(() => runPluginAction(binding, null), TypeError);
    assert.throws(() => runPluginAction(binding, {action: "__proto__", name: "x"}), TypeError);
    assert.throws(() => runPluginAction(binding, {action: "load", name: ""}), TypeError);
});
