import {open, readdir, stat, type FileHandle} from "node:fs/promises";
import {homedir} from "node:os";
import {extname, join} from "node:path";

import type {SystemFontFace, SystemFontFamily} from "../shared/fonts";

const FONT_EXTENSIONS = new Set([".ttf", ".otf", ".ttc", ".otc"]);

function fontDirectories(): string[] {
    const home = homedir();
    switch (process.platform) {
        case "win32":
            return [
                join(process.env.WINDIR ?? "C:\\Windows", "Fonts"),
                join(process.env.LOCALAPPDATA ?? join(home, "AppData", "Local"), "Microsoft", "Windows", "Fonts"),
            ];
        case "darwin":
            return ["/System/Library/Fonts", "/Library/Fonts", join(home, "Library", "Fonts")];
        default:
            return ["/usr/share/fonts", "/usr/local/share/fonts", join(home, ".local", "share", "fonts"), join(home, ".fonts")];
    }
}

async function fontFiles(directory: string): Promise<string[]> {
    const entries = await readdir(directory, {withFileTypes: true}).catch(() => []);
    const nested = await Promise.all(entries.map(async (entry) => {
        const path = join(directory, entry.name);
        if (entry.isDirectory()) {
            return fontFiles(path);
        }
        return FONT_EXTENSIONS.has(extname(entry.name).toLowerCase()) ? [path] : [];
    }));
    return nested.flat();
}

class FontFile {
    constructor(private readonly handle: FileHandle) {}

    async read(position: number, length: number): Promise<DataView> {
        const buffer = Buffer.alloc(length);
        const {bytesRead} = await this.handle.read(buffer, 0, length, position);
        if (bytesRead < length) {
            throw new Error("truncated font file");
        }
        return new DataView(buffer.buffer, buffer.byteOffset, length);
    }
}

const tag = (view: DataView, offset: number) => String.fromCharCode(
    view.getUint8(offset), view.getUint8(offset + 1), view.getUint8(offset + 2), view.getUint8(offset + 3),
);

type Tables = Map<string, { offset: number; length: number }>;

async function tableDirectory(file: FontFile, offset: number): Promise<Tables> {
    const header = await file.read(offset, 12);
    const count = header.getUint16(4);
    const records = await file.read(offset + 12, count * 16);
    const tables: Tables = new Map();
    for (let i = 0; i < count; i++) {
        tables.set(tag(records, i * 16), {offset: records.getUint32(i * 16 + 8), length: records.getUint32(i * 16 + 12)});
    }
    return tables;
}

const NAME = {family: 1, style: 2, fullName: 4, postscript: 6, typographicFamily: 16, typographicStyle: 17} as const;

async function readNames(file: FontFile, table: { offset: number; length: number }): Promise<Map<number, string[]>> {
    const view = await file.read(table.offset, table.length);
    const count = view.getUint16(2);
    const storage = view.getUint16(4);
    const ranked: Array<{ id: number; rank: number; value: string }> = [];
    for (let i = 0; i < count; i++) {
        const record = 6 + i * 12;
        const platform = view.getUint16(record);
        const encoding = view.getUint16(record + 2);
        const language = view.getUint16(record + 4);
        const id = view.getUint16(record + 6);
        const length = view.getUint16(record + 8);
        const start = storage + view.getUint16(record + 10);
        if (start + length > view.byteLength) {
            continue;
        }
        const bytes = new Uint8Array(view.buffer, view.byteOffset + start, length);
        if (platform === 3 && (encoding === 1 || encoding === 10)) {
            ranked.push({id, rank: language === 0x0409 ? 0 : 1, value: new TextDecoder("utf-16be").decode(bytes)});
        } else if (platform === 1 && encoding === 0 && language === 0) {
            ranked.push({id, rank: 2, value: new TextDecoder("latin1").decode(bytes)});
        }
    }
    const names = new Map<number, string[]>();
    for (const {id, value} of ranked.sort((a, b) => a.rank - b.rank)) {
        const trimmed = value.replace(/\0/g, "").trim();
        const list = names.get(id) ?? [];
        if (trimmed && !list.includes(trimmed)) {
            list.push(trimmed);
            names.set(id, list);
        }
    }
    return names;
}

async function readFeatures(file: FontFile, tables: Tables): Promise<string[]> {
    const features = new Set<string>();
    for (const name of ["GSUB", "GPOS"]) {
        const table = tables.get(name);
        if (!table || table.length < 10) {
            continue;
        }
        const header = await file.read(table.offset, 10);
        const listOffset = table.offset + header.getUint16(6);
        const count = (await file.read(listOffset, 2)).getUint16(0);
        const records = await file.read(listOffset + 2, count * 6);
        for (let i = 0; i < count; i++) {
            features.add(tag(records, i * 6));
        }
    }
    return [...features].sort();
}

const fixed = (view: DataView, offset: number) => view.getInt32(offset) / 65536;

interface Variation {
    axes: Array<{ tag: string; min: number; default: number; max: number }>;
    instances: Array<{ style: string; coordinates: Record<string, number>; postscriptName?: string }>;
}

