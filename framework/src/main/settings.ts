import {randomBytes} from "node:crypto";
import {mkdirSync, readFileSync, renameSync, unlinkSync, writeFileSync} from "node:fs";
import {dirname, join, resolve} from "node:path";

import {
    DEFAULT_FONTS,
    faceOptions,
    findFamily,
    normalizeChoice,
    normalizeFonts,
    resolveFaces,
    type FontSettings,
    type SystemFontFamily,
} from "../shared/fonts";

class SettingsFormatError extends Error {
}

const fileLocks = new Map<string, Promise<void>>();

async function withFileLock<T>(file: string, task: () => Promise<T>): Promise<T> {
    const key = resolve(file);
    const previous = fileLocks.get(key) ?? Promise.resolve();
    let release!: () => void;
    const current = new Promise<void>((res) => {
        release = res;
    });
    const tail = previous.then(() => current);
    fileLocks.set(key, tail);
    await previous;
    try {
        return await task();
    } finally {
        release();
        if (fileLocks.get(key) === tail) {
            fileLocks.delete(key);
        }
    }
}

export function settingsFile(coreDir: string): string {
    return join(coreDir, "config", "ui.json");
}

function readSettings(file: string): Record<string, unknown> {
    let text: string;
    try {
        text = readFileSync(file, "utf8");
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code === "ENOENT") {
            return {};
        }
        throw error;
    }
    let settings: unknown;
    try {
        settings = JSON.parse(text);
    } catch (error) {
        throw new SettingsFormatError(`${file} 不是有效的 JSON: ${(error as Error).message}`);
    }
    if (typeof settings !== "object" || settings === null || Array.isArray(settings)) {
        throw new SettingsFormatError(`${file} 不是 JSON 对象`);
    }
    return settings as Record<string, unknown>;
}

export function readFonts(file: string): FontSettings {
    const {fonts} = readSettings(file);
    return fonts === undefined ? normalizeFonts(DEFAULT_FONTS) : normalizeFonts(fonts);
}

export function saveFonts(file: string, choices: unknown, list: () => Promise<SystemFontFamily[]>): Promise<FontSettings> {
    return withFileLock(file, async () => {
        const record = (typeof choices === "object" && choices !== null ? choices : {}) as Record<string, unknown>;
        const chosen = (["ui", "mono"] as const)
            .filter((kind) => record[kind] !== undefined)
            .map((kind) => [kind, normalizeChoice(record[kind])] as const);
        const families = chosen.some(([, choice]) => choice.family) ? await list() : [];
        let fonts: FontSettings;
        try {
            fonts = readFonts(file);
        } catch (error) {
            if (error instanceof SettingsFormatError) {
                fonts = normalizeFonts(DEFAULT_FONTS);
            } else {
                throw error;
            }
        }
        for (const [kind, choice] of chosen) {
            const family = choice.family ? findFamily(families, choice.family) : undefined;
            fonts[kind] = {
                ...choice,
                faces: family ? resolveFaces(faceOptions(family), choice.regular, choice.bold) : []
            };
        }
        return writeFonts(file, fonts);
    });
}

export async function completeFonts(file: string, list: () => Promise<SystemFontFamily[]>): Promise<FontSettings | undefined> {
    const snapshot = readFonts(file);
    const pending = (["ui", "mono"] as const).filter((kind) => snapshot[kind].family && snapshot[kind].faces.length === 0);
    if (pending.length === 0) {
        return undefined;
    }
    const families = await list();
    return withFileLock(file, async () => {
        const current = readFonts(file);
        let changed = false;
        for (const kind of pending) {
            const before = snapshot[kind];
            const now = current[kind];
            if (
                now.faces.length > 0
                || now.family !== before.family
                || now.regular !== before.regular
                || now.bold !== before.bold
            ) {
                continue;
            }
            const family = findFamily(families, now.family);
            if (family) {
                now.faces = resolveFaces(faceOptions(family), now.regular, now.bold);
                changed ||= now.faces.length > 0;
            }
        }
        return changed ? writeFonts(file, current) : undefined;
    });
}

export function writeFonts(file: string, fonts: unknown): FontSettings {
    let settings: Record<string, unknown>;
    try {
        settings = readSettings(file);
    } catch (error) {
        if (error instanceof SettingsFormatError) {
            settings = {};
        } else {
            throw error;
        }
    }
    const saved = normalizeFonts(fonts);
    mkdirSync(dirname(file), {recursive: true});
    const temporary = `${file}.${process.pid}.${randomBytes(6).toString("hex")}.tmp`;
    try {
        writeFileSync(temporary, `${JSON.stringify({...settings, fonts: saved}, null, 4)}\n`);
        renameSync(temporary, file);
    } catch (error) {
        try {
            unlinkSync(temporary);
        } catch {
        }
        throw error;
    }
    return saved;
}
