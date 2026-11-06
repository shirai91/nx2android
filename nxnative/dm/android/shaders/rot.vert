#version 450
// Android port: the present's pre-rotated copy (gfx_vk_present.c). One triangle covers the swapchain image, which is in the panel's own orientation;
// the texture coordinate is an affine map of the position (m: the rows of a 2x2 matrix, t: the offset), so the picture drawn in the window's
// orientation is read a quarter turn round
layout(push_constant) uniform R { vec4 m; vec2 t; } r;
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // (0,0) (2,0) (0,2): the image is the corner [0,1]^2
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    uv = vec2(dot(r.m.xy, p), dot(r.m.zw, p)) + r.t;
}
