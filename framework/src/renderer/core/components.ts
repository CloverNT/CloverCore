import {h, pressable, svg} from "./dom";
import {ICONS} from "./icons";

type Content = Node | string | false | null | undefined;

function contents(content: Content | Content[] | undefined): Content[] {
    return content === undefined ? [] : [content].flat();
}

export type ButtonVariant = "primary" | "secondary";
export function button(
    label: string,
    options: { variant?: ButtonVariant; disabled?: boolean; title?: string; onClick: () => void },
): HTMLButtonElement {
    const {variant = "secondary", disabled = false, title, onClick} = options;
    const element = h(
        "button",
        {class: `q-button q-button--${variant} q-button--small`, type: "button", title, onClick},
        h("span", {}, label),
    );
    setButton(element, label, disabled);
    return element;
}

export function setButton(element: HTMLButtonElement, label: string, disabled: boolean): void {
    element.firstElementChild!.textContent = label;
    element.disabled = disabled;
    element.setAttribute("aria-disabled", String(disabled));
}

export function iconButton(
    icon: string,
    options: { title: string; disabled?: boolean; spinning?: boolean; onClick: () => void },
): HTMLButtonElement {
    const {title, disabled = false, spinning = false, onClick} = options;
    const className = spinning ? "clovernt-icon-btn is-spinning" : "clovernt-icon-btn";
    return h("button", {class: className, type: "button", title, "aria-label": title, disabled, onClick}, svg(icon));
}

export function iconSlot(): HTMLElement {
    return h("span", "clovernt-icon-btn-slot");
}

export function toggleSwitch(options: {
    checked: boolean;
    disabled?: boolean;
    title?: string;
    onToggle: () => void;
}): HTMLElement {
    const {checked, disabled = false, title, onToggle} = options;
    let className = "q-switch clovernt-switch";
    if (checked) className += " is-active";
    if (disabled) className += " is-disabled";
    return pressable(
        h("div", {class: className, "aria-checked": String(checked), "aria-disabled": String(disabled), title},
            h("span", "q-switch__handle"),
        ),
        "switch",
        () => disabled || onToggle(),
    );
}

export type TagColor = "blue" | "grey" | "purple" | "orange" | Tone;

export type Tone = "ok" | "warn" | "error";

const TONES: ReadonlySet<string> = new Set<Tone>(["ok", "warn", "error"]);

export function tag(text: string, color: TagColor, title?: string): HTMLElement {
    const modifier = TONES.has(color) ? `clovernt-tag--${color}` : `q-tag--${color}`;
    return h("span", {class: `q-tag clovernt-tag ${modifier}`, title}, text);
}

export function dot(tone?: Tone): HTMLElement {
    return h("span", tone ? `clovernt-dot is-${tone}` : "clovernt-dot");
}

export function stateLabel(text: string, options: { tone?: Tone; mark?: Node } = {}): HTMLElement {
    const {tone, mark} = options;
    return h("span", tone === "error" ? "clovernt-state is-error" : "clovernt-state", mark ?? dot(tone), text);
}

export function glyph(icon: string, tone?: Tone): HTMLElement {
    return svg(icon, tone ? `clovernt-glyph is-${tone}` : "clovernt-glyph");
}

export function spinner(): HTMLElement {
    return h("span", "clovernt-spinner");
}

export function group(
    title: string,
    body: Content[],
    options: { extra?: Content | Content[]; note?: string } = {},
): HTMLElement {
    const extra = contents(options.extra);
    return h(
        "div",
        "clovernt-group",
        h(
            "div",
            "clovernt-group__head",
            h("span", "clovernt-group__title", title),
            extra.length > 0 && h("span", "clovernt-group__extra", ...extra),
        ),
        h("div", "clovernt-card", ...body),
        options.note && h("div", "clovernt-group__note", options.note),
    );
}

