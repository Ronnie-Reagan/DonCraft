#version 450

const float kPi = 3.14159265359;
const float kShadowBias = 0.0012;
const float kShadowTexel = 1.0 / 2048.0;
const float kMinimumShadow = 0.42;
const float kSurfaceShadingFlat = 0.0;
const float kSurfaceShadingTerrain = 1.0;
const float kSurfaceShadingWater = 2.0;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragMaterial;
layout(location = 0) out vec4 outAccumulation;
layout(location = 1) out float outRevealage;

layout(set = 0, binding = 0) uniform sampler2D shadowMap;

layout(set = 0, binding = 3) uniform SceneData
{
    vec4 cameraPositionTime;
    vec4 sunDirectionAmbient;
    vec4 fogColorDensity;
    vec4 horizonColorSun;
} scene;

layout(push_constant) uniform PushConstants
{
    mat4 worldToClip;
    mat4 worldToShadowClip;
} pc;

struct MaterialSample
{
    vec3 albedo;
    float roughness;
    float metallic;
    float ao;
    float emissive;
};

float Hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float Noise2(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash12(i + vec2(0.0, 0.0));
    float b = Hash12(i + vec2(1.0, 0.0));
    float c = Hash12(i + vec2(0.0, 1.0));
    float d = Hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float Fbm(vec2 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < 4; ++octave)
    {
        value += Noise2(p) * amplitude;
        p = p * 2.03 + vec2(17.1, 11.7);
        amplitude *= 0.5;
    }
    return value;
}

vec3 TriplanarWeights(vec3 normal)
{
    vec3 weights = pow(max(abs(normal), vec3(0.001)), vec3(4.0));
    return weights / max(weights.x + weights.y + weights.z, 0.0001);
}

float TriplanarNoise(vec3 position, vec3 normal, float scale)
{
    vec3 weights = TriplanarWeights(normal);
    float xy = Fbm(position.xy * scale);
    float xz = Fbm(position.xz * scale);
    float yz = Fbm(position.yz * scale);
    return xy * weights.z + xz * weights.y + yz * weights.x;
}

float TriplanarLines(vec3 position, vec3 normal, float scale)
{
    vec3 weights = TriplanarWeights(normal);
    float xy = Noise2(position.xy * scale) * 0.45 + abs(sin(position.x * scale * 1.4)) * 0.55;
    float xz = Noise2(position.xz * scale) * 0.45 + abs(sin(position.z * scale * 1.4)) * 0.55;
    float yz = Noise2(position.yz * scale) * 0.45 + abs(sin(position.y * scale * 1.4)) * 0.55;
    return xy * weights.z + xz * weights.y + yz * weights.x;
}

