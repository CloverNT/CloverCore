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
    // Stands in for electron: records handlers and what each web contents was sent.
    const fake = join(scratch, "node_modules", "electron", "index.js");
    fs.mkdirSync(join(scratch, "node_modules", "electron"), {recursive: true});
    fs.writeFileSync(fake, [
        "const handlers = new Map();",
        "const contents = [];",
        "module.exports = {",
        "    ipcMain: {handle: (channel, handler) => handlers.set(channel, handler)},",
        "    webContents: {getAllWebContents: () => contents},",
        "    BrowserWindow: {}, screen: {},",
        "    handlers, contents,",
        "};",
    ].join("\n"));
    const bundle = join(scratch, "main.cjs");
    buildSync({
        stdin: {
            resolveDir: resolve(__dirname, "../src"),
            contents: 'export * from "./shared/trust"; export * from "./main/ipc"; export {broadcast, broadcastToAll} from "./main/windows";',
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

/** Runs the preload in a page of `origin` against a stand-in electron; resolves to what it did. */
async function runPreload(origin) {
    const did = {exposed: [], invoked: [], listened: [], inserted: [], executed: []};
    const stub = {
        contextBridge: {exposeInMainWorld: (name) => did.exposed.push(name)},
        ipcRenderer: {
            invoke: async (channel) => {
                did.invoked.push(channel);
                if (channel === "clovernt:ui:get-fonts") return {ui: emptyChoice("Test Sans"), mono: emptyChoice()};
                if (channel === "clovernt:renderer-entries") return [{name: "clovernt-ui", source: "ui()"}];
                throw new Error("unexpected " + channel);
            },
            on: (channel) => did.listened.push(channel),
            send() {},
            removeListener() {},
        },
        webFrame: {
            insertCSS: (css) => (did.inserted.push(css), "sheet"),
            removeInsertedCSS() {},
            executeJavaScript: async (code) => void did.executed.push(code),
        },
    };
    const module = {exports: {}};
    const page = vm.createContext({
        module, exports: module.exports, require: (id) => (id === "electron" ? stub : require(id)),
        location: {origin}, process: {contextIsolated: true}, console,
    });
    vm.runInContext(preloadSource, page);
    for (let i = 0; i < 5; i++) {
        await new Promise((done) => setImmediate(done));
    }
    return {did, page};
}

test("the preload gives other pages the fonts and nothing else", async () => {
    const own = await runPreload("app://.");
    assert.equal(typeof own.page.clovernt?.invoke, "function");
    assert.deepEqual(own.did.exposed, ["__clovernt_ipc__"]);
    assert.equal(own.did.executed[0], "bootstrap()", "the bootstrap runs before the entries");
    assert.ok(own.did.executed[1]?.includes("ui()"));
    assert.match(own.did.inserted[0] ?? "", /Test Sans/);
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
