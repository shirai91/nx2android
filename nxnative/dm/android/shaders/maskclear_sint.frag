#version 450
// Signed integer ClearTexture values keep their bits, including negative values.
layout(push_constant) uniform P { ivec4 c; } p;
layout(location = 0) out ivec4 o;
void main() { o = p.c; }