MaterialSample SampleTerrainMaterial(int materialId, vec3 worldPosition, vec3 normal, vec3 baseColor)
{
    float macro = TriplanarNoise(worldPosition, normal, 0.18);
    float detail = TriplanarNoise(worldPosition, normal, 1.15);
    float coarse = TriplanarNoise(worldPosition, normal, 2.9);
    float stripes = TriplanarLines(worldPosition, normal, 0.85);

    MaterialSample materialSample;
    materialSample.albedo = baseColor;
    materialSample.roughness = 0.85;
    materialSample.metallic = 0.0;
    materialSample.ao = 0.92;
    materialSample.emissive = 0.0;

    if (materialId == 1)
    {
        float dune = 0.55 + 0.45 * sin(worldPosition.x * 0.34 + macro * 4.2 + coarse * 1.5);
        float grain = mix(detail, coarse, 0.35);
        materialSample.albedo = mix(vec3(0.54, 0.43, 0.21), vec3(0.92, 0.81, 0.58), dune);
        materialSample.albedo *= 0.84 + grain * 0.32;
        materialSample.roughness = 0.93;
    }
    else if (materialId == 2)
    {
        float puddle = smoothstep(0.52, 0.82, macro + detail * 0.35);
        materialSample.albedo = mix(vec3(0.20, 0.13, 0.09), vec3(0.34, 0.23, 0.15), 0.35 + coarse * 0.65);
        materialSample.albedo *= mix(0.72, 1.08, detail);
        materialSample.roughness = mix(0.38, 0.86, 1.0 - puddle);
        materialSample.ao = 0.88;
    }
    else if (materialId == 4)
    {
        materialSample.albedo = mix(vec3(0.26, 0.19, 0.11), vec3(0.52, 0.39, 0.24), macro);
        materialSample.albedo *= 0.82 + detail * 0.28;
        materialSample.roughness = 0.84;
    }
    else if (materialId == 5)
    {
        float cracks = smoothstep(0.72, 0.92, stripes * 0.5 + coarse * 0.7);
        materialSample.albedo = mix(vec3(0.55, 0.56, 0.57), vec3(0.76, 0.77, 0.78), macro);
        materialSample.albedo = mix(materialSample.albedo, vec3(0.28, 0.26, 0.24), cracks * 0.55);
        materialSample.roughness = 0.74;
    }
    else if (materialId == 6)
    {
        float slope = 1.0 - clamp(normal.y, 0.0, 1.0);
        float grassMask = smoothstep(0.18, 0.72, macro + detail * 0.45 - slope * 0.6);
        vec3 soil = mix(vec3(0.21, 0.16, 0.09), vec3(0.37, 0.27, 0.15), coarse);
        vec3 grass = mix(vec3(0.16, 0.28, 0.10), vec3(0.34, 0.54, 0.18), detail);
        materialSample.albedo = mix(soil, grass, grassMask);
        materialSample.roughness = mix(0.81, 0.96, grassMask);
        materialSample.ao = 0.95;
    }
    else if (materialId == 7)
    {
        float pebbles = smoothstep(0.50, 0.88, coarse);
        materialSample.albedo = mix(vec3(0.38, 0.35, 0.31), vec3(0.68, 0.65, 0.60), detail);
        materialSample.albedo = mix(materialSample.albedo, vec3(0.82, 0.79, 0.74), pebbles * 0.35);
        materialSample.roughness = 0.80;
    }
    else if (materialId == 8)
    {
        float veins = smoothstep(0.65, 0.92, abs(detail - 0.5) * 2.0 + coarse * 0.25);
        materialSample.albedo = mix(vec3(0.08, 0.09, 0.11), vec3(0.22, 0.24, 0.28), macro);
        materialSample.albedo += veins * vec3(0.06, 0.07, 0.08);
        materialSample.roughness = 0.60;
        materialSample.ao = 0.90;
    }
    else if (materialId == 9)
    {
        float plank = smoothstep(0.25, 0.75, stripes);
        materialSample.albedo = mix(vec3(0.33, 0.18, 0.08), vec3(0.68, 0.44, 0.20), plank);
        materialSample.albedo *= 0.88 + macro * 0.22;
        materialSample.roughness = 0.74;
    }
    else
    {
        materialSample.albedo *= 0.82 + detail * 0.24;
        materialSample.roughness = 0.82;
    }

    return materialSample;
}

MaterialSample SampleWaterMaterial(vec3 worldPosition, vec3 normal, float waterDepth)
{
    float ripples = TriplanarNoise(worldPosition + vec3(0.0, 0.0, scene.cameraPositionTime.w * 0.12), normal, 1.6);
    float swirl = TriplanarNoise(worldPosition * vec3(0.6, 1.0, 0.6), normal, 0.55);
    float depthFactor = clamp(waterDepth / 1.4, 0.0, 1.0);

    MaterialSample materialSample;
    materialSample.albedo = mix(vec3(0.07, 0.26, 0.46), vec3(0.18, 0.54, 0.72), 1.0 - depthFactor);
    materialSample.albedo *= 0.90 + ripples * 0.14 + swirl * 0.10;
    materialSample.roughness = mix(0.04, 0.12, depthFactor) + ripples * 0.025;
    materialSample.metallic = 0.0;
    materialSample.ao = 1.0;
    materialSample.emissive = 0.0;
    return materialSample;
}

MaterialSample ResolveMaterial(vec3 worldPosition, vec3 normal)
{
    float shadingModel = fragMaterial.w;
    int materialId = int(round(fragMaterial.x));

    if (abs(shadingModel - kSurfaceShadingWater) < 0.25)
    {
        return SampleWaterMaterial(worldPosition, normal, max(fragMaterial.y, 0.08));
    }

    if (abs(shadingModel - kSurfaceShadingTerrain) < 0.25)
    {
        return SampleTerrainMaterial(materialId, worldPosition, normal, fragColor.rgb);
    }

    MaterialSample materialSample;
    materialSample.albedo = fragColor.rgb;
    materialSample.roughness = clamp(fragMaterial.y > 0.0 ? fragMaterial.y : 0.72, 0.05, 1.0);
    materialSample.metallic = clamp(fragMaterial.z, 0.0, 1.0);
    materialSample.ao = 1.0;
    materialSample.emissive = 0.0;
    return materialSample;
}

float SampleShadowMap(vec2 uv, float compareDepth)
{
    float sampledDepth = texture(shadowMap, uv).r;
    return compareDepth - kShadowBias > sampledDepth ? 0.0 : 1.0;
}