export function row(options: {
    title: Content | Content[];
    tip?: string;
    wrapTip?: boolean;
    lead?: Node;
    tail?: Content | Content[];
    disabled?: boolean;
}): HTMLElement {
    const {title, tip, wrapTip = false, lead, tail, disabled = false} = options;
    return h(
        "div",
        disabled ? "clovernt-row is-disabled" : "clovernt-row",
        lead && h("div", "clovernt-row__lead", lead),
        h(
            "div",
            "clovernt-row__main",
            h("div", "clovernt-row__title", ...contents(title)),
            tip && h("div", {class: wrapTip ? "clovernt-row__tip is-wrap" : "clovernt-row__tip", title: wrapTip ? undefined : tip}, tip),
        ),
        tail !== undefined && h("div", "clovernt-row__tail", ...contents(tail)),
    );
}

export function navRow(options: { title: string; value?: string; onOpen: () => void }): HTMLElement {
    const {title, value, onOpen} = options;
    return pressable(
        h(
            "div",
            "clovernt-row clovernt-row--nav",
            h("div", "clovernt-row__main", h("div", "clovernt-row__title", title)),
            h(
                "div",
                "clovernt-row__tail",
                value && h("span", {class: "clovernt-row__value", title: value}, value),
                svg(ICONS.chevronRight, "clovernt-chevron"),
            ),
        ),
        "button",
        onOpen,
    );
}

export function alert(options: {
    tone?: "warn" | "error";
    text: Content | Content[];
    block?: boolean;
    onClose?: () => void;
}): HTMLElement {
    const {tone, text, block = false, onClose} = options;
    let className = "clovernt-alert";
    if (block) className += " clovernt-alert--block";
    if (tone) className += ` is-${tone}`;
    return h(
        "div",
        {class: className, role: tone === "error" ? "alert" : "status"},
        svg(tone ? ICONS.alertCircle : ICONS.info, "clovernt-alert__icon"),
        h("div", "clovernt-alert__text", ...contents(text)),
        onClose && h(
            "button",
            {class: "clovernt-alert__close", type: "button", title: "关闭", "aria-label": "关闭", onClick: onClose},
            svg(ICONS.close),
        ),
    );
}

export function emptyState(options: {
    icon: string;
    title: string;
    desc?: string;
    detail?: string;
    actions?: Node[];
}): HTMLElement {
    const {icon, title, desc, detail, actions = []} = options;
    return h(
        "div",
        "clovernt-empty",
        svg(icon, "clovernt-empty__icon"),
        h("div", "clovernt-empty__title", title),
        desc && h("div", "clovernt-empty__desc", desc),
        detail && h("div", "clovernt-empty__detail", detail),
        actions.length > 0 && h("div", "clovernt-empty__actions", ...actions),
    );
}

export interface DropdownOption {
    value: string;
    label: string;
}

export class Dropdown {
    readonly element: HTMLButtonElement;
    private readonly label = h("span", "clovernt-dropdown__label");
    private options: DropdownOption[] = [];
    private value = "";
    private menu: HTMLElement | undefined;
    private focused = -1;

    constructor(title: string, private readonly onChange: (value: string) => void) {
        this.element = h(
            "button",
            {
                class: "clovernt-dropdown",
                type: "button",
                title,
                "aria-label": title,
                "aria-haspopup": "listbox",
                "aria-expanded": "false",
                onClick: () => (this.menu ? this.close() : this.open()),
                onKeydown: (event: KeyboardEvent) => this.onKeydown(event),
            },
            this.label,
            svg(ICONS.chevronDown, "clovernt-dropdown__arrow"),
        );
    }

    set(options: DropdownOption[], value: string, disabled = false): void {
        this.options = options;
        this.value = options.some((option) => option.value === value) ? value : (options[0]?.value ?? "");
        this.label.textContent = options.find((option) => option.value === this.value)?.label ?? "";
        this.element.disabled = disabled;
        if (disabled) {
            this.close();
        } else if (this.menu) {
            this.renderMenu();
        }
    }

