import {release} from "node:os";

import type {PacketApi} from "../clovernt.gen";
import type {RuntimeInfo} from "../shared/ipc";
import type {CloverBinding} from "./binding";

const runtime = (globalThis as unknown as { clovernt: { packet: PacketApi } }).clovernt;

export function buildRuntimeInfo(binding: CloverBinding): RuntimeInfo {
    const {electron, chrome, node, v8} = process.versions;
    return {
        version: binding.version,
        loaderVersion: binding.loaderVersion,
        qqVersion: binding.qqVersion,
        coreDir: binding.coreDir,
        pluginsDir: binding.pluginsDir,
        platform: process.platform,
        arch: process.arch,
        osRelease: release(),
        packetReady: runtime.packet.ready(),
        versions: {electron, chrome, node, v8},
    };
}
