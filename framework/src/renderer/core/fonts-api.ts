import {choiceOf, normalizeChoice, type FontSettings, type SystemFontFamily} from "../../shared/fonts";
import {Channel} from "../../shared/ipc";
import type {CloverFontKind, CloverFontSettings, CloverFontsApi, CloverInstalledFont} from "../../shared/public";
import {invoke, subscribe} from "./bridge";

const KINDS: ReadonlyArray<unknown> = ["ui", "mono"];

function publicFonts(fonts: FontSettings): CloverFontSettings {
    return {ui: choiceOf(fonts.ui), mono: choiceOf(fonts.mono)};
}

function publicFamily({name, aliases, faces}: SystemFontFamily): CloverInstalledFont {
    return {
        name,
        aliases: [...aliases],
        faces: faces.map((face) => ({
            style: face.style,
            fullName: face.fullName,
            postscriptName: face.postscriptName,
            weight: face.weight,
            italic: face.italic,
            variable: face.axes?.some((axis) => axis.tag === "wght") === true,
            features: [...face.features],
        })),
    };
}

export function createFontsApi(): CloverFontsApi {
    return Object.freeze({
        get: async () => publicFonts(await invoke(Channel.GetFonts)),
        async set(kind: CloverFontKind, choice: { family: string }) {
            if (!KINDS.includes(kind)) {
                throw new TypeError('clovernt.fonts.set: kind must be "ui" or "mono"');
            }
            if (typeof choice?.family !== "string") {
                throw new TypeError("clovernt.fonts.set: choice.family must be a string");
            }
            return publicFonts(await invoke(Channel.SetFonts, {[kind]: choiceOf(normalizeChoice(choice))}));
        },
        installed: async () => (await invoke(Channel.ListSystemFonts)).map(publicFamily),
        onChange(listener: (fonts: CloverFontSettings) => void) {
            if (typeof listener !== "function") {
                throw new TypeError("clovernt.fonts.onChange: listener must be a function");
            }
            return subscribe(Channel.FontsChanged, (fonts) => listener(publicFonts(fonts)));
        },
    });
}
