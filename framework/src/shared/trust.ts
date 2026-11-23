export const TRUSTED_ORIGIN = "app://.";

export function isTrustedOrigin(origin: string | null | undefined): boolean {
    return origin === TRUSTED_ORIGIN;
}
