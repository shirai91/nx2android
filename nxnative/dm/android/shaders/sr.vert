#version 450
// Android port: the super resolution pass's quad (gfx_vk_present.c, the pause menu's Super resolution). Two triangles cover exactly the game's picture
// rectangle in the window, and the texture coordinate walks the game's own picture rectangle inside its window texture, which sr.frag filters.
// dst: the picture's rectangle in clip space (x0 y0 x1 y1, as the overlay's: x to the right, y down); uv: the source rectangle (fractions of the
// window texture); flip: the window texture is stored bottom-up (vk_window_bottom_up), so its first row is the picture's last
layout(push_constant) uniform S { vec4 vp; vec4 dst; vec4 uv; float flip; float sharp; } s;   // (sharp is the fragment stage's; the block matches in both)
layout(location = 0) out vec4 vTex;
void main() {
    // A triangle whose parameter runs 0..2 would cover the rectangle only for a full-viewport quad: its extension past the rectangle is drawn too,
    // with the texture coordinate clamped to the edge (a smeared band under the picture, where the letterbox belongs)
    const vec2 corner[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 q = corner[gl_VertexIndex];
    gl_Position = vec4(mix(s.dst.xy, s.dst.zw, q), 0.0, 1.0);
    vTex = vec4(mix(s.uv.xy, s.uv.zw, vec2(q.x, s.flip > 0.5 ? 1.0 - q.y : q.y)), 0.0, 0.0);
}