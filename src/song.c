/*
 * PT8 - song data, period tables, MOD loading/saving, sample import/export.
 */
#include "tracker.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

Song song;

int16_t periodTable[16][NUM_NOTES];

/* ProTracker 2.3D period table: 16 finetunes x (36 notes + terminating 0),
   plus padding (the replayer relies on this layout for arpeggio overflow). */
const int16_t ptPeriodTable[16 * 37 + 15] = {
    /* finetune +0 */
    856,808,762,720,678,640,604,570,538,508,480,453,428,404,381,360,339,320,302,285,269,254,240,226,214,202,190,180,170,160,151,143,135,127,120,113,0,
    /* finetune +1 */
    850,802,757,715,674,637,601,567,535,505,477,450,425,401,379,357,337,318,300,284,268,253,239,225,213,201,189,179,169,159,150,142,134,126,119,113,0,
    /* finetune +2 */
    844,796,752,709,670,632,597,563,532,502,474,447,422,398,376,355,335,316,298,282,266,251,237,224,211,199,188,177,167,158,149,141,133,125,118,112,0,
    /* finetune +3 */
    838,791,746,704,665,628,592,559,528,498,470,444,419,395,373,352,332,314,296,280,264,249,235,222,209,198,187,176,166,157,148,140,132,125,118,111,0,
    /* finetune +4 */
    832,785,741,699,660,623,588,555,524,495,467,441,416,392,370,350,330,312,294,278,262,247,233,220,208,196,185,175,165,156,147,139,131,124,117,110,0,
    /* finetune +5 */
    826,779,736,694,655,619,584,551,520,491,463,437,413,390,368,347,328,309,292,276,260,245,232,219,206,195,184,174,164,155,146,138,130,123,116,109,0,
    /* finetune +6 */
    820,774,730,689,651,614,580,547,516,487,460,434,410,387,365,345,325,307,290,274,258,244,230,217,205,193,183,172,163,154,145,137,129,122,115,109,0,
    /* finetune +7 */
    814,768,725,684,646,610,575,543,513,484,457,431,407,384,363,342,323,305,288,272,256,242,228,216,204,192,181,171,161,152,144,136,128,121,114,108,0,
    /* finetune -8 */
    907,856,808,762,720,678,640,604,570,538,508,480,453,428,404,381,360,339,320,302,285,269,254,240,226,214,202,190,180,170,160,151,143,135,127,120,0,
    /* finetune -7 */
    900,850,802,757,715,675,636,601,567,535,505,477,450,425,401,379,357,337,318,300,284,268,253,238,225,212,200,189,179,169,159,150,142,134,126,119,0,
    /* finetune -6 */
    894,844,796,752,709,670,632,597,563,532,502,474,447,422,398,376,355,335,316,298,282,266,251,237,223,211,199,188,177,167,158,149,141,133,125,118,0,
    /* finetune -5 */
    887,838,791,746,704,665,628,592,559,528,498,470,444,419,395,373,352,332,314,296,280,264,249,235,222,209,198,187,176,166,157,148,140,132,125,118,0,
    /* finetune -4 */
    881,832,785,741,699,660,623,588,555,524,494,467,441,416,392,370,350,330,312,294,278,262,247,233,220,208,196,185,175,165,156,147,139,131,123,117,0,
    /* finetune -3 */
    875,826,779,736,694,655,619,584,551,520,491,463,437,413,390,368,347,328,309,292,276,260,245,232,219,206,195,184,174,164,155,146,138,130,123,116,0,
    /* finetune -2 */
    868,820,774,730,689,651,614,580,547,516,487,460,434,410,387,365,345,325,307,290,274,258,244,230,217,205,193,183,172,163,154,145,137,129,122,115,0,
    /* finetune -1 */
    862,814,768,725,684,646,610,575,543,513,484,457,431,407,384,363,342,323,305,288,272,256,242,228,216,203,192,181,171,161,152,144,136,128,121,114,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* extended octaves (as used by FastTracker 2 when saving MODs) */
static const int16_t oct0Periods[12] = { 1712,1616,1525,1440,1357,1281,1209,1141,1077,1017,961,907 };
static const int16_t oct4Periods[12] = { 107,101,95,90,85,80,76,71,67,64,60,57 };

void tables_init(void)
{
    for (int ft = 0; ft < 16; ft++) {
        int fts = (ft < 8) ? ft : ft - 16;
        for (int n = 0; n < NUM_NOTES; n++) {
            int p;
            if (n >= 12 && n < 48) p = ptPeriodTable[ft * 37 + (n - 12)];
            else if (fts == 0) p = (n < 12) ? oct0Periods[n] : oct4Periods[n - 48];
            else if (n < 12) p = ptPeriodTable[ft * 37 + n] * 2;
            else p = (ptPeriodTable[ft * 37 + 24 + (n - 48)] + 1) / 2;
            periodTable[ft][n] = (int16_t)p;
        }
    }
}

int period_to_note(int period)
{
    if (period <= 0) return 0;
    int best = 0, bestDiff = 1 << 30;
    for (int n = 0; n < NUM_NOTES; n++) {
        int d = abs(periodTable[0][n] - period);
        if (d < bestDiff) { bestDiff = d; best = n; }
    }
    return best + 1;
}

const char *note_name(int note)
{
    static const char *names[12] = { "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-" };
    static char buf[8][4];
    static int bi = 0;
    if (note < 1 || note > NUM_NOTES) return "---";
    char *b = buf[bi++ & 7];
    b[0] = names[(note - 1) % 12][0];
    b[1] = names[(note - 1) % 12][1];
    b[2] = (char)('0' + (note - 1) / 12);
    b[3] = 0;
    return b;
}

uint8_t *read_whole_file(const char *path, size_t *size, size_t maxSize)
{
    FILE *f = plat_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > maxSize) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 16);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    memset(buf + got, 0, 16);
    *size = got;
    return buf;
}

