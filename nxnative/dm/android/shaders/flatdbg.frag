#version 450
// Android port, debugging (gfx_vk_exec.c DM_VK_X_FLAT_VS): solid magenta in every colour target, for draws of the vertex shaders named, to see where they land
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
void main() { o0 = o1 = o2 = o3 = o4 = o5 = o6 = o7 = vec4(1.0, 0.0, 1.0, 1.0); }
