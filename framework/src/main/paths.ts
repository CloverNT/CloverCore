import {isAbsolute, relative, sep} from "node:path";

export function isInside(parent: string, child: string): boolean {
    const rel = relative(parent, child);
    return rel !== "" && !isAbsolute(rel) && rel.split(sep)[0] !== "..";
}