/* ------------------------------------------------------------------ */

void sample_clear(int idx)
{
    Sample *s = &song.smp[idx];
    free(s->data);
    memset(s, 0, sizeof(*s));
    s->loopLength = 2;
    s->volume = 64;
}

void song_clear_samples(void)
{
    for (int i = 0; i < NUM_SAMPLES; i++) sample_clear(i);
}

void song_clear_patterns(void)
{
    memset(song.pat, 0, sizeof(song.pat));
    memset(song.orders, 0, sizeof(song.orders));
    song.numOrders = 1;
    song.extendedNotes = false;
    song.ptModule = false;
}

void song_clear_all(void)
{
    memset(song.name, 0, sizeof(song.name));
    song_clear_patterns();
    song_clear_samples();
}

void song_init(void)
{
    memset(&song, 0, sizeof(song));
    tables_init();
    song_clear_all();
}

bool pattern_is_empty(int p)
{
    const uint8_t *b = (const uint8_t *)song.pat[p];
    for (size_t i = 0; i < sizeof(song.pat[0]); i++) if (b[i]) return false;
    return true;
}

int song_num_patterns_to_save(void)
{
    int hi = 0;
    for (int i = 0; i < MAX_ORDERS; i++) if (song.orders[i] > hi) hi = song.orders[i];
    for (int p = MAX_PATTERNS - 1; p > hi; p--)
        if (!pattern_is_empty(p)) { hi = p; break; }
    return hi + 1;
}

bool song_uses_upper_channels(void)
{
    for (int p = 0; p < MAX_PATTERNS; p++)
        for (int r = 0; r < NUM_ROWS; r++)
            for (int c = 4; c < NUM_CH; c++) {
                const Cell *x = &song.pat[p][r][c];
                if (x->note || x->sample || x->cmd || x->param) return true;
            }
    return false;
}

void song_check_extended(void)
{
    song.extendedNotes = false;
    for (int p = 0; p < MAX_PATTERNS; p++)
        for (int r = 0; r < NUM_ROWS; r++)
            for (int c = 0; c < NUM_CH; c++) {
                int n = song.pat[p][r][c].note;
                if (n && (n < NOTE_PT_MIN || n > NOTE_PT_MAX)) { song.extendedNotes = true; return; }
            }
}

void sample_fix_loop(Sample *s)
{
    s->length &= ~1;
    if (s->length > MAX_SAMPLE_LEN) s->length = MAX_SAMPLE_LEN;
    s->loopStart &= ~1;
    s->loopLength &= ~1;
    if (s->loopLength <= 2 || s->loopStart >= s->length) { s->loopStart = 0; s->loopLength = 2; return; }
    if (s->loopStart + s->loopLength > s->length) s->loopLength = s->length - s->loopStart;
    if (s->loopLength <= 2) { s->loopStart = 0; s->loopLength = 2; }
}

/* ------------------------------------------------------------------ */
/* MOD loading                                                         */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void copy_name(char *dst, const uint8_t *src, int len)
{
    int i;
    for (i = 0; i < len && src[i]; i++) {
        uint8_t c = src[i];
        dst[i] = (c < 32 || c == 127) ? ' ' : (char)c;
    }
    dst[i] = 0;
    /* trim trailing spaces */
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = 0;
}

