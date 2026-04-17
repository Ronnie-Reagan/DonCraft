#version 450

layout(location = 0) in vec2 inClipPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = inColor;
    gl_Position = vec4(inClipPosition, 0.0, 1.0);
}
