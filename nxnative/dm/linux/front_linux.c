// Linux port front end (dm/host/front.h). Headless for now: the guest runs on the calling thread and there is no window or text input.
// (To do: an SDL2 window and Vulkan surface, the gamepad, text entry.)
#include "front.h"

void front_run(void (*guest)(void)) { guest(); }
int front_screen_max_fps(void) { return 0; }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;
}
int mtl_tex_gpu_owned(uint64_t serial) { (void)serial; return 0; }
