/*
 * Randomized input stress test for the PT8 app (build with ASan/UBSan).
 *   monkey [module.mod] [iterations] [seed] [sample.wav]
 */
#include "../src/app.h"
#include "../src/tracker.h"
#include "../src/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testenv.h"

static uint32_t fb[SCR_W * SCR_H];
static uint32_t fakeMs = 1000;
static uint64_t played;
static const char *modPath, *smpPath;
void plat_lock(void) {}
void plat_unlock(void) {}
uint64_t plat_audio_played_frames(void) { return played; }
bool plat_audio_ok(void) { return true; }
bool plat_open_dialog(int k, char *p, int s) { snprintf(p, s, "%s", k == FT_MODULE ? modPath : smpPath); return rand() % 2; }
bool plat_save_dialog(int k, const char *d, char *p, int s) { (void)d; snprintf(p, s, "%s", tpath(k == FT_MODULE ? "monkey_out.mod" : "monkey_out.wav")); return rand() % 4 == 0; }
void plat_quit(void) {}
void plat_set_title(const char *t) { (void)t; }
uint32_t plat_ms(void) { return fakeMs; }
const char *plat_config_path(void) { return tpath("monkey.ini"); }
FILE *plat_fopen(const char *p, const char *m) { return fopen(p, m); }

int main(int argc, char **argv)
{
    static char modCopy[600], smpCopy[600];
    /* work on copies: SAVE MOD / SAVE SMP may write back to the loaded files */
    snprintf(modCopy, sizeof modCopy, "%s", tpath("monkey_in.mod"));
    if (!copy_file(argc > 1 ? argv[1] : "../examples/demo8.mod", modCopy)) { fprintf(stderr, "can't read module\n"); return 1; }
    modPath = modCopy;
    snprintf(smpCopy, sizeof smpCopy, "%s", tpath("monkey_in.wav"));
    if (argc > 4) copy_file(argv[4], smpCopy); else write_test_wav(smpCopy);
    smpPath = smpCopy;
    int iters = argc > 2 ? atoi(argv[2]) : 20000;
    srand(argc > 3 ? (unsigned)atoi(argv[3]) : 1u);
    app_init();
    app_open_file(modPath);
    static int16_t abuf[2048 * 2];
    static const int keys[] = { 0x20, 0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x24, 0x23, 0x09, 0x2E, 0x08, 0x2D,
        0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x6B, 0x6D, 0x1B, 0x0D,
        '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F','Z','Y','X','Q','W' };
    for (int i = 0; i < iters; i++) {
        int r = rand() % 100;
        if (r < 35) {
            int vk = keys[rand() % (int)(sizeof keys / sizeof keys[0])];
            int sc = rand() % 0x40;
            int mods = rand() % 8 == 0 ? (1 << (rand() % 3)) : 0;
            if (vk == 'Q' && (mods & KMOD_CTRL)) mods = 0;
            app_key_down(vk, sc, rand() % 10 == 0, rand() % 5 == 0, mods);
        } else if (r < 70) {
            app_mouse_move(rand() % SCR_W, rand() % SCR_H);
            app_mouse_button(rand() % 6 == 0, true);
        } else if (r < 80) {
            app_mouse_move(rand() % SCR_W, rand() % SCR_H);
        } else if (r < 85) {
            app_mouse_wheel(rand() % 2 ? 1 : -1);
        } else if (r < 88) {
            app_char('a' + rand() % 26);
        } else if (r < 89) {
            app_drop_file(rand() % 2 ? modPath : smpPath);
        }
        if (i % 400 == 0) { app_mouse_move(216 + 120, 3 * 11 + 5); app_mouse_button(0, true); }   /* toggle SAMPLER */
        if (i % 400 > 200 && rand() % 3 == 0) { app_mouse_move(rand() % SCR_W, 160 + rand() % 320); app_mouse_button(0, true); }
        app_frame(fb);
        if (rand() % 3) { app_mouse_move(rand() % SCR_W, rand() % SCR_H); app_frame(fb); }
        app_mouse_button(0, false); app_mouse_button(1, false);
        fakeMs += 7 + rand() % 40;
        mixer_render(abuf, 300 + rand() % 1500);
        played = replayer_frames_rendered() > 2000 ? replayer_frames_rendered() - 2000 : 0;
    }
    printf("monkey ok (%d iterations)\n", iters);
    return 0;
}
