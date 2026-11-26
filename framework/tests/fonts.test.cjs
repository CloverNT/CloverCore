const {after, before, test} = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const {tmpdir} = require("node:os");
const {join, resolve} = require("node:path");
const {buildSync} = require("esbuild");

let scratch;
let fonts;
before(() => {
    scratch = fs.mkdtempSync(join(tmpdir(), "clover-fonts-"));
    const bundle = join(scratch, "fonts.cjs");
    buildSync({
        stdin: {
            resolveDir: resolve(__dirname, "../src"),
            contents: [
                'export * from "./shared/fonts";',
                'export {completeFonts, readFonts, saveFonts, settingsFile, writeFonts} from "./main/settings";',
                'export {scanFonts} from "./main/system-fonts";',
            ].join("\n"),
        },
        platform: "node", bundle: true, outfile: bundle, format: "cjs",
    });
    fonts = require(bundle);
});
after(() => fs.rmSync(scratch, {recursive: true, force: true}));

/** Minimal sfnt fonts: a name table (Windows, US English), OS/2 weight and italic bit, GSUB features, fvar. */
const u16 = (value) => { const b = Buffer.alloc(2); b.writeUInt16BE(value); return b; };
const fixed = (value) => { const b = Buffer.alloc(4); b.writeInt32BE(Math.round(value * 65536)); return b; };
function nameTable(names) {
    const entries = Object.entries(names).map(([id, value]) => ({id: Number(id), data: Buffer.from(value, "utf16le").swap16()}));
    const storage = 6 + entries.length * 12;
    const records = [];
    let offset = 0;
    for (const {id, data} of entries) {
        records.push(Buffer.concat([u16(3), u16(1), u16(0x0409), u16(id), u16(data.length), u16(offset)]));
        offset += data.length;
    }
    return Buffer.concat([u16(0), u16(entries.length), u16(storage), ...records, ...entries.map((e) => e.data)]);
}
function os2Table(weight, italic) {
    const table = Buffer.alloc(78);
    table.writeUInt16BE(weight, 4);
    table.writeUInt16BE(italic ? 0x01 : 0x40, 62);
    return table;
}
function gsubTable(tags) {
    return Buffer.concat([u16(1), u16(0), u16(0), u16(10), u16(0), u16(tags.length), ...tags.map((tag) => Buffer.concat([Buffer.from(tag, "latin1"), u16(0)]))]);
}
function fvarTable(axis, instances) {
    const axisRecord = Buffer.concat([Buffer.from(axis.tag, "latin1"), fixed(axis.min), fixed(axis.default), fixed(axis.max), u16(0), u16(256)]);
    const records = instances.map(({nameId, value}) => Buffer.concat([u16(nameId), u16(0), fixed(value)]));
    return Buffer.concat([u16(1), u16(0), u16(16), u16(2), u16(1), u16(20), u16(instances.length), u16(8), axisRecord, ...records]);
}
function sfnt(tables) {
    const entries = Object.entries(tables);
    let offset = 12 + entries.length * 16;
    const directory = [];
    const bodies = [];
    for (const [tag, data] of entries) {
        directory.push(Buffer.concat([Buffer.from(tag.padEnd(4), "latin1"), Buffer.alloc(4), Buffer.alloc(4), Buffer.alloc(4)]));
        directory.at(-1).writeUInt32BE(offset, 8);
        directory.at(-1).writeUInt32BE(data.length, 12);
        const padded = Buffer.alloc((data.length + 3) & ~3);
        data.copy(padded);
        bodies.push(padded);
        offset += padded.length;
    }
    const header = Buffer.concat([Buffer.from([0, 1, 0, 0]), u16(entries.length), u16(0), u16(0), u16(0)]);
    return Buffer.concat([header, ...directory, ...bodies]);
}
function face({family, style, full, ps, weight, italic = false, legacy = family, features = ["liga"]}) {
    return sfnt({name: nameTable({1: legacy, 2: italic ? "Italic" : "Regular", 4: full, 6: ps, 16: family, 17: style}), "OS/2": os2Table(weight, italic), GSUB: gsubTable(features)});
}
function collection(fontsInside) {
    const header = Buffer.concat([Buffer.from("ttcf", "latin1"), u16(1), u16(0), Buffer.alloc(4)]);
    header.writeUInt32BE(fontsInside.length, 8);
    const offsets = [];
    let offset = header.length + fontsInside.length * 4;
    const rebased = fontsInside.map((font) => {
        offsets.push(offset);
        const copy = Buffer.from(font);
        const count = copy.readUInt16BE(4);
        for (let i = 0; i < count; i++) copy.writeUInt32BE(copy.readUInt32BE(12 + i * 16 + 8) + offset, 12 + i * 16 + 8);
        offset += copy.length;
        return copy;
    });
    return Buffer.concat([header, ...offsets.map((value) => { const b = Buffer.alloc(4); b.writeUInt32BE(value); return b; }), ...rebased]);
}

