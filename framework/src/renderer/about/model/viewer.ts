import {FrameLoop} from "../frame-loop";
import {MODEL_CENTER, type QQModel} from "./glb";
import type {FilamentLuts} from "./luts";
import {mat4LensProjection, mat4LookAt, mat4Multiply, type Vec3} from "./math";
import {ModelMotion} from "./motion";
import {GRADE_FS, GRADE_VS, MODEL_FS, MODEL_VS} from "./shaders";

const EXPOSURE = 1 / (1.2 * 32000);
const FOCAL_LENGTH_MM = 30;
const ORBIT_RADIUS = 4.72;
const REFLECTANCE = 0.5;
const MAX_PIXEL_RATIO = 2.5;
const MSAA_SAMPLES = 4;

type Program = { program: WebGLProgram; uniforms: Map<string, WebGLUniformLocation | null> };

interface RenderTarget {
    width: number;
    height: number;
    msaa: WebGLFramebuffer;
    resolve: WebGLFramebuffer;
    texture: WebGLTexture;
    renderbuffers: WebGLRenderbuffer[];
}

function createProgram(gl: WebGL2RenderingContext, vertex: string, fragment: string): Program {
    const program = gl.createProgram();
    for (const [type, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]] as const) {
        const shader = gl.createShader(type)!;
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        gl.attachShader(program, shader);
    }
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(gl.getProgramInfoLog(program) ?? "shader program failed to link");
    }
    const uniforms = new Map<string, WebGLUniformLocation | null>();
    for (let i = 0; i < gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS); i++) {
        const name = gl.getActiveUniform(program, i)!.name.replace(/\[0]$/, "");
        uniforms.set(name, gl.getUniformLocation(program, name));
    }
    return {program, uniforms};
}

