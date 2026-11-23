import {
    autoBold,
    autoRegular,
    choiceOf,
    emptyChoice,
    faceOptions,
    familyFeatures,
    findFamily,
    isGenericFamily,
    isVariableFamily,
    parseFamilies,
    previewCss,
    resolveFaces,
    type FaceOption,
    type FontChoice,
    type FontKind,
} from "../../../shared/fonts";
import type {CloverFontSettings} from "../../../shared/public";
import {alert, Dropdown, group, stateLabel, type DropdownOption} from "../../core/components";
import {h} from "../../core/dom";
import {pageElement, type View, type ViewContext} from "./context";

const KINDS: FontKind[] = ["ui", "mono"];

const TEXT: Record<FontKind, { title: string; tip: string; sample: string; bold: string }> = {
    ui: {
        title: "界面字体",
        tip: "QQ 所有窗口里的文字",
        sample: "敏捷的棕色狐狸跳过了懒狗。The quick brown fox jumps over the lazy dog. 0123456789",
        bold: "加粗的文字 Bold text",
    },
    mono: {
        title: "等宽字体",
        tip: "聊天里的代码块，以及其他等宽显示的文字",
        sample: 'const greeting = "你好，CloverNT"; // 0O 1lI {} [] => != === ->',
        bold: "return greeting;",
    },
};

const DEFAULT_ON: ReadonlySet<string> = new Set(["liga", "calt", "clig", "kern"]);

const FEATURE_NAMES: Record<string, string> = {
    liga: "标准连字",
    calt: "上下文替代",
    clig: "上下文连字",
    dlig: "自由连字",
    kern: "字距调整",
    zero: "斜线零",
    tnum: "等宽数字",
    pnum: "比例数字",
    lnum: "齐线数字",
    onum: "旧式数字",
    case: "大写敏感形式",
    frac: "分数",
    ordn: "序数",
    sups: "上标",
    subs: "下标",
    smcp: "小型大写",
    c2sc: "大写转小型大写",
    salt: "风格替代",
    swsh: "花饰字",
    palt: "比例字宽",
    halt: "半角标点",
};

const FEATURE_GROUPS: Array<{ caption: string; test: (tag: string) => boolean; label: (tag: string) => string }> = [
    {caption: "常用", test: (tag) => tag in FEATURE_NAMES, label: (tag) => FEATURE_NAMES[tag]},
    {caption: "风格集", test: (tag) => /^ss\d\d$/.test(tag), label: (tag) => tag},
    {caption: "字符变体", test: (tag) => /^cv\d\d$/.test(tag), label: (tag) => tag},
];

const featureOrder = (tag: string) => {
    const index = Object.keys(FEATURE_NAMES).indexOf(tag);
    return index < 0 ? tag : String(index).padStart(3, "0");
};

function featureOn(choice: FontChoice, tag: string): boolean {
    return choice.features[tag] ?? DEFAULT_ON.has(tag);
}

function faceLabel(option: FaceOption): string {
    return `${option.style} ${option.weight}`;
}

interface Editor {
    kind: FontKind;
    choice: FontChoice;
    input: HTMLInputElement;
    nameTip: HTMLElement;
    regular: Dropdown;
    bold: Dropdown;
    features: HTMLElement;
    detail: HTMLElement[];
    preview: HTMLElement;
}