int mod_load_mem(const uint8_t *b, size_t size, char *msg, int msgsz)
{
    int numCh = 0, numSmp = 31, dtExtra = 0;
    bool flt8 = false;

    if (size < 600) { snprintf(msg, msgsz, "NOT A MODULE (FILE TOO SMALL)"); return 0; }

    if (size >= 1084) {
        const char *t = (const char *)b + 1080;
        if (!memcmp(t, "M.K.", 4) || !memcmp(t, "M!K!", 4) || !memcmp(t, "M&K!", 4) ||
            !memcmp(t, "N.T.", 4) || !memcmp(t, "FLT4", 4) || !memcmp(t, "4CHN", 4) ||
            !memcmp(t, "EXO4", 4) || !memcmp(t, "LARD", 4) || !memcmp(t, "NSMS", 4) ||
            !memcmp(t, "FEST", 4))
            numCh = 4;
        else if (!memcmp(t, "CD61", 4))
            numCh = 6;
        else if (!memcmp(t, "8CHN", 4) || !memcmp(t, "OCTA", 4) || !memcmp(t, "CD81", 4) ||
                 !memcmp(t, "EXO8", 4) || !memcmp(t, "OKTA", 4))
            numCh = 8;
        else if (!memcmp(t, "FLT8", 4)) { numCh = 8; flt8 = true; }
        else if (t[1] == 'C' && t[2] == 'H' && t[3] == 'N' && t[0] >= '1' && t[0] <= '9')
            numCh = t[0] - '0';
        else if (isdigit((unsigned char)t[0]) && isdigit((unsigned char)t[1]) &&
                 ((t[2] == 'C' && t[3] == 'H') || (t[2] == 'C' && t[3] == 'N')))
            numCh = (t[0] - '0') * 10 + (t[1] - '0');
        else if (!memcmp(t, "TDZ", 3) && t[3] >= '1' && t[3] <= '9')
            numCh = t[3] - '0';
        else if (!memcmp(t, "FA0", 3) && t[3] >= '1' && t[3] <= '8') {
            numCh = t[3] - '0';       /* Digital Tracker (Atari): 4 extra bytes after the tag */
            dtExtra = 4;
        }
    }

    if (numCh == 0) {
        /* maybe an old 15-sample Soundtracker module: sanity check headers */
        numSmp = 15;
        numCh = 4;
        for (int i = 0; i < 15; i++) {
            const uint8_t *h = b + 20 + i * 30;
            if (h[25] > 64 || be16(h + 22) > 0x8000) {
                snprintf(msg, msgsz, "UNKNOWN MODULE FORMAT");
                return 0;
            }
        }
        if (b[470] == 0 || b[470] > 128) { snprintf(msg, msgsz, "UNKNOWN MODULE FORMAT"); return 0; }
        int mx = 0;
        size_t st = 0;
        for (int i = 0; i < 128; i++) {
            if (i < b[470] && b[472 + i] > 63) { snprintf(msg, msgsz, "UNKNOWN MODULE FORMAT"); return 0; }
            if (b[472 + i] > mx) mx = b[472 + i];
        }
        for (int i = 0; i < 15; i++) st += (size_t)be16(b + 20 + i * 30 + 22) * 2;
        if (600 + (size_t)(mx + 1) * 1024 + st > size + 1024 && 600 + (size_t)(b[470] + 1) * 1024 > size) {
            snprintf(msg, msgsz, "UNKNOWN MODULE FORMAT");
            return 0;
        }
    }
    if (numCh < 1 || numCh > 32) { snprintf(msg, msgsz, "UNSUPPORTED CHANNEL COUNT"); return 0; }

    const int hdrSize   = 20 + numSmp * 30;
    const int ordOffset = hdrSize + 2;
    const int patOffset = ((numSmp == 31) ? 1084 : 600) + dtExtra;

    int songLen = b[hdrSize];
    if (songLen < 1) songLen = 1;
    if (songLen > 128) songLen = 128;

    uint8_t orders[128];
    memcpy(orders, b + ordOffset, 128);
    int maxAll = 0, maxUsed = 0;
    for (int i = 0; i < 128; i++) {
        if (flt8) orders[i] >>= 1;
        if (orders[i] >= MAX_PATTERNS) orders[i] = 0;
        if (orders[i] > maxAll) maxAll = orders[i];
        if (i < songLen && orders[i] > maxUsed) maxUsed = orders[i];
    }
    /* ProTracker stores every pattern up to the highest one in the whole order table;
       some trackers leave garbage in unused entries, so check what actually fits */
    size_t smpTotal = 0;
    for (int i = 0; i < numSmp; i++) smpTotal += (size_t)be16(b + 20 + i * 30 + 22) * 2;
    const size_t patSize = (size_t)64 * numCh * 4;
    int numPat = maxAll + 1;
    if ((size_t)patOffset + numPat * patSize + smpTotal > size && maxUsed < maxAll) {
        int alt = maxUsed + 1;
        if ((size_t)patOffset + alt * patSize + smpTotal <= size ||
            (size_t)patOffset + numPat * patSize > size)
            numPat = alt;
    }
    for (int i = 0; i < 128; i++) if (orders[i] >= numPat) orders[i] = 0;

    size_t patBytes = (size_t)numPat * patSize;
    if ((size_t)patOffset + patBytes > size) {
        snprintf(msg, msgsz, "MODULE IS DAMAGED / TRUNCATED");
        return 0;
    }

    song_clear_all();
    copy_name(song.name, b, 20);
    song.numOrders = songLen;
    memcpy(song.orders, orders, 128);

    for (int i = 0; i < numSmp; i++) {
        const uint8_t *h = b + 20 + i * 30;
        Sample *s = &song.smp[i];
        copy_name(s->name, h, 22);
        s->length     = be16(h + 22) * 2;
        s->finetune   = h[24] & 0x0F;
        s->volume     = h[25] > 64 ? 64 : h[25];
        s->loopStart  = be16(h + 26) * 2;
        s->loopLength = be16(h + 28) * 2;
        if (numSmp == 15) s->loopStart /= 2; /* soundtracker: loop start in bytes */
        if (s->loopLength > 2 && s->loopStart + s->loopLength > s->length && s->loopStart / 2 + s->loopLength <= s->length)
            s->loopStart /= 2;   /* some old trackers stored it in bytes */
    }

    const uint8_t *pd = b + patOffset;
    for (int p = 0; p < numPat; p++) {
        for (int r = 0; r < 64; r++) {
            for (int c = 0; c < numCh; c++) {
                const uint8_t *x;
                if (flt8) {
                    int half = c / 4;
                    x = pd + ((size_t)(p * 2 + half) * 64 * 4 + r * 4 + (c & 3)) * 4;
                } else {
                    x = pd + ((size_t)p * 64 * numCh + r * numCh + c) * 4;
                }
                if (c >= NUM_CH) continue;
                Cell *cell = &song.pat[p][r][c];
                int period = ((x[0] & 0x0F) << 8) | x[1];
                cell->note   = (uint8_t)period_to_note(period);
                cell->sample = (uint8_t)((x[0] & 0xF0) | (x[2] >> 4));
                if (cell->sample > 31) cell->sample = 0;
                cell->cmd    = x[2] & 0x0F;
                cell->param  = x[3];
            }
        }
    }

    /* sample data */
    size_t off = patOffset + patBytes;
    bool truncated = false;
    for (int i = 0; i < numSmp; i++) {
        Sample *s = &song.smp[i];
        if (s->length <= 0) { s->length = 0; sample_fix_loop(s); continue; }
        int32_t len = s->length;
        if (off >= size) { truncated = true; s->length = 0; sample_fix_loop(s); continue; }
        if (off + (size_t)len > size) { len = (int32_t)(size - off) & ~1; truncated = true; }
        if (len <= 0) { s->length = 0; sample_fix_loop(s); continue; }
        s->data = (int8_t *)malloc((size_t)len + 4);
        if (!s->data) { s->length = 0; sample_fix_loop(s); continue; }
        memcpy(s->data, b + off, (size_t)len);
        memset(s->data + len, 0, 4);
        off += (size_t)s->length;
        s->length = len;
        sample_fix_loop(s);
    }
    /* 4 channel ProTracker modules always use PT's note range and limits */
    if (numCh == 4 && numSmp == 31) song.extendedNotes = false;
    else song_check_extended();
    song.ptModule = (numCh <= 4);

    if (numCh > NUM_CH)
        snprintf(msg, msgsz, "LOADED (%d CH MOD, ONLY 8 KEPT)", numCh);
    else if (truncated)
        snprintf(msg, msgsz, "LOADED (SAMPLE DATA TRUNCATED)");
    else
        snprintf(msg, msgsz, "MODULE LOADED (%d CH)", numCh);
    return 1;
}

