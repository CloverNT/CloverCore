import type {PluginItem} from "../../../shared/ipc";
import {
    alert,
    button,
    emptyState,
    group,
    iconButton,
    iconSlot,
    setButton,
    spinner,
    stateLabel,
    tag,
    toggleSwitch,
} from "../../core/components";
import {h, svg} from "../../core/dom";
import {ICONS} from "../../core/icons";
import {uiRegistry} from "../../core/ui-registry";
import {ACTION_FAILED, type PluginAction} from "../store";
import {pageElement, type View, type ViewContext} from "./context";
import {KIND_STYLE, kindLabel, pluginAvatar} from "./plugin-info";

type PluginFilter = "all" | "loaded" | "unloaded" | "native" | "js" | "external";

const FILTERS: Record<PluginFilter, { label: string; test: (plugin: PluginItem) => boolean }> = {
    all: {label: "全部", test: () => true},
    loaded: {label: "已启用", test: (p) => p.loaded},
    unloaded: {label: "未启用", test: (p) => !p.loaded},
    native: {label: "Native", test: (p) => p.kind === "native"},
    js: {label: "JS", test: (p) => p.kind === "js"},
    external: {label: "外部加载器", test: (p) => p.kind === "external"},
};

const ENTRY_LABEL = {main: "Main", preload: "Preload", renderer: "Renderer"} as const;

const PENDING_LABEL: Record<PluginAction, string> = {load: "正在启用", unload: "正在停用", reload: "正在重载"};

const FOCUS_HIGHLIGHT_MS = 1600;

function matchesSearch(plugin: PluginItem, query: string): boolean {
    return [plugin.name, plugin.id, plugin.type, plugin.author, plugin.description]
        .some((field) => field?.toLowerCase().includes(query));
}

function pluginMeta(plugin: PluginItem): HTMLElement | undefined {
    const dependencies = plugin.dependencies.map(({name, version, optional}) =>
        `${name}${version ? ` ${version}` : ""}${optional ? "（可选）" : ""}`,
    );
    const items = [
        plugin.author && `作者 ${plugin.author}`,
        plugin.entries.length > 0 && `入口 ${plugin.entries.map((entry) => ENTRY_LABEL[entry]).join(" · ")}`,
        dependencies.length > 0 && `依赖 ${dependencies.join("、")}`,
        plugin.loaded && !plugin.unloadable && "常驻，不支持热卸载",
    ].filter((item): item is string => Boolean(item));
    return items.length > 0 ? h("div", "clovernt-meta", ...items.map((item) => h("span", {}, item))) : undefined;
}

function pluginActions(plugin: PluginItem, {store, push}: ViewContext): HTMLElement {
    const pending = store.pendingPlugins.get(plugin.id);
    const busy = pending !== undefined;
    const pinned = plugin.loaded && !plugin.unloadable;
    const [page] = uiRegistry().pagesForPlugin(plugin.name);
    return h(
        "div",
        "clovernt-plugin-item__actions",
        pending && stateLabel(PENDING_LABEL[pending], {mark: spinner()}),
        page && iconButton(ICONS.settings, {title: `打开${page.title}`, onClick: () => push({view: "page", id: page.id})}),
        iconButton(ICONS.folder, {
            title: "打开插件文件夹",
            onClick: () => void store.openTarget({kind: "plugin", name: plugin.name}),
        }),
        plugin.loaded
            ? iconButton(ICONS.refresh, {
                title: pinned ? "常驻插件不支持热重载" : "热重载",
                disabled: busy || pinned,
                spinning: pending === "reload",
                onClick: () => void store.runPluginAction("reload", plugin),
            })
            : iconSlot(),
        toggleSwitch({
            checked: plugin.loaded,
            disabled: busy || pinned,
            title: pinned ? "常驻插件不支持热卸载" : plugin.loaded ? "停用插件" : "启用插件",
            onToggle: () => void store.runPluginAction(plugin.loaded ? "unload" : "load", plugin),
        }),
    );
}

function pluginItem(plugin: PluginItem, ctx: ViewContext, highlighted: boolean): HTMLElement {
    const {store} = ctx;
    const error = store.pluginErrors.get(plugin.id);
    let className = "clovernt-plugin-item";
    if (!plugin.loaded) className += " is-off";
    if (store.pendingPlugins.has(plugin.id)) className += " is-busy";
    if (highlighted) className += " is-highlighted";
    return h(
        "div",
        {class: className, "data-plugin": plugin.id},
        h(
            "div",
            "clovernt-plugin-item__body",
            pluginAvatar(plugin),
            h(
                "div",
                "clovernt-plugin-item__main",
                h(
                    "div",
                    "clovernt-plugin-item__head",
                    h("span", "clovernt-plugin-item__name", plugin.name),
                    plugin.version && h("span", "clovernt-plugin-item__ver clovernt-num", plugin.version),
                    tag(kindLabel(plugin), KIND_STYLE[plugin.kind].tag),
                    plugin.passive && tag("被动加载", "orange", "只在其他插件依赖它时加载"),
                ),
                plugin.description && h("div", "clovernt-plugin-item__desc", plugin.description),
                pluginMeta(plugin),
            ),
            pluginActions(plugin, ctx),
        ),
        error && alert({
            tone: "error",
            text: `${ACTION_FAILED[error.action]}${error.message ? `：${error.message}` : ""}`,
            onClose: () => store.dismissPluginError(plugin),
        }),
    );
}

