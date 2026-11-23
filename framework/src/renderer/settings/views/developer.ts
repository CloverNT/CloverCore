import {
    GuardFamily,
    isPatchGuardActive,
    isPatchGuardEnabledNext,
    isPatchGuardRestartPending,
} from "../../../shared/guard";
import {alert, button, glyph, group, row, spinner, tag, toggleSwitch, type Tone} from "../../core/components";
import {h} from "../../core/dom";
import {ICONS} from "../../core/icons";
import {showToast} from "../../core/toast";
import {uiRegistry} from "../../core/ui-registry";
import type {SettingsStore} from "../store";
import {rebuiltView, type View, type ViewContext} from "./context";

const GUARD_POLL_MS = 2000;

const PATCHGUARD_LABEL = "PatchGuard 进程完整性检测";

const FAMILIES = [
    {family: GuardFamily.Packet, title: "Packet 数据包接口", desc: "收发网络包，监听并修改相关事件"},
    {family: GuardFamily.Credential, title: "Credential 登录凭据接口", desc: "读取登录凭据（A2、D2、D2Key）"},
] as const;

function lockSummary(mask: number): string {
    switch (mask & GuardFamily.Api) {
        case GuardFamily.Api:
            return "已全部锁定";
        case 0:
            return "已全部解锁";
        case GuardFamily.Credential:
            return "已解锁 Packet";
        default:
            return "已解锁 Credential";
    }
}

function patchGuardSummary(mask: number): { text: string; icon: string; tone?: Tone } {
    const active = isPatchGuardActive(mask);
    const enabledNext = isPatchGuardEnabledNext(mask);
    if (active && enabledNext) {
        return {text: "已开启", icon: ICONS.lock};
    }
    if (active) {
        return {text: "重启后关闭", icon: ICONS.unlock, tone: "warn"};
    }
    if (enabledNext) {
        return {text: "重启后开启", icon: ICONS.lock, tone: "ok"};
    }
    return {text: "已关闭", icon: ICONS.unlock, tone: "warn"};
}

function unsupportedNotice(store: SettingsStore): HTMLElement | undefined {
    if (store.guardMask !== null) {
        return undefined;
    }
    return alert({
        block: true,
        text: "当前 CloverLoader 版本未提供 CloverGuard 接口，敏感接口限制与完整性保护暂不可用。",
    });
}

function confirmationNotice(store: SettingsStore): HTMLElement | undefined {
    const request = store.guardPending;
    if (!request || request.locked) {
        return undefined;
    }
    const action = request.families === GuardFamily.PatchGuard ? "关闭" : "解锁";
    return alert({
        block: true,
        text: `正在等待确认：请在 CloverLoader 弹出的确认框里确认${action} ${request.label}。确认框可能被其他窗口挡住。`,
    });
}

function restartNotice(store: SettingsStore): HTMLElement | undefined {
    const mask = store.guardMask;
    if (mask === undefined || mask === null || !isPatchGuardRestartPending(mask)) {
        return undefined;
    }
    const enabledNext = isPatchGuardEnabledNext(mask);
    return alert({
        block: true,
        tone: enabledNext ? undefined : "warn",
        text: enabledNext
            ? "PatchGuard 将在下次启动 QQ 时开启；当前进程仍未启用完整性检测。"
            : "PatchGuard 将在下次启动 QQ 时关闭；当前进程仍处于保护中。",
    });
}