    readonly close = (): void => {
        if (!this.menu) {
            return;
        }
        this.menu.remove();
        this.menu = undefined;
        this.element.setAttribute("aria-expanded", "false");
        document.removeEventListener("pointerdown", this.onOutside, true);
        document.removeEventListener("scroll", this.onScroll, true);
        removeEventListener("blur", this.close);
        removeEventListener("resize", this.close);
    };

    private open(): void {
        if (this.element.disabled || this.options.length === 0) {
            return;
        }
        this.menu = h("div", {class: "clovernt-menu", role: "listbox", "aria-label": this.element.title});
        this.focused = this.options.findIndex((option) => option.value === this.value);
        this.renderMenu();
        document.body.append(this.menu);
        this.place(this.menu);
        this.element.setAttribute("aria-expanded", "true");
        document.addEventListener("pointerdown", this.onOutside, true);
        document.addEventListener("scroll", this.onScroll, true);
        addEventListener("blur", this.close);
        addEventListener("resize", this.close);
    }

    private renderMenu(): void {
        this.menu?.replaceChildren(...this.options.map((option, index) => {
            let className = "clovernt-menu__item";
            if (option.value === this.value) className += " is-selected";
            if (index === this.focused) className += " is-focused";
            return h(
                "div",
                {class: className, role: "option", "aria-selected": String(option.value === this.value), onClick: () => this.choose(option.value)},
                svg(ICONS.check, "clovernt-menu__check"),
                h("span", "clovernt-menu__text", option.label),
            );
        }));
    }

    private place(menu: HTMLElement): void {
        const anchor = this.element.getBoundingClientRect();
        menu.style.minWidth = `${anchor.width}px`;
        const {offsetWidth: width, offsetHeight: height} = menu;
        const below = anchor.bottom + 4;
        const top = below + height > innerHeight - 8 && anchor.top - 4 - height >= 8 ? anchor.top - 4 - height : below;
        menu.style.top = `${Math.max(8, Math.min(top, innerHeight - height - 8))}px`;
        menu.style.left = `${Math.max(8, Math.min(anchor.left, innerWidth - width - 8))}px`;
    }

    private choose(value: string): void {
        this.close();
        if (value !== this.value) {
            this.set(this.options, value, this.element.disabled);
            this.onChange(value);
        }
    }

    private onKeydown(event: KeyboardEvent): void {
        if (!this.menu) {
            if (["ArrowDown", "ArrowUp", "Enter", " "].includes(event.key)) {
                event.preventDefault();
                this.open();
            }
            return;
        }
        if (event.key === "ArrowDown" || event.key === "ArrowUp") {
            event.preventDefault();
            const step = event.key === "ArrowDown" ? 1 : -1;
            this.focused = (this.focused + step + this.options.length) % this.options.length;
            this.renderMenu();
            this.menu.children[this.focused]?.scrollIntoView({block: "nearest"});
        } else if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            const option = this.options[this.focused];
            if (option) {
                this.choose(option.value);
            }
        } else if (event.key === "Escape" || event.key === "Tab") {
            if (event.key === "Escape") {
                event.preventDefault();
            }
            this.close();
        }
    }

    private readonly onOutside = (event: PointerEvent): void => {
        const target = event.target as Node;
        if (!this.menu?.contains(target) && !this.element.contains(target)) {
            this.close();
        }
    };

    private readonly onScroll = (event: Event): void => {
        if (event.target !== this.menu) {
            this.close();
        }
    };
}

export function avatar(markup: string, modifier: string, imageUrl?: string): HTMLElement {
    const tile = h("div", `clovernt-avatar clovernt-avatar--${modifier}`);
    tile.innerHTML = markup;
    if (imageUrl) {
        const image = h("img", {class: "clovernt-avatar__logo", alt: ""});
        image.addEventListener("load", () => {
            tile.classList.add("has-logo");
            tile.replaceChildren(image);
        }, {once: true});
        image.src = imageUrl;
    }
    return tile;
}
