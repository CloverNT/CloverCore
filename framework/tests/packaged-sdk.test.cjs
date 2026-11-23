const {test} = require("node:test");
const assert = require("node:assert/strict");
const {execFileSync} = require("node:child_process");
const {mkdtempSync, mkdirSync, writeFileSync, rmSync} = require("node:fs");
const {join, resolve, dirname} = require("node:path");

// Run after build. Use the release packager so missing transitive declarations are caught here.
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
    const setup = () => {};
    assert.equal(sdk.definePlugin(setup), setup);
    assert.equal(sdk.EventPriority.Monitor, 5);

    writeFileSync(join(consumer, "consumer.ts"), [
        'import {clovernt, definePlugin, EventPriority, registerSettingsPage, type CloverContext, type LoadedPlugin, type SettingsPageConfig, type SettingsPagePlacement} from "@clovernt/sdk";',
        'import {cloverntRenderer, type CloverFontSettings, type CloverPanelHandle, type CloverPluginInfo, type CloverRuntimeInfo} from "@clovernt/sdk";',
        'definePlugin((ctx: CloverContext) => { ctx.logger.info(ctx.name); ctx.events.once("ready", () => {}, EventPriority.Normal); });',
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
    writeFileSync(join(consumer, "tsconfig.json"), JSON.stringify({compilerOptions: {
        target: "ES2024", module: "ESNext", moduleResolution: "Bundler", strict: true,
        // Match the framework: Electron and @types/node both augment process.noDeprecation.
        noEmit: true, types: ["node"], skipLibCheck: true,
    }, files: ["consumer.ts"]}));
    const tsc = join(dirname(require.resolve("typescript/package.json")), "bin", "tsc");
    execFileSync(process.execPath, [tsc, "-p", consumer], {stdio: "inherit"});
});
