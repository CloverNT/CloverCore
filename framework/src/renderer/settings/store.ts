import {GuardFamily, GuardStatus, isPatchGuardRestartPending} from "../../shared/guard";
import {errorMessage} from "../../shared/errors";
import type {FontSettings, SystemFontFamily} from "../../shared/fonts";
import {Channel, type OpenTargetRequest, type PluginItem, type RuntimeInfo} from "../../shared/ipc";
import type {CloverFontSettings} from "../../shared/public";
import {invoke} from "../core/bridge";
import {showToast} from "../core/toast";

export type PluginAction = "load" | "unload" | "reload";

const ACTION_DONE: Record<PluginAction, string> = {load: "已启用", unload: "已停用", reload: "已重载"};

export const ACTION_FAILED: Record<PluginAction, string> = {load: "加载失败", unload: "停用失败", reload: "重载失败"};

export interface PluginError {
    action: PluginAction;
    message: string;
}

export interface GuardRequest {
    families: number;
    locked: boolean;
    label: string;
}

const GUARD_FAILURE: Record<number, string> = {
    [GuardStatus.DeniedCaller]: "调用方模块身份校验未通过",
    [GuardStatus.DeniedUser]: "已取消或未通过安全确认",
    [GuardStatus.Invalid]: "无效的保护项参数",
    [GuardStatus.Busy]: "已有安全确认窗口正在等待操作，请先完成确认",
    [GuardStatus.WrongThread]: "当前线程上下文不允许执行安全确认",
    [GuardStatus.DatTooNew]: "CloverLoader.dat 来自更高版本加载器，当前处于只读保护状态",
    [GuardStatus.MissingApi]: "当前 CloverLoader 版本未提供 CloverSetLocked 接口",
};

export class SettingsStore {
    info: RuntimeInfo | undefined;
    plugins: PluginItem[] = [];
    guardMask: number | null | undefined;
    loadError: string | undefined;
    fonts: FontSettings | undefined;
    fontsError: string | undefined;
    systemFonts: SystemFontFamily[] | undefined;
    systemFontsError: string | undefined;

    refreshing = false;
    rescanning = false;
    guardPending: GuardRequest | undefined;
    readonly pendingPlugins = new Map<string, PluginAction>();
    readonly pluginErrors = new Map<string, PluginError>();

    private readonly listeners = new Set<() => void>();
    private refreshing$: Promise<void> | undefined;
    private refreshQueued = false;

    subscribe(listener: () => void): () => void {
        this.listeners.add(listener);
        return () => void this.listeners.delete(listener);
    }

    emit(): void {
        for (const listener of [...this.listeners]) {
            listener();
        }
    }

    refresh(): Promise<void> {
        if (this.refreshing$) {
            this.refreshQueued = true;
            return this.refreshing$;
        }
        const run = async (): Promise<void> => {
            do {
                this.refreshQueued = false;
                try {
                    await this.load();
                } catch (error) {
                    this.refreshQueued = false;
                    throw error;
                }
            } while (this.refreshQueued);
        };
        return (this.refreshing$ = run().finally(() => {
            this.refreshing$ = undefined;
        }));
    }

    async refreshWithToast(): Promise<void> {
        try {
            await this.refresh();
            showToast("状态与插件信息已刷新", "success");
        } catch (error) {
            showToast(`刷新失败: ${errorMessage(error)}`, "error");
        }
    }

    setGuardMask(mask: number | null): void {
        if (this.guardMask === mask) {
            return;
        }
        this.guardMask = mask;
        this.emit();
    }

    async syncGuard(): Promise<void> {
        this.setGuardMask(await invoke(Channel.GetGuard));
    }

    async loadSystemFonts(): Promise<void> {
        try {
            this.systemFonts = await invoke(Channel.ListSystemFonts);
            this.systemFontsError = undefined;
        } catch (error) {
            this.systemFontsError = errorMessage(error);
        }
        this.emit();
    }

    setFonts(fonts: FontSettings): void {
        this.fonts = fonts;
        this.fontsError = undefined;
        this.emit();
    }

    async saveFonts(fonts: CloverFontSettings): Promise<void> {
        try {
            this.setFonts(await invoke(Channel.SetFonts, fonts));
        } catch (error) {
            showToast(`保存字体设置失败: ${errorMessage(error)}`, "error", 3200);
        }
    }

