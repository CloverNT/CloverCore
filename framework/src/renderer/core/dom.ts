type Child = Node | string | false | null | undefined;

type Attributes = Record<string, unknown> & {
    class?: string;
    onClick?: (event: MouseEvent) => void;
};

export function h<K extends keyof HTMLElementTagNameMap>(
    tag: K,
    attributes: Attributes | string = {},
    ...children: Child[]
): HTMLElementTagNameMap[K] {
    const element = document.createElement(tag);
    const attrs: Attributes = typeof attributes === "string" ? {class: attributes} : attributes;
    for (const [name, value] of Object.entries(attrs)) {
        if (value === undefined || value === false) {
            continue;
        }
        if (name === "class") {
            element.className = value as string;
        } else if (name.startsWith("on") && typeof value === "function") {
            element.addEventListener(name.slice(2).toLowerCase(), value as EventListener);
        } else if (name in element) {
            (element as unknown as Record<string, unknown>)[name] = value;
        } else {
            element.setAttribute(name, String(value));
        }
    }
    element.append(...children.filter((child): child is Node | string => Boolean(child)));
    return element;
}

export function pressable<E extends HTMLElement>(element: E, role: string, onPress: () => void): E {
    element.setAttribute("role", role);
    element.tabIndex = 0;
    element.addEventListener("click", onPress);
    element.addEventListener("keydown", (event) => {
        if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            onPress();
        }
    });
    return element;
}

export function svg(markup: string, className = "clovernt-btn-icon"): HTMLElement {
    const span = h("span", className);
    span.innerHTML = markup;
    return span;
}
