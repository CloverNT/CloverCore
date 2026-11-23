export const GuardFamily = {
    Packet: 0x01,
    Credential: 0x02,
    PatchGuard: 0x04,
    Api: 0x03,
    All: 0x07,
} as const;

export const PATCHGUARD_NEXT_BIT = 0x08;

export const PatchGuardBit = {
    Active: 0x01,
    Enabled: 0x02,
} as const;

export function patchGuardState(lockedMask: number): number {
    return (lockedMask >>> 2) & 0x03;
}

export function isPatchGuardActive(lockedMask: number): boolean {
    return (patchGuardState(lockedMask) & PatchGuardBit.Active) !== 0;
}

export function isPatchGuardEnabledNext(lockedMask: number): boolean {
    return (patchGuardState(lockedMask) & PatchGuardBit.Enabled) !== 0;
}

export function isPatchGuardRestartPending(lockedMask: number): boolean {
    return isPatchGuardActive(lockedMask) !== isPatchGuardEnabledNext(lockedMask);
}

export const GuardStatus = {
    Ok: 0,
    DeniedCaller: 1,
    DeniedUser: 2,
    Invalid: 3,
    Busy: 4,
    WrongThread: 5,
    PersistFailed: 6,
    DatTooNew: 7,
    Unavailable: 8,
    MissingApi: -1,
} as const;

export interface GuardSetRequest {
    families: number;
    locked: boolean;
}

export interface GuardSetResult {
    status: number;
    lockedMask: number | null;
}
