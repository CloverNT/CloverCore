export interface FilamentLuts {
    lutSize: number;
    lut: Uint32Array<ArrayBuffer>;
    dfgSize: number;
    dfg: Uint16Array<ArrayBuffer>;
}

export function parseFilamentLuts(bytes: Uint8Array): FilamentLuts {
    const header = new DataView(bytes.buffer, bytes.byteOffset, 16);
    const lutSize = header.getUint32(4, true);
    const dfgSize = header.getUint32(8, true);
    const lutEnd = 16 + lutSize ** 3 * 4;
    const dfgEnd = lutEnd + dfgSize ** 2 * 3 * 2;
    return {
        lutSize,
        lut: new Uint32Array(bytes.slice(16, lutEnd).buffer),
        dfgSize,
        dfg: new Uint16Array(bytes.slice(lutEnd, dfgEnd).buffer),
    };
}
