#version 450

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform PushConstants
{
    mat4 worldToClip;
} pc;

void main()
{
    gl_Position = pc.worldToClip * vec4(inPosition, 1.0);
}
