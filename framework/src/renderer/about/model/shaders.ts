import {MAX_LIGHTS} from "./glb";

export const MODEL_VS = `#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4 uViewProj;

out vec3 vWorldPos;
out vec3 vWorldNormal;
out vec2 vUv;

void main() {
    vWorldPos = aPos;
    vWorldNormal = aNormal;
    vUv = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
`;

export const MODEL_FS = `#version 300 es
precision highp float;

in vec3 vWorldPos;
in vec3 vWorldNormal;
in vec2 vUv;

uniform sampler2D uBaseColorTex;
uniform sampler2D uDfg;

uniform vec4 uBaseColorFactor;
uniform float uMetallic;
uniform float uRoughness;
uniform float uReflectance;
uniform vec3 uEmissive;
uniform vec3 uCameraPos;
uniform float uExposure;

uniform int uLightCount;
uniform vec4 uLightPosInvFalloff[${MAX_LIGHTS}];
uniform vec4 uLightColorIntensity[${MAX_LIGHTS}];

out vec4 fragColor;

const float INV_PI = 0.31830987;
const float SPECULAR_AA_VARIANCE = 0.15;
const float SPECULAR_AA_THRESHOLD = 0.04;
const float MEDIUMP_FLT_MAX = 65504.0;

void main() {
    vec3 n = normalize(vWorldNormal);
    if (!gl_FrontFacing) {
        n = -n;
    }
    vec3 v = normalize(uCameraPos - vWorldPos);

    vec4 base = uBaseColorFactor * texture(uBaseColorTex, vUv);
    vec3 diffuseColor = base.rgb * (1.0 - uMetallic);
    vec3 f0 = base.rgb * uMetallic + vec3(0.16 * uReflectance * uReflectance * (1.0 - uMetallic));

    vec3 du = dFdx(n);
    vec3 dv = dFdy(n);
    float variance = SPECULAR_AA_VARIANCE * (dot(du, du) + dot(dv, dv));
    float r2 = uRoughness * uRoughness;
    float perceptualRoughness = sqrt(sqrt(clamp(r2 * r2 + min(2.0 * variance, SPECULAR_AA_THRESHOLD), 0.0, 1.0)));
    perceptualRoughness = clamp(perceptualRoughness, 0.089, 1.0);
    float roughness = perceptualRoughness * perceptualRoughness;

    float NoV = max(dot(n, v), 1e-4);
    vec3 dfg = textureLod(uDfg, vec2(NoV, perceptualRoughness), 0.0).xyz;
    vec3 energyCompensation = vec3(1.0) + f0 * (1.0 / dfg.y - 1.0);

    vec3 color = vec3(0.0);
    for (int i = 0; i < uLightCount; i++) {
        vec3 posToLight = uLightPosInvFalloff[i].xyz - vWorldPos;
        float distanceSq = dot(posToLight, posToLight);
        float factor = distanceSq * uLightPosInvFalloff[i].w;
        float smoothFactor = clamp(1.0 - factor * factor, 0.0, 1.0);
        float attenuation = smoothFactor * smoothFactor / max(distanceSq, 1e-4);
        vec3 l = posToLight * inversesqrt(max(distanceSq, 1e-8));
        float NoL = clamp(dot(n, l), 0.0, 1.0);
        if (NoL <= 0.0 || attenuation <= 0.0) {
            continue;
        }
        vec3 h = normalize(v + l);
        float NoH = clamp(dot(n, h), 0.0, 1.0);
        float LoH = clamp(dot(l, h), 0.0, 1.0);

        vec3 nxh = cross(n, h);
        float a = NoH * roughness;
        float k = roughness / (dot(nxh, nxh) + a * a);
        float D = min(k * k * INV_PI, MEDIUMP_FLT_MAX);
        float V = min(0.5 / mix(2.0 * NoL * NoV, NoL + NoV, roughness), MEDIUMP_FLT_MAX);
        float f5 = pow(1.0 - LoH, 5.0);
        vec3 F = vec3(f5) + f0 * (1.0 - f5);

        vec3 Fr = D * V * F * energyCompensation;
        vec3 Fd = diffuseColor * INV_PI;
        float intensity = uLightColorIntensity[i].w * uExposure * attenuation;
        color += (Fd + Fr) * NoL * uLightColorIntensity[i].rgb * intensity;
    }
    fragColor = vec4((color + uEmissive) * base.a, base.a);
}
`;

export const GRADE_VS = `#version 300 es
precision highp float;
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
`;

export const GRADE_FS = `#version 300 es
precision highp float;
precision highp sampler3D;
in vec2 vUv;
uniform sampler2D uColor;
uniform sampler3D uLut;
uniform vec2 uLutScaleOffset;
out vec4 fragColor;
const float MIN_ALPHA = 6.1035156e-05;
void main() {
    vec4 c = textureLod(uColor, vUv, 0.0);
    vec3 rgb = c.rgb / (c.a + MIN_ALPHA);
    vec3 logC = log2(rgb * 5.555556 + vec3(0.047996)) * 0.073499784 + vec3(0.386036);
    rgb = textureLod(uLut, vec3(uLutScaleOffset.x) + logC * uLutScaleOffset.y, 0.0).rgb;
    fragColor = vec4(rgb * (c.a + MIN_ALPHA), c.a);
}
`;
