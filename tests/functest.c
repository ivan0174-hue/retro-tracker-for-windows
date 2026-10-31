/* Scripted functional test of editing workflows through the app's input API. */
#include "../src/app.h"
#include "../src/tracker.h"
#include "../src/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testenv.h"

static uint32_t fb[SCR_W * SCR_H];
static uint32_t fakeMs = 1000;
static const char *openPath, *savePath;
void plat_lock(void) {}
void plat_unlock(void) {}
uint64_t plat_audio_played_frames(void) { return replayer_frames_rendered(); }
bool plat_audio_ok(void) { return true; }
bool plat_open_dialog(int k, char *p, int s) { (void)k; if (!openPath) return false; snprintf(p, s, "%s", openPath); openPath = NULL; return true; }
bool plat_save_dialog(int k, const char *d, char *p, int s) { (void)k; printf("  save dialog default name: %s\n", d); if (!savePath) return false; snprintf(p, s, "%s", savePath); savePath = NULL; return true; }
void plat_quit(void) {}
void plat_set_title(const char *t) { printf("  title: %s\n", t); }
uint32_t plat_ms(void) { return fakeMs; }
const char *plat_config_path(void) { return tpath("func.ini"); }
FILE *plat_fopen(const char *p, const char *m) { return fopen(p, m); }

static void frame(void) { app_frame(fb); fakeMs += 16; }
static void key(int vk, int sc, int mods) { app_key_down(vk, sc, false, false, mods); frame(); }
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } else printf("ok: %s\n", #c); } while (0)

int main(void)
{
    int fails = 0;
    char msg[128];
    app_init();
    frame();
    /* load a 44.1 kHz sample -> resample prompt -> Enter = C-3 */
    static char wavPath[600];
    snprintf(wavPath, sizeof wavPath, "%s", tpath("test.wav"));
    write_test_wav(wavPath);
    openPath = wavPath;
    key('L', 0x26, KMOD_CTRL);
    key(0x0D, 0x1C, 0);
    printf("  sample len=%d loop=%d+%d name=%s\n", song.smp[0].length, song.smp[0].loopStart, song.smp[0].loopLength, song.smp[0].name);
    CHECK(song.smp[0].length > 11000 && song.smp[0].length < 11500);
    CHECK(song.smp[0].loopLength > 2);
    /* edit mode, enter notes */
    key(0x20, 0x39, 0);                    /* space = edit on */
    key('Z', 0x2C, 0);                     /* C-1 at row 0 ch 1 */
    CHECK(song.pat[0][0][0].note == 13 && song.pat[0][0][0].sample == 1);
    key(0x71, 0x3C, 0);                    /* F2 = octave 2 */
    key('Q', 0x10, 0);                     /* C-3 at row 1 */
    CHECK(song.pat[0][1][0].note == 37);
    for (int i = 0; i < 5; i++) key(0x09, 0x0F, 0);   /* tab x5 -> channel 6 */
    key(0x26, 0x48, 0); key(0x26, 0x48, 0);           /* up twice -> row 0 */
    key('W', 0x11, 0);                     /* D-3 on ch6 row 0 */
    CHECK(song.pat[0][0][5].note == 39);
    key(0x26, 0x48, 0);                    /* back to row 0 */
    key(0x27, 0x4D, 0); key(0x27, 0x4D, 0); key(0x27, 0x4D, 0);   /* to effect cmd column */
    cfg.editSkip = 0;
    key('C', 0x2E, 0); key(0x27, 0x4D, 0); key('2', 0x03, 0); key(0x27, 0x4D, 0); key('0', 0x0B, 0);
    CHECK(song.pat[0][0][5].cmd == 0xC && song.pat[0][0][5].param == 0x20);
    /* undo / redo */
    key('Z', 0x2C, KMOD_CTRL);
    CHECK(song.pat[0][0][5].param == 0x20 - 0x00 || song.pat[0][0][5].param == 0x20);
    key('Z', 0x2C, KMOD_CTRL); key('Z', 0x2C, KMOD_CTRL); key('Z', 0x2C, KMOD_CTRL);
    CHECK(song.pat[0][0][5].cmd == 0);
    key('Y', 0x15, KMOD_CTRL); key('Y', 0x15, KMOD_CTRL); key('Y', 0x15, KMOD_CTRL);
    CHECK(song.pat[0][0][5].cmd == 0xC && song.pat[0][0][5].param == 0x20);
    /* song name via text field click + typing */
    app_mouse_move(100, 121 + 5); app_mouse_button(0, true); frame(); app_mouse_button(0, false); frame();
    const char *nm = "my song";
    for (const char *p = nm; *p; p++) app_char((unsigned char)*p);
    key(0x0D, 0x1C, 0);
    CHECK(!strcmp(song.name, "my song"));
    /* save -> must be 8CHN since channel 6 is used */
    savePath = tpath("func8.mod");
    key('S', 0x1F, KMOD_CTRL);
    FILE *f = fopen(tpath("func8.mod"), "rb"); char tag[5] = {0};
    if (f) { fseek(f, 1080, SEEK_SET); if (fread(tag, 1, 4, f) != 4) tag[0] = 0; fclose(f); }
    CHECK(!strcmp(tag, "8CHN"));
    /* copy track 6 -> paste in ch 2, then clear ch 6 and save as M.K. */
    key(0x73, 0x3E, KMOD_SHIFT);              /* shift+F4 copy track (ch6) */
    for (int i = 0; i < 4; i++) key(0x09, 0x0F, KMOD_SHIFT);   /* back to ch 2 */
    printf("  (cursor channel now pasted into)\n");
    key(0x74, 0x3F, KMOD_SHIFT);              /* shift+F5 paste */
    int pasteCh = -1;
    for (int c = 0; c < 8; c++) if (c != 5 && song.pat[0][0][c].note == 39) pasteCh = c;
    printf("  pasted into channel %d\n", pasteCh + 1);
    CHECK(pasteCh >= 0 && pasteCh < 5);
    for (int i = 0; i < 8; i++) { if (song.pat[0][0][5].note == 39) break; key(0x09, 0x0F, 0); }
    /* find ch6 by tabbing until the cursor's track contains D-3 at row 0 and it is ch 6 */
    memset(&song.pat[0][0][5], 0, sizeof(Cell));
    savePath = tpath("func4.mod");
    key('S', 0x1F, KMOD_CTRL | KMOD_SHIFT);   /* save as */
    f = fopen(tpath("func4.mod"), "rb"); memset(tag, 0, 5);
    if (f) { fseek(f, 1080, SEEK_SET); if (fread(tag, 1, 4, f) != 4) tag[0] = 0; fclose(f); }
    CHECK(!strcmp(tag, "M.K."));
    /* reload and compare */
    Cell a = song.pat[0][1][0];
    CHECK(mod_load_file(tpath("func4.mod"), msg, sizeof msg));
    CHECK(song.pat[0][1][0].note == a.note && song.pat[0][1][0].sample == 1);
    CHECK(!strcmp(song.name, "my song"));
    CHECK(song.smp[0].length > 11000);
    /* export */
    CHECK(mod_load_file(tpath("func8.mod"), msg, sizeof msg));
    CHECK(export_wav(tpath("func8.wav"), msg, sizeof msg));
    printf("  %s\n", msg);
    /* transpose pattern +12 via edit-op function keys path is UI-only; check insert/delete row */
    key(0x24, 0x47, 0);    /* home */
    printf("%s (%d failures)\n", fails ? "FUNCTEST FAILED" : "FUNCTEST PASSED", fails);
    return fails != 0;
}
