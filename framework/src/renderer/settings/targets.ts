import {uiRegistry} from "../core/ui-registry";
import type {Route} from "./views/context";

const OVERVIEW: Route = {view: "overview"};

export function routesFor(target: string): Route[] {
    const registry = uiRegistry();
    const page = registry.page(target) ?? registry.pagesForPlugin(target)[0];
    if (page) {
        const route: Route = {view: "page", id: page.id};
        return page.placement === "sidebar" ? [route] : [OVERVIEW, route];
    }
    if (target === "plugins" || target === "developer" || target === "fonts") {
        return [OVERVIEW, {view: target}];
    }
    return [OVERVIEW];
}

export function targetOf(route: Route): string {
    return route.view === "page" ? route.id : route.view;
}