export function pluginsView(ctx: ViewContext, focus?: string): View {
    const {store} = ctx;
    let filter: PluginFilter = "all";
    let query = "";
    let highlight = focus;
    let scrollToHighlight = focus !== undefined;
    let highlightTimer: number | undefined;

    const setQuery = (value: string) => {
        query = input.value = value;
        update();
    };
    const input = h("input", {
        type: "text",
        placeholder: "搜索名称、作者、描述或类型",
        "aria-label": "搜索插件",
        onInput: () => setQuery(input.value),
    });
    const clear = h(
        "button",
        {class: "clovernt-search__clear", type: "button", title: "清空搜索", "aria-label": "清空搜索", onClick: () => {
            setQuery("");
            input.focus();
        }},
        svg(ICONS.clear),
    );
    const rescanButton = button("重新扫描", {onClick: () => void store.rescan()});
    const toolbar = h(
        "div",
        "clovernt-toolbar",
        h("div", "clovernt-search", svg(ICONS.search), input, clear),
        rescanButton,
        button("打开插件目录", {onClick: () => void store.openTarget({kind: "pluginsDir"})}),
    );
    const filterBar = h("div", "clovernt-filters");
    const list = h("div", "clovernt-plugin-groups");
    const element = pageElement();
    element.append(toolbar, filterBar, list);

    function renderFilters(): void {
        const shown = (Object.keys(FILTERS) as PluginFilter[]).filter(
            (id) => id !== "external" || filter === id || store.plugins.some(FILTERS.external.test),
        );
        filterBar.replaceChildren(...shown.map((id) => h(
            "button",
            {
                class: filter === id ? "clovernt-chip is-active" : "clovernt-chip",
                type: "button",
                "aria-pressed": String(filter === id),
                onClick: () => {
                    filter = id;
                    update();
                },
            },
            FILTERS[id].label,
            h("span", "clovernt-chip__count clovernt-num", String(store.plugins.filter(FILTERS[id].test).length)),
        )));
    }

    function renderEmpty(): HTMLElement | undefined {
        if (store.plugins.length === 0) {
            if (store.loadError && !store.info) {
                return emptyState({
                    icon: ICONS.alertCircle,
                    title: "无法读取插件列表",
                    desc: store.loadError,
                    actions: [button("重试", {onClick: () => void store.refreshWithToast()})],
                });
            }
            return store.info && emptyState({
                icon: ICONS.externalPlugin,
                title: "还没有安装插件",
                desc: "把包含 clover.json 的插件文件夹放进下面的目录，然后重新扫描。",
                detail: store.info.pluginsDir,
                actions: [
                    button("打开插件目录", {variant: "primary", onClick: () => void store.openTarget({kind: "pluginsDir"})}),
                    button(store.rescanning ? "扫描中..." : "重新扫描", {disabled: store.rescanning, onClick: () => void store.rescan()}),
                ],
            });
        }
        const term = query.trim();
        return emptyState({
            icon: ICONS.search,
            title: "没有匹配的插件",
            desc: term ? `没有名称、作者、描述或类型包含「${term}」的插件` : `没有${FILTERS[filter].label}的插件`,
            actions: [term
                ? button("清除搜索", {onClick: () => setQuery("")})
                : button("显示全部", {onClick: () => {
                    filter = "all";
                    update();
                }})],
        });
    }

    function renderList(): void {
        const term = query.trim().toLowerCase();
        const shown = store.plugins.filter((plugin) => FILTERS[filter].test(plugin) && (!term || matchesSearch(plugin, term)));
        const groups = [
            {title: "已启用", plugins: shown.filter((plugin) => plugin.loaded)},
            {title: "未启用", plugins: shown.filter((plugin) => !plugin.loaded)},
        ].filter((entry) => entry.plugins.length > 0);
        if (groups.length === 0) {
            const empty = renderEmpty();
            list.replaceChildren(...(empty ? [empty] : []));
            return;
        }
        list.replaceChildren(...groups.map(({title, plugins}) =>
            group(title, plugins.map((plugin) => pluginItem(plugin, ctx, plugin.id === highlight))),
        ));
        const target = scrollToHighlight && highlight ? list.querySelector(`[data-plugin="${CSS.escape(highlight)}"]`) : null;
        if (target) {
            scrollToHighlight = false;
            requestAnimationFrame(() => target.isConnected && target.scrollIntoView({block: "center"}));
            highlightTimer = window.setTimeout(() => {
                highlight = undefined;
                update();
            }, FOCUS_HIGHLIGHT_MS);
        }
    }

    function update(): void {
        const installed = store.plugins.length > 0;
        toolbar.hidden = !installed && store.info !== undefined;
        filterBar.hidden = !installed;
        clear.hidden = query === "";
        setButton(rescanButton, store.rescanning ? "扫描中..." : "重新扫描", store.rescanning);
        renderFilters();
        renderList();
    }

    update();
    return {element, update, dispose: () => clearTimeout(highlightTimer)};
}
