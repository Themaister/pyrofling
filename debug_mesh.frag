#version 450
precision mediump float;

layout(location = 0) in mediump vec4 vColor;
layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = vColor;
}