let families;
before(async () => {
    const dir = join(scratch, "installed");
    fs.mkdirSync(join(dir, "nested"), {recursive: true});
    // Weights like MiSans, which strays from the CSS scale.
    fs.writeFileSync(join(dir, "TestSans-Regular.ttf"), face({family: "Test Sans", style: "Regular", full: "Test Sans", ps: "TestSans-Regular", weight: 330, features: ["liga", "ss01", "cv02", "zero"]}));
    fs.writeFileSync(join(dir, "TestSans-Normal.ttf"), face({family: "Test Sans", style: "Normal", full: "Test Sans Normal", ps: "TestSans-Normal", weight: 305, legacy: "Test Sans Normal"}));
    fs.writeFileSync(join(dir, "TestSans-Medium.ttf"), face({family: "Test Sans", style: "Medium", full: "Test Sans Medium", ps: "TestSans-Medium", weight: 380, legacy: "Test Sans Medium"}));
    fs.writeFileSync(join(dir, "nested", "TestSans-Bold.otf"), face({family: "Test Sans", style: "Bold", full: "Test Sans Bold", ps: "TestSans-Bold", weight: 630}));
    fs.writeFileSync(join(dir, "TestSans-Heavy.ttf"), face({family: "Test Sans", style: "Heavy", full: "Test Sans Heavy", ps: "TestSans-Heavy", weight: 700, legacy: "Test Sans Heavy"}));
    fs.writeFileSync(join(dir, "TestSans-Italic.ttf"), face({family: "Test Sans", style: "Italic", full: "Test Sans Italic", ps: "TestSans-Italic", weight: 330, italic: true}));
    fs.writeFileSync(join(dir, "TestMono.ttc"), collection([
        face({family: "Test Mono", style: "Regular", full: "Test Mono", ps: "TestMono-Regular", weight: 400}),
        face({family: "Test Mono", style: "Bold", full: "Test Mono Bold", ps: "TestMono-Bold", weight: 700}),
    ]));
    fs.writeFileSync(join(dir, "TestVF.ttf"), sfnt({
        name: nameTable({1: "Test VF", 2: "Regular", 4: "Test VF", 6: "TestVF", 16: "Test VF", 17: "Regular", 256: "Weight", 257: "Light", 258: "Regular", 259: "Bold"}),
        "OS/2": os2Table(400, false),
        fvar: fvarTable({tag: "wght", min: 100, default: 400, max: 900}, [{nameId: 257, value: 300}, {nameId: 258, value: 400}, {nameId: 259, value: 700}]),
    }));
    fs.writeFileSync(join(dir, "broken.ttf"), Buffer.from("not a font"));
    fs.writeFileSync(join(dir, "readme.txt"), "fonts");
    families = await fonts.scanFonts([dir, join(scratch, "missing")]);
});

const family = (name) => families.find((f) => f.name === name);
const choice = (fields) => ({...fonts.emptyChoice(), ...fields});

test("the scan groups faces into typographic families, from collections and subfolders, skipping other files", () => {
    assert.deepEqual(families.map((f) => f.name), ["Test Mono", "Test Sans", "Test VF"]);
    const sans = family("Test Sans");
    assert.deepEqual(sans.faces.map((f) => [f.style, f.weight, f.italic]), [
        ["Normal", 305, false], ["Regular", 330, false], ["Medium", 380, false], ["Bold", 630, false], ["Heavy", 700, false], ["Italic", 330, true],
    ]);
    assert.ok(sans.aliases.includes("Test Sans Medium"), "older per-weight names find the family");
    assert.deepEqual(sans.faces[1].features, ["cv02", "liga", "ss01", "zero"]);
    assert.deepEqual(family("Test Mono").faces.map((f) => f.fullName), ["Test Mono", "Test Mono Bold"]);
    const vf = family("Test VF").faces[0];
    assert.deepEqual(vf.axes, [{tag: "wght", min: 100, default: 400, max: 900}]);
    assert.deepEqual(vf.instances.map((i) => [i.style, i.coordinates.wght]), [["Light", 300], ["Regular", 400], ["Bold", 700]]);
    assert.equal(fonts.findFamily(families, "test sans medium, Arial"), family("Test Sans"));
});

