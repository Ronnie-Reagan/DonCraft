#version 450

const vec3 kShadowTint = vec3(0.52, 0.56, 0.62);
const float kShadowBias = 0.0012;
const float kShadowTexel = 1.0 / 2048.0;
const float kMinimumShadow = 0.42;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 0) out vec4 outAccumulation;
layout(location = 1) out float outRevealage;

layout(set = 0, binding = 0) uniform sampler2D shadowMap;

layout(push_constant) uniform PushConstants
{
    mat4 worldToClip;
    mat4 worldToShadowClip;
} pc;

float SampleShadowMap(vec2 uv, float compareDepth)
{
    const float sampledDepth = texture(shadowMap, uv).r;
    return compareDepth - kShadowBias > sampledDepth ? 0.0 : 1.0;
}

float ComputeWeight(float alpha, float depth)
{
    const float depthWeight = clamp(1.0 - depth * 0.9, 0.1, 1.0);
    return clamp(alpha * 4.0 + 0.01, 0.01, 4.0) * depthWeight;
}

void main()
{
    vec4 shadowClip = pc.worldToShadowClip * vec4(fragWorldPosition, 1.0);
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

    vec3 litColor = fragColor.rgb * mix(kShadowTint, vec3(1.0), shadow);
    float alpha = clamp(fragColor.a, 0.0, 0.98);
    if (alpha <= 0.001)
    {
        discard;
    }

    float weight = ComputeWeight(alpha, gl_FragCoord.z);
    outAccumulation = vec4(litColor * alpha * weight, alpha * weight);
    outRevealage = alpha;
}
