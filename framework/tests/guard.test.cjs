const {after, before, test} = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const {tmpdir} = require("node:os");
const {join, resolve} = require("node:path");
const {build} = require("esbuild");

let scratch;
let guard;
let SettingsStore;
let toasts;
let ipcHandler;

before(async () => {
    scratch = fs.mkdtempSync(join(tmpdir(), "clover-guard-"));
    const bundle = join(scratch, "guard.cjs");
    await build({
        stdin: {
            resolveDir: resolve(__dirname, "../src"),
            contents: [
                'export * as guard from "./shared/guard";',
                'export {SettingsStore} from "./renderer/settings/store";',
            ].join("\n"),
        },
        platform: "node",
        bundle: true,
        outfile: bundle,
        format: "cjs",
        plugins: [{
            name: "stubs",
            setup(build) {
                build.onResolve({filter: /\/core\/toast$/}, () => ({path: "toast", namespace: "stub"}));
                build.onLoad({filter: /^toast$/, namespace: "stub"}, () => ({
                    contents: "export const showToast = (...args) => globalThis.__cloverTestToasts.push(args);",
                }));
                build.onResolve({filter: /\/core\/bridge$/}, () => ({path: "bridge", namespace: "stub"}));
                build.onLoad({filter: /^bridge$/, namespace: "stub"}, () => ({
                    contents: "export const invoke = (ch, ...args) => globalThis.__cloverTestInvoke(ch, ...args);",
                }));
            },
        }],
    });
    globalThis.__cloverTestToasts = toasts = [];
    globalThis.__cloverTestInvoke = (ch, ...args) => ipcHandler(ch, ...args);
    ({guard, SettingsStore} = require(bundle));
});

after(() => {
    delete globalThis.__cloverTestToasts;
    delete globalThis.__cloverTestInvoke;
    fs.rmSync(scratch, {recursive: true, force: true});
});

test("guard family masks and PatchGuard 2-bit state match CloverLoader Nt.h", () => {
    const {
        GuardFamily,
        PATCHGUARD_NEXT_BIT,
        PatchGuardBit,
        patchGuardState,
        isPatchGuardActive,
        isPatchGuardEnabledNext,
        isPatchGuardRestartPending,
    } = guard;

    assert.equal(GuardFamily.Packet, 0x01);
    assert.equal(GuardFamily.Credential, 0x02);
    assert.equal(GuardFamily.Api, 0x03);
    assert.equal(GuardFamily.PatchGuard, 0x04);
    assert.equal(GuardFamily.All, 0x07);
    assert.equal(PATCHGUARD_NEXT_BIT, 0x08);

    // Default: all APIs locked, PatchGuard active now and enabled for next launch (0x0F).
    const defaultLocked = GuardFamily.All | PATCHGUARD_NEXT_BIT;
    assert.equal(patchGuardState(defaultLocked), PatchGuardBit.Active | PatchGuardBit.Enabled);
    assert.equal(isPatchGuardActive(defaultLocked), true);
    assert.equal(isPatchGuardEnabledNext(defaultLocked), true);
    assert.equal(isPatchGuardRestartPending(defaultLocked), false);

    // Disabled during an active session: active now (0x04), disabled next launch (0x00).
    const pendingDisable = GuardFamily.All;
    assert.equal(patchGuardState(pendingDisable), PatchGuardBit.Active);
    assert.equal(isPatchGuardActive(pendingDisable), true);
    assert.equal(isPatchGuardEnabledNext(pendingDisable), false);
    assert.equal(isPatchGuardRestartPending(pendingDisable), true);

    // Launched with PatchGuard disabled: inactive now and disabled next launch (0x03).
    const disabledSession = GuardFamily.Api;
    assert.equal(patchGuardState(disabledSession), 0);
    assert.equal(isPatchGuardActive(disabledSession), false);
    assert.equal(isPatchGuardEnabledNext(disabledSession), false);
    assert.equal(isPatchGuardRestartPending(disabledSession), false);

    // Re-enabled while disabled in this process: inactive now, enabled next launch (0x0B).
    const pendingEnable = GuardFamily.Api | PATCHGUARD_NEXT_BIT;
    assert.equal(patchGuardState(pendingEnable), PatchGuardBit.Enabled);
    assert.equal(isPatchGuardActive(pendingEnable), false);
    assert.equal(isPatchGuardEnabledNext(pendingEnable), true);
    assert.equal(isPatchGuardRestartPending(pendingEnable), true);
});

test("SettingsStore.setGuard updates mask and reports restart vs session-override semantics", async () => {
    const {GuardFamily, GuardStatus, PATCHGUARD_NEXT_BIT} = guard;
    const store = new SettingsStore();

    // Disabling PatchGuard while active -> pending restart toast.
    toasts.length = 0;
    ipcHandler = async (_ch, req) => {
        assert.deepEqual(req, {families: GuardFamily.PatchGuard, locked: false});
        return {status: GuardStatus.Ok, lockedMask: GuardFamily.All};
    };
    await store.setGuard(GuardFamily.PatchGuard, false, "PatchGuard 进程完整性检测");
    assert.equal(store.guardMask, GuardFamily.All);
    assert.equal(store.guardPending, undefined);
    assert.equal(toasts.length, 1);
    assert.match(toasts[0][0], /已关闭.*重启 QQ 后生效/);
    assert.equal(toasts[0][1], "success");

    // Re-enabling PatchGuard before restarting -> restored toast (no restart needed).
    toasts.length = 0;
    ipcHandler = async () => ({status: GuardStatus.Ok, lockedMask: GuardFamily.All | PATCHGUARD_NEXT_BIT});
    await store.setGuard(GuardFamily.PatchGuard, true, "PatchGuard 进程完整性检测");
    assert.equal(store.guardMask, GuardFamily.All | PATCHGUARD_NEXT_BIT);
    assert.match(toasts[0][0], /已恢复开启/);
    assert.equal(toasts[0][1], "success");

    // PersistFailed on PatchGuard -> error toast (PatchGuard has no session override).
    toasts.length = 0;
    ipcHandler = async () => ({status: GuardStatus.PersistFailed, lockedMask: GuardFamily.All | PATCHGUARD_NEXT_BIT});
    await store.setGuard(GuardFamily.PatchGuard, false, "PatchGuard 进程完整性检测");
    assert.match(toasts[0][0], /写入 CloverLoader\.dat 失败，设置未更改/);
    assert.equal(toasts[0][1], "error");

    // PersistFailed on ApiGuard unlock -> info toast (session-only override took effect).
    toasts.length = 0;
    ipcHandler = async () => ({status: GuardStatus.PersistFailed, lockedMask: GuardFamily.Credential | GuardFamily.PatchGuard | PATCHGUARD_NEXT_BIT});
    await store.setGuard(GuardFamily.Packet, false, "Packet 数据包接口");
    assert.match(toasts[0][0], /已在当前会话解锁/);
    assert.equal(toasts[0][1], "info");

    // Older CloverLoader without CloverGetLocked / CloverSetLocked -> null mask and MissingApi error toast.
    toasts.length = 0;
    ipcHandler = async (ch) => {
        if (ch === "clovernt:ui:get-guard") {
            return null;
        }
        return {status: GuardStatus.MissingApi, lockedMask: null};
    };
    await store.syncGuard();
    assert.equal(store.guardMask, null);
    await store.setGuard(GuardFamily.Packet, false, "Packet 数据包接口");
    assert.equal(store.guardMask, null);
    assert.match(toasts[0][0], /CloverSetLocked/);
    assert.equal(toasts[0][1], "error");
});
