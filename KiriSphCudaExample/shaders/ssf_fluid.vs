#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out VS_OUT { vec2 texCoord; }
vs_out;

void main() {
  gl_Position = vec4(aPos, 0, 1.0);
  vs_out.texCoord = aTexCoord;
}