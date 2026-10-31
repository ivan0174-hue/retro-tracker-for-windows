/*
 * Headless test tool for the PT8 core (Linux build):
 *   coretest render <in.mod> <out.wav>       render with PT8 replayer
 *   coretest roundtrip <in.mod> <tmp.mod>    load -> save -> load and compare
 *   coretest xmp <in.mod> <out.wav>          reference render with libxmp
 *   coretest info <in.mod>
 */
#include "../src/tracker.h"
#include "../src/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef WITH_XMP
#include <xmp.h>
#endif

void plat_lock(void) {}
void plat_unlock(void) {}
uint64_t plat_audio_played_frames(void) { return replayer_frames_rendered(); }
bool plat_audio_ok(void) { return true; }
bool plat_open_dialog(int k, char *p, int s) { (void)k; (void)p; (void)s; return false; }
bool plat_save_dialog(int k, const char *d, char *p, int s) { (void)k; (void)d; (void)p; (void)s; return false; }
void plat_quit(void) {}
void plat_set_title(const char *t) { (void)t; }
uint32_t plat_ms(void) { return 0; }
const char *plat_config_path(void) { return "/tmp/pt8-coretest.ini"; }
FILE *plat_fopen(const char *p, const char *m) { return fopen(p, m); }

static int cmp_song(Song *a, Song *b)
{
    int diffs = 0;
    if (a->numOrders != b->numOrders || memcmp(a->orders, b->orders, 128)) { printf("orders differ\n"); diffs++; }
    if (strcmp(a->name, b->name)) { printf("name differs '%s' '%s'\n", a->name, b->name); diffs++; }
    for (int p = 0; p < MAX_PATTERNS; p++)
        if (memcmp(a->pat[p], b->pat[p], sizeof(a->pat[p]))) { printf("pattern %d differs\n", p); diffs++; }
    for (int i = 0; i < NUM_SAMPLES; i++) {
        Sample *x = &a->smp[i], *y = &b->smp[i];
        if (x->length != y->length || x->loopStart != y->loopStart || x->loopLength != y->loopLength ||
            x->volume != y->volume || x->finetune != y->finetune || strcmp(x->name, y->name)) {
            printf("sample %d header differs (%d/%d %d/%d %d/%d)\n", i, x->length, y->length, x->loopStart, y->loopStart, x->loopLength, y->loopLength);
            diffs++;
        } else if (x->length && memcmp(x->data, y->data, (size_t)x->length)) {
            printf("sample %d data differs\n", i); diffs++;
        }
    }
    return diffs;
}

