import type {FontSettings} from "../../../shared/fonts";
import type {PluginItem, RuntimeInfo} from "../../../shared/ipc";
import {button, dot, group, iconButton, navRow, row, spinner, stateLabel, type Tone} from "../../core/components";
import {h, pressable, svg} from "../../core/dom";
import {ICONS} from "../../core/icons";
import {platformName} from "../../core/platform";
import {PROJECT} from "../../core/project";
import {showToast} from "../../core/toast";
import {uiRegistry} from "../../core/ui-registry";
import {ACTION_FAILED} from "../store";
import {rebuiltView, type View, type ViewContext} from "./context";
import {kindLabel, PLUGIN_STATE, pluginAvatar, pluginState, type PluginState} from "./plugin-info";

const PLUGIN_PREVIEW_LIMIT = 8;

const PREVIEW_ORDER: Record<PluginState, number> = {failed: 0, on: 1, off: 2};

interface Issue {
    tone: "warn" | "error";
    title: string;
    desc?: string;
    open?: () => void;
}

function issues({store, push}: ViewContext): Issue[] {
    const found: Issue[] = [];
    for (const plugin of store.plugins) {
        const error = store.pluginErrors.get(plugin.id);
        if (error) {
            found.push({
                tone: "error",
                title: `${plugin.name} ${ACTION_FAILED[error.action]}`,
                desc: error.message || undefined,
                open: () => push({view: "plugins", focus: plugin.id}),
            });
        }
    }
    if (store.info && !store.info.packetReady) {
        found.push({tone: "warn", title: "Packet 通道未就绪", desc: "依赖 Packet 接口的插件可能无法正常工作"});
    }
    return found;
}

function pluginSummary({store}: ViewContext): string {
    if (store.plugins.length === 0) {
        return "还没有安装插件";
    }
    const counts: Record<PluginState, number> = {on: 0, off: 0, failed: 0};
    for (const plugin of store.plugins) {
        counts[pluginState(plugin, store)]++;
    }
    return [
        `${counts.on} 个插件已启用`,
        counts.failed > 0 && `${counts.failed} 个加载失败`,
        counts.off > 0 && `${counts.off} 个未启用`,
    ].filter(Boolean).join("，");
}

function specs(info: RuntimeInfo): HTMLElement {
    const items: Array<[string, Array<Node | string>]> = [
        ["CloverCore", [info.version]],
        ["CloverLoader", [info.loaderVersion]],
        ["QQ", [info.qqVersion]],
        ["Packet 通道", info.packetReady ? [dot("ok"), "已就绪"] : [dot("warn"), "未就绪"]],
        ["平台", [`${platformName(info.platform)} ${info.arch}`]],
        ["Electron", [info.versions.electron]],
    ];
    return h(
        "div",
        "clovernt-specs",
        ...items.map(([key, value]) => h(
            "div",
            "clovernt-spec",
            h("span", "clovernt-spec__key", key),
            h("span", {class: "clovernt-spec__val clovernt-num", title: typeof value[0] === "string" ? value[0] : undefined}, ...value),
        )),
    );
}

function statusCard(ctx: ViewContext): HTMLElement {
    const {store} = ctx;
    const found = issues(ctx);
    let tone: Tone | undefined;
    let mark: Node;
    let title: string;
    let sub: string;
    if (store.info) {
        tone = found.length > 0 ? "warn" : "ok";
        mark = svg(found.length > 0 ? ICONS.statusWarn : ICONS.statusOk);
        title = found.length > 0 ? `有 ${found.length} 项需要处理` : "运行正常";
        sub = pluginSummary(ctx);
    } else if (store.loadError) {
        tone = "error";
        mark = svg(ICONS.statusWarn);
        title = "无法读取运行状态";
        sub = store.loadError;
    } else {
        mark = spinner();
        title = "正在读取运行状态";
        sub = "";
    }
    return h(
        "div",
        "clovernt-card clovernt-status",
        h(
            "div",
            "clovernt-status__head",
            h("span", tone ? `clovernt-status__icon is-${tone}` : "clovernt-status__icon", mark),
            h("div", "clovernt-status__text", h("div", "clovernt-status__title", title), sub && h("div", "clovernt-status__sub", sub)),
            h(
                "div",
                "clovernt-status__tail",
                iconButton(ICONS.refresh, {title: "刷新", spinning: store.refreshing, onClick: () => void store.refreshWithToast()}),
            ),
        ),
        found.length > 0 && h("div", "clovernt-issues", ...found.map(({tone: issueTone, title: issueTitle, desc, open}) => h(
            "div",
            "clovernt-issue",
            svg(issueTone === "error" ? ICONS.alertCircle : ICONS.warning, `clovernt-issue__icon is-${issueTone}`),
            h(
                "div",
                "clovernt-issue__main",
                h("div", "clovernt-issue__title", issueTitle),
                desc && h("div", "clovernt-issue__desc", desc),
            ),
            open && button("查看", {onClick: open}),
        ))),
        store.info && specs(store.info),
    );
}

