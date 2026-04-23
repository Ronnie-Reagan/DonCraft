#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D accumulationTexture;
layout(set = 0, binding = 2) uniform sampler2D revealageTexture;

void main()
{
    vec4 accumulation = texture(accumulationTexture, fragUv);
    float revealage = texture(revealageTexture, fragUv).r;

    float alpha = clamp(1.0 - revealage, 0.0, 1.0);
    if (alpha <= 0.0001)
    {
        discard;
    }

    vec3 color = accumulation.rgb / max(accumulation.a, 1.0e-5);
    outColor = vec4(color, alpha);
}