int main(int argc, char **argv)
{
    char msg[256];
    if (argc < 3) { fprintf(stderr, "usage\n"); return 1; }
    song_init();
    replayer_init(48000);
    if (!strcmp(argv[1], "render") && argc >= 4) {
        if (!mod_load_file(argv[2], msg, sizeof msg)) { printf("LOAD FAIL: %s\n", msg); return 2; }
        printf("load: %s\n", msg);
        if (argc >= 5) cfg.stereoSep = atoi(argv[4]);
        if (!export_wav(argv[3], msg, sizeof msg)) { printf("EXPORT FAIL: %s\n", msg); return 3; }
        printf("export: %s\n", msg);
        return 0;
    }
    if (!strcmp(argv[1], "info")) {
        if (!mod_load_file(argv[2], msg, sizeof msg)) { printf("LOAD FAIL: %s\n", msg); return 2; }
        printf("%s | name='%s' orders=%d ext=%d upper=%d pats=%d\n", msg, song.name, song.numOrders, song.extendedNotes,
               song_uses_upper_channels(), song_num_patterns_to_save());
        for (int i = 0; i < NUM_SAMPLES; i++) if (song.smp[i].length)
            printf("  smp %2d '%s' len=%d ls=%d ll=%d v=%d ft=%d\n", i + 1, song.smp[i].name, song.smp[i].length,
                   song.smp[i].loopStart, song.smp[i].loopLength, song.smp[i].volume, song.smp[i].finetune);
        return 0;
    }
    if (!strcmp(argv[1], "roundtrip") && argc >= 4) {
        if (!mod_load_file(argv[2], msg, sizeof msg)) { printf("LOAD FAIL: %s\n", msg); return 2; }
        static Song a;
        a = song;
        for (int i = 0; i < NUM_SAMPLES; i++) if (song.smp[i].data) {
            a.smp[i].data = malloc((size_t)song.smp[i].length + 4);
            memcpy(a.smp[i].data, song.smp[i].data, (size_t)song.smp[i].length);
        }
        if (!mod_save_file(argv[3], msg, sizeof msg)) { printf("SAVE FAIL: %s\n", msg); return 3; }
        printf("save: %s\n", msg);
        if (!mod_load_file(argv[3], msg, sizeof msg)) { printf("RELOAD FAIL: %s\n", msg); return 4; }
        int d = cmp_song(&a, &song);
        printf("roundtrip diffs: %d\n", d);
        return d ? 5 : 0;
    }
#ifdef WITH_XMP
    if (!strcmp(argv[1], "trace")) {
        /* compare per-tick period/volume against libxmp */
        extern void replayer_debug_voice(int, int *, int *, int *);
        extern void replayer_debug_tick(void);
        extern int replayer_debug_pos(int *, int *);
        extern void replayer_debug_mix(int);
        extern double replayer_debug_ticklen(void);
        double acc = 0;
        if (!mod_load_file(argv[2], msg, sizeof msg)) { printf("LOAD FAIL: %s\n", msg); return 2; }
        int maxTicks = argc >= 4 ? atoi(argv[3]) : 3000;
        int verbose = argc >= 5 ? atoi(argv[4]) : 0;
        xmp_context ctx = xmp_create_context();
        if (xmp_load_module(ctx, argv[2]) < 0) { printf("xmp load fail\n"); return 2; }
        xmp_start_player(ctx, 48000, 0);
        int nch = 0;
        { struct xmp_module_info mi; xmp_get_module_info(ctx, &mi); nch = mi.mod->chn; }
        if (nch > 8) nch = 8;
        replayer_play(PLAY_SONG, 0, 0, 0);
        int perMis = 0, volMis = 0, posMis = 0, shown = 0;
        for (int t = 0; t < maxTicks; t++) {
            if (xmp_play_frame(ctx) != 0) break;
            struct xmp_frame_info fi; xmp_get_frame_info(ctx, &fi);
            if (fi.loop_count > 0) break;
            replayer_debug_tick();
            acc += replayer_debug_ticklen();
            int nmix = (int)acc; acc -= nmix;
            replayer_debug_mix(nmix);
            int o, r; int tk = replayer_debug_pos(&o, &r);
            if (verbose == 2 && t > maxTicks - 40) printf("t%d mine %d/%d tk%d | xmp %d/%d f%d sp%d bpm%d\n", t, o, r, tk, fi.pos, fi.row, fi.frame, fi.speed, fi.bpm);
            if (o != fi.pos || r != fi.row) { posMis++; if (shown < 10) { printf("t%d POS mine %d/%d xmp %d/%d\n", t, o, r, fi.pos, fi.row); shown++; } }
            for (int c = 0; c < nch; c++) {
                int per, vol, act; replayer_debug_voice(c, &per, &vol, &act);
                struct xmp_channel_info *ci = &fi.channel_info[c];
                int xp = (int)(ci->period / 4096.0 + 0.5), xv = ci->volume;
                if (!act || (xv == 0 && vol == 0)) continue;
                int pmin = song.extendedNotes ? 28 : 113;
                int myp = per; (void)pmin;
                if (abs(myp - xp) > 1) { perMis++; if (shown < 25 || verbose) { printf("t%d o%d r%d tk%d ch%d PER mine %d xmp %d\n", t, o, r, tk, c, myp, xp); shown++; } }
                if (vol != xv) { volMis++; if (shown < 25 || verbose) { printf("t%d o%d r%d tk%d ch%d VOL mine %d xmp %d\n", t, o, r, tk, c, vol, xv); shown++; } }
            }
        }
        printf("mismatch: pos %d per %d vol %d\n", posMis, perMis, volMis);
        return 0;
    }
    if (!strcmp(argv[1], "xmp") && argc >= 4) {
        xmp_context ctx = xmp_create_context();
        if (xmp_load_module(ctx, argv[2]) < 0) { printf("xmp load fail\n"); return 2; }
        xmp_start_player(ctx, 48000, 0);
        xmp_set_player(ctx, XMP_PLAYER_INTERP, XMP_INTERP_NEAREST);
        xmp_set_player(ctx, XMP_PLAYER_MIX, 30);
        FILE *f = fopen(argv[3], "wb");
        uint8_t hdr[44] = {0};
        fwrite(hdr, 1, 44, f);
        struct xmp_frame_info fi;
        size_t total = 0;
        while (xmp_play_frame(ctx) == 0) {
            xmp_get_frame_info(ctx, &fi);
            if (fi.loop_count > 0) break;
            fwrite(fi.buffer, 1, fi.buffer_size, f);
            total += fi.buffer_size;
            if (total > 48000u * 4 * 3600) break;
        }
        fseek(f, 0, SEEK_SET);
        uint32_t v;
        fwrite("RIFF", 1, 4, f); v = 36 + total; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
        v = 16; fwrite(&v, 4, 1, f); uint16_t s16 = 1; fwrite(&s16, 2, 1, f); s16 = 2; fwrite(&s16, 2, 1, f);
        v = 48000; fwrite(&v, 4, 1, f); v = 48000 * 4; fwrite(&v, 4, 1, f); s16 = 4; fwrite(&s16, 2, 1, f); s16 = 16; fwrite(&s16, 2, 1, f);
        fwrite("data", 1, 4, f); v = total; fwrite(&v, 4, 1, f);
        fclose(f);
        printf("xmp: %.2f s\n", total / 4.0 / 48000);
        xmp_end_player(ctx);
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return 0;
    }
#endif
    fprintf(stderr, "bad args\n");
    return 1;
}
