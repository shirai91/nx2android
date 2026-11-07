#version 450
// Integer ClearTexture values keep their bits; the pipeline selects channels.
layout(push_constant) uniform P { uvec4 c; } p;
layout(location = 0) out uvec4 o;
void main() { o = p.c; }
