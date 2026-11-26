const {after, before, test} = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const {tmpdir} = require("node:os");
const {join, resolve} = require("node:path");
const vm = require("node:vm");
const {buildSync} = require("esbuild");

let scratch;
let main;
let electron;
let preloadSource;
before(() => {
    scratch = fs.mkdtempSync(join(tmpdir(), "clover-trust-"));
    const fake = join(scratch, "node_modules", "electron", "index.js");
    fs.mkdirSync(join(scratch, "node_modules", "electron"), {recursive: true});
    fs.writeFileSync(fake, [
        "const handlers = new Map();",
        "const contents = [];",
        "const openedPaths = [];",
        "const openedUrls = [];",
        "module.exports = {",
        "    ipcMain: {handle: (channel, handler) => handlers.set(channel, handler)},",
        "    webContents: {getAllWebContents: () => contents},",
        "    shell: {",
        "        openPath: async (p) => (openedPaths.push(p), ''),",
        "        openExternal: async (u) => void openedUrls.push(u),",
        "    },",
        "    BrowserWindow: {}, screen: {},",
        "    handlers, contents, openedPaths, openedUrls,",
        "};",
    ].join("\n"));
    const bundle = join(scratch, "main.cjs");
    buildSync({
        stdin: {
            resolveDir: resolve(__dirname, "../src"),
            contents: 'export * from "./shared/trust"; export * from "./shared/errors"; export * from "./main/ipc"; export {broadcast, broadcastToAll} from "./main/windows"; export {openTarget} from "./main/shell";',
        },
        platform: "node", bundle: true, outfile: bundle, format: "cjs", external: ["electron"],
    });
    main = require(bundle);
    electron = require(fake);
    const preload = join(scratch, "preload.cjs");
    buildSync({
        entryPoints: [resolve(__dirname, "../src/preload/index.ts")],
        platform: "node", bundle: true, outfile: preload, format: "cjs", external: ["electron"],
        define: {MAIN_WORLD_BOOTSTRAP: JSON.stringify("bootstrap()")},
    });
    preloadSource = fs.readFileSync(preload, "utf8");
});
after(() => fs.rmSync(scratch, {recursive: true, force: true}));

const event = (origin) => ({senderFrame: origin === null ? null : {origin}});

test("only QQ's own pages are trusted", () => {
    assert.equal(main.TRUSTED_ORIGIN, "app://.");
    assert.ok(main.isTrustedOrigin("app://."));
    for (const origin of ["app://pagehost", "https://pd.qq.com", "null", "", undefined, null]) {
        assert.ok(!main.isTrustedOrigin(origin), String(origin));
    }
    assert.ok(main.isTrustedSender(event("app://.")));
    assert.ok(!main.isTrustedSender(event(null)), "a closed or navigated frame is not trusted");
});

test("handlers turn away other pages unless opened to every page", async () => {
    main.handle("clovernt:ui:get-plugins", () => "plugins");
    main.handle("clovernt:ui:get-fonts", () => "fonts", {anyOrigin: true});
    const plugins = electron.handlers.get("clovernt:ui:get-plugins");
    const fonts = electron.handlers.get("clovernt:ui:get-fonts");
    assert.equal(await plugins(event("app://.")), "plugins");
    for (const origin of ["app://pagehost", "https://pd.qq.com", null]) {
        assert.throws(() => plugins(event(origin)), /is not available to/, String(origin));
        assert.equal(await fonts(event(origin)), "fonts");
    }
});

const emptyChoice = (family = "") => ({family, regular: "", bold: "", features: {}, faces: []});

let bootstrapSource;
before(() => {
    const out = buildSync({
        entryPoints: [resolve(__dirname, "../src/preload/main-world.ts")],
        platform: "browser",
        target: "chrome128",
        bundle: true,
        format: "iife",
        write: false,
    });
    bootstrapSource = out.outputFiles[0].text;
});

