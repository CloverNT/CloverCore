import {mkdirSync, readFileSync, renameSync, writeFileSync} from "node:fs";
import {dirname, join} from "node:path";

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
        throw new Error(`${file} 不是有效的 JSON: ${(error as Error).message}`);
    }
    if (typeof settings !== "object" || settings === null || Array.isArray(settings)) {
        throw new Error(`${file} 不是 JSON 对象`);
    }
    return settings as Record<string, unknown>;
}

export function readFonts(file: string): FontSettings {
    const {fonts} = readSettings(file);
    return fonts === undefined ? normalizeFonts(DEFAULT_FONTS) : normalizeFonts(fonts);
}

export async function saveFonts(file: string, choices: unknown, list: () => Promise<SystemFontFamily[]>): Promise<FontSettings> {
    const record = (typeof choices === "object" && choices !== null ? choices : {}) as Record<string, unknown>;
    const chosen = (["ui", "mono"] as const).filter((kind) => record[kind] !== undefined).map((kind) => [kind, normalizeChoice(record[kind])] as const);
    const families = chosen.some(([, choice]) => choice.family) ? await list() : [];
    // Read after the scan, so that a save made meanwhile is kept.
    let fonts: FontSettings;
    try {
        fonts = readFonts(file);
    } catch {
        fonts = normalizeFonts(DEFAULT_FONTS);
    }
    for (const [kind, choice] of chosen) {
        const family = choice.family ? findFamily(families, choice.family) : undefined;
        fonts[kind] = {...choice, faces: family ? resolveFaces(faceOptions(family), choice.regular, choice.bold) : []};
    }
    return writeFonts(file, fonts);
}

export async function completeFonts(file: string, list: () => Promise<SystemFontFamily[]>): Promise<FontSettings | undefined> {
    const fonts = readFonts(file);
    const pending = (["ui", "mono"] as const).filter((kind) => fonts[kind].family && fonts[kind].faces.length === 0);
    if (pending.length === 0) {
        return undefined;
    }
    const families = await list();
    let changed = false;
    for (const kind of pending) {
        const family = findFamily(families, fonts[kind].family);
        if (family) {
            fonts[kind].faces = resolveFaces(faceOptions(family), fonts[kind].regular, fonts[kind].bold);
            changed ||= fonts[kind].faces.length > 0;
        }
    }
    return changed ? writeFonts(file, fonts) : undefined;
}

export function writeFonts(file: string, fonts: unknown): FontSettings {
    let settings: Record<string, unknown>;
    try {
        settings = readSettings(file);
    } catch {
        settings = {};
    }
    const saved = normalizeFonts(fonts);
    mkdirSync(dirname(file), {recursive: true});
    const temporary = `${file}.tmp`;
    writeFileSync(temporary, `${JSON.stringify({...settings, fonts: saved}, null, 4)}\n`);
    renameSync(temporary, file);
    return saved;
}
