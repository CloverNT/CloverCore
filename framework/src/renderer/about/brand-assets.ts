import {Channel} from "../../shared/ipc";
import {invoke} from "../core/bridge";
import {parseQQModel, type QQModel} from "./model/glb";
import {parseFilamentLuts, type FilamentLuts} from "./model/luts";

export interface BrandAssets {
    fallbackUrl: string;
    wordmarkUrl: string;
    model: QQModel;
    luts: FilamentLuts;
}

let loading: Promise<BrandAssets> | undefined;

function pngUrl(bytes: Uint8Array): string {
    return URL.createObjectURL(new Blob([bytes.slice()], {type: "image/png"}));
}

export function loadBrandAssets(): Promise<BrandAssets> {
    return (loading ??= invoke(Channel.GetAboutAssets).then(async (assets) => ({
        fallbackUrl: pngUrl(assets.fallbackPng),
        wordmarkUrl: pngUrl(assets.wordmarkPng),
        model: await parseQQModel(assets.glb),
        luts: parseFilamentLuts(assets.luts),
    })));
}