test("weights map to the faces named regular, medium and bold, whatever the font's own weights", () => {
    const options = fonts.faceOptions(family("Test Sans"));
    assert.deepEqual(fonts.resolveFaces(options, "", ""), [
        {src: ["Test Sans", "TestSans-Regular"], weight: [1, 449], italic: false},
        {src: ["Test Sans Italic", "TestSans-Italic"], weight: [1, 449], italic: true},
        {src: ["Test Sans Medium", "TestSans-Medium"], weight: [450, 549], italic: false},
        {src: ["Test Sans Bold", "TestSans-Bold"], weight: [550, 1000], italic: false},
    ]);
    const picked = fonts.resolveFaces(options, "Test Sans Normal", "Test Sans Heavy");
    assert.deepEqual(picked.map((f) => [f.src[0], f.weight]), [["Test Sans Normal", [1, 449]], ["Test Sans Medium", [450, 549]], ["Test Sans Heavy", [550, 1000]]]);
    const mono = fonts.resolveFaces(fonts.faceOptions(family("Test Mono")), "", "");
    assert.deepEqual(mono.map((f) => [f.src[0], f.weight]), [["Test Mono", [1, 549]], ["Test Mono Bold", [550, 1000]]]);
});

test("a variable font has no faces to pick, its weight axis follows the CSS weight", () => {
    const vf = family("Test VF");
    assert.ok(fonts.isVariableFamily(vf));
    assert.deepEqual(fonts.faceOptions(vf), []);
    assert.deepEqual(fonts.resolveFaces(fonts.faceOptions(vf), "", ""), []);
    assert.ok(!fonts.isVariableFamily(family("Test Sans")));
    assert.deepEqual(fonts.familyFeatures(family("Test Sans"), ""), ["cv02", "liga", "ss01", "zero"]);
    assert.deepEqual(fonts.familyFeatures(family("Test Sans"), "Test Sans Bold"), ["liga"]);
});

test("font names are split on commas and lose their quotes and control characters", () => {
    assert.deepEqual(fonts.parseFamilies(" \"Microsoft YaHei\" , 'Noto Sans SC',,  Inter "), ["Microsoft YaHei", "Noto Sans SC", "Inter"]);
    assert.deepEqual(fonts.parseFamilies("Fira\u0000 Code\n"), ["Fira Code"]);
    assert.deepEqual(fonts.parseFamilies("  "), []);
});

test("names become CSS strings that cannot end early, while generic families stay keywords", () => {
    assert.equal(fonts.cssFamily('a"b\\c'), '"a\\"b\\\\c"');
    assert.equal(fonts.cssFamily("Monospace"), "monospace");
    const css = fonts.fontCss({ui: choice({family: 'Evil"} body { display: none } x {'}), mono: choice({})});
    assert.ok(css.includes('"Evil\\"} body { display: none } x {"'));
    assert.equal(css.split("!important").length, 2, "one declaration in one rule");
});

test("no font names means no stylesheet", () => {
    assert.equal(fonts.fontCss({ui: choice({}), mono: choice({family: " , "})}), "");
});

test("without faces the typed fonts come before QQ's, leaving code and math alone", () => {
    const css = fonts.fontCss({ui: choice({family: "HarmonyOS Sans SC"}), mono: choice({})});
    assert.ok(css.startsWith(":not(code, kbd, samp, pre, tt, .CodeMirror, .katex, math, :is(code, kbd, samp, pre, tt, .CodeMirror, .katex, math) *) { font-family: "));
    assert.ok(css.includes('font-family: "Color Emoji", "HarmonyOS Sans SC", system-ui, '));
    assert.ok(css.endsWith(' sans-serif, "Apple Braille", "Color Emoji Fix" !important; }'));
});