async function runPreload(origin, {
    subframe = false,
    oopif = false,
    withIsMainFrame = true,
    slowBootstrap = false
} = {}) {
    const did = {exposed: [], invoked: [], sent: [], listened: [], inserted: [], executed: []};
    const frameId = subframe ? 42 : 7;
    const webFrame = {
        routingId: frameId,
        get top() {
            return oopif ? null : {routingId: 7};
        },
        insertCSS: (css) => (did.inserted.push(css), "sheet"),
        removeInsertedCSS() {
        },
        executeJavaScript: async (code) => {
            if (slowBootstrap && code === "bootstrap()") {
                await new Promise((done) => setImmediate(done));
                await new Promise((done) => setImmediate(done));
            }
            did.executed.push(code);
        },
    };
    const proc = {contextIsolated: true};
    if (withIsMainFrame) {
        proc.isMainFrame = !subframe;
    }
    const stub = {
        contextBridge: {exposeInMainWorld: (name) => did.exposed.push(name)},
        ipcRenderer: {
            invoke: async (channel) => {
                did.invoked.push(channel);
                if (channel === "clovernt:ui:get-fonts") return {ui: emptyChoice("Test Sans"), mono: emptyChoice()};
                if (channel === "clovernt:renderer-entries") return [{name: "clovernt-ui", source: "ui()"}];
                if (channel === "my-plugin:ping") return "pong";
                if (channel === "my-plugin:fail") {
                    throw new Error("Error invoking remote method 'my-plugin:fail': Error: Error: connection refused");
                }
                if (channel === "my-plugin:type-fail") {
                    throw new Error("Error invoking remote method 'my-plugin:type-fail': TypeError: invalid payload");
                }
                throw new Error("unexpected " + channel);
            },
            on: (channel) => did.listened.push(channel),
            send: (channel) => did.sent.push(channel),
            removeListener() {},
        },
        webFrame,
    };
    const module = {exports: {}};
    const page = vm.createContext({
        module, exports: module.exports, require: (id) => (id === "electron" ? stub : require(id)),
        location: {origin}, process: proc, console,
    });
    vm.runInContext(preloadSource, page);
    for (let i = 0; i < 8; i++) {
        await new Promise((done) => setImmediate(done));
    }
    return {did, page};
}

test("the preload gives other pages the fonts and nothing else", async () => {
    for (const withIsMainFrame of [true, false]) {
        const own = await runPreload("app://.", {slowBootstrap: true, withIsMainFrame});
        assert.equal(typeof own.page.clovernt?.invoke, "function");
        assert.deepEqual(own.did.exposed, ["__clovernt_ipc__"]);
        assert.equal(own.did.executed[0], "bootstrap()", "the bootstrap completes before the entries even when async");
        assert.ok(own.did.executed[1]?.includes("ui()"));
        assert.match(own.did.inserted[0] ?? "", /Test Sans/);

        const childFrame = await runPreload("app://.", {subframe: true, withIsMainFrame});
        assert.deepEqual(childFrame.did.exposed, []);
        assert.deepEqual(childFrame.did.executed, [], "subframes do not re-run main-world bootstrap or renderer entries");
    }

    const oopifChild = await runPreload("app://.", {subframe: true, oopif: true, withIsMainFrame: false});
    assert.deepEqual(oopifChild.did.executed, []);

    for (const origin of ["https://pd.qq.com", "app://pagehost", "null"]) {
        const {did, page} = await runPreload(origin);
        assert.equal(page.clovernt, undefined, origin);
        assert.deepEqual(did.exposed, [], origin);
        assert.deepEqual(did.executed, [], origin);
        assert.deepEqual(did.invoked, ["clovernt:ui:get-fonts"], origin);
        assert.deepEqual(did.listened, ["clovernt:ui:fonts-changed"], origin);
        assert.equal(did.inserted.length, 1, origin);
        assert.match(did.inserted[0], /Test Sans/, origin);
    }
});

test("main-world bootstrap succeeds when contextBridge defines non-configurable __clovernt_ipc__", () => {
    const win = vm.createContext({
        localStorage: {
            getItem: () => null, setItem() {
            }, removeItem() {
            }
        },
        HTMLElement: class HTMLElement {
        },
        CustomEvent: class CustomEvent {
        },
        dispatchEvent() {
        },
        addEventListener() {
        },
        console,
    });
    win.window = win;
    win.globalThis = win;
    // Electron's contextBridge.exposeInMainWorld defines window[key] as ReadOnly | DontDelete (configurable: false).
    Object.defineProperty(win, "__clovernt_ipc__", {
        value: {
            log: () => ({}), invoke: async () => ({}), send() {
            }, on: () => () => {
            }
        },
        writable: false,
        configurable: false,
        enumerable: true,
    });
    vm.runInContext(bootstrapSource, win);
    assert.equal(typeof win.clovernt?.invoke, "function");
    assert.equal(typeof win.clovernt?.ui?.registerSettingsPage, "function");
    assert.equal(typeof win.clovernt?.runtime?.info, "function");
    assert.equal(typeof win.clovernt?.fonts?.get, "function");
});

