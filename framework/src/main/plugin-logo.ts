import {readFileSync, realpathSync, statSync} from "node:fs";
import {extname, isAbsolute, resolve} from "node:path";

import {isInside} from "./paths";

const MAX_BYTES = 256 * 1024;
const PNG_SIGNATURE = Buffer.from("89504e470d0a1a0a", "hex");

const SIGNATURES = {
    png: (bytes: Buffer) => bytes.subarray(0, 8).equals(PNG_SIGNATURE),
    jpeg: (bytes: Buffer) => bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff,
    webp: (bytes: Buffer) => bytes.toString("latin1", 0, 4) === "RIFF" && bytes.toString("latin1", 8, 12) === "WEBP",
};

const FORMAT_BY_EXTENSION = new Map<string, keyof typeof SIGNATURES>([
    [".png", "png"],
    [".jpg", "jpeg"],
    [".jpeg", "jpeg"],
    [".webp", "webp"],
]);

export function readPluginLogo(pluginDir: string, logo: string): string | undefined {
    const format = FORMAT_BY_EXTENSION.get(extname(logo).toLowerCase());
    // Drive letters, URL schemes and NTFS streams all contain a colon.
    if (!format || isAbsolute(logo) || logo.includes(":") || logo.split(/[\\/]/).includes("..")) {
        return undefined;
    }
    try {
        const root = realpathSync(pluginDir);
        const file = realpathSync(resolve(root, logo));
        if (!isInside(root, file)) {
            return undefined;
        }
        const stats = statSync(file);
        if (!stats.isFile() || stats.size === 0 || stats.size > MAX_BYTES) {
            return undefined;
        }
        const bytes = readFileSync(file);
        return SIGNATURES[format](bytes) ? `data:image/${format};base64,${bytes.toString("base64")}` : undefined;
    } catch {
        return undefined; // missing or unreadable
    }
}
