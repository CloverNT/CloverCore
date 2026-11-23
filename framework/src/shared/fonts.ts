import type {CloverFontChoice} from "./public";

export interface AliasFace {
    src: string[];
    weight: [number, number];
    italic: boolean;
}

export interface FontChoice extends CloverFontChoice {
    faces: AliasFace[];
}

export interface FontSettings {
    ui: FontChoice;
    mono: FontChoice;
}

export type FontKind = keyof FontSettings;

export interface SystemFontFace {
    style: string;
    fullName: string;
    postscriptName: string;
    weight: number;
    italic: boolean;
    features: string[];
    axes?: Array<{ tag: string; min: number; default: number; max: number }>;
    instances?: Array<{ style: string; coordinates: Record<string, number>; postscriptName?: string }>;
}

export interface SystemFontFamily {
    name: string;
    aliases: string[];
    faces: SystemFontFace[];
}

export interface FaceOption {
    key: string;
    style: string;
    weight: number;
    italic: boolean;
    src: string[];
    features: string[];
}

export const emptyChoice = (): FontChoice => ({family: "", regular: "", bold: "", features: {}, faces: []});

export const DEFAULT_FONTS: FontSettings = {ui: emptyChoice(), mono: emptyChoice()};

const MAX_LENGTH = 256;

const GENERIC_FAMILIES: ReadonlySet<string> = new Set([
    "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "math", "emoji", "fangsong",
    "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded",
]);

const QQ_MONO = ["Consolas", "Monaco", '"Andale Mono"', '"Ubuntu Mono"'];

const QQ_STACK: Record<FontKind, (families: string[]) => string[]> = {
    ui: (families) => [
        '"Color Emoji"', ...families, "system-ui", '"PingFang SC"', '"PingFangSC-Regular"', '"Microsoft YaHei"',
        '"Hiragino Sans GB"', '"Heiti SC"', '"WenQuanYi Micro Hei"', "Arial", "Helvetica", "sans-serif",
        '"Apple Braille"', '"Color Emoji Fix"',
    ],
    mono: (families) => families.length > 0
        ? [...families, ...QQ_MONO, '"PingFang SC"', '"Microsoft YaHei"', "monospace"]
        : [...QQ_MONO, "monospace"],
};

const ALIAS: Record<FontKind, string> = {ui: "CloverNT UI", mono: "CloverNT Mono"};

const CODE = "code, kbd, samp, pre, tt, .CodeMirror";
const MATH = ".katex, math";

const SELECTOR: Record<FontKind, string> = {
    ui: `:not(${CODE}, ${MATH}, :is(${CODE}, ${MATH}) *)`,
    mono: `:is(${CODE}), :is(${CODE}) *`,
};

export function isGenericFamily(name: string): boolean {
    return GENERIC_FAMILIES.has(name.toLowerCase());
}

