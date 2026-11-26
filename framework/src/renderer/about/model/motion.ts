function cubicBezier(x1: number, y1: number, x2: number, y2: number): (t: number) => number {
    const cx = 3 * x1;
    const bx = 3 * (x2 - x1) - cx;
    const ax = 1 - cx - bx;
    const cy = 3 * y1;
    const by = 3 * (y2 - y1) - cy;
    const ay = 1 - cy - by;
    return (t) => {
        if (t <= 0 || t >= 1) {
            return Math.min(1, Math.max(0, t));
        }
        let u = t;
        for (let i = 0; i < 14; i++) {
            const error = ((ax * u + bx) * u + cx) * u - t;
            const slope = (3 * ax * u + 2 * bx) * u + cx;
            if (Math.abs(error) < 1e-3 || Math.abs(slope) < 1e-6) {
                break;
            }
            u -= error / slope;
        }
        return ((ay * u + by) * u + cy) * u;
    };
}

const easeStandard = cubicBezier(0.25, 0.1, 0.25, 1);
const easeOutQuint = cubicBezier(0.23, 1, 0.32, 1);

function easeOvershoot(t: number): number {
    const s = Math.min(1, t) - 1;
    return s * s * (3 * s + 2) + 1;
}

interface Rotation {
    x: number;
    y: number;
}

type Animation = (elapsed: number) => Rotation | undefined;

const ENTRANCE_DELAY_MS = 1000;
const IDLE_DELAY_MS = 2000;
const DRAG_THRESHOLD_PX = 5;
const DRAG_DEGREES_PER_PX = 0.5;

function springBack(from: Rotation, durationMs: number): Animation {
    return (elapsed) => {
        if (elapsed >= durationMs) {
            return undefined;
        }
        const k = 1 - easeOvershoot(elapsed / durationMs);
        return {x: from.x * k, y: from.y * k};
    };
}

const IDLE_ANIMATIONS: Array<(from: Rotation) => Animation> = [
    // One slow full turn.
    (from) => (elapsed) =>
        elapsed < 8000 ? {x: from.x + (360 - from.x) * easeStandard(elapsed / 8000), y: from.y} : undefined,
    // Turn around, pause showing the back, turn back.
    (from) => (elapsed) => {
        if (elapsed < 600) return {x: from.x + (180 - from.x) * easeStandard(elapsed / 600), y: from.y};
        if (elapsed < 2000) return {x: 180, y: from.y};
        if (elapsed < 2600) return {x: 180 + 180 * easeStandard((elapsed - 2000) / 600), y: from.y};
        return undefined;
    },
    (from) => {
        const target = Math.random() < 1 / 3 ? -485 : 485;
        return (elapsed) => {
            if (elapsed < 3000) return {x: from.x + (target - from.x) * easeOutQuint(elapsed / 3000), y: from.y};
            if (elapsed < 4000) return {x: target * (1 - easeOvershoot((elapsed - 3000) / 1000)), y: from.y};
            return undefined;
        };
    },
];

function releaseSpring(from: Rotation, speed: number): Animation {
    const completeTurn = (degrees: number) => {
        if (Math.abs(degrees) <= 30) {
            return degrees;
        }
        const wrapped = degrees % 360;
        return wrapped > 0 ? wrapped - 360 : wrapped + 360;
    };
    const start = {x: completeTurn(from.x), y: completeTurn(from.y)};
    const x0 = 200;
    const v0 = -Math.abs(speed / 3);
    const omega0 = Math.sqrt(20);
    const zeta = 0.65;
    const omegaD = omega0 * Math.sqrt(1 - zeta * zeta);
    const b = (v0 + zeta * omega0 * x0) / omegaD;
    return (elapsed) => {
        const t = elapsed / 1000;
        const envelope = Math.exp(-zeta * omega0 * t);
        const cos = Math.cos(omegaD * t);
        const sin = Math.sin(omegaD * t);
        const value = envelope * (x0 * cos + b * sin);
        const velocity = -zeta * omega0 * value + envelope * omegaD * (b * cos - x0 * sin);
        if ((Math.abs(value) < 0.45 && Math.abs(velocity) < 2.5) || t > 4.5) {
            return undefined;
        }
        return {x: start.x * (value / x0), y: start.y * (value / x0)};
    };
}

export class ModelMotion {
    rotation: Rotation = {x: 0, y: 0};

    private animation: { run: Animation; start: number } | undefined;
    private timer = 0;
    private entered = false;
    private idleRound: number[] = [];
    private drag: { pointerId: number; lastX: number; lastY: number; moved: boolean; samples: Array<{ x: number; y: number; t: number }> } | undefined;

    constructor(private readonly stage: HTMLElement) {
        stage.addEventListener("pointerdown", this.onPointerDown);
        stage.addEventListener("pointermove", this.onPointerMove);
        stage.addEventListener("pointerup", this.onPointerUp);
        stage.addEventListener("pointercancel", this.onPointerUp);
    }