float ComputeShadow(vec3 worldPosition)
{
    vec4 shadowClip = pc.worldToShadowClip * vec4(worldPosition, 1.0);
    shadowClip.xyz /= max(shadowClip.w, 1.0e-6);

    vec2 shadowUv = shadowClip.xy * 0.5 + 0.5;
    float shadowDepth = shadowClip.z;
    float shadow = 1.0;

    if (shadowUv.x >= 0.0 && shadowUv.x <= 1.0 &&
        shadowUv.y >= 0.0 && shadowUv.y <= 1.0 &&
        shadowDepth >= 0.0 && shadowDepth <= 1.0)
    {
        float visibility = 0.0;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                vec2 sampleUv = shadowUv + vec2(float(x), float(y)) * kShadowTexel;
                visibility += SampleShadowMap(sampleUv, shadowDepth);
            }
        }
        shadow = max(visibility / 9.0, kMinimumShadow);
    }

    return shadow;
}

vec3 ShadeMaterial(MaterialSample material, vec3 worldPosition, vec3 normal, float shadow)
{
    vec3 N = normalize(normal);
    vec3 V = normalize(scene.cameraPositionTime.xyz - worldPosition);
    if (length(V) <= 1.0e-5)
    {
        V = vec3(0.0, 0.0, 1.0);
    }
    vec3 L = normalize(scene.sunDirectionAmbient.xyz);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float roughness = clamp(material.roughness, 0.04, 1.0);
    float metallic = clamp(material.metallic, 0.0, 1.0);
    float alphaRoughness = roughness * roughness;
    float alphaSquared = alphaRoughness * alphaRoughness;
    float denom = max((NdotH * NdotH) * (alphaSquared - 1.0) + 1.0, 0.0001);
    float D = alphaSquared / (kPi * denom * denom);
    float k = ((roughness + 1.0) * (roughness + 1.0)) * 0.125;
    float Gv = NdotV / mix(NdotV, 1.0, k);
    float Gl = NdotL / mix(NdotL, 1.0, k);
    float G = Gv * Gl;

    vec3 F0 = mix(vec3(0.04), material.albedo, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    vec3 specular = (D * G * F) / max(4.0 * NdotL * NdotV, 0.0001);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kd * material.albedo / kPi;

    float skyFactor = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    float horizonFactor = clamp(1.0 - abs(N.y), 0.0, 1.0);
    vec3 skyColor = mix(scene.horizonColorSun.rgb, scene.fogColorDensity.rgb * 1.08, skyFactor);
    vec3 ambient = material.albedo * skyColor * (scene.sunDirectionAmbient.w * (0.55 + skyFactor * 0.85) * material.ao);
    vec3 bounce = scene.horizonColorSun.rgb * material.albedo * (0.10 + horizonFactor * 0.08);
    vec3 direct = (diffuse + specular) * NdotL * shadow * scene.horizonColorSun.w;
    vec3 color = ambient + bounce + direct + material.albedo * material.emissive;

    float distanceToCamera = length(scene.cameraPositionTime.xyz - worldPosition);
    float fogFactor = 1.0 - exp(-distanceToCamera * scene.fogColorDensity.w);
    color = mix(color, scene.fogColorDensity.rgb, clamp(fogFactor, 0.0, 1.0));
    return color;
}

float ComputeWeight(float alpha, float depth)
{
    float depthWeight = clamp(1.0 - depth * 0.9, 0.1, 1.0);
    return clamp(alpha * 4.0 + 0.01, 0.01, 4.0) * depthWeight;
}

void main()
{
    float alpha = clamp(fragColor.a, 0.0, 0.98);
    if (alpha <= 0.001)
    {
        discard;
    }

    vec3 normal = length(fragNormal) > 0.0001 ? normalize(fragNormal) : vec3(0.0, 1.0, 0.0);
    MaterialSample material = ResolveMaterial(fragWorldPosition, normal);
    float shadow = ComputeShadow(fragWorldPosition);
    vec3 litColor = ShadeMaterial(material, fragWorldPosition, normal, shadow);

    if (abs(fragMaterial.w - kSurfaceShadingWater) < 0.25)
    {
        vec3 viewDirection = normalize(scene.cameraPositionTime.xyz - fragWorldPosition);
        float fresnel = pow(1.0 - max(dot(normal, viewDirection), 0.0), 5.0);
        litColor = mix(litColor, scene.fogColorDensity.rgb * 1.16, fresnel * 0.45);
    }

    float weight = ComputeWeight(alpha, gl_FragCoord.z);
    outAccumulation = vec4(clamp(litColor, 0.0, 1.0) * alpha * weight, alpha * weight);
    outRevealage = alpha;
}
