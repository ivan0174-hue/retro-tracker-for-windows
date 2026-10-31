/*
 * PT8 - a ProTracker-style 8 channel tracker
 * Shared definitions: song data, replayer and file I/O.
 */
#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define APP_NAME      "PT8"
#define APP_LONGNAME  "PT8 - 8 CHANNEL PROTRACKER"
#define APP_VERSION   "1.0.1"

#define NUM_CH          8
#define NUM_ROWS        64
#define MAX_PATTERNS    128
#define MAX_ORDERS      128
#define NUM_SAMPLES     31
#define MAX_SAMPLE_LEN  0x1FFFE      /* 131070 bytes, ProTracker limit */

/* Notes are stored as 1..60 (C-0..B-4). ProTracker's range is C-1..B-3. */
#define NUM_NOTES       60
#define NOTE_PT_MIN     13           /* C-1 */
#define NOTE_PT_MAX     48           /* B-3 */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PAULA_PAL_CLK   3546895.0
#define C3_RATE         16574        /* Hz at which a sample plays at original pitch on C-3 */

typedef struct {
    uint8_t note;    /* 0 = none, 1..60 */
    uint8_t sample;  /* 0 = none, 1..31 */
    uint8_t cmd;     /* 0..15 */
    uint8_t param;   /* 0..255 */
} Cell;

typedef struct {
    char     name[23];
    int8_t  *data;
    int32_t  length;      /* bytes, even */
    int32_t  loopStart;   /* bytes */
    int32_t  loopLength;  /* bytes, <= 2 means no loop */
    uint8_t  finetune;    /* 0..15 (0..7 = 0..+7, 8..15 = -8..-1) */
    uint8_t  volume;      /* 0..64 */
} Sample;

typedef struct {
    char     name[21];
    uint8_t  orders[MAX_ORDERS];
    int      numOrders;          /* 1..128 */
    Sample   smp[NUM_SAMPLES];
    Cell     pat[MAX_PATTERNS][NUM_ROWS][NUM_CH];
    bool     extendedNotes;      /* song uses notes outside the PT range */
    bool     ptModule;           /* loaded from a 4 channel ProTracker module (8xx = no panning) */
} Song;

extern Song song;

typedef struct {
    int  stereoSep;      /* 0..100 % */
    int  amp;            /* 1..16, output gain = amp/16 */
    bool interpolation;  /* linear interpolation (off = authentic Amiga) */
    bool a500Filter;     /* A500 4.4kHz low-pass (off = A1200) */
    bool ledEmulation;   /* honour E0x LED filter */
    bool panning;        /* 8xx / E8x set panning (not in original PT) */
    int  saveFormat;     /* 0 = auto (M.K. if ch 5-8 unused), 1 = always 8CHN */
    int  editSkip;       /* rows to advance after entering a note */
    bool followPlay;
    int  audioBuffer;    /* frames per audio buffer (Windows waveOut) */
} Settings;

extern Settings cfg;
extern bool chanMuted[NUM_CH];

/* ---------------- song.c ---------------- */
extern int16_t periodTable[16][NUM_NOTES];   /* [finetune][note-1], extended 5 octaves */
extern const int16_t ptPeriodTable[16 * 37 + 15];  /* exact ProTracker layout */
void  tables_init(void);
int   period_to_note(int period);            /* 0 if period == 0 */
const char *note_name(int note);             /* "C-1", "---" ... */

void  song_init(void);
void  song_clear_all(void);                  /* caller must hold audio lock */
void  song_clear_patterns(void);
void  song_clear_samples(void);              /* caller must hold audio lock */
void  sample_clear(int idx);                 /* caller must hold audio lock */
void  sample_fix_loop(Sample *s);
int   song_num_patterns_to_save(void);
bool  song_uses_upper_channels(void);
void  song_check_extended(void);
bool  pattern_is_empty(int p);

int   mod_load_file(const char *path, char *msg, int msgsz);   /* caller holds lock */
int   mod_load_mem(const uint8_t *buf, size_t size, char *msg, int msgsz);
int   mod_save_file(const char *path, char *msg, int msgsz);

typedef struct {
    float   *data;        /* mono, -1..1 */
    int32_t  frames;
    int      rate;
    int32_t  loopStart, loopLength;   /* in frames; loopLength 0 = none */
    char     name[23];
} RawSample;

int   sample_import(const char *path, RawSample *out, char *msg, int msgsz);
void  rawsample_free(RawSample *rs);
/* convert & install; targetRate 0 = keep sample data as-is. Caller holds lock. */
void  sample_install(int idx, const RawSample *rs, int targetRate);
int   sample_save_wav(int idx, const char *path, char *msg, int msgsz);

/* ---------------- replayer.c ---------------- */
enum { PLAY_STOPPED = 0, PLAY_SONG, PLAY_PATTERN };

typedef struct {
    int     smp;
    int32_t start, len;
    int32_t pos;
    int     regSmp;               /* next (loop) block */
    int32_t regStart, regLen;
    double  delta;
    int     volume;
    bool    active;
} ScopeVoice;

typedef struct {
    uint64_t   frame;
    int        mode, order, pattern, row, speed, bpm;
    bool       led;
    ScopeVoice v[NUM_CH];
} PlaySnap;

void  replayer_init(int outputRate);
/* all of the following must be called with the audio lock held */
void  replayer_play(int mode, int order, int row, int pattern);
void  replayer_stop(void);
void  replayer_stop_voices(void);
void  replayer_jam(int ch, int smp, int note);
void  replayer_play_sample_range(int ch, int smp, int32_t start, int32_t end, int note);
void  replayer_reset_sync(void);
int   replayer_mode(void);
void  replayer_set_bpm(int bpm);
void  replayer_set_speed(int speed);
int   replayer_bpm(void);
int   replayer_speed(void);
void  replayer_current(int *order, int *pattern, int *row);
void  mixer_render(int16_t *out, int frames);
bool  replayer_snapshot(uint64_t playedFrame, PlaySnap *out);   /* takes lock itself */
uint64_t replayer_frames_rendered(void);
void  replayer_invalidate_sample(int idx);
int   export_wav(const char *path, char *msg, int msgsz);     /* caller holds lock */
double song_duration_seconds(void);                            /* caller holds lock */

/* ---------------- util ---------------- */
uint8_t *read_whole_file(const char *path, size_t *size, size_t maxSize);

#endif
