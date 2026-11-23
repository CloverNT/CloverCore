import {Channel} from "../shared/ipc";
import type {CloverLogger} from "../shared/log";
import {readAboutAssets} from "./assets";
import {loadBinding} from "./binding";
import {handle} from "./ipc";
import {collectPlugins, runPluginAction} from "./plugins";
import {buildRuntimeInfo} from "./runtime-info";
import {completeFonts, readFonts, saveFonts, settingsFile} from "./settings";
import {listSystemFonts} from "./system-fonts";
import {openTarget} from "./shell";
import {broadcast, broadcastToAll, ensureSettingsWindowWidth} from "./windows";

const binding = loadBinding();
const logger = (globalThis as unknown as { clovernt: { log(scope: string): CloverLogger } }).clovernt.log("clovernt-ui");
const uiSettings = settingsFile(binding.coreDir);

handle(Channel.GetRuntimeInfo, () => buildRuntimeInfo(binding));
handle(Channel.GetAboutAssets, () => readAboutAssets());
handle(Channel.GetPlugins, () => collectPlugins(binding));
handle(Channel.PluginAction, (_event, request) => {
    const result = runPluginAction(binding, request);
    broadcast(Channel.PluginsChanged);
    return result;
});
handle(Channel.OpenTarget, (_event, request) => openTarget(binding, request));
handle(Channel.EnsureWindowSize, (event) => ensureSettingsWindowWidth(event.sender));
handle(Channel.GetGuard, () => binding.getGuardLocked());
handle(Channel.SetGuard, async (_event, {families, locked}) => {
    const result = await binding.setGuardLocked(families, locked);
    broadcast(Channel.GuardChanged, result.lockedMask);
    return result;
});
// Every page applies the fonts, web views included.
handle(Channel.GetFonts, () => readFonts(uiSettings), {anyOrigin: true});
handle(Channel.SetFonts, async (_event, choices) => {
    const saved = await saveFonts(uiSettings, choices, listSystemFonts);
    broadcastToAll(Channel.FontsChanged, saved);
    return saved;
});
handle(Channel.ListSystemFonts, () => listSystemFonts());

setTimeout(() => {
    completeFonts(uiSettings, listSystemFonts).then(
        (saved) => saved && broadcastToAll(Channel.FontsChanged, saved),
        (error: unknown) => logger.error(`completing the font settings failed: ${String(error)}`),
    );
}, 5000);
