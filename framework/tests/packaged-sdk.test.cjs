const {test} = require("node:test");
const assert = require("node:assert/strict");
const {execFileSync} = require("node:child_process");
const {mkdtempSync, mkdirSync, writeFileSync, rmSync} = require("node:fs");
const {join, resolve, dirname} = require("node:path");

test("published SDK resolves in a downstream plugin without exposing framework DTOs", (t) => {
    const root = resolve(__dirname, "..");
    const consumer = mkdtempSync(join(root, "node_modules", ".sdk-consumer-"));
    t.after(() => rmSync(consumer, {recursive: true, force: true}));
    const output = join(consumer, "node_modules", "@clovernt", "sdk");
    mkdirSync(dirname(output), {recursive: true});
    execFileSync(process.execPath, [join(root, "scripts", "make-sdk-package.mjs"), "--out", output]);

    const sdk = require(output);
    assert.deepEqual(Object.keys(sdk).sort(), [
        "EventPriority", "clovernt", "cloverntRenderer", "definePlugin", "openCloverSettings", "registerSettingsPage", "unregisterSettingsPage",
    ]);
    const setup = () => {
    };
    assert.equal(sdk.definePlugin(setup), setup);
    assert.equal(sdk.EventPriority.Monitor, 5);

    writeFileSync(join(consumer, "consumer.ts"), [
        'import {clovernt, definePlugin, EventPriority, registerSettingsPage, type CloverBuiltinEventMap, type CloverContext, type LoadedPlugin, type Packet, type SettingsPageConfig, type SettingsPagePlacement} from "@clovernt/sdk";',
        'import {cloverntRenderer, type CloverFontSettings, type CloverPanelHandle, type CloverPluginInfo, type CloverRuntimeInfo} from "@clovernt/sdk";',
        'const _checkBuiltinTopic: keyof CloverBuiltinEventMap = "clovernt:packet-send"; void _checkBuiltinTopic;',
        'declare module "@clovernt/sdk" {',
        '    interface CloverEventMap {',
        '        "demo:rpc": [req: string, reply: (ok: boolean) => number];',
        '        "demo:ping": void;',
        '    }',
        '}',
        'definePlugin((ctx: CloverContext) => {',
        '    ctx.logger.info(ctx.name);',
        '    ctx.events.once("ready", () => {}, EventPriority.Normal);',
        '    const offSend = ctx.events.on("clovernt:packet-send", (pkt, ev) => {',
        '        pkt.body = new Uint8Array([1, 2, 3]);',
        '        if (ev.canCancel) ev.cancel();',
        '    }, EventPriority.High);',
        '    ctx.events.off(offSend);',
        '    ctx.events.on("clovernt:dll-load", (info, ev) => {',
        '        const base: number | bigint = info.baseAddress; void base;',
        '        // @ts-expect-error clovernt:dll-load is non-cancellable.',
        '        ev.cancel();',
        '    });',
        '    ctx.events.on("clovernt:credential", (_cred, ev) => {',
        '        // @ts-expect-error clovernt:credential is non-cancellable.',
        '        ev.cancel();',
        '    });',
        '    ctx.events.monitor("clovernt:packet-send", (pkt, ev) => {',
        '        void pkt.command; void ev.cancelled;',
        '        // @ts-expect-error Monitor listeners receive readonly payloads.',
        '        pkt.body = new Uint8Array(0);',
        '        // @ts-expect-error Monitor listeners cannot cancel events.',
        '        ev.cancel();',
        '    });',
        '    ctx.events.on("demo:rpc", (req, reply, ev) => {',
        '        const n: number = reply(req.length > 0); void n; void ev.sourcePlugin;',
        '    });',
        '    const cancelledRpc: boolean = ctx.events.emit("demo:rpc", "hello", (ok) => (ok ? 1 : 0)); void cancelledRpc;',
        '    ctx.events.emit("demo:ping");',
        '    // @ts-expect-error Wrong argument types for typed CloverEventMap topic.',
        '    ctx.events.emit("demo:rpc", 123);',
        '    // @ts-expect-error Plugins cannot spoof clovernt:* kernel topics.',
        '    ctx.events.emit("clovernt:packet-send", {} as Packet);',
        '    // @ts-expect-error Plugins cannot emit arbitrary clovernt:* reserved namespace topics.',
        '    ctx.events.emit("clovernt:fake-kernel-event", 1);',
        '});',
        'const plugins: LoadedPlugin[] = clovernt.plugins; const queued: number = clovernt.rescan(); void plugins; void queued;',
        'const pg: [boolean, number, boolean, boolean, boolean] = [clovernt.packet.guardAvailable(), clovernt.packet.patchGuardState(), clovernt.packet.patchGuardActive(), clovernt.packet.patchGuardEnabledNext(), clovernt.packet.patchGuardRestartPending()]; void pg;',
        'const recv = clovernt.packet.receive("cmd", 100); const injected: boolean | undefined = recv?.injected; void injected;',
        'const page: SettingsPageConfig = {id: "p", title: "P", render: host => host};',
        'const unregister: () => void = registerSettingsPage(page); unregister();',
        'const placement: SettingsPagePlacement = "sidebar"; registerSettingsPage({...page, id: "s", placement})();',
        '// @ts-expect-error Pages are placed in the panel or the sidebar only.',
        'registerSettingsPage({...page, placement: "toolbar"});',
        'const panel: CloverPanelHandle = cloverntRenderer.ui.mount(document.body, {route: "plugins"}); panel.onChange(() => void panel.title)(); panel.navigate("fonts"); panel.dispose();',
        'cloverntRenderer.ui.setHost({open: (target: string) => void target}, {about: false}); cloverntRenderer.ui.setHost(null);',
        'const offPages: () => void = cloverntRenderer.ui.onSettingsPagesChange((pages) => void pages.length); offPages(); const dev: boolean = cloverntRenderer.ui.isDeveloperMode(); void dev;',
        'const listed: Promise<CloverPluginInfo[]> = cloverntRenderer.runtime.plugins(); const runtimeInfo: Promise<CloverRuntimeInfo> = cloverntRenderer.runtime.info(); void listed; void runtimeInfo;',
        'void cloverntRenderer.runtime.reloadPlugin("cloverjs:Greeting"); void cloverntRenderer.runtime.openFolder({plugin: "Greeting"});',
        'const chosen: Promise<CloverFontSettings> = cloverntRenderer.fonts.set("mono", {family: "Cascadia Code"}); void chosen;',
        "// @ts-expect-error Guard controls stay on CloverNT's developer page.",
        'void cloverntRenderer.runtime.setGuard;',
        '// @ts-expect-error The fonts are "ui" and "mono".',
        'void cloverntRenderer.fonts.set("emoji", {family: "x"});',
        '// @ts-expect-error Framework UI channels are not SDK exports.',
        'import type {Channel} from "@clovernt/sdk";',
        '// @ts-expect-error Framework IPC DTOs are not SDK exports.',
        'import type {PluginItem} from "@clovernt/sdk";',
    ].join("\n"));
    writeFileSync(join(consumer, "tsconfig.json"), JSON.stringify({
        compilerOptions: {
            target: "ES2024", module: "ESNext", moduleResolution: "Bundler", strict: true,
            noEmit: true, types: ["node"], skipLibCheck: true,
        }, files: ["consumer.ts"]
    }));
    const tsc = join(dirname(require.resolve("typescript/package.json")), "bin", "tsc");
    execFileSync(process.execPath, [tsc, "-p", consumer], {stdio: "inherit"});
});