    async runPluginAction(action: PluginAction, plugin: PluginItem): Promise<void> {
        if (this.pendingPlugins.has(plugin.id)) {
            return;
        }
        this.pendingPlugins.set(plugin.id, action);
        this.pluginErrors.delete(plugin.id);
        this.emit();
        try {
            const result = await invoke(Channel.PluginAction, {action, name: plugin.id});
            await this.refresh();
            if (result.ok) {
                showToast(`${plugin.name} ${ACTION_DONE[action]}`, "success");
            } else {
                this.failPlugin(plugin, action, result.error ?? "");
            }
        } catch (error) {
            this.failPlugin(plugin, action, errorMessage(error));
        } finally {
            this.pendingPlugins.delete(plugin.id);
            this.emit();
        }
    }

    dismissPluginError(plugin: PluginItem): void {
        this.pluginErrors.delete(plugin.id);
        this.emit();
    }

    async rescan(): Promise<void> {
        if (this.rescanning) {
            return;
        }
        this.rescanning = true;
        this.emit();
        try {
            const result = await invoke(Channel.PluginAction, {action: "rescan"});
            await this.refresh();
            if (!result.ok) {
                showToast(result.error || "重新扫描失败", "error");
            } else if (result.count > 0) {
                showToast(`扫描完成，已自动排队加载 ${result.count} 个新扩展插件`, "success");
            } else {
                showToast("扫描完成，插件列表已同步", "success");
            }
        } catch (error) {
            showToast(errorMessage(error), "error");
        } finally {
            this.rescanning = false;
            this.emit();
        }
    }

    async setGuard(families: number, locked: boolean, label: string): Promise<void> {
        if (this.guardPending) {
            showToast("已有未完成的请求，请先完成当前请求", "info");
            return;
        }
        this.guardPending = {families, locked, label};
        this.emit();
        try {
            const {status, lockedMask} = await invoke(Channel.SetGuard, {families, locked});
            this.setGuardMask(lockedMask);
            const patchGuardOnly = families === GuardFamily.PatchGuard;
            if (status === GuardStatus.Ok) {
                if (patchGuardOnly) {
                    const pending = lockedMask !== null && isPatchGuardRestartPending(lockedMask);
                    if (locked) {
                        showToast(pending ? `已开启 ${label}，重启 QQ 后生效` : `已恢复开启 ${label}`, "success", 2600);
                    } else {
                        showToast(pending ? `已关闭 ${label}，重启 QQ 后生效` : `已保持关闭 ${label}`, "success", 2600);
                    }
                } else {
                    showToast(locked ? `已上锁 ${label}` : `已解锁 ${label}`, "success", 2400);
                }
            } else if (status === GuardStatus.PersistFailed) {
                if (patchGuardOnly) {
                    showToast(`${label}：写入 CloverLoader.dat 失败，设置未更改`, "error", 3200);
                } else {
                    const detail = locked ? "已在当前进程上锁，但移除 CloverLoader.dat 失败" : "已在当前会话解锁，但写入 CloverLoader.dat 失败";
                    showToast(`${label}：${detail}`, "info", 3200);
                }
            } else if (status === GuardStatus.DeniedUser) {
                showToast(GUARD_FAILURE[status], "info", 2200);
            } else {
                showToast(GUARD_FAILURE[status] ?? "CloverGuard 不可用或自检未通过", "error", 3200);
            }
        } catch (error) {
            showToast(errorMessage(error), "error", 3200);
        } finally {
            this.guardPending = undefined;
            this.emit();
        }
    }

    async openTarget(request: OpenTargetRequest): Promise<void> {
        const error = await invoke(Channel.OpenTarget, request).catch(errorMessage);
        if (error) {
            showToast(error, "error");
        }
    }

    private async load(): Promise<void> {
        this.refreshing = true;
        this.emit();
        try {
            const fonts = invoke(Channel.GetFonts).then(
                (value) => [value, undefined] as const,
                (error: unknown) => [undefined, errorMessage(error)] as const,
            );
            [this.info, this.plugins, this.guardMask, [this.fonts, this.fontsError]] = await Promise.all([
                invoke(Channel.GetRuntimeInfo),
                invoke(Channel.GetPlugins),
                invoke(Channel.GetGuard),
                fonts,
            ]);
            this.loadError = undefined;
        } catch (error) {
            this.loadError = errorMessage(error);
            throw error;
        } finally {
            this.refreshing = false;
            this.emit();
        }
    }

    private failPlugin(plugin: PluginItem, action: PluginAction, message: string): void {
        this.pluginErrors.set(plugin.id, {action, message});
        const reason = message ? `：${message}` : "";
        showToast(`${plugin.name} ${ACTION_FAILED[action]}${reason}`, "error", 3600);
    }
}
