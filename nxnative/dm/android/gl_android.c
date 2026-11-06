// OpenGL ES games on Android: the device's own EGL and OpenGL ES driver run the game's GL calls. FM24 Touch (Sports Interactive's engine on NVIDIA's OpenGL driver,
// subsdk0 NintendoSDK_OpenGL, which the host does not load) renders with OpenGL ES 3.1: its 1,224 shaders are GLSL ES text (RomFS sigraphics/shaders/gles), and its
// code calls 158 GL functions, fetched through eglGetProcAddress (dm/tools/loader_census.py). main imports 8 EGL functions, which bind to the ones here through the host's
// import table (hle.h swhle); eglGetProcAddress hands out EGL names from that table and GL names from the device's driver (Apple has a GL → Metal layer instead, dm/gfx/gl/hle_gl.m).
// Model:
//   · every driver call runs on the calling thread's big host stack (hle_bigstack.c): a game thread's stack can be 8 KB, a GL driver call needs more. A function the
//     driver hands out goes to the game as a stub of a table here (its address in x16, then dm_bigstack_enter); the few with more than 8 integer arguments (texture uploads,
//     blits, image copies) have wrappers that take their stack arguments and run the call on the big stack (gl_big);
//   · the game's window surface is a pbuffer of the vi layer's size (its default framebuffer, 0). eglSwapBuffers blits it, flipped, into one of three AHardwareBuffer-backed
//     textures, waits for the GL work (glFinish), and the Vulkan present path shows that buffer (vk_present_ahb, gfx_vk_present.c): the overlay, the pause menu and the frame
//     pacing stay as for NVN games;
//   · BC1 / BC2 / BC3 / BC4 / BC5 / BC7 textures (FM24's DDS: DXT1 and DXT5) are decoded on the CPU: Adreno's OpenGL ES has no BC formats;
//   · desktop-only calls the game asks for (glDrawBuffer, glPolygonMode, glClipControl) map to their ES forms or do nothing; the ANGLE / EXT names to the core ES ones.
#include "../host/hle.h"
#include "gfx_vk.h"
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec/bcdec.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

#define GLOG(...) LOG("[gl] " __VA_ARGS__)
#define RED1(...) do { static int once_; if (!once_) { once_ = 1; LOG("[gl] [RED] " __VA_ARGS__); } } while (0)

typedef uint32_t GLenum, GLuint, GLbitfield, GLboolean; typedef int32_t GLint, GLsizei; typedef float GLfloat; typedef intptr_t GLsizeiptr, GLintptr;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface, *EGLImageKHR, *EGLClientBuffer; typedef int32_t EGLint; typedef uint32_t EGLBoolean, EGLenum;
#define EGL_NONE 0x3038
#define EGL_TRUE 1

// ---------- the device's EGL and GLES ----------
static void *g_egl_lib, *g_gles_lib;
static void *(*r_eglGetProcAddress)(const char *);
static void *dev_sym(const char *name) {
    if (!g_egl_lib) { g_egl_lib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL); g_gles_lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
                      if (!g_egl_lib || !g_gles_lib) die("[gl] the device's libEGL.so / libGLESv3.so cannot be loaded");
                      r_eglGetProcAddress = (void *(*)(const char *))dlsym(g_egl_lib, "eglGetProcAddress"); }
    void *f = dlsym(!strncmp(name, "egl", 3) ? g_egl_lib : g_gles_lib, name);
    if (!f && r_eglGetProcAddress) f = r_eglGetProcAddress(name);
    return f;
}
#define REAL(ret, name, ...) static ret (*r_##name)(__VA_ARGS__); if (!r_##name) r_##name = (ret (*)(__VA_ARGS__))dev_sym(#name)

// ---------- running a call on the big host stack ----------
// gl_big(fn, arg): fn(arg) on this thread's big stack (dm_bigstack_enter with x16 = fn)
uint64_t gl_big(uint64_t (*fn)(void *), void *arg);
__asm__(".text\n.p2align 2\n.globl gl_big\n.hidden gl_big\ngl_big:\n    mov x16, x0\n    mov x0, x1\n    b dm_bigstack_enter\n");
// The table of driver functions handed to the game: stub k loads g_gl_fn[k] into x16 and enters the big stack with the game's registers as they are
#define NTRAMP 1024
void *g_gl_fn[NTRAMP]; static int g_ntramp;
extern const char gl_tramps[];
__asm__(".text\n.p2align 3\n.globl gl_tramps\n.hidden gl_tramps\ngl_tramps:\n.set gl_tk, 0\n.rept 1024\n"
        "    adrp x16, g_gl_fn\n    add x16, x16, :lo12:g_gl_fn\n    ldr x16, [x16, #(gl_tk * 8)]\n    b dm_bigstack_enter\n.set gl_tk, gl_tk + 1\n.endr\n");   // 16 bytes a stub