test("faces become an alias family; a bold face turns fake bold off, and features are set", () => {
    const faces = fonts.resolveFaces(fonts.faceOptions(family("Test Mono")), "", "");
    const css = fonts.fontCss({ui: choice({}), mono: choice({family: "Test Mono, Cascadia Code", faces, features: {calt: false, ss01: true}})});
    assert.equal(css, [
        '@font-face { font-family: "CloverNT Mono"; src: local("Test Mono"), local("TestMono-Regular"); font-weight: 1 549; font-style: normal; }',
        '@font-face { font-family: "CloverNT Mono"; src: local("Test Mono Bold"), local("TestMono-Bold"); font-weight: 550 1000; font-style: normal; }',
        ':is(code, kbd, samp, pre, tt, .CodeMirror), :is(code, kbd, samp, pre, tt, .CodeMirror) * { ' +
        'font-family: "CloverNT Mono", "Test Mono", "Cascadia Code", Consolas, Monaco, "Andale Mono", "Ubuntu Mono", "PingFang SC", "Microsoft YaHei", monospace !important; ' +
        'font-feature-settings: "calt" 0, "ss01" 1 !important; font-synthesis-weight: none !important; }',
    ].join("\n"));
});

test("an empty field previews exactly QQ's own stack", () => {
    assert.equal(fonts.fontStack(choice({}), "mono"), 'Consolas, Monaco, "Andale Mono", "Ubuntu Mono", monospace');
    assert.ok(fonts.fontStack(choice({family: " "}), "ui").startsWith('"Color Emoji", system-ui, "PingFang SC"'));
});

test("saved settings keep only well-formed values, and a plain string is a font list", () => {
    assert.deepEqual(fonts.normalizeFonts({ui: "  Inter ", mono: 5}), {ui: choice({family: "Inter"}), mono: choice({})});
    assert.deepEqual(fonts.normalizeFonts(null), {ui: choice({}), mono: choice({})});
    const normalized = fonts.normalizeFonts({ui: {
        family: "x".repeat(1000), regular: 1, features: {liga: false, "bad tag": true, zero: "yes"},
        faces: [{src: ["A", 2], weight: [1, 549]}, {src: [], weight: [1, 2]}, {src: ["B"], weight: [600, 2000]}],
    }});
    assert.equal(normalized.ui.family.length, 256);
    assert.equal(normalized.ui.regular, "");
    assert.deepEqual(normalized.ui.features, {liga: false});
    assert.deepEqual(normalized.ui.faces, [{src: ["A"], weight: [1, 549], italic: false}]);
});

test("fonts are saved beside the file's other settings and read back", () => {
    const coreDir = fs.mkdtempSync(join(scratch, "core-"));
    const file = fonts.settingsFile(coreDir);
    assert.equal(file, join(coreDir, "config", "ui.json"));
    assert.deepEqual(fonts.readFonts(file), {ui: choice({}), mono: choice({})}, "no file means QQ's fonts");
    fs.mkdirSync(join(coreDir, "config"));
    fs.writeFileSync(file, JSON.stringify({other: 1, fonts: {ui: "MiSans"}}));
    fonts.writeFonts(file, {ui: choice({family: " Inter "}), mono: choice({})});
    assert.deepEqual(JSON.parse(fs.readFileSync(file, "utf8")), {other: 1, fonts: {ui: choice({family: "Inter"}), mono: choice({})}});
});

test("fonts saved without faces get them once the installed fonts are known", async () => {
    const coreDir = fs.mkdtempSync(join(scratch, "core-"));
    const file = fonts.settingsFile(coreDir);
    fs.mkdirSync(join(coreDir, "config"));
    fs.writeFileSync(file, JSON.stringify({fonts: {ui: "Test Sans", mono: "Not Installed"}}));
    const saved = await fonts.completeFonts(file, async () => families);
    assert.deepEqual(saved.ui.faces.map((f) => f.src[0]), ["Test Sans", "Test Sans Italic", "Test Sans Medium", "Test Sans Bold"]);
    assert.deepEqual(saved.mono.faces, []);
    assert.deepEqual(fonts.readFonts(file), saved);
    assert.equal(await fonts.completeFonts(file, async () => families), undefined, "a font that is not installed changes nothing");
});

