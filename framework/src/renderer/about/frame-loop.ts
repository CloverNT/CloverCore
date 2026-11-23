export class FrameLoop {
    private frame = 0;

    constructor(
        private readonly target: Element,
        private readonly render: (now: number) => void,
        private readonly hooks: { onStart?(): void; onStop?(): void } = {},
    ) {}

    resume(): void {
        if (this.frame === 0 && this.target.checkVisibility()) {
            this.hooks.onStart?.();
            this.render(performance.now());
            this.frame = requestAnimationFrame(this.tick);
        }
    }

    stop(): void {
        if (this.frame !== 0) {
            cancelAnimationFrame(this.frame);
            this.frame = 0;
            this.hooks.onStop?.();
        }
    }

    private readonly tick = (now: number): void => {
        if (!this.target.checkVisibility()) {
            this.frame = 0;
            this.hooks.onStop?.();
            return;
        }
        this.render(now);
        this.frame = requestAnimationFrame(this.tick);
    };
}
