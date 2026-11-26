const IPC_REMOTE_PREFIX = /^Error invoking remote method '[^']+':\s*/;
const ERROR_CLASS_PREFIX = /^(TypeError|RangeError|SyntaxError|ReferenceError|URIError|EvalError|AggregateError|Error):\s*(.+)$/s;

function stripErrorPrefixes(raw: string): { kind: string | undefined; message: string } {
    let text = raw.trimStart();
    let kind: string | undefined;
    for (; ;) {
        const ipcMatch = IPC_REMOTE_PREFIX.exec(text);
        if (ipcMatch) {
            text = text.slice(ipcMatch[0].length).trimStart();
            continue;
        }
        const errMatch = ERROR_CLASS_PREFIX.exec(text);
        if (errMatch) {
            if (kind === undefined && errMatch[1] !== "Error") {
                kind = errMatch[1];
            }
            text = errMatch[2].trimStart();
            continue;
        }
        break;
    }
    return {kind, message: text || raw};
}

export function errorMessage(error: unknown): string {
    if (error instanceof Error) {
        return stripErrorPrefixes(error.message || error.name).message;
    }
    if (error !== null && typeof error === "object" && typeof (error as { message?: unknown }).message === "string") {
        return stripErrorPrefixes((error as { message: string }).message).message;
    }
    return stripErrorPrefixes(String(error)).message;
}

export function unwrapError(error: unknown): Error {
    const raw = error instanceof Error
        ? error.message
        : error !== null && typeof error === "object" && typeof (error as { message?: unknown }).message === "string"
            ? (error as { message: string }).message
            : String(error);
    const {kind, message} = stripErrorPrefixes(raw);
    if (error instanceof Error && error.message === message && (kind === undefined || error.name === kind)) {
        return error;
    }
    const effectiveKind = kind ?? (error instanceof Error ? error.name : undefined);
    switch (effectiveKind) {
        case "TypeError":
            return new TypeError(message);
        case "RangeError":
            return new RangeError(message);
        case "SyntaxError":
            return new SyntaxError(message);
        case "ReferenceError":
            return new ReferenceError(message);
        case "URIError":
            return new URIError(message);
        case "EvalError":
            return new EvalError(message);
        default:
            return new Error(message);
    }
}