int mod_load_file(const char *path, char *msg, int msgsz)
{
    size_t size;
    uint8_t *buf = read_whole_file(path, &size, 64u << 20);
    if (!buf) { snprintf(msg, msgsz, "CAN'T OPEN FILE"); return 0; }
    int ok = mod_load_mem(buf, size, msg, msgsz);
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/* MOD saving                                                          */

static void put16(FILE *f, int v) { fputc((v >> 8) & 0xFF, f); fputc(v & 0xFF, f); }

int mod_save_file(const char *path, char *msg, int msgsz)
{
    bool eight = (cfg.saveFormat == 1) || song_uses_upper_channels();
    int numCh = eight ? 8 : 4;
    int numPat = song_num_patterns_to_save();

    FILE *f = plat_fopen(path, "wb");
    if (!f) { snprintf(msg, msgsz, "CAN'T CREATE FILE"); return 0; }

    char name[20] = { 0 };
    memcpy(name, song.name, strnlen(song.name, 20));
    fwrite(name, 1, 20, f);

    for (int i = 0; i < NUM_SAMPLES; i++) {
        Sample *s = &song.smp[i];
        char sn[22] = { 0 };
        memcpy(sn, s->name, strnlen(s->name, 22));
        fwrite(sn, 1, 22, f);
        int len = s->data ? s->length : 0;
        put16(f, len / 2);
        fputc(s->finetune & 0x0F, f);
        fputc(s->volume > 64 ? 64 : s->volume, f);
        if (s->loopLength > 2 && len > 0) {
            put16(f, s->loopStart / 2);
            put16(f, s->loopLength / 2);
        } else {
            put16(f, 0);
            put16(f, 1);
        }
    }
    fputc(song.numOrders, f);
    fputc(127, f);
    fwrite(song.orders, 1, 128, f);
    if (eight) fwrite("8CHN", 1, 4, f);
    else fwrite(numPat > 64 ? "M!K!" : "M.K.", 1, 4, f);

    for (int p = 0; p < numPat; p++)
        for (int r = 0; r < 64; r++)
            for (int c = 0; c < numCh; c++) {
                const Cell *x = &song.pat[p][r][c];
                int period = x->note ? periodTable[0][x->note - 1] : 0;
                uint8_t o[4];
                o[0] = (uint8_t)((x->sample & 0xF0) | ((period >> 8) & 0x0F));
                o[1] = (uint8_t)(period & 0xFF);
                o[2] = (uint8_t)(((x->sample & 0x0F) << 4) | (x->cmd & 0x0F));
                o[3] = x->param;
                fwrite(o, 1, 4, f);
            }

    for (int i = 0; i < NUM_SAMPLES; i++) {
        Sample *s = &song.smp[i];
        if (s->data && s->length > 0) fwrite(s->data, 1, (size_t)s->length, f);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) { snprintf(msg, msgsz, "WRITE ERROR!"); return 0; }
    snprintf(msg, msgsz, eight ? "SAVED AS 8 CHANNEL MOD (8CHN)" : "SAVED AS 4 CHANNEL MOD (M.K.)");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Sample import                                                       */

void rawsample_free(RawSample *rs)
{
    free(rs->data);
    rs->data = NULL;
    rs->frames = 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void name_from_path(char *dst, const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
    int n = 0;
    for (const char *p = base; *p && n < 22; p++) {
        if (*p == '.' && strchr(p + 1, '.') == NULL) break;
        dst[n++] = *p;
    }
    dst[n] = 0;
}

static int parse_wav(const uint8_t *b, size_t size, RawSample *rs, char *msg, int msgsz)
{
    int fmtTag = 0, chans = 0, rate = 0, bits = 0, align = 0;
    const uint8_t *data = NULL;
    uint32_t dataSize = 0;
    bool haveLoop = false;
    uint32_t lpStart = 0, lpEnd = 0;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *ck = b + pos;
        uint32_t ckSize = le32(ck + 4);
        size_t avail = size - (pos + 8);
        if (ckSize > avail) ckSize = (uint32_t)avail;
        if (!memcmp(ck, "fmt ", 4) && ckSize >= 16) {
            fmtTag = le16(ck + 8);
            chans  = le16(ck + 10);
            rate   = (int)le32(ck + 12);
            align  = le16(ck + 20);
            bits   = le16(ck + 22);
            if (fmtTag == 0xFFFE && ckSize >= 26) fmtTag = le16(ck + 8 + 24);
        } else if (!memcmp(ck, "data", 4)) {
            data = ck + 8;
            dataSize = ckSize;
        } else if (!memcmp(ck, "smpl", 4) && ckSize >= 36 + 24) {
            uint32_t nLoops = le32(ck + 8 + 28);
            if (nLoops > 0) {
                lpStart = le32(ck + 8 + 36 + 8);
                lpEnd   = le32(ck + 8 + 36 + 12);
                haveLoop = lpEnd > lpStart;
            }
        }
        pos += 8 + ckSize + (ckSize & 1);
    }
    if (!data || chans < 1 || rate <= 0) { snprintf(msg, msgsz, "WAV FILE IS DAMAGED"); return 0; }
    if (!((fmtTag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmtTag == 3 && (bits == 32 || bits == 64)))) {
        snprintf(msg, msgsz, "UNSUPPORTED WAV FORMAT");
        return 0;
    }
    int bps = bits / 8;
    if (align < bps * chans) align = bps * chans;
    int32_t frames = (int32_t)(dataSize / (uint32_t)align);
    if (frames <= 0) { snprintf(msg, msgsz, "WAV FILE IS EMPTY"); return 0; }
    if (frames > 50 * 1000 * 1000) frames = 50 * 1000 * 1000;
    rs->data = (float *)malloc(sizeof(float) * (size_t)frames);
    if (!rs->data) { snprintf(msg, msgsz, "OUT OF MEMORY"); return 0; }
    for (int32_t i = 0; i < frames; i++) {
        double acc = 0;
        for (int c = 0; c < chans; c++) {
            const uint8_t *p = data + (size_t)i * align + (size_t)c * bps;
            double v;
            if (fmtTag == 3) {
                if (bits == 32) { float fv; memcpy(&fv, p, 4); v = fv; }
                else { double dv; memcpy(&dv, p, 8); v = dv; }
            } else if (bits == 8) v = (p[0] - 128) / 128.0;
            else if (bits == 16) v = (int16_t)le16(p) / 32768.0;
            else if (bits == 24) v = ((int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8) / 8388608.0;
            else v = (int32_t)le32(p) / 2147483648.0;
            acc += v;
        }
        rs->data[i] = (float)(acc / chans);
    }
    rs->frames = frames;
    rs->rate = rate;
    if (haveLoop && lpStart < (uint32_t)frames) {
        if (lpEnd >= (uint32_t)frames) lpEnd = (uint32_t)frames - 1;
        rs->loopStart = (int32_t)lpStart;
        rs->loopLength = (int32_t)(lpEnd - lpStart + 1);
    }
    return 1;
}

static int parse_8svx(const uint8_t *b, size_t size, RawSample *rs, char *msg, int msgsz)
{
    uint32_t oneShot = 0, repeat = 0;
    int rate = C3_RATE, comp = 0;
    const uint8_t *body = NULL;
    uint32_t bodySize = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *ck = b + pos;
        uint32_t ckSize = be32(ck + 4);
        size_t avail = size - (pos + 8);
        if (ckSize > avail) ckSize = (uint32_t)avail;
        if (!memcmp(ck, "VHDR", 4) && ckSize >= 20) {
            oneShot = be32(ck + 8);
            repeat  = be32(ck + 12);
            rate    = be16(ck + 20);
            comp    = ck[23];
        } else if (!memcmp(ck, "NAME", 4) && ckSize > 0) {
            copy_name(rs->name, ck + 8, ckSize < 22 ? (int)ckSize : 22);
        } else if (!memcmp(ck, "BODY", 4)) {
            body = ck + 8;
            bodySize = ckSize;
        }
        pos += 8 + ckSize + (ckSize & 1);
    }
    if (!body || bodySize == 0) { snprintf(msg, msgsz, "IFF FILE IS DAMAGED"); return 0; }
    if (rate <= 0) rate = C3_RATE;
    int8_t *pcm = NULL;
    uint32_t n = bodySize;
    if (comp == 1) {
        /* Fibonacci delta */
        static const int8_t fib[16] = { -34,-21,-13,-8,-5,-3,-2,-1,0,1,2,3,5,8,13,21 };
        if (bodySize < 2) { snprintf(msg, msgsz, "IFF FILE IS DAMAGED"); return 0; }
        n = (bodySize - 2) * 2;
        pcm = (int8_t *)malloc(n ? n : 1);
        int8_t x = (int8_t)body[1];
        for (uint32_t i = 0; i < n; i++) {
            uint8_t d = body[2 + i / 2];
            d = (i & 1) ? (d & 15) : (d >> 4);
            x = (int8_t)(x + fib[d]);
            pcm[i] = x;
        }
    } else if (comp != 0) {
        snprintf(msg, msgsz, "UNSUPPORTED IFF COMPRESSION");
        return 0;
    }
    uint32_t total = oneShot + repeat;
    if (total == 0 || total > n) total = n;
    rs->data = (float *)malloc(sizeof(float) * total);
    if (!rs->data) { free(pcm); snprintf(msg, msgsz, "OUT OF MEMORY"); return 0; }
    for (uint32_t i = 0; i < total; i++)
        rs->data[i] = (pcm ? pcm[i] : (int8_t)body[i]) / 128.0f;
    free(pcm);
    rs->frames = (int32_t)total;
    rs->rate = rate;
    if (repeat > 2 && oneShot + repeat <= total) {
        rs->loopStart = (int32_t)oneShot;
        rs->loopLength = (int32_t)repeat;
    }
    return 1;
}

static double ieee80(const uint8_t *p)
{
    int e = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t m = 0;
    for (int i = 0; i < 8; i++) m = (m << 8) | p[2 + i];
    if (e == 0 && m == 0) return 0;
    double f = ldexp((double)m, e - 16383 - 63);
    return (p[0] & 0x80) ? -f : f;
}

static int parse_aiff(const uint8_t *b, size_t size, bool aifc, RawSample *rs, char *msg, int msgsz)
{
    int chans = 0, bits = 0;
    uint32_t frames = 0;
    double rate = 0;
    bool little = false;
    const uint8_t *ssnd = NULL;
    uint32_t ssndSize = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *ck = b + pos;
        uint32_t ckSize = be32(ck + 4);
        size_t avail = size - (pos + 8);
        if (ckSize > avail) ckSize = (uint32_t)avail;
        if (!memcmp(ck, "COMM", 4) && ckSize >= 18) {
            chans  = be16(ck + 8);
            frames = be32(ck + 10);
            bits   = be16(ck + 14);
            rate   = ieee80(ck + 16);
            if (aifc && ckSize >= 22) {
                if (!memcmp(ck + 26, "sowt", 4)) little = true;
                else if (memcmp(ck + 26, "NONE", 4) && memcmp(ck + 26, "twos", 4)) {
                    snprintf(msg, msgsz, "UNSUPPORTED AIFF COMPRESSION");
                    return 0;
                }
            }
        } else if (!memcmp(ck, "SSND", 4) && ckSize >= 8) {
            uint32_t offs = be32(ck + 8);
            ssnd = ck + 16 + offs;
            ssndSize = ckSize >= 8 + offs ? ckSize - 8 - offs : 0;
        }
        pos += 8 + ckSize + (ckSize & 1);
    }
    if (!ssnd || chans < 1 || bits < 1 || bits > 32 || rate <= 0) { snprintf(msg, msgsz, "AIFF FILE IS DAMAGED"); return 0; }
    int bps = (bits + 7) / 8;
    uint32_t maxFrames = ssndSize / (uint32_t)(bps * chans);
    if (frames > maxFrames) frames = maxFrames;
    if (frames == 0) { snprintf(msg, msgsz, "AIFF FILE IS EMPTY"); return 0; }
    rs->data = (float *)malloc(sizeof(float) * frames);
    if (!rs->data) { snprintf(msg, msgsz, "OUT OF MEMORY"); return 0; }
    for (uint32_t i = 0; i < frames; i++) {
        double acc = 0;
        for (int c = 0; c < chans; c++) {
            const uint8_t *p = ssnd + ((size_t)i * chans + c) * bps;
            int32_t v = 0;
            for (int k = 0; k < bps; k++) {
                uint8_t byte = little ? p[bps - 1 - k] : p[k];
                v = (v << 8) | byte;
            }
            v <<= (32 - bps * 8);
            acc += v / 2147483648.0;
        }
        rs->data[i] = (float)(acc / chans);
    }
    rs->frames = (int32_t)frames;
    rs->rate = (int)(rate + 0.5);
    return 1;
}

int sample_import(const char *path, RawSample *rs, char *msg, int msgsz)
{
    memset(rs, 0, sizeof(*rs));
    size_t size;
    uint8_t *b = read_whole_file(path, &size, 256u << 20);
    if (!b) { snprintf(msg, msgsz, "CAN'T OPEN FILE"); return 0; }
    name_from_path(rs->name, path);
    int ok;
    if (size >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4))
        ok = parse_wav(b, size, rs, msg, msgsz);
    else if (size >= 12 && !memcmp(b, "FORM", 4) && !memcmp(b + 8, "8SVX", 4))
        ok = parse_8svx(b, size, rs, msg, msgsz);
    else if (size >= 12 && !memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4)))
        ok = parse_aiff(b, size, b[11] == 'C', rs, msg, msgsz);
    else {
        /* raw 8-bit signed */
        size_t n = size > 50000000 ? 50000000 : size;
        rs->data = (float *)malloc(sizeof(float) * (n ? n : 1));
        ok = rs->data != NULL && n > 0;
        if (ok) {
            for (size_t i = 0; i < n; i++) rs->data[i] = (int8_t)b[i] / 128.0f;
            rs->frames = (int32_t)n;
            rs->rate = C3_RATE;
        } else snprintf(msg, msgsz, "FILE IS EMPTY");
    }
    free(b);
    if (ok) snprintf(msg, msgsz, "SAMPLE LOADED");
    return ok;
}

