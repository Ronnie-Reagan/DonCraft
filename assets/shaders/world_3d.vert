#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inMaterial;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragMaterial;

layout(push_constant) uniform PushConstants
{
    mat4 worldToClip;
    mat4 worldToShadowClip;
} pc;

void main()
{
    fragColor = inColor;
    fragWorldPosition = inPosition;
    fragNormal = inNormal;
    fragMaterial = inMaterial;
    gl_Position = pc.worldToClip * vec4(inPosition, 1.0);
}