export function parseFamilies(value: string): string[] {
    return value
        .split(",")
        .map((name) => name.trim().replace(/^(["'])(.*)\1$/s, "$2").replace(/[\u0000-\u001f\u007f]/g, "").trim())
        .filter(Boolean);
}

function cssString(value: string): string {
    return `"${value.replace(/[\u0000-\u001f\u007f]/g, "").replace(/["\\]/g, "\\$&")}"`;
}

export function cssFamily(name: string): string {
    return isGenericFamily(name) ? name.toLowerCase() : cssString(name);
}

export function fontStack(choice: FontChoice, kind: FontKind, alias: string = ALIAS[kind]): string {
    const families = parseFamilies(choice.family).map(cssFamily);
    return QQ_STACK[kind](choice.faces.length > 0 ? [cssString(alias), ...families] : families).join(", ");
}

function fontFaces(choice: FontChoice, alias: string): string[] {
    return choice.faces.map(({src, weight, italic}) =>
        `@font-face { font-family: ${cssString(alias)}; src: ${src.map((name) => `local(${cssString(name)})`).join(", ")}; ` +
        `font-weight: ${weight[0]} ${weight[1]}; font-style: ${italic ? "italic" : "normal"}; }`,
    );
}

function declarations(choice: FontChoice, kind: FontKind, alias: string): string {
    const lines = [`font-family: ${fontStack(choice, kind, alias)} !important;`];
    const features = Object.entries(choice.features).map(([tag, on]) => `${cssString(tag)} ${on ? 1 : 0}`);
    if (features.length > 0) {
        lines.push(`font-feature-settings: ${features.join(", ")} !important;`);
    }
    if (choice.faces.some((face) => face.weight[1] >= 600)) {
        lines.push("font-synthesis-weight: none !important;");
    }
    return lines.join(" ");
}

export function fontCss(fonts: FontSettings): string {
    const rules: string[] = [];
    for (const kind of ["ui", "mono"] as const) {
        const choice = fonts[kind];
        if (parseFamilies(choice.family).length > 0) {
            rules.push(...fontFaces(choice, ALIAS[kind]), `${SELECTOR[kind]} { ${declarations(choice, kind, ALIAS[kind])} }`);
        }
    }
    return rules.join("\n");
}

export function previewCss(choice: FontChoice, kind: FontKind, selector: string): string {
    const alias = `CloverNT Preview ${kind}`;
    return [...fontFaces(choice, alias), `${selector} { ${declarations(choice, kind, alias)} }`].join("\n");
}

const isVariable = (face: SystemFontFace) => face.axes?.some((axis) => axis.tag === "wght") === true;

export function faceOptions(family: SystemFontFamily): FaceOption[] {
    return family.faces.filter((face) => !isVariable(face)).map((face) => ({
        key: face.fullName,
        style: face.style,
        weight: face.weight,
        italic: face.italic,
        src: [face.fullName, face.postscriptName].filter(Boolean),
        features: face.features,
    }));
}

export function isVariableFamily(family: SystemFontFamily): boolean {
    return family.faces.length > 0 && family.faces.every(isVariable);
}

export function familyFeatures(family: SystemFontFamily, regularKey: string): string[] {
    const options = faceOptions(family);
    const regular = options.find((option) => option.key === regularKey) ?? autoRegular(options);
    return regular?.features ?? family.faces.find((face) => !face.italic)?.features ?? family.faces[0]?.features ?? [];
}

const REGULAR_STYLES = ["regular", "normal", "book", "roman", "text"];
const BOLD_STYLES = ["bold", "semibold", "demibold", "semi bold", "demi bold"];

function byStyle(options: FaceOption[], styles: string[]): FaceOption | undefined {
    for (const style of styles) {
        const match = options.find((option) => option.style.toLowerCase() === style);
        if (match) {
            return match;
        }
    }
    return undefined;
}

function closest(options: FaceOption[], weight: number): FaceOption | undefined {
    return options.reduce<FaceOption | undefined>(
        (best, option) => (!best || Math.abs(option.weight - weight) < Math.abs(best.weight - weight) ? option : best),
        undefined,
    );
}

export function autoRegular(options: FaceOption[]): FaceOption | undefined {
    const upright = options.filter((option) => !option.italic);
    return byStyle(upright, REGULAR_STYLES) ?? closest(upright, 400);
}

export function autoBold(options: FaceOption[], regular: FaceOption): FaceOption | undefined {
    const heavier = options.filter((option) => !option.italic && option.weight > regular.weight);
    return byStyle(heavier, BOLD_STYLES) ?? closest(heavier.filter((option) => option.weight >= regular.weight + 150), regular.weight + 300);
}

function italicOf(options: FaceOption[], upright: FaceOption): FaceOption | undefined {
    const italics = options.filter((option) => option.italic);
    const named = upright.style.toLowerCase() === "regular" ? "italic" : `${upright.style} italic`.toLowerCase();
    return italics.find((option) => option.style.toLowerCase() === named) ?? italics.find((option) => option.weight === upright.weight);
}

export function resolveFaces(options: FaceOption[], regularKey: string, boldKey: string): AliasFace[] {
    const regular = options.find((option) => option.key === regularKey) ?? autoRegular(options);
    if (!regular) {
        return [];
    }
    const bold = options.find((option) => option.key === boldKey) ?? autoBold(options, regular);
    const medium = options.find((option) =>
        !option.italic && option.style.toLowerCase() === "medium" && option.weight > regular.weight && (!bold || option.weight < bold.weight));
    const tiers: Array<[FaceOption, [number, number]]> = [[regular, [1, medium ? 449 : 549]]];
    if (medium) {
        tiers.push([medium, [450, 549]]);
    }
    if (bold) {
        tiers.push([bold, [550, 1000]]);
    }
    return tiers.flatMap(([face, weight]) => {
        const italic = italicOf(options, face);
        return [{src: face.src, weight, italic: false}, ...(italic ? [{src: italic.src, weight, italic: true}] : [])];
    });
}

export function findFamily(families: readonly SystemFontFamily[], typed: string): SystemFontFamily | undefined {
    const [first] = parseFamilies(typed);
    if (!first) {
        return undefined;
    }
    const name = first.toLowerCase();
    return families.find((family) => family.name.toLowerCase() === name || family.aliases.some((alias) => alias.toLowerCase() === name));
}

const FEATURE_TAG = /^[a-zA-Z0-9 ]{4}$/;
const MAX_FEATURES = 64;
const MAX_FACES = 12;

export function normalizeChoice(value: unknown): FontChoice {
    if (typeof value === "string") {
        return {...emptyChoice(), family: value.trim().slice(0, MAX_LENGTH)};
    }
    const record = (typeof value === "object" && value !== null ? value : {}) as Record<string, unknown>;
    const text = (field: unknown) => (typeof field === "string" ? field.trim().slice(0, MAX_LENGTH) : "");
    const features = Object.fromEntries(
        Object.entries(typeof record.features === "object" && record.features !== null ? record.features : {})
            .filter(([tag, on]) => FEATURE_TAG.test(tag) && typeof on === "boolean")
            .slice(0, MAX_FEATURES),
    ) as Record<string, boolean>;
    const faces = (Array.isArray(record.faces) ? record.faces : []).slice(0, MAX_FACES).flatMap((face: unknown): AliasFace[] => {
        const {
            src,
            weight,
            italic
        } = (typeof face === "object" && face !== null ? face : {}) as Record<string, unknown>;
        const names = Array.isArray(src) ? src.filter((name): name is string => typeof name === "string" && name.length > 0).map(text) : [];
        const range = Array.isArray(weight) && weight.length === 2 && weight.every((w) => Number.isFinite(w) && w >= 1 && w <= 1000)
            ? [Math.round(weight[0]), Math.round(weight[1])] as [number, number]
            : undefined;
        return names.length > 0 && range && range[0] <= range[1] ? [{
            src: names.slice(0, 2),
            weight: range,
            italic: italic === true
        }] : [];
    });
    return {family: text(record.family), regular: text(record.regular), bold: text(record.bold), features, faces};
}

export function choiceOf({family, regular, bold, features}: CloverFontChoice): CloverFontChoice {
    return {family, regular, bold, features: {...features}};
}

export function normalizeFonts(value: unknown): FontSettings {
    const record = (typeof value === "object" && value !== null ? value : {}) as Record<string, unknown>;
    return {ui: normalizeChoice(record.ui), mono: normalizeChoice(record.mono)};
}
