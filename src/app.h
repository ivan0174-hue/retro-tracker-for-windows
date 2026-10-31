/*
 * PT8 - application (UI) interface. The UI draws into a fixed-size 32-bit
 * framebuffer; the platform layer scales it to the window.
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <stdbool.h>

#define SCR_W 720
#define SCR_H 480

enum { KMOD_SHIFT = 1, KMOD_CTRL = 2, KMOD_ALT = 4 };

void app_init(void);
void app_shutdown(void);
void app_frame(uint32_t *fb);
/* vk = Windows virtual key code, scancode = PC set-1 scancode */
void app_key_down(int vk, int scancode, bool extended, bool repeat, int mods);
void app_char(unsigned ch);
void app_mouse_move(int x, int y);
void app_mouse_button(int button, bool down);   /* 0 = left, 1 = right */
void app_mouse_wheel(int delta);                /* +1 = up, -1 = down */
void app_drop_file(const char *path);
bool app_request_quit(void);                    /* true = may quit now */
void app_open_file(const char *path);

#endif
