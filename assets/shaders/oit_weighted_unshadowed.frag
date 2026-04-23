#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 0) out vec4 outAccumulation;
layout(location = 1) out float outRevealage;

float ComputeWeight(float alpha, float depth)
{
    const float depthWeight = clamp(1.0 - depth * 0.9, 0.1, 1.0);
    return clamp(alpha * 4.0 + 0.01, 0.01, 4.0) * depthWeight;
}

void main()
{
    float alpha = clamp(fragColor.a, 0.0, 0.98);
    if (alpha <= 0.001)
    {
        discard;
    }

    float weight = ComputeWeight(alpha, gl_FragCoord.z);
    outAccumulation = vec4(fragColor.rgb * alpha * weight, alpha * weight);
    outRevealage = alpha;
}
