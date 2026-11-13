#version 450
#if INTEGER_TARGET
layout(location=0) out uvec4 color;
void main() { color=uvec4(17,32,64,128); }
#else
#if BOUND
layout(early_fragment_tests) in;
layout(set=0,binding=0) uniform Params { float width; } params;
layout(set=0,binding=1) buffer Writes { uint count; } writes;
#endif
layout(location=0) out vec4 color;
void main() {
#if BOUND
    atomicAdd(writes.count, 1);
    float width = params.width;
#else
    float width = 8;
#endif
    color = vec4(1,0,0,gl_FragCoord.x / width);
}
#endif
