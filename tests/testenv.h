/*
 * Shared helpers for the PT8 test programs (Linux/macOS, run without Windows).
 * Files are written to $PT8_TEST_DIR, or $TMPDIR/pt8-tests, or /tmp/pt8-tests.
 */
#ifndef PT8_TESTENV_H
#define PT8_TESTENV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline const char *test_dir(void)
{
    static char dir[512];
    if (!dir[0]) {
        const char *d = getenv("PT8_TEST_DIR");
        if (d && *d) snprintf(dir, sizeof dir, "%s", d);
        else {
            const char *t = getenv("TMPDIR");
            snprintf(dir, sizeof dir, "%s/pt8-tests", (t && *t) ? t : "/tmp");
        }
        mkdir(dir, 0755);
    }
    return dir;
}

/* path of a file in the test directory (a few rotating buffers) */
static inline const char *tpath(const char *name)
{
    static char buf[8][600];
    static int i;
    char *b = buf[i++ & 7];
    snprintf(b, 600, "%s/%s", test_dir(), name);
    return b;
}

static inline int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    if (!a) return 0;
    b = fopen(to, "wb");
    if (!b) { fclose(a); return 0; }
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, a)) > 0) fwrite(tmp, 1, n, b);
    fclose(a);
    fclose(b);
    return 1;
}

static inline void put32le(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static inline void put16le(FILE *f, uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

/* 44.1 kHz stereo 16-bit WAV, 30000 frames (440/660 Hz), with a 'smpl' loop 1000..20000 */
static inline void write_test_wav(const char *path)
{
    const uint32_t rate = 44100, frames = 30000, dataBytes = frames * 4;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite("RIFF", 1, 4, f); put32le(f, 4 + 24 + 8 + dataBytes + 8 + 60); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32le(f, 16);
    put16le(f, 1); put16le(f, 2); put32le(f, rate); put32le(f, rate * 4); put16le(f, 4); put16le(f, 16);
    fwrite("data", 1, 4, f); put32le(f, dataBytes);
    for (uint32_t i = 0; i < frames; i++) {
        put16le(f, (uint16_t)(int16_t)(12000 * sin(i * 2 * M_PI * 440 / rate)));
        put16le(f, (uint16_t)(int16_t)(9000 * sin(i * 2 * M_PI * 660 / rate)));
    }
    fwrite("smpl", 1, 4, f); put32le(f, 60);
    put32le(f, 0); put32le(f, 0); put32le(f, (uint32_t)(1e9 / rate)); put32le(f, 60);
    put32le(f, 0); put32le(f, 0); put32le(f, 0); put32le(f, 1); put32le(f, 0);
    put32le(f, 0); put32le(f, 0); put32le(f, 1000); put32le(f, 20000); put32le(f, 0); put32le(f, 0);
    fclose(f);
}

#endif