test("preload IPC bridge blocks internal Electron, QQNT, and native CloverNT channels", async () => {
    const {page, did} = await runPreload("app://.");
    const api = page.clovernt;
    for (const channel of [
        "", "clovernt:renderer-entries", "clovernt:log", "clovernt:ui:unknown",
        "IPC_UP_1", "IPC_DOWN_2", "ELECTRON_BROWSER_GET_BUILTIN", "electron:internal", "___!boot", "nodeIKernelMsg",
    ]) {
        assert.throws(() => api.invoke(channel), /IPC channel/, `invoke ${channel}`);
        assert.throws(() => api.send(channel), /IPC channel/, `send ${channel}`);
        assert.throws(() => api.on(channel, () => {
        }), /IPC channel/, `on ${channel}`);
    }
    assert.throws(() => api.send("clovernt:ui:get-plugins"), /IPC channel/, "clovernt:* cannot be sent directly");
    assert.throws(() => api.on("clovernt:ui:get-plugins", () => {
    }), /IPC channel/, "invoke-only clovernt:* cannot be listened to");
    assert.throws(() => api.on("my-plugin:event", null), {
        name: "TypeError",
        message: /IPC listener must be a function/
    });

    assert.equal(await api.invoke("my-plugin:ping"), "pong");
    api.send("my-plugin:notify", 1);
    assert.ok(did.sent.includes("my-plugin:notify"));
    const off = api.on("clovernt:ui:plugins-changed", () => {
    });
    off();
});

test("errors are unwrapped without nested Error: or Electron IPC prefixes", async () => {
    assert.equal(main.errorMessage(new Error("Error: TypeError: Error: boom")), "boom");
    assert.equal(main.errorMessage("Error invoking remote method 'clovernt:ui:set-guard': TypeError: bad mask"), "bad mask");
    assert.equal(main.errorMessage("Error:"), "Error:");

    const typed = main.unwrapError(new Error("Error invoking remote method 'x': TypeError: Error: bad input"));
    assert.ok(typed instanceof TypeError);
    assert.equal(typed.message, "bad input");

    const {page} = await runPreload("app://.");
    await assert.rejects(() => page.clovernt.invoke("my-plugin:fail"), {
        name: "Error",
        message: "connection refused",
    });
    await assert.rejects(() => page.clovernt.invoke("my-plugin:type-fail"), {
        name: "TypeError",
        message: "invalid payload",
    });
});

test("openTarget rejects malformed URLs, non-HTTPS URLs, traversal, files, and symlinks", async () => {
    const root = fs.mkdtempSync(join(scratch, "shell-"));
    const pluginsDir = join(root, "plugins");
    const outsideDir = join(root, "outside");
    const validPlugin = join(pluginsDir, "GoodPlugin");
    fs.mkdirSync(validPlugin, {recursive: true});
    fs.mkdirSync(outsideDir, {recursive: true});
    fs.writeFileSync(join(pluginsDir, "NotADir.txt"), "hi");
    try {
        fs.symlinkSync(outsideDir, join(pluginsDir, "EscapedLink"), "junction");
    } catch {
        // Skip symlink creation if environment forbids it.
    }
    const binding = {coreDir: root, pluginsDir};

    assert.match(await main.openTarget(binding, {kind: "external", url: "not a url"}), /无效的外部链接/);
    assert.match(await main.openTarget(binding, {kind: "external", url: "http://example.com"}), /HTTPS/);
    assert.equal(await main.openTarget(binding, {kind: "external", url: "https://example.com/a"}), "");
    assert.equal(electron.openedUrls.at(-1), "https://example.com/a");

    for (const bad of ["", ".", "..", "../outside", "a/b", "a\\b", "NotADir.txt", "Missing", "EscapedLink"]) {
        assert.match(await main.openTarget(binding, {kind: "plugin", name: bad}), /无效的插件目录/, bad);
    }
    assert.equal(await main.openTarget(binding, {kind: "plugin", name: "GoodPlugin"}), "");
    assert.equal(electron.openedPaths.at(-1), fs.realpathSync(validPlugin));
});

test("broadcasts reach QQ's pages, and font changes reach every page", () => {
    const page = (origin, destroyed = false) => ({sent: [], isDestroyed: () => destroyed, mainFrame: {origin}, send(...args) { this.sent.push(args); }});
    const own = page("app://.");
    const web = page("https://pd.qq.com");
    const closed = page("app://.", true);
    electron.contents.push(own, web, closed);
    main.broadcast("clovernt:ui:plugins-changed");
    main.broadcastToAll("clovernt:ui:fonts-changed", {ui: {}, mono: {}});
    assert.deepEqual(own.sent.map(([channel]) => channel), ["clovernt:ui:plugins-changed", "clovernt:ui:fonts-changed"]);
    assert.deepEqual(web.sent.map(([channel]) => channel), ["clovernt:ui:fonts-changed"]);
    assert.deepEqual(closed.sent, []);
});
