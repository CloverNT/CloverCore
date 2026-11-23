const {after, before, test} = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const {tmpdir} = require("node:os");
const {join, resolve} = require("node:path");
const {buildSync} = require("esbuild");

let scratch;
let stylesheet;
before(() => {
    scratch = fs.mkdtempSync(join(tmpdir(), "clover-styles-"));
    const bundle = join(scratch, "stylesheet.cjs");
    buildSync({
        entryPoints: [resolve(__dirname, "../src/renderer/stylesheet.ts")],
        platform: "node", bundle: true, outfile: bundle, format: "cjs", loader: {".css": "text"},
    });
    stylesheet = require(bundle);
});
after(() => fs.rmSync(scratch, {recursive: true, force: true}));

/** The style rules of a stylesheet, looking into @media and @supports; keyframes and font faces are skipped. */
function rules(css) {
    const found = [];
    const walk = (text) => {
        let i = 0;
        while (i < text.length) {
            const open = text.indexOf("{", i);
            if (open < 0) break;
            const prelude = text.slice(i, open).trim();
            let depth = 1;
            let j = open + 1;
            while (depth > 0 && j < text.length) {
                if (text[j] === "{") depth++;
                if (text[j] === "}") depth--;
                j++;
            }
            const body = text.slice(open + 1, j - 1);
            if (/^@(media|supports)/.test(prelude)) {
                walk(body);
            } else if (!prelude.startsWith("@")) {
                found.push({selectors: prelude.split(/,(?![^(]*\))/).map((s) => s.trim()), body});
            }
            i = j;
        }
    };
    walk(css.replace(/\/\*[\s\S]*?\*\//g, ""));
    return found;
}

const read = (file) => fs.readFileSync(resolve(__dirname, "../src/renderer", file), "utf8");
const selectorsOf = (file) => rules(read(file)).flatMap((rule) => rule.selectors);

test("CloverNT's rules are not in a cascade layer, which QQ's unlayered reset would beat", () => {
    assert.ok(!/@layer\b/.test(stylesheet.css));
});

test("tokens are declared without specificity, so any rule of a theme overrides them", () => {
    const declaring = rules(stylesheet.css).filter((rule) => /--clovernt-[\w-]+\s*:/.test(rule.body));
    assert.ok(declaring.length >= 2);
    for (const rule of declaring) {
        for (const selector of rule.selectors) {
            assert.match(selector, /^:where\([^)]*\)$/, selector);
        }
    }
});

test("every CloverNT token a rule uses is declared", () => {
    const declared = new Set([...stylesheet.css.matchAll(/(--clovernt-[\w-]+)\s*:/g)].map((m) => m[1]));
    const used = new Set([...stylesheet.css.matchAll(/var\((--clovernt-[\w-]+)/g)].map((m) => m[1]));
    assert.deepEqual([...used].filter((name) => !declared.has(name)), []);
    for (const name of ["--clovernt-radius-card", "--clovernt-card-padding", "--clovernt-text-body", "--clovernt-ok-bg"]) {
        assert.ok(declared.has(name), name);
    }
});

test("only integration.css and the About page style QQ's own elements", () => {
    const files = ["core/styles/tokens.css", "core/styles/components.css", "core/styles/toast.css", "settings/styles/host.css",
        "settings/styles/panel.css", "settings/styles/overview.css", "settings/styles/plugins.css", "settings/styles/fonts.css"];
    const tokenScopes = [":where(:root)", ":where(body)", ':where(body[q-theme="dark"])'];
    for (const file of files) {
        const foreign = selectorsOf(file).filter((s) => !s.includes("clovernt") && !tokenScopes.includes(s));
        assert.deepEqual(foreign, [], file);
    }
    const qqFacing = /\.q-[\w-]+|\.nav-item|\.nav-bar|\[data-clovernt-hidden\]|\[hidden\]/;
    assert.deepEqual(selectorsOf("settings/styles/integration.css").filter((s) => !qqFacing.test(s)), []);
    for (const file of [...files, "settings/styles/integration.css", "about/about.css"]) {
        assert.ok(stylesheet.css.includes(read(file)), file + " is in the stylesheet");
    }
});