static void *tramp(void *fn) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; void *r = NULL;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < g_ntramp; i++) if (g_gl_fn[i] == fn) { r = (void *)(gl_tramps + 16 * i); break; }
    if (!r && g_ntramp < NTRAMP) { g_gl_fn[g_ntramp] = fn; r = (void *)(gl_tramps + 16 * g_ntramp++); }
    pthread_mutex_unlock(&mu);
    if (!r) RED1("more than %d GL functions: the rest run on the game's stack", NTRAMP);
    return r ? r : fn;
}

// ---------- wrappers (registered by name, looked up before the driver) ----------
typedef struct { const char *name; void *fn; } glw_ent;
#define GLW(name, ret, ...)                                                                                                         \
    static ret w_##name(__VA_ARGS__) __attribute__((used));                                                                        \
    extern const char w_##name##_bs[];                                                                                              \
    __asm__(".text\n.p2align 2\n.globl w_" #name "_bs\n.hidden w_" #name "_bs\nw_" #name "_bs:\n    adrp x16, w_" #name "\n    add x16, x16, :lo12:w_" #name "\n    b dm_bigstack_enter\n"); \
    __attribute__((used, section("swglw"))) static const glw_ent e_##name = {#name, (void *)w_##name##_bs};                       \
    static ret w_##name(__VA_ARGS__)
// More than 8 integer arguments: the wrapper runs on the game's stack (it reads the stack arguments), packs them and runs the work on the big stack
#define GLW_STACK(name, ret, ...)                                                                                                   \
    static ret w_##name(__VA_ARGS__);                                                                                               \
    __attribute__((used, section("swglw"))) static const glw_ent e_##name = {#name, (void *)w_##name};                             \
    static ret w_##name(__VA_ARGS__)
extern const glw_ent __start_swglw[], __stop_swglw[];

enum { GL_TEXTURE_2D = 0x0DE1, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_SKIP_ROWS = 0x0CF3, GL_UNPACK_SKIP_PIXELS = 0x0CF4, GL_UNPACK_IMAGE_HEIGHT = 0x806E,
       GL_UNPACK_SKIP_IMAGES = 0x806D, GL_PIXEL_UNPACK_BUFFER = 0x88EC, GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF, GL_MAP_READ_BIT = 1, GL_RGBA = 0x1908, GL_RED = 0x1903, GL_RG = 0x8227,
       GL_UNSIGNED_BYTE = 0x1401, GL_BYTE = 0x1400, GL_HALF_FLOAT = 0x140B, GL_RGBA8 = 0x8058, GL_SRGB8_ALPHA8 = 0x8C43, GL_R8 = 0x8229, GL_R8_SNORM = 0x8F94, GL_RG8 = 0x822B,
       GL_RG8_SNORM = 0x8F95, GL_RGBA16F = 0x881A, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA, GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
       GL_COLOR_BUFFER_BIT = 0x4000, GL_NEAREST = 0x2600, GL_SCISSOR_TEST = 0x0C11, GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_TEXTURE_BINDING_2D = 0x8069,
       GL_RASTERIZER_DISCARD = 0x8C89 };

// BC formats → what they decode into (dec as gfx_vk_tex.c: 1 BC1 RGB, 2 BC1, 3 BC2, 4 BC3, 5 / 6 BC4, 7 / 8 BC5, 11 BC7); 0 = not BC
typedef struct { int dec, bpb; GLenum ifmt, fmt, type; int dbpp; } bcfmt;
static int bc_of(GLenum f, bcfmt *o) {
    switch (f) {
    case 0x83F0: *o = (bcfmt){1, 8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;          // COMPRESSED_RGB_S3TC_DXT1
    case 0x8C4C: *o = (bcfmt){1, 8, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;
    case 0x83F1: *o = (bcfmt){2, 8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;          // RGBA_S3TC_DXT1
    case 0x8C4D: *o = (bcfmt){2, 8, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;
    case 0x83F2: *o = (bcfmt){3, 16, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;         // DXT3
    case 0x8C4E: *o = (bcfmt){3, 16, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;
    case 0x83F3: *o = (bcfmt){4, 16, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;         // DXT5
    case 0x8C4F: *o = (bcfmt){4, 16, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;
    case 0x8DBB: *o = (bcfmt){5, 8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1}; return 1;              // RED_RGTC1
    case 0x8DBC: *o = (bcfmt){6, 8, GL_R8_SNORM, GL_RED, GL_BYTE, 1}; return 1;
    case 0x8DBD: *o = (bcfmt){7, 16, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2}; return 1;             // RG_RGTC2
    case 0x8DBE: *o = (bcfmt){8, 16, GL_RG8_SNORM, GL_RG, GL_BYTE, 2}; return 1;
    case 0x8E8C: *o = (bcfmt){11, 16, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;        // RGBA_BPTC_UNORM
    case 0x8E8D: *o = (bcfmt){11, 16, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return 1;
    }
    return 0;
}
static void bc_dec(const bcfmt *f, const uint8_t *src, uint32_t w, uint32_t h, uint32_t z, uint8_t *dst) {
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4; size_t row = (size_t)w * f->dbpp; uint8_t t[4 * 4 * 4];
    for (uint32_t s = 0; s < z; s++) for (uint32_t y = 0; y < by; y++) for (uint32_t x = 0; x < bx; x++) {
        const uint8_t *b = src + (((size_t)s * by + y) * bx + x) * f->bpb;
        switch (f->dec) {
        case 1: case 2: bcdec_bc1(b, t, 16); if (f->dec == 1) for (int i = 0; i < 16; i++) t[i * 4 + 3] = 255; break;
        case 3: bcdec_bc2(b, t, 16); break;
        case 4: bcdec_bc3(b, t, 16); break;
        case 5: case 6: bcdec_bc4(b, t, 4, f->dec == 6); break;
        case 7: case 8: bcdec_bc5(b, t, 8, f->dec == 8); break;
        default: bcdec_bc7(b, t, 16); break;
        }
        uint32_t x0 = x * 4, y0 = y * 4, cw = w - x0 < 4 ? w - x0 : 4, ch = h - y0 < 4 ? h - y0 : 4;
        for (uint32_t r = 0; r < ch; r++) memcpy(dst + ((size_t)s * h + y0 + r) * row + (size_t)x0 * f->dbpp, t + r * 4 * f->dbpp, (size_t)cw * f->dbpp);
    }
}
// The compressed bytes: from client memory, or (an unpack buffer bound) mapped from that buffer at the offset the pointer holds. *unmap: a buffer to unmap after
static const uint8_t *src_bytes(const void *data, GLsizei size, int *unmap) {
    REAL(void, glGetIntegerv, GLenum, GLint *); REAL(void *, glMapBufferRange, GLenum, GLintptr, GLsizeiptr, GLbitfield);
    GLint pbo = 0; r_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo); *unmap = 0;
    if (!pbo) return data;
    *unmap = 1; return r_glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, (GLintptr)data, size, GL_MAP_READ_BIT);
}
// Plain uploads of the decoded texels: client memory, unpacked tightly (the game's unpack state is for its compressed data); restored after
typedef struct { GLint pbo, align, row, skr, skp, ih, ski; } unpack_t;
static void unpack_plain(unpack_t *u) {
    REAL(void, glGetIntegerv, GLenum, GLint *); REAL(void, glPixelStorei, GLenum, GLint); REAL(void, glBindBuffer, GLenum, GLuint);
    r_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &u->pbo); r_glGetIntegerv(GL_UNPACK_ALIGNMENT, &u->align); r_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &u->row);
    r_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &u->skr); r_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &u->skp); r_glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &u->ih); r_glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &u->ski);
    if (u->pbo) r_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    r_glPixelStorei(GL_UNPACK_ALIGNMENT, 1); r_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); r_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); r_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    r_glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0); r_glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
}
static void unpack_restore(const unpack_t *u) {
    REAL(void, glPixelStorei, GLenum, GLint); REAL(void, glBindBuffer, GLenum, GLuint);
    r_glPixelStorei(GL_UNPACK_ALIGNMENT, u->align); r_glPixelStorei(GL_UNPACK_ROW_LENGTH, u->row); r_glPixelStorei(GL_UNPACK_SKIP_ROWS, u->skr); r_glPixelStorei(GL_UNPACK_SKIP_PIXELS, u->skp);
    r_glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, u->ih); r_glPixelStorei(GL_UNPACK_SKIP_IMAGES, u->ski);
    if (u->pbo) r_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)u->pbo);
}
// One upload, 2D or 3D, image or sub-image, run on the big stack (the 3D and sub-image forms have up to 11 arguments)
typedef struct { int sub, dims; GLenum target; GLint level, x, y, z; GLsizei w, h, d; GLenum fmt; GLsizei size; const void *data; GLint border; } cup_t;
static uint64_t cup_run(void *a) {
    cup_t *c = a; bcfmt f;
    REAL(void, glCompressedTexImage2D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
    REAL(void, glCompressedTexImage3D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei, const void *);
    REAL(void, glCompressedTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
    REAL(void, glCompressedTexSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void *);
    REAL(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    REAL(void, glTexImage3D, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    REAL(void, glTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    REAL(void, glTexSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
    REAL(GLboolean, glUnmapBuffer, GLenum);
    if (!bc_of(c->fmt, &f)) {   // not BC: as the game asked
        if (c->dims == 2) { if (c->sub) r_glCompressedTexSubImage2D(c->target, c->level, c->x, c->y, c->w, c->h, c->fmt, c->size, c->data); else r_glCompressedTexImage2D(c->target, c->level, c->fmt, c->w, c->h, c->border, c->size, c->data); }
        else { if (c->sub) r_glCompressedTexSubImage3D(c->target, c->level, c->x, c->y, c->z, c->w, c->h, c->d, c->fmt, c->size, c->data); else r_glCompressedTexImage3D(c->target, c->level, c->fmt, c->w, c->h, c->d, c->border, c->size, c->data); }
        return 0;
    }
    { static int said; if (!said++) GLOG("BC textures (first: format %#x, %dx%d) are decoded on the CPU: OpenGL ES on Adreno has no BC formats", c->fmt, c->w, c->h); }
    uint32_t z = c->dims == 3 ? (uint32_t)c->d : 1; int unmap; const uint8_t *src = src_bytes(c->data, c->size, &unmap);
    size_t need = (size_t)((c->w + 3) / 4) * ((c->h + 3) / 4) * z * f.bpb;
    uint8_t *tx = src && (size_t)c->size >= need ? malloc((size_t)c->w * c->h * z * f.dbpp) : NULL;
    if (tx) bc_dec(&f, src, (uint32_t)c->w, (uint32_t)c->h, z, tx);
    else if (src) RED1("BC upload of %d bytes for %dx%dx%u needs %zu: left empty", c->size, c->w, c->h, z, need);
    if (unmap) r_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    unpack_t u; unpack_plain(&u);
    if (c->dims == 2) { if (c->sub) r_glTexSubImage2D(c->target, c->level, c->x, c->y, c->w, c->h, f.fmt, f.type, tx); else r_glTexImage2D(c->target, c->level, (GLint)f.ifmt, c->w, c->h, 0, f.fmt, f.type, tx); }
    else { if (c->sub) r_glTexSubImage3D(c->target, c->level, c->x, c->y, c->z, c->w, c->h, c->d, f.fmt, f.type, tx); else r_glTexImage3D(c->target, c->level, (GLint)f.ifmt, c->w, c->h, c->d, 0, f.fmt, f.type, tx); }
    unpack_restore(&u); free(tx);
    return 0;
}
GLW(glCompressedTexImage2D, void, GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei n, const void *p) { cup_t c = {0, 2, t, l, 0, 0, 0, w, h, 1, f, n, p, b}; cup_run(&c); }
GLW_STACK(glCompressedTexImage3D, void, GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLsizei d, GLint b, GLsizei n, const void *p) { cup_t c = {0, 3, t, l, 0, 0, 0, w, h, d, f, n, p, b}; gl_big(cup_run, &c); }
GLW_STACK(glCompressedTexSubImage2D, void, GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei n, const void *p) { cup_t c = {1, 2, t, l, x, y, 0, w, h, 1, f, n, p, 0}; gl_big(cup_run, &c); }
GLW_STACK(glCompressedTexSubImage3D, void, GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLsizei n, const void *p) { cup_t c = {1, 3, t, l, x, y, z, w, h, d, f, n, p, 0}; gl_big(cup_run, &c); }
// Immutable storage in a BC format: the decoded format instead
GLW(glTexStorage2D, void, GLenum t, GLsizei levels, GLenum f, GLsizei w, GLsizei h) { REAL(void, glTexStorage2D, GLenum, GLsizei, GLenum, GLsizei, GLsizei); bcfmt b; r_glTexStorage2D(t, levels, bc_of(f, &b) ? b.ifmt : f, w, h); }
GLW(glTexStorage3D, void, GLenum t, GLsizei levels, GLenum f, GLsizei w, GLsizei h, GLsizei d) { REAL(void, glTexStorage3D, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei); bcfmt b; r_glTexStorage3D(t, levels, bc_of(f, &b) ? b.ifmt : f, w, h, d); }
// More than 8 integer arguments, passed through on the big stack
typedef struct { uint64_t a[16]; } args_t;
#define A(i, T) (T)x->a[i]
static uint64_t p_glTexImage2D(void *v) { args_t *x = v; REAL(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    r_glTexImage2D(A(0, GLenum), A(1, GLint), A(2, GLint), A(3, GLsizei), A(4, GLsizei), A(5, GLint), A(6, GLenum), A(7, GLenum), A(8, const void *)); return 0; }
static uint64_t p_glTexSubImage2D(void *v) { args_t *x = v; REAL(void, glTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    r_glTexSubImage2D(A(0, GLenum), A(1, GLint), A(2, GLint), A(3, GLint), A(4, GLsizei), A(5, GLsizei), A(6, GLenum), A(7, GLenum), A(8, const void *)); return 0; }
static uint64_t p_glTexImage3D(void *v) { args_t *x = v; REAL(void, glTexImage3D, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    r_glTexImage3D(A(0, GLenum), A(1, GLint), A(2, GLint), A(3, GLsizei), A(4, GLsizei), A(5, GLsizei), A(6, GLint), A(7, GLenum), A(8, GLenum), A(9, const void *)); return 0; }
static uint64_t p_glTexSubImage3D(void *v) { args_t *x = v; REAL(void, glTexSubImage3D, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
    r_glTexSubImage3D(A(0, GLenum), A(1, GLint), A(2, GLint), A(3, GLint), A(4, GLint), A(5, GLsizei), A(6, GLsizei), A(7, GLsizei), A(8, GLenum), A(9, GLenum), A(10, const void *)); return 0; }
static uint64_t p_glBlitFramebuffer(void *v) { args_t *x = v; REAL(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    r_glBlitFramebuffer(A(0, GLint), A(1, GLint), A(2, GLint), A(3, GLint), A(4, GLint), A(5, GLint), A(6, GLint), A(7, GLint), A(8, GLbitfield), A(9, GLenum)); return 0; }
static uint64_t p_glCopyImageSubData(void *v) { args_t *x = v; REAL(void, glCopyImageSubData, GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei);
    r_glCopyImageSubData(A(0, GLuint), A(1, GLenum), A(2, GLint), A(3, GLint), A(4, GLint), A(5, GLint), A(6, GLuint), A(7, GLenum), A(8, GLint), A(9, GLint), A(10, GLint), A(11, GLint), A(12, GLsizei), A(13, GLsizei), A(14, GLsizei)); return 0; }
GLW_STACK(glTexImage2D, void, GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5, GLenum a6, GLenum a7, const void *a8) {
    args_t x = {{a0, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, a6, a7, (uint64_t)a8}}; gl_big(p_glTexImage2D, &x); }
GLW_STACK(glTexSubImage2D, void, GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5, GLenum a6, GLenum a7, const void *a8) {
    args_t x = {{a0, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, a6, a7, (uint64_t)a8}}; gl_big(p_glTexSubImage2D, &x); }
GLW_STACK(glTexImage3D, void, GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLsizei a5, GLint a6, GLenum a7, GLenum a8, const void *a9) {
    args_t x = {{a0, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, (uint64_t)a6, a7, a8, (uint64_t)a9}}; gl_big(p_glTexImage3D, &x); }
GLW_STACK(glTexSubImage3D, void, GLenum a0, GLint a1, GLint a2, GLint a3, GLint a4, GLsizei a5, GLsizei a6, GLsizei a7, GLenum a8, GLenum a9, const void *a10) {
    args_t x = {{a0, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, (uint64_t)a6, (uint64_t)a7, a8, a9, (uint64_t)a10}}; gl_big(p_glTexSubImage3D, &x); }
GLW_STACK(glBlitFramebuffer, void, GLint a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5, GLint a6, GLint a7, GLbitfield a8, GLenum a9) {
    args_t x = {{(uint64_t)a0, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, (uint64_t)a6, (uint64_t)a7, a8, a9}}; gl_big(p_glBlitFramebuffer, &x); }
GLW_STACK(glBlitFramebufferANGLE, void, GLint a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5, GLint a6, GLint a7, GLbitfield a8, GLenum a9) {
    args_t x = {{(uint64_t)a0, (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, (uint64_t)a6, (uint64_t)a7, a8, a9}}; gl_big(p_glBlitFramebuffer, &x); }
GLW_STACK(glCopyImageSubDataEXT, void, GLuint a0, GLenum a1, GLint a2, GLint a3, GLint a4, GLint a5, GLuint a6, GLenum a7, GLint a8, GLint a9, GLint a10, GLint a11, GLsizei a12, GLsizei a13, GLsizei a14) {
    args_t x = {{a0, a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, a6, a7, (uint64_t)a8, (uint64_t)a9, (uint64_t)a10, (uint64_t)a11, (uint64_t)a12, (uint64_t)a13, (uint64_t)a14}}; gl_big(p_glCopyImageSubData, &x); }
GLW_STACK(glCopyImageSubData, void, GLuint a0, GLenum a1, GLint a2, GLint a3, GLint a4, GLint a5, GLuint a6, GLenum a7, GLint a8, GLint a9, GLint a10, GLint a11, GLsizei a12, GLsizei a13, GLsizei a14) {
    args_t x = {{a0, a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4, (uint64_t)a5, a6, a7, (uint64_t)a8, (uint64_t)a9, (uint64_t)a10, (uint64_t)a11, (uint64_t)a12, (uint64_t)a13, (uint64_t)a14}}; gl_big(p_glCopyImageSubData, &x); }
// Desktop OpenGL names in their OpenGL ES form, or nothing where ES has none
GLW(glRenderbufferStorageMultisampleANGLE, void, GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h) { REAL(void, glRenderbufferStorageMultisample, GLenum, GLsizei, GLenum, GLsizei, GLsizei); r_glRenderbufferStorageMultisample(t, s, f, w, h); }
GLW(glDrawBuffer, void, GLenum m) { REAL(void, glDrawBuffers, GLsizei, const GLenum *); r_glDrawBuffers(1, &m); }
GLW(glPolygonMode, void, GLenum face, GLenum mode) { (void)face; if (mode != 0x1B02) RED1("glPolygonMode(%#x): OpenGL ES fills only", mode); }   // FILL
GLW(glClipControl, void, GLenum origin, GLenum depth) {
    static void (*f)(GLenum, GLenum); static int looked; if (!looked) { looked = 1; f = (void (*)(GLenum, GLenum))dev_sym("glClipControlEXT"); }
    if (f) f(origin, depth); else RED1("glClipControl(%#x, %#x): the driver has no GL_EXT_clip_control", origin, depth);
}
GLW(glFrameTerminatorGREMEDY, void, void) {}
GLW(glStringMarkerGREMEDY, void, GLsizei n, const void *s) { (void)n; (void)s; }

// ---------- EGL ----------
// Android's EGL objects are handed to the game as they are; the window surface is a pbuffer, and the frames reach the screen through Vulkan (below)
typedef struct { AHardwareBuffer *ahb; void *img; GLuint tex, fbo; vkimg_t *vk; uint64_t ticket; } slot_t;
static struct { EGLDisplay dpy; EGLConfig cfg; EGLSurface surf; EGLContext ctx; int w, h, interval; slot_t slot[3]; int cur; uint64_t frames; } G = {.interval = 1};
#define EGLB(name, ret, ...)                                                                                                        \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                                                        \
    extern const char h_##name##_bs[];                                                                                              \
    __asm__(".text\n.p2align 2\n.globl h_" #name "_bs\n.hidden h_" #name "_bs\nh_" #name "_bs:\n    adrp x16, h_" #name "\n    add x16, x16, :lo12:h_" #name "\n    b dm_bigstack_enter\n"); \
    __attribute__((used, DM_SECTION("swhle"))) static const hle_ent e_##name = {#name, (void *)h_##name##_bs};                     \
    static ret h_##name(__VA_ARGS__)
static void egl_init(void) {
    if (G.dpy) return;
    setenv("DM_GL_ACTIVE", "1", 1);   // the Vulkan side needs no shader bundle (gfx_vk_shader.c vk_shaders_open)
    REAL(EGLDisplay, eglGetDisplay, void *); REAL(EGLBoolean, eglInitialize, EGLDisplay, EGLint *, EGLint *);
    G.dpy = r_eglGetDisplay(NULL); EGLint ma = 0, mi = 0;
    if (!G.dpy || !r_eglInitialize(G.dpy, &ma, &mi)) die("[gl] the device's EGL does not initialize");
    GLOG("device EGL %d.%d", ma, mi);
}
EGLB(eglGetDisplay, EGLDisplay, void *native) { (void)native; egl_init(); return G.dpy; }
EGLB(eglInitialize, EGLBoolean, EGLDisplay d, EGLint *major, EGLint *minor) { (void)d; egl_init(); if (major) *major = 1; if (minor) *minor = 5; return EGL_TRUE; }
EGLB(eglTerminate, EGLBoolean, EGLDisplay d) { (void)d; return EGL_TRUE; }
EGLB(eglReleaseThread, EGLBoolean, void) { return EGL_TRUE; }
EGLB(eglBindAPI, EGLBoolean, EGLenum api) { REAL(EGLBoolean, eglBindAPI, EGLenum); if (api != 0x30a0) GLOG("eglBindAPI(%#x): OpenGL ES instead", api); return r_eglBindAPI(0x30a0); }
EGLB(eglQueryAPI, EGLenum, void) { return 0x30a0; }
EGLB(eglGetError, EGLint, void) { REAL(EGLint, eglGetError, void); return r_eglGetError(); }
// The device's config for RGBA8, depth 24, stencil 8, OpenGL ES 3, pbuffer: whatever the game asked (it asks for a window config of the Switch's driver)
EGLB(eglChooseConfig, EGLBoolean, EGLDisplay d, const EGLint *attr, EGLConfig *cfg, EGLint n, EGLint *num) {
    (void)d; (void)attr; egl_init();
    REAL(EGLBoolean, eglChooseConfig, EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    if (!G.cfg) { const EGLint want[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3040, 0x40, 0x3033, 0x1, EGL_NONE};   // RGBA 8, DEPTH 24, STENCIL 8, ES3_BIT, PBUFFER_BIT
                  EGLint got = 0; if (!r_eglChooseConfig(G.dpy, want, &G.cfg, 1, &got) || !got) die("[gl] no EGL config for RGBA8 D24S8 OpenGL ES 3 pbuffer"); }
    if (cfg && n > 0) cfg[0] = G.cfg;
    if (num) *num = 1;
    return EGL_TRUE;
}
EGLB(eglGetConfigAttrib, EGLBoolean, EGLDisplay d, EGLConfig c, EGLint attr, EGLint *v) { (void)d; (void)c; REAL(EGLBoolean, eglGetConfigAttrib, EGLDisplay, EGLConfig, EGLint, EGLint *); return r_eglGetConfigAttrib(G.dpy, G.cfg, attr, v); }
EGLB(eglQueryString, const char *, EGLDisplay d, EGLint name) { (void)d; egl_init(); REAL(const char *, eglQueryString, EGLDisplay, EGLint); return r_eglQueryString(G.dpy, name); }
// An OpenGL ES 3.2 context, whatever version or profile the game asks (the Switch's driver takes desktop versions too); a shared one shares with the device's
EGLB(eglCreateContext, EGLContext, EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attr) {
    (void)d; (void)c; (void)attr; egl_init();
    REAL(EGLContext, eglCreateContext, EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    if (!G.cfg) { EGLConfig cc; EGLint n; h_eglChooseConfig(G.dpy, NULL, &cc, 1, &n); }
    const EGLint a[] = {0x3098, 3, 0x30fb, 2, EGL_NONE};   // CONTEXT_MAJOR_VERSION 3, MINOR 2
    EGLContext ctx = r_eglCreateContext(G.dpy, G.cfg, share, a);
    if (!ctx) { const EGLint a1[] = {0x3098, 3, 0x30fb, 1, EGL_NONE}; ctx = r_eglCreateContext(G.dpy, G.cfg, share, a1); }
    if (!ctx) die("[gl] the device's EGL makes no OpenGL ES 3.1 context");
    if (!G.ctx) G.ctx = ctx;
    GLOG("EGL context %p (OpenGL ES 3.2%s)", ctx, share ? ", shared" : "");
    return ctx;
}
EGLB(eglDestroyContext, EGLBoolean, EGLDisplay d, EGLContext c) { (void)d; REAL(EGLBoolean, eglDestroyContext, EGLDisplay, EGLContext); return r_eglDestroyContext(G.dpy, c); }
static EGLSurface pbuffer(int w, int h) {
    REAL(EGLSurface, eglCreatePbufferSurface, EGLDisplay, EGLConfig, const EGLint *);
    if (!G.cfg) { EGLConfig cc; EGLint n; h_eglChooseConfig(G.dpy, NULL, &cc, 1, &n); }
    const EGLint a[] = {0x3057, w, 0x3056, h, EGL_NONE};   // WIDTH, HEIGHT
    return r_eglCreatePbufferSurface(G.dpy, G.cfg, a);
}
void dm_vi_layer_size(int *w, int *h);   // hle_vi.c: the size of the last sized vi::CreateLayer
EGLB(eglCreateWindowSurface, EGLSurface, EGLDisplay d, EGLConfig c, void *win, const EGLint *attr) {
    (void)d; (void)c; (void)win; (void)attr; egl_init();
    int w, h; dm_vi_layer_size(&w, &h); if (w <= 0 || h <= 0) { w = 1920; h = 1080; }
    EGLSurface s = pbuffer(w, h); if (!s) die("[gl] the device's EGL makes no %dx%d pbuffer", w, h);
    G.surf = s; G.w = w; G.h = h;
    GLOG("window surface: a %dx%d pbuffer, shown through Vulkan", w, h);
    return s;
}
EGLB(eglCreatePbufferSurface, EGLSurface, EGLDisplay d, EGLConfig c, const EGLint *attr) {
    (void)d; (void)c; int w = 1, h = 1; for (int i = 0; attr && attr[i] != EGL_NONE && i < 32; i += 2) { if (attr[i] == 0x3057) w = attr[i + 1]; else if (attr[i] == 0x3056) h = attr[i + 1]; }
    egl_init(); return pbuffer(w, h);
}
EGLB(eglDestroySurface, EGLBoolean, EGLDisplay d, EGLSurface s) { (void)d; REAL(EGLBoolean, eglDestroySurface, EGLDisplay, EGLSurface); if (s == G.surf) G.surf = NULL; return r_eglDestroySurface(G.dpy, s); }
EGLB(eglQuerySurface, EGLBoolean, EGLDisplay d, EGLSurface s, EGLint attr, EGLint *v) { (void)d; REAL(EGLBoolean, eglQuerySurface, EGLDisplay, EGLSurface, EGLint, EGLint *); return r_eglQuerySurface(G.dpy, s, attr, v); }
EGLB(eglMakeCurrent, EGLBoolean, EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
    (void)d; REAL(EGLBoolean, eglMakeCurrent, EGLDisplay, EGLSurface, EGLSurface, EGLContext); egl_init();
    EGLBoolean ok = r_eglMakeCurrent(G.dpy, dr, rd, c);
    if (!ok) { REAL(EGLint, eglGetError, void); RED1("eglMakeCurrent failed: %#x", r_eglGetError()); }
    return ok;
}
EGLB(eglGetCurrentContext, EGLContext, void) { REAL(EGLContext, eglGetCurrentContext, void); return r_eglGetCurrentContext(); }
EGLB(eglGetCurrentSurface, EGLSurface, EGLint which) { REAL(EGLSurface, eglGetCurrentSurface, EGLint); return r_eglGetCurrentSurface(which); }
EGLB(eglGetCurrentDisplay, EGLDisplay, void) { return G.dpy; }
EGLB(eglSwapInterval, EGLBoolean, EGLDisplay d, EGLint n) { (void)d; if (n != G.interval) { GLOG("swap interval %d", n); G.interval = n; } return EGL_TRUE; }
EGLB(eglWaitGL, EGLBoolean, void) { REAL(void, glFinish, void); r_glFinish(); return EGL_TRUE; }
EGLB(eglWaitNative, EGLBoolean, EGLint e) { (void)e; return EGL_TRUE; }

// The frame to the screen: the pbuffer's image, flipped into an AHardwareBuffer the Vulkan present path shows (gfx_vk_present.c vk_present_ahb)
vkimg_t *vk_image_from_ahb(AHardwareBuffer *b, uint32_t w, uint32_t h);
uint64_t vk_present_vkimg(vkimg_t *im, double period);
void mtl_present_wait(uint64_t tk);
void dm_guest_frame_tick(void);
static int slot_make(slot_t *s) {
    REAL(void *, eglGetNativeClientBufferANDROID, const AHardwareBuffer *); REAL(EGLImageKHR, eglCreateImageKHR, EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    REAL(void, glEGLImageTargetTexture2DOES, GLenum, void *); REAL(void, glGenTextures, GLsizei, GLuint *); REAL(void, glBindTexture, GLenum, GLuint);
    REAL(void, glGenFramebuffers, GLsizei, GLuint *); REAL(void, glBindFramebuffer, GLenum, GLuint); REAL(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint);
    REAL(void, glGetIntegerv, GLenum, GLint *);
    AHardwareBuffer_Desc dd = {(uint32_t)G.w, (uint32_t)G.h, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                               AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 0, 0, 0};
    if (AHardwareBuffer_allocate(&dd, &s->ahb)) { RED1("AHardwareBuffer_allocate %dx%d failed", G.w, G.h); return 0; }
    const EGLint ia[] = {0x30D2, EGL_TRUE, EGL_NONE};   // IMAGE_PRESERVED
    s->img = r_eglCreateImageKHR(G.dpy, NULL, 0x3140, r_eglGetNativeClientBufferANDROID(s->ahb), ia);   // EGL_NATIVE_BUFFER_ANDROID
    if (!s->img) { RED1("eglCreateImageKHR for the frame buffer failed"); return 0; }
    GLint tex0 = 0, fb0 = 0; r_glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0); r_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb0);
    r_glGenTextures(1, &s->tex); r_glBindTexture(GL_TEXTURE_2D, s->tex); r_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, s->img);
    r_glGenFramebuffers(1, &s->fbo); r_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo); r_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    r_glBindTexture(GL_TEXTURE_2D, (GLuint)tex0); r_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fb0);
    s->vk = vk_image_from_ahb(s->ahb, (uint32_t)G.w, (uint32_t)G.h);
    return s->vk != NULL;
}
EGLB(eglSwapBuffers, EGLBoolean, EGLDisplay d, EGLSurface surf) {
    (void)d; (void)surf;
    REAL(void, glGetIntegerv, GLenum, GLint *); REAL(void, glBindFramebuffer, GLenum, GLuint); REAL(GLboolean, glIsEnabled, GLenum); REAL(void, glDisable, GLenum); REAL(void, glEnable, GLenum);
    REAL(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum); REAL(void, glFinish, void);
    slot_t *s = &G.slot[G.cur];
    if (s->ticket) mtl_present_wait(s->ticket);   // its last present has been blitted to the screen: drawing into it again is safe
    if (!s->vk && !slot_make(s)) { dm_guest_frame_tick(); return EGL_TRUE; }
    GLint rf = 0, df = 0; r_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rf); r_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &df);
    GLboolean sc = r_glIsEnabled(GL_SCISSOR_TEST), rd = r_glIsEnabled(GL_RASTERIZER_DISCARD);
    if (sc) r_glDisable(GL_SCISSOR_TEST); if (rd) r_glDisable(GL_RASTERIZER_DISCARD);
    r_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); r_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
    r_glBlitFramebuffer(0, 0, G.w, G.h, 0, G.h, G.w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);   // GL's rows are bottom-up, the window image's top-down
    r_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rf); r_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)df);
    if (sc) r_glEnable(GL_SCISSOR_TEST); if (rd) r_glEnable(GL_RASTERIZER_DISCARD);
    r_glFinish();   // the GL work is done before Vulkan reads the buffer
    int vs = G.interval < 1 ? 1 : G.interval > 4 ? 4 : G.interval;
    s->ticket = vk_present_vkimg(s->vk, vs / 60.0);
    G.cur = (G.cur + 1) % 3;
    if (++G.frames <= 3 || G.frames % 1800 == 0) GLOG("frame %llu presented (%dx%d)", (unsigned long long)G.frames, G.w, G.h);
    dm_guest_frame_tick();
    return EGL_TRUE;
}
// GL names: a wrapper here, else the device driver's function behind a big-stack stub; EGL names: this layer's (the host's import table); anything else null
static void *hle_lookup(const char *name) {   // the host's import table (hle.c resolve_import), by name
    static const hle_ent *sec; static unsigned long sz;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    for (unsigned long i = 0; sec && i < sz / sizeof *sec; i++) if (!strcmp(sec[i].name, name)) return sec[i].fn;
    return NULL;
}
EGLB(eglGetProcAddress, void *, const char *name) {
    static int asked, found; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (!name) return NULL;
    void *fn = NULL;
    if (!strncmp(name, "egl", 3)) fn = hle_lookup(name);
    else if (!strncmp(name, "gl", 2)) {
        for (const glw_ent *e = __start_swglw; e < __stop_swglw; e++) if (!strcmp(e->name, name)) { fn = e->fn; break; }
        if (!fn) { void *d = dev_sym(name); if (d) fn = tramp(d); }
    }
    pthread_mutex_lock(&mu); asked++; if (fn) found++;
    if (asked % 200 == 0) GLOG("eglGetProcAddress: %d names asked, %d found (last %s)", asked, found, name);
    pthread_mutex_unlock(&mu);
    return fn;
}
