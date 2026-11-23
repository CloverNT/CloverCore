import type {SettingsStore} from "../store";

export type Route =
    | { view: "overview" }
    | { view: "plugins"; focus?: string }
    | { view: "developer" }
    | { view: "fonts" }
    | { view: "page"; id: string };

export interface ViewContext {
    readonly store: SettingsStore;

    push(route: Route): void;
}

export interface View {
    readonly element: HTMLElement;

    update(): void;

    dispose?(): void;
}

export function pageElement(): HTMLElement {
    const element = document.createElement("div");
    element.className = "clovernt-page";
    return element;
}

export function rebuiltView(render: () => Array<Node | false | undefined>): View {
    const element = pageElement();
    const update = () => element.replaceChildren(...render().filter((node): node is Node => Boolean(node)));
    update();
    return {element, update};
}