function createTexture(gl: WebGL2RenderingContext, target: number, filter: number, wrap: number): WebGLTexture {
    const texture = gl.createTexture();
    gl.bindTexture(target, texture);
    gl.texParameteri(target, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(target, gl.TEXTURE_MAG_FILTER, filter === gl.LINEAR_MIPMAP_LINEAR ? gl.LINEAR : filter);
    gl.texParameteri(target, gl.TEXTURE_WRAP_S, wrap);
    gl.texParameteri(target, gl.TEXTURE_WRAP_T, wrap);
    gl.texParameteri(target, gl.TEXTURE_WRAP_R, wrap);
    return texture;
}

export class ModelViewer {
    readonly loop: FrameLoop;
    private readonly motion: ModelMotion;
    private readonly gl: WebGL2RenderingContext;
    private scene!: Program;
    private grade!: Program;
    private vertexArray!: WebGLVertexArrayObject;
    private indexBuffer!: WebGLBuffer;
    private baseColor!: WebGLTexture;
    private dfg!: WebGLTexture;
    private lut!: WebGLTexture;
    private target: RenderTarget | undefined;

    private readonly centroids: Float32Array<ArrayBuffer>;
    private readonly depths: Float32Array<ArrayBuffer>;
    private readonly order: Uint32Array<ArrayBuffer>;
    private readonly sortedIndices: Uint16Array<ArrayBuffer>;

    constructor(
        private readonly canvas: HTMLCanvasElement,
        stage: HTMLElement,
        private readonly model: QQModel,
        private readonly luts: FilamentLuts,
    ) {
        const gl = canvas.getContext("webgl2", {alpha: true, premultipliedAlpha: true, antialias: false, depth: false});
        if (!gl?.getExtension("EXT_color_buffer_float")) {
            throw new Error("WebGL2 with float color buffers is required");
        }
        this.gl = gl;

        const {positions, indices} = model;
        const triangles = indices.length / 3;
        this.centroids = new Float32Array(triangles * 3);
        for (let t = 0; t < triangles; t++) {
            for (let axis = 0; axis < 3; axis++) {
                this.centroids[t * 3 + axis] = (positions[indices[t * 3] * 3 + axis] + positions[indices[t * 3 + 1] * 3 + axis] + positions[indices[t * 3 + 2] * 3 + axis]) / 3;
            }
        }
        this.depths = new Float32Array(triangles);
        this.order = Uint32Array.from({length: triangles}, (_, t) => t);
        this.sortedIndices = new Uint16Array(indices.length);

        this.createResources();
        canvas.addEventListener("webglcontextrestored", () => {
            this.target = undefined;
            this.createResources();
        });
        this.motion = new ModelMotion(stage);
        this.loop = new FrameLoop(canvas, (now) => this.render(now), {
            onStart: () => this.motion.resume(),
            onStop: () => this.motion.pause(),
        });
    }

    private createResources(): void {
        const {gl, model, luts} = this;
        this.scene = createProgram(gl, MODEL_VS, MODEL_FS);
        this.grade = createProgram(gl, GRADE_VS, GRADE_FS);

        this.vertexArray = gl.createVertexArray();
        gl.bindVertexArray(this.vertexArray);
        [model.positions, model.normals, model.uvs].forEach((data, location) => {
            gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
            gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
            gl.enableVertexAttribArray(location);
            gl.vertexAttribPointer(location, location === 2 ? 2 : 3, gl.FLOAT, false, 0, 0);
        });
        this.indexBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.indexBuffer);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, model.indices, gl.DYNAMIC_DRAW);
        gl.bindVertexArray(null);

        this.baseColor = createTexture(gl, gl.TEXTURE_2D, gl.LINEAR_MIPMAP_LINEAR, gl.REPEAT);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, model.wrapS);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, model.wrapT);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.SRGB8_ALPHA8, gl.RGBA, gl.UNSIGNED_BYTE, model.texture);
        gl.generateMipmap(gl.TEXTURE_2D);

        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        this.dfg = createTexture(gl, gl.TEXTURE_2D, gl.LINEAR, gl.CLAMP_TO_EDGE);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB16F, luts.dfgSize, luts.dfgSize, 0, gl.RGB, gl.HALF_FLOAT, luts.dfg);
        this.lut = createTexture(gl, gl.TEXTURE_3D, gl.LINEAR, gl.CLAMP_TO_EDGE);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RGB10_A2, luts.lutSize, luts.lutSize, luts.lutSize, 0, gl.RGBA, gl.UNSIGNED_INT_2_10_10_10_REV, luts.lut);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    }

    private renderTarget(width: number, height: number): RenderTarget {
        const {gl} = this;
        if (this.target?.width === width && this.target.height === height) {
            return this.target;
        }
        if (this.target) {
            gl.deleteFramebuffer(this.target.msaa);
            gl.deleteFramebuffer(this.target.resolve);
            gl.deleteTexture(this.target.texture);
            this.target.renderbuffers.forEach((buffer) => gl.deleteRenderbuffer(buffer));
        }
        const texture = createTexture(gl, gl.TEXTURE_2D, gl.NEAREST, gl.CLAMP_TO_EDGE);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, width, height, 0, gl.RGBA, gl.HALF_FLOAT, null);
        const resolve = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, resolve);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);

        const samples = Math.min(MSAA_SAMPLES, gl.getParameter(gl.MAX_SAMPLES) as number);
        const renderbuffers = [[gl.COLOR_ATTACHMENT0, gl.RGBA16F], [gl.DEPTH_ATTACHMENT, gl.DEPTH_COMPONENT24]].map(([attachment, format]) => {
            const buffer = gl.createRenderbuffer();
            gl.bindRenderbuffer(gl.RENDERBUFFER, buffer);
            gl.renderbufferStorageMultisample(gl.RENDERBUFFER, samples, format, width, height);
            return [attachment, buffer] as const;
        });
        const msaa = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, msaa);
        for (const [attachment, buffer] of renderbuffers) {
            gl.framebufferRenderbuffer(gl.FRAMEBUFFER, attachment, gl.RENDERBUFFER, buffer);
        }
        this.target = {width, height, msaa, resolve, texture, renderbuffers: renderbuffers.map(([, buffer]) => buffer)};
        return this.target;
    }

    private sortTriangles(view: Float32Array): void {
        const {centroids, depths, order, sortedIndices} = this;
        const indices = this.model.indices;
        for (let t = 0; t < depths.length; t++) {
            depths[t] = view[2] * centroids[t * 3] + view[6] * centroids[t * 3 + 1] + view[10] * centroids[t * 3 + 2] + view[14];
        }
        // The order barely changes between frames, which keeps the sort cheap.
        order.sort((a, b) => depths[a] - depths[b]);
        for (let i = 0; i < order.length; i++) {
            sortedIndices.set(indices.subarray(order[i] * 3, order[i] * 3 + 3), i * 3);
        }
    }

    private render(now: number): void {
        const {gl, canvas, model} = this;
        if (gl.isContextLost()) {
            return;
        }
        this.motion.update(now);
        const ratio = Math.min(devicePixelRatio, MAX_PIXEL_RATIO);
        const width = Math.max(1, Math.round(canvas.clientWidth * ratio));
        const height = Math.max(1, Math.round(canvas.clientHeight * ratio));
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
        }

        const phi = (this.motion.rotation.x * Math.PI) / 180;
        const theta = (-this.motion.rotation.y * Math.PI) / 180;
        const eye: Vec3 = [
            MODEL_CENTER[0] + ORBIT_RADIUS * Math.cos(theta) * Math.sin(phi),
            MODEL_CENTER[1] + ORBIT_RADIUS * Math.sin(theta),
            MODEL_CENTER[2] + ORBIT_RADIUS * Math.cos(theta) * Math.cos(phi),
        ];
        const up: Vec3 = [-Math.sin(theta) * Math.sin(phi), Math.cos(theta), -Math.sin(theta) * Math.cos(phi)];
        const view = mat4LookAt(eye, MODEL_CENTER, up);
        const viewProjection = mat4Multiply(mat4LensProjection(FOCAL_LENGTH_MM, width / height, 0.05, 1000), view);
        this.sortTriangles(view);

        const target = this.renderTarget(width, height);
        gl.bindFramebuffer(gl.FRAMEBUFFER, target.msaa);
        gl.viewport(0, 0, width, height);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
        gl.enable(gl.DEPTH_TEST);
        gl.depthMask(false);
        gl.enable(gl.CULL_FACE);

        const {program, uniforms} = this.scene;
        const uniform = (name: string) => uniforms.get(name) ?? null;
        const lightData = (pick: (light: QQModel["lights"][number]) => number[]) => Float32Array.from(model.lights.flatMap(pick));
        gl.useProgram(program);
        gl.uniformMatrix4fv(uniform("uViewProj"), false, viewProjection);
        gl.uniform4fv(uniform("uBaseColorFactor"), model.material.baseColorFactor);
        gl.uniform1f(uniform("uMetallic"), model.material.metallicFactor);
        gl.uniform1f(uniform("uRoughness"), model.material.roughnessFactor);
        gl.uniform1f(uniform("uReflectance"), REFLECTANCE);
        gl.uniform3fv(uniform("uEmissive"), model.material.emissiveFactor);
        gl.uniform3fv(uniform("uCameraPos"), eye);
        gl.uniform1f(uniform("uExposure"), EXPOSURE);
        gl.uniform1i(uniform("uLightCount"), model.lights.length);
        gl.uniform4fv(uniform("uLightPosInvFalloff"), lightData((l) => [...l.position, l.inverseRangeSq]));
        gl.uniform4fv(uniform("uLightColorIntensity"), lightData((l) => [...l.color, l.intensity]));
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.baseColor);
        gl.uniform1i(uniform("uBaseColorTex"), 0);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, this.dfg);
        gl.uniform1i(uniform("uDfg"), 1);

        gl.bindVertexArray(this.vertexArray);
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.indexBuffer);
        gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0, this.sortedIndices);
        for (const face of [gl.FRONT, gl.BACK]) {
            gl.cullFace(face);
            gl.drawElements(gl.TRIANGLES, this.sortedIndices.length, gl.UNSIGNED_SHORT, 0);
        }
        gl.bindVertexArray(null);
        gl.disable(gl.CULL_FACE);
        gl.disable(gl.DEPTH_TEST);
        gl.depthMask(true);
        gl.disable(gl.BLEND);

        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, target.msaa);
        gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, target.resolve);
        gl.blitFramebuffer(0, 0, width, height, 0, 0, width, height, gl.COLOR_BUFFER_BIT, gl.NEAREST);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.useProgram(this.grade.program);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, target.texture);
        gl.uniform1i(this.grade.uniforms.get("uColor") ?? null, 0);
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_3D, this.lut);
        gl.uniform1i(this.grade.uniforms.get("uLut") ?? null, 2);
        gl.uniform2f(this.grade.uniforms.get("uLutScaleOffset") ?? null, 0.5 / this.luts.lutSize, (this.luts.lutSize - 1) / this.luts.lutSize);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
    }
}