export function fontsView({store}: ViewContext): View {
    const previewStyle = h("style");
    const notice = h("div");
    const editors = {} as Record<FontKind, Editor>;

    const optionsFor = (typed: string): FaceOption[] => {
        const family = store.systemFonts && findFamily(store.systemFonts, typed);
        return family ? faceOptions(family) : [];
    };

    const resolve = (choice: FontChoice): FontChoice => {
        const faces = optionsFor(choice.family);
        return {...choice, faces: faces.length > 0 ? resolveFaces(faces, choice.regular, choice.bold) : []};
    };

    const edited = (editor: Editor): FontChoice => resolve({...editor.choice, family: editor.input.value.trim()});

    const save = () => {
        const fonts = {} as CloverFontSettings;
        for (const kind of KINDS) {
            editors[kind].choice = edited(editors[kind]);
            fonts[kind] = choiceOf(editors[kind].choice);
        }
        void store.saveFonts(fonts);
    };

    for (const kind of KINDS) {
        const editor: Editor = {
            kind,
            choice: emptyChoice(),
            input: h("input", {
                class: "clovernt-input clovernt-font-input",
                type: "text",
                placeholder: "默认",
                spellcheck: false,
                "aria-label": TEXT[kind].title,
                onInput: () => {
                    const [family] = parseFamilies(editor.input.value);
                    if (family !== parseFamilies(editor.choice.family)[0]) {
                        editor.choice = {...emptyChoice(), family: editor.input.value};
                    }
                    render(editor);
                },
                onChange: save,
            }),
            nameTip: h("div", "clovernt-row__tip"),
            regular: new Dropdown("常规字重", (value) => {
                editor.choice = {...editor.choice, regular: value};
                save();
            }),
            bold: new Dropdown("粗体字重", (value) => {
                editor.choice = {...editor.choice, bold: value};
                save();
            }),
            features: h("div", "clovernt-font-features"),
            detail: [],
            preview: h(
                "div",
                `clovernt-font-preview is-${kind}`,
                h("div", {}, TEXT[kind].sample),
                h("div", "clovernt-font-preview__bold", TEXT[kind].bold),
            ),
        };
        editors[kind] = editor;
    }

    const row = (title: string, tip: Node | string, control: Node) => h(
        "div",
        "clovernt-row",
        h("div", "clovernt-row__main", h("div", "clovernt-row__title", title), typeof tip === "string" ? h("div", "clovernt-row__tip", tip) : tip),
        h("div", "clovernt-row__tail", control),
    );

    const element = pageElement();
    element.append(
        previewStyle,
        notice,
        ...KINDS.map((kind) => {
            const editor = editors[kind];
            editor.detail = [
                row("常规", "普通文字用的分型", editor.regular.element),
                row("粗体", "加粗文字用的分型", editor.bold.element),
                editor.features,
            ];
            const reset = h("button", {class: "clovernt-group__action", type: "button", onClick: () => {
                editor.input.value = "";
                editor.choice = emptyChoice();
                save();
            }}, "恢复默认");
            return group(TEXT[kind].title, [row("字体", editor.nameTip, editor.input), ...editor.detail], {extra: reset});
        }),
        group("预览", KINDS.map((kind) => editors[kind].preview), {
            note: "填写已安装字体的名称，多个用逗号分隔，靠前的优先。",
        }),
    );

    function renderPick(dropdown: Dropdown, faces: FaceOption[], picked: string, auto: FaceOption | undefined): void {
        const upright = faces.filter((option) => !option.italic);
        const options: DropdownOption[] = [
            {value: "", label: auto ? `自动（${faceLabel(auto)}）` : "自动"},
            ...upright.map((option) => ({value: option.key, label: faceLabel(option)})),
        ];
        dropdown.set(options, picked, upright.length === 0);
    }

    function renderFeatures(editor: Editor, tags: string[]): void {
        const groups = FEATURE_GROUPS.map(({caption, test, label}) => {
            const chips = tags.filter(test).sort((a, b) => featureOrder(a).localeCompare(featureOrder(b))).map((tag) => {
                const on = featureOn(editor.choice, tag);
                return h("button", {
                    class: on ? "clovernt-chip is-active" : "clovernt-chip",
                    type: "button",
                    title: tag,
                    "aria-pressed": String(on),
                    onClick: () => {
                        const features = {...editor.choice.features};
                        if (!on === DEFAULT_ON.has(tag)) {
                            delete features[tag];
                        } else {
                            features[tag] = !on;
                        }
                        editor.choice = {...editor.choice, features};
                        save();
                    },
                }, label(tag));
            });
            return chips.length > 0 && h("div", "clovernt-font-features__group", h("span", "clovernt-font-features__caption", caption), h("div", "clovernt-filters", ...chips));
        }).filter((node): node is HTMLDivElement => Boolean(node));
        editor.features.replaceChildren(
            h("div", "clovernt-font-features__head",
                h("div", "clovernt-row__title", "OpenType 特性"),
                h("div", "clovernt-row__tip", groups.length > 0 ? "此字体支持的排版特性" : "此字体没有可以调整的特性")),
            ...groups,
        );
    }

    function render(editor: Editor): void {
        const typed = editor.input.value;
        const families = parseFamilies(typed);
        const family = store.systemFonts && findFamily(store.systemFonts, typed);
        const faces = optionsFor(typed);
        const missing = store.systemFonts
            ? families.filter((name) => !isGenericFamily(name) && !findFamily(store.systemFonts!, name))
            : [];
        let tip: Node | string = TEXT[editor.kind].tip;
        if (store.systemFontsError) {
            tip = stateLabel(`读不到已安装的字体：${store.systemFontsError}`, {tone: "warn"});
        } else if (missing.length > 0) {
            tip = stateLabel(`没有找到${missing.map((name) => `「${name}」`).join("")}，这个名称不会生效`, {tone: "warn"});
        } else if (families.length > 0 && !store.systemFonts) {
            tip = "正在读取已安装的字体…";
        } else if (family && isVariableFamily(family)) {
            tip = "可变字体：粗细跟随文字本身的字重，没有分型可选";
        }
        editor.nameTip.replaceChildren(tip);

        const regular = faces.find((option) => option.key === editor.choice.regular) ?? autoRegular(faces);
        renderPick(editor.regular, faces, editor.choice.regular, autoRegular(faces));
        renderPick(editor.bold, faces, editor.choice.bold, regular && autoBold(faces, regular));
        renderFeatures(editor, family ? familyFeatures(family, editor.choice.regular) : []);
        editor.detail[0].hidden = editor.detail[1].hidden = faces.length === 0;
        editor.features.hidden = !family;
        previewStyle.textContent = KINDS.map((kind) => {
            return previewCss(edited(editors[kind]), kind, `.clovernt-font-preview.is-${kind}, .clovernt-font-preview.is-${kind} *`);
        }).join("\n");
    }

    function update(): void {
        for (const kind of KINDS) {
            const editor = editors[kind];
            const saved = store.fonts?.[kind];
            if (saved && document.activeElement !== editor.input) {
                editor.choice = saved;
                editor.input.value = saved.family;
            }
            render(editor);
        }
        notice.hidden = !store.fontsError;
        notice.replaceChildren(...(store.fontsError
            ? [alert({tone: "error", block: true, text: `读取字体设置失败：${store.fontsError}。保存后会重新写入这个文件。`})]
            : []));
    }

    update();
    void store.loadSystemFonts();
    return {
        element,
        update,
        dispose: () => KINDS.forEach((kind) => [editors[kind].regular, editors[kind].bold].forEach((dropdown) => dropdown.close())),
    };
}
