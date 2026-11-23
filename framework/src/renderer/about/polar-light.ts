import {FrameLoop} from "./frame-loop";

const GRID = 50;
const FREQUENCY = 0.025;
const FRAME_STEP = 0.5;
const INTERVAL_MS = 33;
const MAX_FRAME_GAP_MS = 100;
const FALLOFF = {radius: 0.6, centerX: 0.5, centerY: 0.4, centerFade: 0.5, edgeFade: 0};
const COLORS = ["#FFFFFF", "#E8FFF0", "#7CD1F8", "#0099FF", "#026EFF", "#C862FB", "#FF8A74", "#FAE366", "#FFFFFF"]
    .map((hex) => [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16)) as [number, number, number]);

const PRIME_X = 501125321;
const PRIME_Y = 1136930381;
const PRIME_Z = 1720413743;
const SEED = 1337;
const HASH_MULTIPLIER = 0x27d4eb2d;
const EXP_DENOMINATOR = Math.exp(-2) - 1;

const BASE_GRADIENTS = [
    0, 1, 1, 0, 0, -1, 1, 0, 0, 1, -1, 0, 0, -1, -1, 0,
    1, 0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, -1, 0, -1, 0,
    1, 1, 0, 0, -1, 1, 0, 0, 1, -1, 0, 0, -1, -1, 0, 0,
];
const GRADIENTS = Float32Array.from([
    ...Array.from({length: 5}, () => BASE_GRADIENTS).flat(),
    1, 1, 0, 0, 0, -1, 1, 0, -1, 1, 0, 0, 0, -1, -1, 0,
]);

function lerp(a: number, b: number, t: number): number {
    return a + (b - a) * t;
}

function quintic(t: number): number {
    return t * t * t * (t * (t * 6 - 15) + 10);
}

function gradient(xPrimed: number, yPrimed: number, zPrimed: number, xd: number, yd: number, zd: number): number {
    let hash = Math.imul(SEED ^ xPrimed ^ yPrimed ^ zPrimed, HASH_MULTIPLIER);
    hash = (hash ^ (hash >> 15)) & 252;
    return xd * GRADIENTS[hash]! + yd * GRADIENTS[hash | 1]! + zd * GRADIENTS[hash | 2]!;
}

function perlin(x: number, y: number, z: number): number {
    const fx = Math.floor(x);
    const fy = Math.floor(y);
    const fz = Math.floor(z);
    const xd0 = x - fx;
    const yd0 = y - fy;
    const zd0 = z - fz;
    const xd1 = xd0 - 1;
    const yd1 = yd0 - 1;
    const zd1 = zd0 - 1;
    const x0 = Math.imul(fx, PRIME_X);
    const y0 = Math.imul(fy, PRIME_Y);
    const z0 = Math.imul(fz, PRIME_Z);
    const x1 = (x0 + PRIME_X) | 0;
    const y1 = (y0 + PRIME_Y) | 0;
    const z1 = (z0 + PRIME_Z) | 0;
    const xs = quintic(xd0);
    const xf00 = lerp(gradient(x0, y0, z0, xd0, yd0, zd0), gradient(x1, y0, z0, xd1, yd0, zd0), xs);
    const xf10 = lerp(gradient(x0, y1, z0, xd0, yd1, zd0), gradient(x1, y1, z0, xd1, yd1, zd0), xs);
    const xf01 = lerp(gradient(x0, y0, z1, xd0, yd0, zd1), gradient(x1, y0, z1, xd1, yd0, zd1), xs);
    const xf11 = lerp(gradient(x0, y1, z1, xd0, yd1, zd1), gradient(x1, y1, z1, xd1, yd1, zd1), xs);
    const ys = quintic(yd0);
    return lerp(lerp(xf00, xf10, ys), lerp(xf01, xf11, ys), quintic(zd0)) * 0.9649214148521423;
}

function noise(x: number, y: number, time: number): number {
    const xy = (x + y) * FREQUENCY;
    const s2 = xy * -0.211324865405187;
    const z = time * FREQUENCY * 0.577350269189626;
    return perlin(x * FREQUENCY + s2 - z, y * FREQUENCY + s2 - z, z + xy * 0.577350269189626);
}

const ALPHA = Uint8Array.from({length: GRID * GRID}, (_, i) => {
    const distance = ((FALLOFF.centerX * GRID - (i % GRID)) ** 2 + (FALLOFF.centerY * GRID - Math.floor(i / GRID)) ** 2)
        / (FALLOFF.radius * GRID) ** 2;
    const transfer = (Math.exp(-2 * distance) - 1) / EXP_DENOMINATOR;
    const fade = Math.min(1, Math.max(0, 1 - FALLOFF.centerFade + (FALLOFF.centerFade - FALLOFF.edgeFade) * transfer));
    return (1 - fade) * 255;
});

export class PolarLight {
    readonly loop: FrameLoop;
    private readonly context: CanvasRenderingContext2D;
    private readonly image = new ImageData(GRID, GRID);
    private time = 0;
    private lastFrame = 0;

    constructor(canvas: HTMLCanvasElement) {
        canvas.width = canvas.height = GRID;
        this.context = canvas.getContext("2d")!;
        this.loop = new FrameLoop(canvas, (now) => this.render(now), {onStart: () => (this.lastFrame = 0)});
    }

    private render(now: number): void {
        this.paint();
        this.context.putImageData(this.image, 0, 0);
        const elapsed = this.lastFrame ? Math.min(MAX_FRAME_GAP_MS, now - this.lastFrame) : 0;
        this.lastFrame = now;
        this.time += FRAME_STEP * (elapsed / INTERVAL_MS);
    }

    private paint(): void {
        const pixels = this.image.data;
        const segments = COLORS.length - 1;
        for (let cell = 0, i = 0; cell < GRID * GRID; cell++) {
            const scale = Math.min(segments, Math.max(0, (noise(cell % GRID, Math.floor(cell / GRID), this.time) + 1) * 0.5 * segments));
            const index = Math.min(Math.floor(scale), segments - 1);
            const t = scale - index;
            const [r0, g0, b0] = COLORS[index]!;
            const [r1, g1, b1] = COLORS[index + 1]!;
            pixels[i++] = lerp(r0, r1, t);
            pixels[i++] = lerp(g0, g1, t);
            pixels[i++] = lerp(b0, b1, t);
            pixels[i++] = ALPHA[cell]!;
        }
    }
}
