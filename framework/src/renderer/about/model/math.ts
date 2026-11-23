export type Vec3 = readonly [number, number, number];
export type Vec4 = readonly [number, number, number, number];

export type Mat4 = Float32Array<ArrayBuffer>;

export function mat4Identity(): Mat4 {
    return Float32Array.of(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
}

export function mat4Multiply(a: Float32Array, b: Float32Array): Mat4 {
    const out = new Float32Array(16);
    for (let column = 0; column < 4; column++) {
        for (let r = 0; r < 4; r++) {
            let sum = 0;
            for (let k = 0; k < 4; k++) {
                sum += a[k * 4 + r]! * b[column * 4 + k]!;
            }
            out[column * 4 + r] = sum;
        }
    }
    return out;
}

export function mat4LensProjection(focalLengthMm: number, aspect: number, near: number, far: number): Mat4 {
    const f = (2 * focalLengthMm) / 24;
    const nf = 1 / (near - far);
    const out = new Float32Array(16);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (far + near) * nf;
    out[11] = -1;
    out[14] = 2 * far * near * nf;
    return out;
}

function normalize(x: number, y: number, z: number): Vec3 {
    const length = Math.hypot(x, y, z) || 1;
    return [x / length, y / length, z / length];
}

export function mat4LookAt(eye: Vec3, target: Vec3, up: Vec3): Mat4 {
    const z = normalize(eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]);
    const x = normalize(up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]);
    const y: Vec3 = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]];
    const dot = (v: Vec3) => -(v[0] * eye[0] + v[1] * eye[1] + v[2] * eye[2]);
    return Float32Array.of(x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, dot(x), dot(y), dot(z), 1);
}

export function transformPoint(m: Float32Array, x: number, y: number, z: number): Vec3 {
    return [
        m[0]! * x + m[4]! * y + m[8]! * z + m[12]!,
        m[1]! * x + m[5]! * y + m[9]! * z + m[13]!,
        m[2]! * x + m[6]! * y + m[10]! * z + m[14]!,
    ];
}

export function transformDirection(m: Float32Array, x: number, y: number, z: number): Vec3 {
    return normalize(
        m[0]! * x + m[4]! * y + m[8]! * z,
        m[1]! * x + m[5]! * y + m[9]! * z,
        m[2]! * x + m[6]! * y + m[10]! * z,
    );
}
