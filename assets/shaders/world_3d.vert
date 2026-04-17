#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec3 fragWorldPosition;

layout(push_constant) uniform PushConstants
{
    mat4 worldToClip;
    mat4 worldToShadowClip;
} pc;

void main()
{
    fragColor = inColor;
    fragWorldPosition = inPosition;
    gl_Position = pc.worldToClip * vec4(inPosition, 1.0);
}