function guardGroup(store: SettingsStore): HTMLElement {
    const mask = store.guardMask;
    const pending = store.guardPending;
    const supported = mask !== undefined && mask !== null;
    const busy = !supported || pending !== undefined;
    const lockedMask = supported ? (mask & GuardFamily.Api) : 0;
    const familyRows = FAMILIES.map(({family, title, desc}) => {
        const locked = (lockedMask & family) !== 0;
        const waiting = pending !== undefined && !pending.locked && (pending.families & family) !== 0;
        return row({
            title: supported ? [title, locked ? tag("已上锁", "grey") : tag("已解锁", "warn")] : title,
            tip: desc,
            wrapTip: true,
            tail: [
                waiting && spinner(),
                button(waiting ? "等待确认..." : locked ? "解锁" : "上锁", {
                    disabled: busy,
                    onClick: () => void store.setGuard(family, !locked, title),
                }),
            ],
        });
    });
    const batchRow = row({
        title: "批量操作",
        tip: "一次解锁或上锁全部敏感接口",
        tail: [
            button("全部解锁", {
                disabled: busy || lockedMask === 0,
                onClick: () => void store.setGuard(lockedMask, false, lockedMask === GuardFamily.Api
                    ? "Packet 与 Credential 接口"
                    : FAMILIES.find(({family}) => (lockedMask & family) !== 0)!.title),
            }),
            button("全部上锁", {
                disabled: busy || lockedMask === GuardFamily.Api,
                onClick: () => void store.setGuard(GuardFamily.Api, true, "全部敏感接口"),
            }),
        ],
    });
    return group("敏感接口", [...familyRows, batchRow], {
        extra: mask === undefined
            ? undefined
            : mask === null
                ? "当前加载器不支持"
                : [glyph(lockedMask === GuardFamily.Api ? ICONS.lock : ICONS.unlock, lockedMask === GuardFamily.Api ? undefined : "warn"), lockSummary(mask)],
        note: "解锁需要在 CloverLoader 弹出的确认框里确认，上锁立即生效。不用时请保持上锁。",
    });
}

function patchGuardGroup(store: SettingsStore): HTMLElement {
    const mask = store.guardMask;
    const pending = store.guardPending;
    const supported = mask !== undefined && mask !== null;
    const busy = !supported || pending !== undefined;
    const active = supported && isPatchGuardActive(mask);
    const enabledNext = supported && isPatchGuardEnabledNext(mask);
    const restartPending = supported && active !== enabledNext;
    const waiting = pending !== undefined && !pending.locked && (pending.families & GuardFamily.PatchGuard) !== 0;
    const summary = supported ? patchGuardSummary(mask) : undefined;

    const tags = !supported
        ? []
        : [
            active ? tag("生效中", "grey") : tag("已关闭", "warn"),
            restartPending && (enabledNext ? tag("重启后开启", "ok") : tag("重启后关闭", "warn")),
        ];

    return group("完整性保护", [
        row({
            title: [PATCHGUARD_LABEL, ...tags],
            tip: "防止 QQ 的关键运行时数据被篡改，以及非法的模块加载",
            wrapTip: true,
            tail: [
                waiting && spinner(),
                waiting && h("span", "clovernt-row__value", "等待确认..."),
                toggleSwitch({
                    checked: enabledNext,
                    disabled: busy,
                    title: !supported
                        ? "当前 CloverLoader 不支持 PatchGuard"
                        : enabledNext
                            ? "关闭 PatchGuard（下次启动生效）"
                            : "开启 PatchGuard（下次启动生效）",
                    onToggle: () => void store.setGuard(GuardFamily.PatchGuard, !enabledNext, PATCHGUARD_LABEL),
                }),
            ],
        }),
    ], {
        extra: mask === null ? "当前加载器不支持" : summary && [glyph(summary.icon, summary.tone), summary.text],
        note: "关闭需要在 CloverLoader 弹出的确认框里确认，更改在下次启动 QQ 时生效。",
    });
}

function developerModeGroup(): HTMLElement {
    return group("开发者模式", [
        row({
            title: "启用开发者模式",
            tip: "关闭后「开发者选项」入口会从概览消失。在「关于QQ」页连点 5 次 CloverCore 版本号可以重新开启。",
            wrapTip: true,
            tail: toggleSwitch({
                checked: true,
                title: "关闭开发者模式",
                onToggle: () => {
                    uiRegistry().setDeveloperMode(false);
                    showToast("已关闭开发者模式", "info");
                },
            }),
        }),
    ]);
}

export function developerView({store}: ViewContext): View {
    const view = rebuiltView(() => [
        unsupportedNotice(store),
        confirmationNotice(store),
        restartNotice(store),
        guardGroup(store),
        patchGuardGroup(store),
        developerModeGroup(),
    ]);
    const timer = setInterval(() => {
        if (!document.hidden && store.guardPending === undefined && store.guardMask !== null) {
            store.syncGuard().catch(() => undefined); // the next poll retries
        }
    }, GUARD_POLL_MS);
    void store.syncGuard();
    return {...view, dispose: () => clearInterval(timer)};
}
