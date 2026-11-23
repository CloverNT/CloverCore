import type {PluginManager} from "../clovernt.gen";
import type {GuardSetResult} from "../shared/guard";

export interface CloverBinding {
    readonly version: string;
    readonly loaderVersion: string;
    readonly qqVersion: string;
    readonly coreDir: string;
    readonly pluginsDir: string;
    readonly pluginManager: PluginManager;

    getGuardLocked(): number | null;

    setGuardLocked(families: number, locked: boolean): Promise<GuardSetResult>;
}

export function loadBinding(): CloverBinding {
    return (process as unknown as { _linkedBinding(name: string): CloverBinding })._linkedBinding("clovernt");
}