/* windowed-sinc resampler */
static float *resample(const float *in, int32_t inLen, double ratio /* out/in */, int32_t *outLen)
{
    int32_t n = (int32_t)floor(inLen * ratio + 0.5);
    if (n < 1) n = 1;
    float *out = (float *)malloc(sizeof(float) * (size_t)n);
    if (!out) return NULL;
    const double cutoff = (ratio < 1.0 ? ratio : 1.0) * 0.97;
    const int taps = 16;
    const double radius = taps / cutoff;
    for (int32_t i = 0; i < n; i++) {
        double center = i / ratio;
        int32_t lo = (int32_t)ceil(center - radius), hi = (int32_t)floor(center + radius);
        double acc = 0, wsum = 0;
        for (int32_t k = lo; k <= hi; k++) {
            double x = (k - center) * cutoff;
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            double t = (k - center) / radius;              /* -1..1 */
            double w = 0.42 + 0.5 * cos(M_PI * t) + 0.08 * cos(2 * M_PI * t);   /* Blackman */
            double c = sinc * w;
            float s = (k >= 0 && k < inLen) ? in[k] : 0.0f;
            acc += s * c;
            wsum += c;
        }
        out[i] = (float)(wsum != 0 ? acc / wsum : 0);
    }
    *outLen = n;
    return out;
}