function pluginCell(plugin: PluginItem, {store, push}: ViewContext): HTMLElement {
    const state = pluginState(plugin, store);
    const {label, tone} = PLUGIN_STATE[state];
    return pressable(
        h(
            "div",
            {class: state === "on" ? "clovernt-plugin-cell" : "clovernt-plugin-cell is-off", title: `在插件管理中查看 ${plugin.name}`},
            pluginAvatar(plugin),
            h(
                "div",
                "clovernt-plugin-cell__main",
                h("div", "clovernt-plugin-cell__name", plugin.name),
                h("div", "clovernt-plugin-cell__meta", [kindLabel(plugin), plugin.version].filter(Boolean).join(" · ")),
            ),
            stateLabel(label, {tone}),
        ),
        "button",
        () => push({view: "plugins", focus: plugin.id}),
    );
}

function pluginsGroup(ctx: ViewContext): HTMLElement {
    const {store, push} = ctx;
    const preview = [...store.plugins]
        .sort((a, b) =>
            PREVIEW_ORDER[pluginState(a, store)] - PREVIEW_ORDER[pluginState(b, store)] || a.name.localeCompare(b.name, "zh-CN"),
        )
        .slice(0, PLUGIN_PREVIEW_LIMIT);
    const count = store.plugins.length > 0 ? `共 ${store.plugins.length} 个` : "还没有安装插件";
    return group("插件", [
        preview.length > 0 && h("div", "clovernt-plugin-grid", ...preview.map((plugin) => pluginCell(plugin, ctx))),
        navRow({title: "管理插件", value: store.info ? count : undefined, onOpen: () => push({view: "plugins"})}),
    ]);
}

function fontSummary(fonts: FontSettings | undefined): string | undefined {
    if (!fonts) {
        return undefined;
    }
    return [fonts.ui.family, fonts.mono.family && `等宽 ${fonts.mono.family}`].filter(Boolean).join("，") || "默认";
}

function moreGroup({store, push}: ViewContext): HTMLElement {
    const registry = uiRegistry();
    const rows = [
        navRow({title: "字体", value: fontSummary(store.fonts), onOpen: () => push({view: "fonts"})}),
        ...registry.panelPages().map((page) =>
            navRow({title: page.title, value: page.plugin, onOpen: () => push({view: "page", id: page.id})}),
        ),
    ];
    if (registry.isDeveloperMode) {
        rows.push(navRow({title: "开发者选项", onOpen: () => push({view: "developer"})}));
    }
    return group("更多设置", rows);
}

function directoriesGroup({store}: ViewContext): HTMLElement {
    return group("目录", [
        row({
            title: "插件目录",
            tip: store.info?.pluginsDir,
            tail: [
                button(store.rescanning ? "扫描中..." : "重新扫描", {disabled: store.rescanning, onClick: () => void store.rescan()}),
                button("打开", {onClick: () => void store.openTarget({kind: "pluginsDir"})}),
            ],
        }),
        row({
            title: "CloverCore 目录",
            tip: store.info?.coreDir,
            tail: button("打开", {onClick: () => void store.openTarget({kind: "coreDir"})}),
        }),
    ]);
}

function copyGroupNumber(): void {
    navigator.clipboard.writeText(PROJECT.qqGroup).then(
        () => showToast(`已复制群号 ${PROJECT.qqGroup}`, "success"),
        () => showToast(`群号: ${PROJECT.qqGroup}`, "info"),
    );
}

function footer({store}: ViewContext): HTMLElement {
    const link = (label: string, icon: string, title: string, onClick: () => void) =>
        h("button", {class: "clovernt-footer__link", type: "button", title, onClick}, label, svg(icon));
    return h(
        "div",
        "clovernt-footer",
        link("GitHub", ICONS.externalLink, PROJECT.repository, () => void store.openTarget({kind: "external", url: PROJECT.repository})),
        h("span", {}, "·"),
        link(`交流群 ${PROJECT.qqGroup}`, ICONS.copy, "复制群号", copyGroupNumber),
        h("span", {}, "·"),
        h("span", {title: PROJECT.licenseName}, PROJECT.license),
    );
}

export function overviewView(ctx: ViewContext): View {
    return rebuiltView(() => [statusCard(ctx), pluginsGroup(ctx), moreGroup(ctx), directoriesGroup(ctx), footer(ctx)]);
}
