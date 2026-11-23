import {mat4Identity, mat4Multiply, transformDirection, transformPoint, type Mat4, type Vec3, type Vec4} from "./math";

export const MAX_LIGHTS = 4;
const DEFAULT_LIGHT_RANGE = 10;
export const MODEL_CENTER: Vec3 = [0, 0, -4];

export interface PointLight {
    position: Vec3;
    color: Vec3;
    intensity: number;
    inverseRangeSq: number;
}

export interface QQModel {
    positions: Float32Array<ArrayBuffer>;
    normals: Float32Array<ArrayBuffer>;
    uvs: Float32Array<ArrayBuffer>;
    indices: Uint16Array<ArrayBuffer>;
    material: {
        baseColorFactor: Vec4;
        metallicFactor: number;
        roughnessFactor: number;
        emissiveFactor: Vec3;
    };
    wrapS: number;
    wrapT: number;
    texture: ImageBitmap;
    lights: PointLight[];
}

interface Gltf {
    scene?: number;
    scenes: Array<{ nodes: number[] }>;
    nodes: Array<{
        mesh?: number;
        matrix?: number[];
        children?: number[];
        extensions?: { KHR_lights_punctual?: { light: number } };
    }>;
    meshes: Array<{
        primitives: Array<{ attributes: Record<"POSITION" | "NORMAL" | "TEXCOORD_0", number>; indices: number; material?: number }>;
    }>;
    accessors: Array<{ bufferView: number; byteOffset?: number; count: number; type: "SCALAR" | "VEC2" | "VEC3" }>;
    bufferViews: Array<{ byteOffset?: number; byteLength: number }>;
    materials: Array<{
        pbrMetallicRoughness: { baseColorFactor: Vec4; metallicFactor: number; roughnessFactor: number };
        emissiveFactor: Vec3;
    }>;
    samplers: Array<{ wrapS: number; wrapT: number }>;
    images: Array<{ bufferView: number; mimeType: string }>;
    extensions: { KHR_lights_punctual: { lights: Array<{ color: Vec3; intensity: number; range?: number }> } };
}

const COMPONENTS = {SCALAR: 1, VEC2: 2, VEC3: 3} as const;

export async function parseQQModel(glb: Uint8Array): Promise<QQModel> {
    const view = new DataView(glb.buffer, glb.byteOffset, glb.byteLength);
    const jsonLength = view.getUint32(12, true);
    const gltf = JSON.parse(new TextDecoder().decode(glb.subarray(20, 20 + jsonLength))) as Gltf;
    const binaryStart = 20 + jsonLength + 8;

    const bufferView = (index: number): Uint8Array<ArrayBuffer> => {
        const {byteOffset = 0, byteLength} = gltf.bufferViews[index];
        return glb.slice(binaryStart + byteOffset, binaryStart + byteOffset + byteLength);
    };
    const accessor = (index: number, bytesPerComponent: number): ArrayBuffer => {
        const {bufferView: viewIndex, byteOffset = 0, count, type} = gltf.accessors[index];
        return bufferView(viewIndex).buffer.slice(byteOffset, byteOffset + count * COMPONENTS[type] * bytesPerComponent);
    };

    const worldMatrices: Mat4[] = [];
    const visit = (nodeIndex: number, parent: Mat4): void => {
        const node = gltf.nodes[nodeIndex];
        const world = node.matrix ? mat4Multiply(parent, Float32Array.from(node.matrix)) : parent;
        worldMatrices[nodeIndex] = world;
        node.children?.forEach((child) => visit(child, world));
    };
    gltf.scenes[gltf.scene ?? 0].nodes.forEach((root) => visit(root, mat4Identity()));

    const meshNode = gltf.nodes.findIndex((node) => node.mesh !== undefined);
    const meshMatrix = worldMatrices[meshNode];
    const primitive = gltf.meshes[gltf.nodes[meshNode].mesh!].primitives[0];
    const positions = new Float32Array(accessor(primitive.attributes.POSITION, 4));
    const normals = new Float32Array(accessor(primitive.attributes.NORMAL, 4));

    const min = [Infinity, Infinity, Infinity];
    const max = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < positions.length; i += 3) {
        const point = transformPoint(meshMatrix, positions[i], positions[i + 1], positions[i + 2]);
        positions.set(point, i);
        normals.set(transformDirection(meshMatrix, normals[i], normals[i + 1], normals[i + 2]), i);
        for (let axis = 0; axis < 3; axis++) {
            min[axis] = Math.min(min[axis], point[axis]);
            max[axis] = Math.max(max[axis], point[axis]);
        }
    }
    const center = min.map((value, axis) => (value + max[axis]) / 2);
    const scale = 2 / Math.max(...max.map((value, axis) => value - min[axis]));
    const fit = (x: number, y: number, z: number): Vec3 => [
        (x - center[0]) * scale + MODEL_CENTER[0],
        (y - center[1]) * scale + MODEL_CENTER[1],
        (z - center[2]) * scale + MODEL_CENTER[2],
    ];
    for (let i = 0; i < positions.length; i += 3) {
        positions.set(fit(positions[i], positions[i + 1], positions[i + 2]), i);
    }

    const lightDefinitions = gltf.extensions.KHR_lights_punctual.lights;
    const lights = gltf.nodes.flatMap((node, index): PointLight[] => {
        const reference = node.extensions?.KHR_lights_punctual;
        if (!reference) {
            return [];
        }
        const light = lightDefinitions[reference.light];
        const matrix = worldMatrices[index];
        const range = light.range || DEFAULT_LIGHT_RANGE;
        return [{
            position: fit(matrix[12], matrix[13], matrix[14]),
            color: light.color,
            intensity: light.intensity,
            inverseRangeSq: 1 / range ** 2,
        }];
    }).slice(0, MAX_LIGHTS);

    const {pbrMetallicRoughness: pbr, emissiveFactor} = gltf.materials[primitive.material ?? 0];
    const image = gltf.images[0];
    const texture = await createImageBitmap(new Blob([bufferView(image.bufferView)], {type: image.mimeType}), {
        premultiplyAlpha: "none",
    });
    return {
        positions,
        normals,
        uvs: new Float32Array(accessor(primitive.attributes.TEXCOORD_0, 4)),
        indices: new Uint16Array(accessor(primitive.indices, 2)),
        material: {
            baseColorFactor: pbr.baseColorFactor,
            metallicFactor: pbr.metallicFactor,
            roughnessFactor: pbr.roughnessFactor,
            emissiveFactor,
        },
        wrapS: gltf.samplers[0].wrapS,
        wrapT: gltf.samplers[0].wrapT,
        texture,
        lights,
    };
}