void sample_install(int idx, const RawSample *rs, int targetRate)
{
    const float *src = rs->data;
    int32_t frames = rs->frames;
    float *tmp = NULL;
    int32_t ls = rs->loopStart, ll = rs->loopLength;

    if (targetRate > 0 && targetRate != rs->rate && rs->rate > 0) {
        double ratio = (double)targetRate / rs->rate;
        tmp = resample(src, frames, ratio, &frames);
        if (tmp) {
            src = tmp;
            ls = (int32_t)floor(ls * ratio + 0.5);
            ll = (int32_t)floor(ll * ratio + 0.5);
        } else frames = rs->frames;
    }

    int32_t len = frames;
    if (len > MAX_SAMPLE_LEN) len = MAX_SAMPLE_LEN;
    int32_t alloc = (len + 1) & ~1;
    int8_t *d = (int8_t *)calloc((size_t)alloc + 4, 1);
    if (!d) { free(tmp); return; }
    for (int32_t i = 0; i < len; i++) {
        float v = src[i] * 128.0f;
        int iv = (int)lrintf(v);
        if (iv > 127) iv = 127;
        if (iv < -128) iv = -128;
        d[i] = (int8_t)iv;
    }
    free(tmp);

    Sample *s = &song.smp[idx];
    free(s->data);
    s->data = d;
    s->length = alloc;
    memset(s->name, 0, sizeof(s->name));
    memcpy(s->name, rs->name, strnlen(rs->name, 22));
    s->volume = 64;
    s->finetune = 0;
    s->loopStart = 0;
    s->loopLength = 2;
    if (ll > 2) {
        s->loopStart = ls;
        s->loopLength = ll;
    }
    sample_fix_loop(s);
    replayer_invalidate_sample(idx);
}

