import type {EventPriority as NativeEventPriority} from "../clovernt.gen";

export interface DllLoadPayload {
    baseName: string;
    fullPath: string;
    baseAddress: string;
    imageSize: number;
}

export interface PacketEventPayload {
    seq: number;
    command: string;
    uin: string;
    uid: string;
    encryption: "none" | "d2key" | "zero";
    body: string;
    injected: boolean;
}

export interface O3EventPayload {
    command: string;
    body: string;
}

export interface CredentialEventPayload {
    uin: string;
    uid: string;
    a2: string;
    d2: string;
    d2Key: string;
}

export interface CloverEventMap {
    "dll-load": DllLoadPayload;
    "packet-send": PacketEventPayload;
    "packet-recv": PacketEventPayload;
    "o3-send": O3EventPayload;
    "o3-recv": O3EventPayload;
    "credential": CredentialEventPayload;

    [name: string]: unknown;
}

export const EventPriority = {
    Lowest: 0,
    Low: 1,
    Normal: 2,
    High: 3,
    Highest: 4,
    Monitor: 5,
} as const satisfies Record<keyof typeof NativeEventPriority, NativeEventPriority>;

export type EventPriorityValue = (typeof EventPriority)[keyof typeof EventPriority];

export interface CloverEvents {
    on<K extends keyof CloverEventMap>(
        name: K,
        callback: (payload: CloverEventMap[K], control: { cancel(): void }) => void,
        priority?: EventPriorityValue,
    ): number;

    once<K extends keyof CloverEventMap>(
        name: K,
        callback: (payload: CloverEventMap[K]) => void,
        priority?: EventPriorityValue,
    ): number;

    off(id: number): boolean;

    emit(name: string, payload?: unknown): boolean;
}
