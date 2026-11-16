// Linux port: the window is an SDL2 window (linux/front_linux.c); only the type the backend's signatures name remains.
#pragma once
typedef struct ANativeWindow ANativeWindow;
int ANativeWindow_getWidth(ANativeWindow *w);    // front_linux.c: the SDL window's drawable size
int ANativeWindow_getHeight(ANativeWindow *w);