static void wr32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void wr16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int sample_save_wav(int idx, const char *path, char *msg, int msgsz)
{
    Sample *s = &song.smp[idx];
    if (!s->data || s->length <= 0) { snprintf(msg, msgsz, "SAMPLE IS EMPTY"); return 0; }
    FILE *f = plat_fopen(path, "wb");
    if (!f) { snprintf(msg, msgsz, "CAN'T CREATE FILE"); return 0; }
    bool loop = s->loopLength > 2;
    uint32_t dataSize = (uint32_t)s->length;
    uint32_t smplSize = loop ? 36 + 24 : 0;
    uint32_t riffSize = 4 + (8 + 16) + (8 + dataSize + (dataSize & 1)) + (loop ? 8 + smplSize : 0);
    fwrite("RIFF", 1, 4, f); wr32(f, riffSize); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16);
    wr16(f, 1); wr16(f, 1); wr32(f, C3_RATE); wr32(f, C3_RATE); wr16(f, 1); wr16(f, 8);
    fwrite("data", 1, 4, f); wr32(f, dataSize);
    for (int32_t i = 0; i < s->length; i++) fputc((uint8_t)(s->data[i] + 128), f);
    if (dataSize & 1) fputc(0, f);
    if (loop) {
        fwrite("smpl", 1, 4, f); wr32(f, smplSize);
        wr32(f, 0); wr32(f, 0);
        wr32(f, (uint32_t)(1000000000.0 / C3_RATE));
        wr32(f, 60); wr32(f, 0); wr32(f, 0); wr32(f, 0);
        wr32(f, 1); wr32(f, 0);
        wr32(f, 0); wr32(f, 0);
        wr32(f, (uint32_t)s->loopStart);
        wr32(f, (uint32_t)(s->loopStart + s->loopLength - 1));
        wr32(f, 0); wr32(f, 0);
    }
    bool ok = !ferror(f);
    fclose(f);
    snprintf(msg, msgsz, ok ? "SAMPLE SAVED" : "WRITE ERROR!");
    return ok;
}