async function readVariation(file: FontFile, table: { offset: number; length: number }, names: Map<number, string[]>): Promise<Variation> {
    const view = await file.read(table.offset, table.length);
    const axesOffset = view.getUint16(4);
    const axisCount = view.getUint16(8);
    const axisSize = view.getUint16(10);
    const instanceCount = view.getUint16(12);
    const instanceSize = view.getUint16(14);
    const axes = Array.from({length: axisCount}, (_, i) => {
        const at = axesOffset + i * axisSize;
        return {tag: tag(view, at), min: fixed(view, at + 4), default: fixed(view, at + 8), max: fixed(view, at + 12)};
    });
    const instancesOffset = axesOffset + axisCount * axisSize;
    const instances = Array.from({length: instanceCount}, (_, i) => {
        const at = instancesOffset + i * instanceSize;
        const coordinates = Object.fromEntries(axes.map((axis, a) => [axis.tag, fixed(view, at + 4 + a * 4)]));
        const postscriptId = instanceSize >= 6 + axisCount * 4 ? view.getUint16(at + 4 + axisCount * 4) : 0xffff;
        return {
            style: names.get(view.getUint16(at))?.[0] ?? "",
            coordinates,
            postscriptName: postscriptId !== 0xffff ? names.get(postscriptId)?.[0] : undefined,
        };
    });
    return {axes, instances};
}

async function readFace(file: FontFile, offset: number): Promise<SystemFontFace & { families: string[] } | undefined> {
    const tables = await tableDirectory(file, offset);
    const nameTable = tables.get("name");
    if (!nameTable) {
        return undefined;
    }
    const names = await readNames(file, nameTable);
    const families = [...(names.get(NAME.typographicFamily) ?? []), ...(names.get(NAME.family) ?? [])];
    const os2 = tables.get("OS/2");
    const os2View = os2 && os2.length >= 64 ? await file.read(os2.offset, 64) : undefined;
    const variation = tables.get("fvar") ? await readVariation(file, tables.get("fvar")!, names) : undefined;
    const face: SystemFontFace & { families: string[] } = {
        families: [...new Set(families)],
        style: names.get(NAME.typographicStyle)?.[0] ?? names.get(NAME.style)?.[0] ?? "Regular",
        fullName: names.get(NAME.fullName)?.[0] ?? "",
        postscriptName: names.get(NAME.postscript)?.[0] ?? "",
        weight: os2View?.getUint16(4) ?? 400,
        italic: ((os2View?.getUint16(62) ?? 0) & 0x201) !== 0,
        features: await readFeatures(file, tables),
    };
    if (variation) {
        face.axes = variation.axes;
        face.instances = variation.instances;
    }
    return face;
}

async function readFontFile(path: string): Promise<Array<SystemFontFace & { families: string[] }>> {
    const handle = await open(path, "r");
    try {
        const file = new FontFile(handle);
        const header = await file.read(0, 12);
        if (tag(header, 0) !== "ttcf") {
            const face = await readFace(file, 0);
            return face ? [face] : [];
        }
        const count = header.getUint32(8);
        const offsets = await file.read(12, count * 4);
        const faces = await Promise.all(Array.from({length: count}, (_, i) => readFace(file, offsets.getUint32(i * 4))));
        return faces.filter((face) => face !== undefined);
    } finally {
        await handle.close();
    }
}

export async function scanFonts(directories: string[]): Promise<SystemFontFamily[]> {
    const paths = (await Promise.all(directories.map(fontFiles))).flat();
    const faces = (await Promise.all(paths.map((path) => readFontFile(path).catch(() => [])))).flat();
    const families = new Map<string, SystemFontFamily>();
    for (const {families: names, ...face} of faces) {
        const [name] = names;
        if (!name || name.startsWith(".")) {
            continue;
        }
        const family = families.get(name.toLowerCase()) ?? {name, aliases: [], faces: []};
        for (const alias of names.slice(1)) {
            if (alias !== family.name && !family.aliases.includes(alias)) {
                family.aliases.push(alias);
            }
        }
        if (!family.faces.some((existing) => existing.fullName === face.fullName && existing.weight === face.weight)) {
            family.faces.push(face);
        }
        families.set(name.toLowerCase(), family);
    }
    const sorted = [...families.values()].sort((a, b) => a.name.localeCompare(b.name));
    for (const family of sorted) {
        family.faces.sort((a, b) => Number(a.italic) - Number(b.italic) || a.weight - b.weight);
    }
    return sorted;
}

let cache: { key: string; families: Promise<SystemFontFamily[]> } | undefined;

export async function listSystemFonts(): Promise<SystemFontFamily[]> {
    const directories = fontDirectories();
    const key = (await Promise.all(directories.map((dir) => stat(dir).then((s) => `${dir}@${s.mtimeMs}`, () => dir)))).join("|");
    if (cache?.key !== key) {
        cache = {key, families: scanFonts(directories)};
    }
    return cache.families;
}