    update(now: number): void {
        if (!this.animation) {
            return;
        }
        const rotation = this.animation.run(now - this.animation.start);
        if (rotation) {
            this.rotation = rotation;
        } else {
            this.rotation = {x: 0, y: 0};
            this.animation = undefined;
            this.schedule(IDLE_DELAY_MS, () => this.playIdle());
        }
    }

    resume(): void {
        if (this.entered) {
            this.rotation = {x: 0, y: 0};
            this.schedule(IDLE_DELAY_MS, () => this.playIdle());
        } else {
            this.entered = true;
            this.schedule(ENTRANCE_DELAY_MS, () => {
                this.rotation = {x: -340, y: this.rotation.y};
                this.play(springBack(this.rotation, 1000));
            });
        }
    }

    pause(): void {
        clearTimeout(this.timer);
        this.animation = undefined;
        this.drag = undefined;
    }

    dispose(): void {
        this.pause();
        this.stage.removeEventListener("pointerdown", this.onPointerDown);
        this.stage.removeEventListener("pointermove", this.onPointerMove);
        this.stage.removeEventListener("pointerup", this.onPointerUp);
        this.stage.removeEventListener("pointercancel", this.onPointerUp);
    }

    private play(run: Animation): void {
        clearTimeout(this.timer);
        this.animation = {run, start: performance.now()};
    }

    private schedule(delayMs: number, action: () => void): void {
        clearTimeout(this.timer);
        this.timer = window.setTimeout(() => {
            if (!this.drag && !this.animation) {
                action();
            }
        }, delayMs);
    }

    private playIdle(): void {
        if (this.idleRound.length === 0) {
            this.idleRound = IDLE_ANIMATIONS.map((_, i) => i).sort(() => Math.random() - 0.5);
        }
        this.play(IDLE_ANIMATIONS[this.idleRound.pop()!](this.rotation));
    }

    private tap(localX: number, localY: number): void {
        const from = this.rotation;
        if (Math.abs(from.x) > 10 || Math.abs(from.y) > 10) {
            this.play(springBack(from, 1000));
            return;
        }
        const halfWidth = Math.max(1, this.stage.clientWidth / 2);
        const halfHeight = Math.max(1, this.stage.clientHeight / 2);
        const amplitude = () => 40 + Math.floor(Math.random() * 30);
        const peak = {
            x: (amplitude() * (halfWidth - localX)) / halfWidth,
            y: (amplitude() * (halfHeight - localY)) / halfHeight,
        };
        this.play((elapsed) => {
            if (elapsed < 220) {
                const k = easeOutQuint(elapsed / 220);
                return {x: from.x + (peak.x - from.x) * k, y: from.y + (peak.y - from.y) * k};
            }
            return springBack(peak, 600)(elapsed - 220);
        });
    }

    private readonly onPointerDown = (event: PointerEvent): void => {
        if (event.button !== 0) {
            return;
        }
        this.pause();
        this.stage.setPointerCapture(event.pointerId);
        this.drag = {
            pointerId: event.pointerId,
            lastX: event.clientX,
            lastY: event.clientY,
            moved: false,
            samples: [{x: event.clientX, y: event.clientY, t: performance.now()}],
        };
    };

    private readonly onPointerMove = (event: PointerEvent): void => {
        const drag = this.drag;
        if (!drag || event.pointerId !== drag.pointerId) {
            return;
        }
        const first = drag.samples[0];
        drag.moved ||= Math.hypot(event.clientX - first.x, event.clientY - first.y) > DRAG_THRESHOLD_PX;
        if (drag.moved) {
            this.rotation = {
                x: this.rotation.x + (drag.lastX - event.clientX) * DRAG_DEGREES_PER_PX,
                y: this.rotation.y + (drag.lastY - event.clientY) * DRAG_DEGREES_PER_PX,
            };
        }
        drag.lastX = event.clientX;
        drag.lastY = event.clientY;
        const now = performance.now();
        drag.samples.push({x: event.clientX, y: event.clientY, t: now});
        // Release velocity is measured over roughly the last 120 ms.
        while (drag.samples.length > 8 && now - drag.samples[0].t > 120) {
            drag.samples.shift();
        }
    };

    private readonly onPointerUp = (event: PointerEvent): void => {
        const drag = this.drag;
        if (!drag || event.pointerId !== drag.pointerId) {
            return;
        }
        this.drag = undefined;
        if (!drag.moved) {
            const rect = this.stage.getBoundingClientRect();
            this.tap(event.clientX - rect.left, event.clientY - rect.top);
            return;
        }
        const first = drag.samples[0];
        const last = drag.samples[drag.samples.length - 1];
        const seconds = (last.t - first.t) / 1000;
        const speed = seconds > 0.005 ? Math.hypot(last.x - first.x, last.y - first.y) / seconds : 0;
        this.play(releaseSpring(this.rotation, Math.min(speed, 8000)));
    };
}