test("saving resolves the faces of the fonts given, from the installed fonts, and keeps the other", async () => {
    const coreDir = fs.mkdtempSync(join(scratch, "core-"));
    const file = fonts.settingsFile(coreDir);
    let scans = 0;
    const list = async () => (scans++, families);
    const first = await fonts.saveFonts(file, {ui: {family: "Test Sans", features: {ss01: true}}, mono: {family: "Not Installed"}}, list);
    assert.deepEqual(first.ui.faces.map((f) => f.src[0]), ["Test Sans", "Test Sans Italic", "Test Sans Medium", "Test Sans Bold"]);
    assert.deepEqual(first.ui.features, {ss01: true});
    assert.deepEqual(first.mono, choice({family: "Not Installed"}));
    const forged = [{src: ["Forged"], weight: [1, 1000], italic: false}];
    const second = await fonts.saveFonts(file, {mono: {family: "Test Sans", faces: forged}}, list);
    assert.deepEqual(second.ui, first.ui, "the font not given is kept");
    assert.ok(second.mono.faces.length > 0 && second.mono.faces.every((f) => f.src[0] !== "Forged"), "faces come from the installed fonts");
    assert.deepEqual(fonts.readFonts(file), second);
    await fonts.saveFonts(file, {ui: {family: ""}}, list);
    assert.equal(scans, 2, "clearing a font needs no scan");
    assert.deepEqual(fonts.choiceOf(second.mono), {family: "Test Sans", regular: "", bold: "", features: {}});
});

test("a corrupt settings file is reported, and the next save replaces it", () => {
    const coreDir = fs.mkdtempSync(join(scratch, "core-"));
    const file = fonts.settingsFile(coreDir);
    fs.mkdirSync(join(coreDir, "config"));
    fs.writeFileSync(file, "{not json");
    assert.throws(() => fonts.readFonts(file), /不是有效的 JSON/);
    fs.writeFileSync(file, "[]");
    assert.throws(() => fonts.readFonts(file), /不是 JSON 对象/);
    fonts.writeFonts(file, {ui: choice({family: "Inter"}), mono: choice({})});
    assert.equal(fonts.readFonts(file).ui.family, "Inter");
});

test("malformed fonts with absurd table lengths or TTC counts are skipped safely", async () => {
    const badDir = fs.mkdtempSync(join(scratch, "bad-fonts-"));
    // TTC header claiming 0xffffffff faces
    const badTtc = Buffer.alloc(16);
    badTtc.write("ttcf", 0, "ascii");
    badTtc.writeUInt32BE(0xffffffff, 8);
    fs.writeFileSync(join(badDir, "huge.ttc"), badTtc);
    // Single font with name table claiming 1 GiB length
    const badTtf = Buffer.alloc(32);
    badTtf.writeUInt32BE(0x00010000, 0);
    badTtf.writeUInt16BE(1, 4);
    badTtf.write("name", 12, "ascii");
    badTtf.writeUInt32BE(28, 20);
    badTtf.writeUInt32BE(0x40000000, 24);
    fs.writeFileSync(join(badDir, "huge-table.ttf"), badTtf);
    assert.deepEqual(await fonts.scanFonts([badDir]), []);
});

test("concurrent saveFonts and completeFonts serialize cleanly without clobbering newer saves", async () => {
    const coreDir = fs.mkdtempSync(join(scratch, "race-"));
    const file = fonts.settingsFile(coreDir);
    fs.mkdirSync(join(coreDir, "config"));
    fs.writeFileSync(file, JSON.stringify({fonts: {ui: "Test Sans", mono: ""}}));

    let releaseScan;
    const slowList = () => new Promise((res) => {
        releaseScan = () => res(families);
    });
    const completing = fonts.completeFonts(file, slowList);
    // While completeFonts is scanning, user concurrently updates ui and mono
    const saveUi = fonts.saveFonts(file, {ui: {family: "Test Mono"}}, async () => families);
    const saveMono = fonts.saveFonts(file, {mono: {family: "Test Sans"}}, async () => families);
    await Promise.all([saveUi, saveMono]);
    releaseScan();
    assert.equal(await completing, undefined, "completeFonts must not clobber a newer save");
    const final = fonts.readFonts(file);
    assert.equal(final.ui.family, "Test Mono");
    assert.equal(final.mono.family, "Test Sans");
    assert.ok(final.ui.faces.length > 0 && final.mono.faces.length > 0);
});
