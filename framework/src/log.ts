import {LogLevel, type LogLevelValue} from "./bridge";

export interface CloverLogger {
    debug(message: string): void;

    info(message: string): void;

    warn(message: string): void;

    error(message: string): void;

    fatal(message: string): void;
}

export type LogSink = (level: LogLevelValue, scope: string, message: string) => void;

export function makeLogger(sink: LogSink, scope: string): CloverLogger {
    return {
        debug: (message) => sink(LogLevel.Debug, scope, message),
        info: (message) => sink(LogLevel.Info, scope, message),
        warn: (message) => sink(LogLevel.Warning, scope, message),
        error: (message) => sink(LogLevel.Error, scope, message),
        fatal: (message) => sink(LogLevel.Fatal, scope, message),
    };
}
