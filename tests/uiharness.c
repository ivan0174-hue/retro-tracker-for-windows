/*
 * Headless UI harness: drives the PT8 app with synthetic input and writes
 * framebuffer screenshots (PPM) so the UI can be checked without Windows.
 *   uiharness [module.mod] [any second arg = start from position 1]
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
static int quitReq;

void plat_lock(void) {}
void plat_unlock(void) {}
uint64_t plat_audio_played_frames(void) { return played; }
bool plat_audio_ok(void) { return true; }
static const char *nextOpen;
bool plat_open_dialog(int k, char *p, int s) { (void)k; if (!nextOpen) return false; snprintf(p, s, "%s", nextOpen); nextOpen = NULL; return true; }
static const char *nextSave;
bool plat_save_dialog(int k, const char *d, char *p, int s) { (void)k; (void)d; if (!nextSave) return false; snprintf(p, s, "%s", nextSave); nextSave = NULL; return true; }
void plat_quit(void) { quitReq = 1; }
void plat_set_title(const char *t) { printf("TITLE: %s\n", t); }
uint32_t plat_ms(void) { return fakeMs; }
const char *plat_config_path(void) { return tpath("uiharness.ini"); }
FILE *plat_fopen(const char *p, const char *m) { return fopen(p, m); }

static void shot(const char *name)
{
    char file[128];
    snprintf(file, sizeof file, "shot_%s.ppm", name);
    FILE *f = fopen(tpath(file), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    for (int i = 0; i < SCR_W * SCR_H; i++) { uint32_t c = fb[i]; fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f); }
    fclose(f);
}

static void frame(void) { app_frame(fb); fakeMs += 16; }
static void click(int x, int y) { app_mouse_move(x, y); app_mouse_button(0, true); frame(); app_mouse_button(0, false); frame(); }
static void key(int vk, int sc, int mods) { app_key_down(vk, sc, false, false, mods); frame(); }
static void advanceAudio(int frames)
{
    static int16_t buf[4096 * 2];
    while (frames > 0) { int n = frames > 4096 ? 4096 : frames; mixer_render(buf, n); frames -= n; }
    played = replayer_frames_rendered() > 3000 ? replayer_frames_rendered() - 3000 : 0;
}

int main(int argc, char **argv)
{
    app_init();
    frame();
    shot("01_empty");
    if (argc > 1) { app_open_file(argv[1]); frame(); frame(); }
    shot("02_loaded");
    /* play song for ~3 seconds */
    if (argc > 2) key(0x27, 0x4D, KMOD_ALT);   /* Alt+Right: next position */
    key(0x75, 0x40, 0);           /* F6 */
    for (int i = 0; i < 180; i++) { advanceAudio(800); frame(); }
    shot("03_playing");
    key(0x77, 0x42, 0);           /* F8 stop */
    key(0x20, 0x39, 0);           /* space: edit mode on */
    key(0x27, 0x4D, 0); key(0x27, 0x4D, 0); key(0x27, 0x4D, 0);   /* cursor to effect column */
    frame();
    shot("04_edit");
    click(216 + 120, 3 * 11 + 5);  /* SAMPLER button (col 2, row 3) */
    for (int i = 0; i < 3; i++) frame();
    shot("05_sampler");
    /* mark a range by dragging */
    app_mouse_move(200, 250); app_mouse_button(0, true); frame();
    app_mouse_move(400, 250); frame(); app_mouse_button(0, false); frame();
    shot("06_sampler_range");
    click(216 + 120, 3 * 11 + 5);  /* back to pattern */
    click(216 + 120, 2 * 11 + 5);  /* EDIT OP. */
    frame();
    shot("07_editop");
    click(216 + 40, 8 * 11 + 5);   /* SETUP */
    frame();
    shot("08_setup");
    click(216 + 120, 8 * 11 + 5);  /* HELP */
    frame();
    shot("09_help");
    click(216 + 120, 1 * 11 + 5);  /* CLEAR -> dialog */
    frame();
    shot("10_dialog");
    app_key_down(0x1B, 0x01, false, false, 0); frame();
    click(216 + 40, 10 * 11 + 5);  /* POS ED. */
    frame();
    shot("11_posed");
    printf("done quit=%d\n", quitReq);
    return 0;
}
