/*
 * PT8 - user interface (platform independent, draws into a framebuffer).
 * Layout and workflow follow ProTracker 2.3D, widened for 8 channels.
 */
#include "app.h"
#include "tracker.h"
#include "platform.h"
#include "font8x8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define W SCR_W
#define H SCR_H

/* Windows virtual key codes */
enum {
    K_BACK = 0x08, K_TAB = 0x09, K_RETURN = 0x0D, K_SHIFT = 0x10, K_CONTROL = 0x11, K_MENU = 0x12,
    K_ESCAPE = 0x1B, K_SPACE = 0x20, K_PRIOR = 0x21, K_NEXT = 0x22, K_END = 0x23, K_HOME = 0x24,
    K_LEFT = 0x25, K_UP = 0x26, K_RIGHT = 0x27, K_DOWN = 0x28, K_INSERT = 0x2D, K_DELETE = 0x2E,
    K_NUMPAD0 = 0x60, K_NUMPAD9 = 0x69, K_MULTIPLY = 0x6A, K_ADD = 0x6B, K_SUBTRACT = 0x6D,
    K_DECIMAL = 0x6E, K_DIVIDE = 0x6F,
    K_F1 = 0x70, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12
};

/* ------------------------------------------------------------------ */
/* palette                                                             */

#define C_FACE      0xA6A8B0
#define C_LIGHT     0xE6E8EE
#define C_SHADOW    0x55575F
#define C_TEXT      0x101014
#define C_TEXTDIM   0x4C4E58
#define C_VALBG     0x1A1C24
#define C_VALTXT    0xE8ECF4
#define C_BTNDOWN   0x8A8C96
#define C_LIT       0xF0D070
#define C_RED       0xB02828
#define C_GREEN     0x2C8A3A

#define P_BG        0x0A0C12
#define P_BEAT      0x121622
#define P_BAR       0x2A3870
#define P_BAR_EDIT  0x702A2E
#define P_BAR_REC   0x7A3A10
#define P_DIM       0x343C50
#define P_NOTE      0xE4E8F2
#define P_SMP       0x8CC8F0
#define P_FX        0xF0B47A
#define P_ROWNUM    0x7A8296
#define P_ROW16     0xE0C070
#define P_SEP       0x283044
#define P_CURSOR    0xF0C860
#define P_PLAYROW   0x1C2A48

#define S_BG        0x000000
#define S_WAVE      0xFFD25C
#define S_MUTED     0x6A3424
#define S_MID       0x1E1E2A

/* ------------------------------------------------------------------ */
/* layout                                                              */

#define RH          11          /* row height of the top panels */
#define TOP_H       (RH * 11)
#define LP_W        216         /* left panel */
#define VAL_X       112
#define VAL_W       60
#define ARR1_X      174
#define ARR_W       21
#define BTN_X       216
#define BTN_W       80
#define SCOPE_X     376
#define SCOPE_W     (W - SCOPE_X)
#define INFO_Y      TOP_H
#define PAT_Y       (INFO_Y + RH * 2)
#define PAT_HDR_H   12
#define ROW_H       10
#define VIS_ROWS    32
#define CENTER_ROW  16
#define ROWNUM_W    36
#define CH_W        85

/* ------------------------------------------------------------------ */
/* state                                                               */

enum { SCR_PATTERN = 0, SCR_SAMPLER };
enum { OV_NONE = 0, OV_EDITOP, OV_SETUP, OV_POSED };

static uint32_t *FB;
static uint32_t now;

static int  curOrder, curRow, curCh, curCol, curSample, octave = 1;
static bool editMode;
static int  screen = SCR_PATTERN, overlay = OV_NONE;
static bool showHelp, showAbout;
static bool modified;
static bool transposeAll = true;
static char songPath[1024];
static char status[96] = "ALL RIGHT";
static bool statusErr;
static uint32_t statusTime;
static PlaySnap snap;
static bool snapOk;
static int  liveMode;
static uint64_t playStartFrame;
static int  lastTitleState = -1;

/* mouse */
static struct { int x, y; bool lDown, rDown, lPress, rPress; int wheel; } ms;
static bool inputOn = true;
static int  activeId, activeBtn;
static uint32_t repeatAt;

/* text editing */
static char *teBuf;
static int   teMax, teCur;
static int   teX, teY, teW;

/* dialog */
typedef void (*DlgFn)(int choice);
static struct {
    bool active;
    char line1[80], line2[80];
    const char *btn[4];
    int nbtn;
    DlgFn fn;
} dlg;

/* clipboards */
static Cell clipTrack[NUM_ROWS];
static bool clipTrackOk;
static Cell clipPat[NUM_ROWS][NUM_CH];
static bool clipPatOk;
static Cell clipCmd[NUM_ROWS];
static bool clipCmdOk;
static int8_t *clipSmp;
static int32_t clipSmpLen;

/* sampler view */
static int32_t viewStart, viewLen, selStart = -1, selEnd = -1;
static int  smpDrag;   /* 0 none, 1 select, 2 loop start, 3 loop end */
static int32_t selAnchor;
static int  samplerFor = -1;

/* pending operations for dialogs */
static RawSample pendingRaw;
static int pendingSmp;
static char pendingPath[1024];

/* undo */
#define UNDO_MAX 192
typedef struct { int pat, group; Cell data[NUM_ROWS][NUM_CH]; } UndoEnt;
static UndoEnt *undoBuf, *redoBuf;
static int undoCount, redoCount, undoGroup;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void setStatus(const char *msg, bool err)
{
    size_t n = strlen(msg);
    if (n > sizeof status - 1) n = sizeof status - 1;   /* status line shows at most 95 chars */
    memcpy(status, msg, n);
    status[n] = 0;
    statusErr = err;
    statusTime = now ? now : plat_ms();
}

static void setModified(void) { modified = true; }

static bool isPlaying(void) { return liveMode != PLAY_STOPPED; }

static int viewPattern(void)
{
    if (isPlaying() && snapOk && snap.mode != PLAY_STOPPED && cfg.followPlay) return snap.pattern;
    return song.orders[curOrder];
}

static bool recording(void) { return editMode && isPlaying() && cfg.followPlay; }

static int editRow(void)
{
    if (recording() && snapOk && snap.mode != PLAY_STOPPED) return snap.row;
    return curRow;
}

static const char *baseName(const char *p)
{
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */

static void fill(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = FB + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}
static void hline(int x, int y, int w, uint32_t c) { fill(x, y, w, 1, c); }
static void vline(int x, int y, int h, uint32_t c) { fill(x, y, 1, h, c); }
static void frameRect(int x, int y, int w, int h, uint32_t c)
{
    hline(x, y, w, c); hline(x, y + h - 1, w, c); vline(x, y, h, c); vline(x + w - 1, y, h, c);
}

static void glyph(int x, int y, unsigned ch, uint32_t c)
{
    if (ch > 255) ch = '?';
    const unsigned char *g = font8x8[ch];
    for (int r = 0; r < 8; r++) {
        unsigned b = g[r];
        int yy = y + r;
        if (!b || yy < 0 || yy >= H) continue;
        uint32_t *row = FB + yy * W;
        for (int k = 0; k < 8; k++)
            if (b & (1u << k)) {
                int xx = x + k;
                if (xx >= 0 && xx < W) row[xx] = c;
            }
    }
}

static unsigned utf8dec(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*s++ & 0x3F); }
    else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2;
    }
    *ps = (const char *)s;
    return c;
}

static int textLen(const char *s) { int n = 0; while (*s) { utf8dec(&s); n++; } return n; }

static int text(int x, int y, const char *s, uint32_t c)
{
    while (*s) { glyph(x, y, utf8dec(&s), c); x += 8; }
    return x;
}

static int textRaw(int x, int y, const char *s, int maxc, uint32_t c)
{
    for (int i = 0; i < maxc && s[i]; i++, x += 8) glyph(x, y, (unsigned char)s[i], c);
    return x;
}

static int textf(int x, int y, uint32_t c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return text(x, y, buf, c);
}

static void textCenter(int x, int y, int w, int h, const char *s, uint32_t c)
{
    int tw = textLen(s) * 8;
    text(x + (w - tw) / 2, y + (h - 8) / 2 + 1, s, c);
}

static void raised(int x, int y, int w, int h)
{
    fill(x, y, w, h, C_FACE);
    hline(x, y, w, C_LIGHT); vline(x, y, h, C_LIGHT);
    hline(x, y + h - 1, w, C_SHADOW); vline(x + w - 1, y, h, C_SHADOW);
}

static void sunken(int x, int y, int w, int h, uint32_t bg)
{
    fill(x, y, w, h, bg);
    hline(x, y, w, C_SHADOW); vline(x, y, h, C_SHADOW);
    hline(x, y + h - 1, w, C_LIGHT); vline(x + w - 1, y, h, C_LIGHT);
}

/* ------------------------------------------------------------------ */
/* widgets                                                             */

static bool hit(int x, int y, int w, int h) { return ms.x >= x && ms.x < x + w && ms.y >= y && ms.y < y + h; }
static int  wid(int x, int y, int w, int h) { return 1 + x + y * 1009 + w * 7919 + h * 104729; }

static bool button(int x, int y, int w, int h, const char *label, bool lit)
{
    int id = wid(x, y, w, h);
    bool pressed = false;
    if (inputOn && hit(x, y, w, h) && ms.lPress) {
        ms.lPress = false;
        activeId = id;
        activeBtn = 0;
        pressed = true;
    }
    bool down = (activeId == id && ms.lDown);
    if (down) {
        sunken(x, y, w, h, C_BTNDOWN);
    } else {
        raised(x, y, w, h);
    }
    if (lit) fill(x + 2, y + 2, 3, h - 4, C_LIT);
    textCenter(x + (lit ? 2 : 0), y, w, h, label, C_TEXT);
    return pressed;
}

/* returns 0, 1 (left click / repeat) or 2 (right click / repeat: fast) */
static int arrowBtn(int x, int y, int w, int h, bool up)
{
    int id = wid(x, y, w, h);
    int r = 0;
    if (inputOn && hit(x, y, w, h) && (ms.lPress || ms.rPress)) {
        activeBtn = ms.lPress ? 0 : 1;
        r = activeBtn ? 2 : 1;
        ms.lPress = ms.rPress = false;
        activeId = id;
        repeatAt = now + 330;
    } else if (activeId == id && (activeBtn ? ms.rDown : ms.lDown) && hit(x, y, w, h) && (int32_t)(now - repeatAt) >= 0) {
        repeatAt = now + 45;
        r = activeBtn ? 2 : 1;
    }
    bool down = activeId == id && (ms.lDown || ms.rDown);
    if (down) sunken(x, y, w, h, C_BTNDOWN); else raised(x, y, w, h);
    glyph(x + (w - 8) / 2, y + (h - 8) / 2 + 1, up ? 1 : 2, C_TEXT);
    return r;
}

static void textField(int x, int y, int w, int h, char *buf, int maxLen)
{
    sunken(x, y, w, h, C_VALBG);
    bool editing = teBuf == buf;
    if (editing) fill(x + 1, y + 1, w - 2, h - 2, 0x30364A);
    textRaw(x + 3, y + (h - 8) / 2 + 1, buf, maxLen, C_VALTXT);
    if (editing && ((now / 400) & 1) == 0) fill(x + 3 + teCur * 8, y + 2, 2, h - 4, P_CURSOR);
    if (inputOn && hit(x, y, w, h) && ms.lPress) {
        ms.lPress = false;
        teBuf = buf;
        teMax = maxLen;
        teCur = (int)strlen(buf);
        teX = x; teY = y; teW = w;
    }
}

static void ask(const char *l1, const char *l2, DlgFn fn, int n, const char *b0, const char *b1, const char *b2, const char *b3)
{
    dlg.active = true;
    snprintf(dlg.line1, sizeof dlg.line1, "%s", l1 ? l1 : "");
    snprintf(dlg.line2, sizeof dlg.line2, "%s", l2 ? l2 : "");
    dlg.btn[0] = b0; dlg.btn[1] = b1; dlg.btn[2] = b2; dlg.btn[3] = b3;
    dlg.nbtn = n;
    dlg.fn = fn;
}

static void dlgChoose(int i)
{
    dlg.active = false;
    if (dlg.fn) dlg.fn(i);
}

/* ------------------------------------------------------------------ */
/* undo                                                                */

static void undoBegin(void) { undoGroup++; redoCount = 0; }

static void undoSave(int pat)
{
    if (!undoBuf) return;
    if (undoCount == UNDO_MAX) { memmove(undoBuf, undoBuf + 1, sizeof(UndoEnt) * (UNDO_MAX - 1)); undoCount--; }
    UndoEnt *e = &undoBuf[undoCount++];
    e->pat = pat;
    e->group = undoGroup;
    memcpy(e->data, song.pat[pat], sizeof(e->data));
    setModified();
}

static void undoClear(void) { undoCount = redoCount = 0; }

static void undoStep(bool redo)
{
    UndoEnt *src = redo ? redoBuf : undoBuf, *dst = redo ? undoBuf : redoBuf;
    int *sc = redo ? &redoCount : &undoCount, *dc = redo ? &undoCount : &redoCount;
    if (!*sc) { setStatus(redo ? "NOTHING TO REDO" : "NOTHING TO UNDO", false); return; }
    int g = src[*sc - 1].group;
    while (*sc && src[*sc - 1].group == g) {
        UndoEnt *e = &src[--*sc];
        if (*dc == UNDO_MAX) { memmove(dst, dst + 1, sizeof(UndoEnt) * (UNDO_MAX - 1)); (*dc)--; }
        UndoEnt *d = &dst[(*dc)++];
        d->pat = e->pat;
        d->group = g;
        memcpy(d->data, song.pat[e->pat], sizeof(d->data));
        memcpy(song.pat[e->pat], e->data, sizeof(e->data));
    }
    setModified();
    setStatus(redo ? "REDONE" : "UNDONE", false);
}

/* ------------------------------------------------------------------ */
/* config                                                              */

static void cfgLoad(void)
{
    FILE *f = plat_fopen(plat_config_path(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "stereosep")) cfg.stereoSep = clampi(v, 0, 100);
        else if (!strcmp(line, "amplify")) cfg.amp = clampi(v, 1, 16);
        else if (!strcmp(line, "interpolation")) cfg.interpolation = v != 0;
        else if (!strcmp(line, "a500filter")) cfg.a500Filter = v != 0;
        else if (!strcmp(line, "ledfilter")) cfg.ledEmulation = v != 0;
        else if (!strcmp(line, "panning")) cfg.panning = v != 0;
        else if (!strcmp(line, "saveformat")) cfg.saveFormat = clampi(v, 0, 1);
        else if (!strcmp(line, "editskip")) cfg.editSkip = clampi(v, 0, 16);
        else if (!strcmp(line, "followplay")) cfg.followPlay = v != 0;
        else if (!strcmp(line, "octave")) octave = clampi(v, 1, 2);
        else if (!strcmp(line, "audiobuffer")) cfg.audioBuffer = clampi(v, 128, 8192);
    }
    fclose(f);
}

static void cfgSave(void)
{
    FILE *f = plat_fopen(plat_config_path(), "w");
    if (!f) return;
    fprintf(f, "[PT8]\nstereosep=%d\namplify=%d\ninterpolation=%d\na500filter=%d\nledfilter=%d\npanning=%d\n"
               "saveformat=%d\neditskip=%d\nfollowplay=%d\noctave=%d\naudiobuffer=%d\n",
            cfg.stereoSep, cfg.amp, cfg.interpolation, cfg.a500Filter, cfg.ledEmulation, cfg.panning,
            cfg.saveFormat, cfg.editSkip, cfg.followPlay, octave, cfg.audioBuffer);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* playback control                                                    */

static void startPlay(int mode, bool fromRow)
{
    plat_lock();
    int row = fromRow ? curRow : 0;
    replayer_play(mode, curOrder, row, song.orders[curOrder]);
    playStartFrame = replayer_frames_rendered();
    liveMode = replayer_mode();
    plat_unlock();
}

static void stopPlay(void)
{
    plat_lock();
    replayer_stop();
    liveMode = PLAY_STOPPED;
    plat_unlock();
}

static void jam(int note)
{
    if (note < 1 || note > NUM_NOTES) return;
    plat_lock();
    replayer_jam(curCh, curSample, note);
    plat_unlock();
}

/* ------------------------------------------------------------------ */
/* file operations                                                     */

static void updateTitle(void)
{
    char t[1200];
    if (songPath[0]) snprintf(t, sizeof t, "PT8 - %s%s", baseName(songPath), modified ? " *" : "");
    else snprintf(t, sizeof t, "PT8 - 8 channel ProTracker%s", modified ? " *" : "");
    plat_set_title(t);
}

static void loadModuleNow(const char *path)
{
    char msg[128];
    plat_lock();
    replayer_stop();
    liveMode = PLAY_STOPPED;
    int ok = mod_load_file(path, msg, sizeof msg);
    replayer_reset_sync();
    plat_unlock();
    if (ok) {
        snprintf(songPath, sizeof songPath, "%s", path);
        modified = false;
        curOrder = curRow = curSample = 0;
        selStart = selEnd = -1;
        samplerFor = -1;
        undoClear();
        setStatus(msg, false);
    } else {
        setStatus(msg, true);
    }
}

static void loadSampleNow(const char *path);

static void onDiscardLoad(int c)
{
    if (c == 0) loadModuleNow(pendingPath);
}

static void openModuleDialog(void)
{
    char path[1024];
    if (!plat_open_dialog(FT_MODULE, path, sizeof path)) return;
    if (modified) {
        snprintf(pendingPath, sizeof pendingPath, "%s", path);
        ask("SONG HAS UNSAVED CHANGES.", "LOAD MODULE ANYWAY?", onDiscardLoad, 2, "YES", "NO", NULL, NULL);
    } else {
        loadModuleNow(path);
    }
}

static void saveModule(bool saveAs)
{
    char path[1024], msg[128];
    if (!saveAs && songPath[0]) {
        snprintf(path, sizeof path, "%s", songPath);
    } else {
        char def[64];
        snprintf(def, sizeof def, "%s.mod", song.name[0] ? song.name : "untitled");
        for (char *p = def; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
        if (!plat_save_dialog(FT_MODULE, def, path, sizeof path)) return;
    }
    plat_lock();
    int ok = mod_save_file(path, msg, sizeof msg);
    plat_unlock();
    if (ok) {
        snprintf(songPath, sizeof songPath, "%s", path);
        modified = false;
    }
    setStatus(msg, !ok);
}

static void onResample(int c)
{
    int rate = (c == 0) ? C3_RATE : (c == 1) ? C3_RATE / 2 : 0;
    plat_lock();
    sample_install(pendingSmp, &pendingRaw, rate);
    plat_unlock();
    rawsample_free(&pendingRaw);
    selStart = selEnd = -1;
    samplerFor = -1;
    setModified();
    setStatus("SAMPLE LOADED", false);
}

static void loadSampleNow(const char *path)
{
    char msg[128];
    rawsample_free(&pendingRaw);
    if (!sample_import(path, &pendingRaw, msg, sizeof msg)) { setStatus(msg, true); return; }
    pendingSmp = curSample;
    if (pendingRaw.rate > 22050) {
        char l1[80];
        snprintf(l1, sizeof l1, "SAMPLE RATE IS %d HZ.", pendingRaw.rate);
        ask(l1, "CONVERT TO PLAY AT ORIGINAL PITCH ON:", onResample, 3, "C-3", "C-2", "KEEP", NULL);
    } else {
        onResample(2);
        if (pendingRaw.frames > MAX_SAMPLE_LEN) setStatus("SAMPLE TRUNCATED TO 128 KB", true);
    }
}

static void openSampleDialog(void)
{
    char path[1024];
    if (plat_open_dialog(FT_SAMPLE, path, sizeof path)) loadSampleNow(path);
}

static void saveSampleDialog(void)
{
    char path[1024], msg[128], def[64];
    Sample *s = &song.smp[curSample];
    if (!s->data || s->length <= 0) { setStatus("SAMPLE IS EMPTY", true); return; }
    snprintf(def, sizeof def, "%s.wav", s->name[0] ? s->name : "sample");
    for (char *p = def; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    if (!plat_save_dialog(FT_SAMPLE_SAVE, def, path, sizeof path)) return;
    int ok = sample_save_wav(curSample, path, msg, sizeof msg);
    setStatus(msg, !ok);
}

static void exportWavDialog(void)
{
    char path[1024], msg[128], def[64];
    snprintf(def, sizeof def, "%s.wav", song.name[0] ? song.name : "song");
    for (char *p = def; *p; p++) if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    if (!plat_save_dialog(FT_WAV_EXPORT, def, path, sizeof path)) return;
    plat_lock();
    liveMode = PLAY_STOPPED;
    int ok = export_wav(path, msg, sizeof msg);
    plat_unlock();
    setStatus(msg, !ok);
}

static void onClear(int c)
{
    plat_lock();
    replayer_stop();
    liveMode = PLAY_STOPPED;
    if (c == 0) { memset(song.name, 0, sizeof song.name); song_clear_patterns(); }
    else if (c == 1) song_clear_samples();
    else if (c == 2) { song_clear_all(); songPath[0] = 0; }
    replayer_reset_sync();
    plat_unlock();
    if (c <= 2) {
        if (c != 1) { curOrder = curRow = 0; undoClear(); }
        selStart = selEnd = -1;
        samplerFor = -1;
        modified = (c != 2);
        setStatus(c == 0 ? "SONG CLEARED" : c == 1 ? "SAMPLES CLEARED" : "ALL CLEARED", false);
    }
}

static void onKillSample(int c)
{
    if (c != 0) return;
    plat_lock();
    replayer_invalidate_sample(curSample);
    sample_clear(curSample);
    plat_unlock();
    selStart = selEnd = -1;
    setModified();
    setStatus("SAMPLE KILLED", false);
}

static void onQuit(int c)
{
    if (c == 0) { modified = false; plat_quit(); }
}

/* ------------------------------------------------------------------ */
/* pattern editing                                                     */

static void advanceRow(int n)
{
    if (recording()) return;
    curRow = (curRow + n) & 63;
}

static void putNote(int note)
{
    if (note < NOTE_PT_MIN || note > NOTE_PT_MAX) return;
    if (editMode && screen == SCR_PATTERN) {
        int p = viewPattern(), r = editRow();
        undoBegin(); undoSave(p);
        Cell *c = &song.pat[p][r][curCh];
        c->note = (uint8_t)note;
        c->sample = (uint8_t)(curSample + 1);
        advanceRow(cfg.editSkip);
    }
    jam(note);
}

static void putHex(int v)
{
    if (!editMode || screen != SCR_PATTERN || curCol == 0) return;
    int p = viewPattern(), r = editRow();
    Cell *c = &song.pat[p][r][curCh];
    undoBegin(); undoSave(p);
    switch (curCol) {
        case 1: if (v > 1) return; c->sample = (uint8_t)((v << 4) | (c->sample & 0x0F)); if (c->sample > 31) c->sample = 31; break;
        case 2: c->sample = (uint8_t)((c->sample & 0xF0) | v); if (c->sample > 31) c->sample = 31; break;
        case 3: c->cmd = (uint8_t)v; break;
        case 4: c->param = (uint8_t)((v << 4) | (c->param & 0x0F)); break;
        case 5: c->param = (uint8_t)((c->param & 0xF0) | v); break;
    }
    advanceRow(cfg.editSkip);
}

static void deleteCell(bool all)
{
    if (!editMode) return;
    int p = viewPattern(), r = editRow();
    Cell *c = &song.pat[p][r][curCh];
    undoBegin(); undoSave(p);
    if (all) memset(c, 0, sizeof *c);
    else if (curCol <= 2) { c->note = 0; c->sample = 0; }
    else { c->cmd = 0; c->param = 0; }
    advanceRow(cfg.editSkip);
}

static void insertRow(bool allChannels)
{
    if (!editMode) return;
    int p = viewPattern(), r = editRow();
    undoBegin(); undoSave(p);
    for (int ch = 0; ch < NUM_CH; ch++) {
        if (!allChannels && ch != curCh) continue;
        for (int i = NUM_ROWS - 1; i > r; i--) song.pat[p][i][ch] = song.pat[p][i - 1][ch];
        memset(&song.pat[p][r][ch], 0, sizeof(Cell));
    }
}

static void deleteRowAbove(bool allChannels)
{
    if (!editMode || curRow == 0 || recording()) return;
    int p = viewPattern(), r = curRow - 1;
    undoBegin(); undoSave(p);
    for (int ch = 0; ch < NUM_CH; ch++) {
        if (!allChannels && ch != curCh) continue;
        for (int i = r; i < NUM_ROWS - 1; i++) song.pat[p][i][ch] = song.pat[p][i + 1][ch];
        memset(&song.pat[p][NUM_ROWS - 1][ch], 0, sizeof(Cell));
    }
    curRow--;
}

static void trackOp(int op /* 0 cut 1 copy 2 paste */)
{
    int p = viewPattern();
    if (op != 1) { undoBegin(); undoSave(p); }
    if (op <= 1) {
        for (int r = 0; r < NUM_ROWS; r++) clipTrack[r] = song.pat[p][r][curCh];
        clipTrackOk = true;
        if (op == 0) for (int r = 0; r < NUM_ROWS; r++) memset(&song.pat[p][r][curCh], 0, sizeof(Cell));
        setStatus(op == 0 ? "TRACK CUT" : "TRACK COPIED", false);
    } else if (clipTrackOk) {
        for (int r = 0; r < NUM_ROWS; r++) song.pat[p][r][curCh] = clipTrack[r];
        setStatus("TRACK PASTED", false);
    } else setStatus("BUFFER IS EMPTY", true);
}

static void patternOp(int op)
{
    int p = viewPattern();
    if (op != 1) { undoBegin(); undoSave(p); }
    if (op <= 1) {
        memcpy(clipPat, song.pat[p], sizeof clipPat);
        clipPatOk = true;
        if (op == 0) memset(song.pat[p], 0, sizeof(song.pat[p]));
        setStatus(op == 0 ? "PATTERN CUT" : "PATTERN COPIED", false);
    } else if (clipPatOk) {
        memcpy(song.pat[p], clipPat, sizeof clipPat);
        setStatus("PATTERN PASTED", false);
    } else setStatus("BUFFER IS EMPTY", true);
}

static void cmdOp(int op)
{
    int p = viewPattern();
    if (op != 1) { undoBegin(); undoSave(p); }
    if (op <= 1) {
        for (int r = 0; r < NUM_ROWS; r++) clipCmd[r] = song.pat[p][r][curCh];
        clipCmdOk = true;
        if (op == 0) for (int r = 0; r < NUM_ROWS; r++) { song.pat[p][r][curCh].cmd = 0; song.pat[p][r][curCh].param = 0; }
        setStatus(op == 0 ? "COMMANDS CUT" : "COMMANDS COPIED", false);
    } else if (clipCmdOk) {
        for (int r = 0; r < NUM_ROWS; r++) { song.pat[p][r][curCh].cmd = clipCmd[r].cmd; song.pat[p][r][curCh].param = clipCmd[r].param; }
        setStatus("COMMANDS PASTED", false);
    } else setStatus("BUFFER IS EMPTY", true);
}

/* scope: 0 track, 1 pattern, 2 song */
static void transpose(int scope, int semis)
{
    int lo = song.extendedNotes ? 1 : NOTE_PT_MIN, hi = song.extendedNotes ? NUM_NOTES : NOTE_PT_MAX;
    int pats[MAX_PATTERNS], np = 0;
    if (scope == 2) {
        bool used[MAX_PATTERNS] = { false };
        for (int i = 0; i < song.numOrders; i++) used[song.orders[i]] = true;
        for (int p = 0; p < MAX_PATTERNS; p++) if (used[p]) pats[np++] = p;
    } else pats[np++] = viewPattern();
    undoBegin();
    int changed = 0;
    for (int k = 0; k < np; k++) {
        int p = pats[k];
        undoSave(p);
        for (int r = 0; r < NUM_ROWS; r++)
            for (int ch = 0; ch < NUM_CH; ch++) {
                if (scope == 0 && ch != curCh) continue;
                Cell *c = &song.pat[p][r][ch];
                if (!c->note) continue;
                if (!transposeAll && c->sample != curSample + 1) continue;
                int n = c->note + semis;
                if (n < lo || n > hi) continue;
                c->note = (uint8_t)n;
                changed++;
            }
    }
    char m[64];
    snprintf(m, sizeof m, "%d NOTES TRANSPOSED", changed);
    setStatus(m, false);
}

/* ------------------------------------------------------------------ */
/* sample editing helpers                                              */

static Sample *cs(void) { return &song.smp[curSample]; }

static void smpReplace(int8_t *data, int32_t len, int32_t ls, int32_t ll)
{
    plat_lock();
    Sample *s = cs();
    replayer_invalidate_sample(curSample);
    free(s->data);
    if (len <= 0) { free(data); data = NULL; len = 0; }
    s->data = data;
    s->length = len;
    s->loopStart = ls;
    s->loopLength = ll;
    sample_fix_loop(s);
    plat_unlock();
    setModified();
}

static int8_t *smpAlloc(int32_t len) { return (int8_t *)calloc((size_t)(len > 0 ? len : 1) + 4, 1); }

static bool haveSel(void)
{
    Sample *s = cs();
    return selStart >= 0 && selEnd > selStart && s->data && selEnd <= s->length;
}

static void selRange(int32_t *a, int32_t *b)
{
    if (haveSel()) { *a = selStart; *b = selEnd; }
    else { *a = 0; *b = cs()->length; }
}

static void smpSetLength(int32_t newLen)
{
    Sample *s = cs();
    newLen = clampi(newLen, 0, MAX_SAMPLE_LEN) & ~1;
    if (newLen == s->length) return;
    int8_t *d = smpAlloc(newLen);
    if (!d) return;
    if (s->data) memcpy(d, s->data, (size_t)(newLen < s->length ? newLen : s->length));
    smpReplace(d, newLen, s->loopStart, s->loopLength);
}

static void smpCut(bool keepInClip)
{
    Sample *s = cs();
    if (!haveSel()) { setStatus("NO RANGE MARKED", true); return; }
    int32_t a = selStart & ~1, b = (selEnd + 1) & ~1;
    if (b > s->length) b = s->length;
    int32_t n = b - a;
    if (keepInClip) {
        free(clipSmp);
        clipSmp = (int8_t *)malloc((size_t)n + 1);
        memcpy(clipSmp, s->data + a, (size_t)n);
        clipSmpLen = n;
    }
    int32_t newLen = s->length - n;
    int8_t *d = smpAlloc(newLen);
    memcpy(d, s->data, (size_t)a);
    memcpy(d + a, s->data + b, (size_t)(s->length - b));
    int32_t ls = s->loopStart, ll = s->loopLength;
    if (ll > 2) {
        if (ls >= b) ls -= n;
        else if (ls + ll > a) { ls = 0; ll = 2; }
    }
    smpReplace(d, newLen, ls, ll);
    selStart = selEnd = a;
    setStatus("RANGE CUT", false);
}

static void smpCopy(void)
{
    Sample *s = cs();
    if (!haveSel()) { setStatus("NO RANGE MARKED", true); return; }
    free(clipSmp);
    clipSmpLen = selEnd - selStart;
    clipSmp = (int8_t *)malloc((size_t)clipSmpLen + 1);
    memcpy(clipSmp, s->data + selStart, (size_t)clipSmpLen);
    setStatus("RANGE COPIED", false);
}

static void smpPaste(void)
{
    Sample *s = cs();
    if (!clipSmp || clipSmpLen <= 0) { setStatus("BUFFER IS EMPTY", true); return; }
    int32_t at = (selStart >= 0 && selStart <= s->length) ? (selStart & ~1) : 0;
    int32_t n = clipSmpLen;
    if (s->length + n > MAX_SAMPLE_LEN) n = (MAX_SAMPLE_LEN - s->length) & ~1;
    if (n <= 0) { setStatus("SAMPLE TOO LONG", true); return; }
    int32_t newLen = (s->length + n + 1) & ~1;
    int8_t *d = smpAlloc(newLen);
    if (s->data) memcpy(d, s->data, (size_t)at);
    memcpy(d + at, clipSmp, (size_t)n);
    if (s->data) memcpy(d + at + n, s->data + at, (size_t)(s->length - at));
    int32_t ls = s->loopStart, ll = s->loopLength;
    if (ll > 2 && ls >= at) ls += n;
    smpReplace(d, newLen, ls, ll);
    selStart = at; selEnd = at + n;
    setStatus("RANGE PASTED", false);
}

static void smpCrop(void)
{
    Sample *s = cs();
    if (!haveSel()) { setStatus("NO RANGE MARKED", true); return; }
    int32_t a = selStart & ~1, b = (selEnd + 1) & ~1;
    if (b > s->length) b = s->length;
    int32_t n = b - a;
    int8_t *d = smpAlloc(n);
    memcpy(d, s->data + a, (size_t)n);
    int32_t ls = s->loopStart - a, ll = s->loopLength;
    if (ll > 2) {
        if (ls < 0) { ll += ls; ls = 0; }
        if (ls + ll > n) ll = n - ls;
        if (ll <= 2) { ls = 0; ll = 2; }
    }
    smpReplace(d, n, ls, ll);
    selStart = 0; selEnd = n;
    viewStart = 0; viewLen = n;
    setStatus("SAMPLE CROPPED", false);
}

static void smpProcess(int op)
{
    Sample *s = cs();
    if (!s->data || s->length <= 0) { setStatus("SAMPLE IS EMPTY", true); return; }
    int32_t a, b;
    selRange(&a, &b);
    int32_t n = b - a;
    if (n <= 0) return;
    plat_lock();
    int8_t *d = s->data;
    switch (op) {
        case 0: /* reverse */
            for (int32_t i = 0; i < n / 2; i++) { int8_t t = d[a + i]; d[a + i] = d[b - 1 - i]; d[b - 1 - i] = t; }
            break;
        case 1: { /* normalize */
            int mx = 1;
            for (int32_t i = a; i < b; i++) { int v = abs(d[i]); if (v > mx) mx = v; }
            for (int32_t i = a; i < b; i++) { int v = (int)lrint(d[i] * 127.0 / mx); d[i] = (int8_t)clampi(v, -128, 127); }
        } break;
        case 2: /* fade in */
            for (int32_t i = 0; i < n; i++) d[a + i] = (int8_t)(d[a + i] * i / n);
            break;
        case 3: /* fade out */
            for (int32_t i = 0; i < n; i++) d[a + i] = (int8_t)(d[a + i] * (n - i) / n);
            break;
        case 4: { /* center (remove DC) */
            long sum = 0;
            for (int32_t i = a; i < b; i++) sum += d[i];
            int off = (int)(sum / n);
            for (int32_t i = a; i < b; i++) d[i] = (int8_t)clampi(d[i] - off, -128, 127);
        } break;
        case 5: /* half volume */
            for (int32_t i = a; i < b; i++) d[i] = (int8_t)(d[i] / 2);
            break;
        case 6: /* double volume (boost) */
            for (int32_t i = a; i < b; i++) d[i] = (int8_t)clampi(d[i] * 2, -128, 127);
            break;
    }
    plat_unlock();
    setModified();
    static const char *names[] = { "REVERSED", "NORMALIZED", "FADED IN", "FADED OUT", "CENTERED", "VOLUME HALVED", "VOLUME BOOSTED" };
    setStatus(names[op], false);
}

static void smpResample(bool up)
{
    Sample *s = cs();
    if (!s->data || s->length <= 0) { setStatus("SAMPLE IS EMPTY", true); return; }
    int32_t newLen;
    int8_t *d;
    if (up) {
        newLen = s->length * 2;
        if (newLen > MAX_SAMPLE_LEN) newLen = MAX_SAMPLE_LEN;
        d = smpAlloc(newLen);
        for (int32_t i = 0; i < newLen; i++) {
            int32_t k = i / 2;
            int v = s->data[k];
            if ((i & 1) && k + 1 < s->length) v = (v + s->data[k + 1]) / 2;
            d[i] = (int8_t)v;
        }
        smpReplace(d, newLen, s->loopStart * 2, s->loopLength * 2);
        setStatus("UPSAMPLED (PLAYS 1 OCTAVE LOWER)", false);
    } else {
        newLen = (s->length / 2 + 1) & ~1;
        d = smpAlloc(newLen);
        for (int32_t i = 0; i < newLen; i++) {
            int32_t k = i * 2;
            int v = k < s->length ? s->data[k] : 0;
            if (k + 1 < s->length) v = (v + s->data[k + 1]) / 2;
            d[i] = (int8_t)v;
        }
        smpReplace(d, newLen, s->loopStart / 2, s->loopLength / 2);
        setStatus("DOWNSAMPLED (PLAYS 1 OCTAVE HIGHER)", false);
    }
    selStart = selEnd = -1;
    viewStart = 0; viewLen = cs()->length;
}

static void smpToggleLoop(void)
{
    Sample *s = cs();
    if (!s->data || s->length <= 2) return;
    plat_lock();
    if (s->loopLength > 2) { s->loopStart = 0; s->loopLength = 2; }
    else if (haveSel() && selEnd - selStart > 2) { s->loopStart = selStart & ~1; s->loopLength = (selEnd - selStart) & ~1; }
    else { s->loopStart = 0; s->loopLength = s->length; }
    sample_fix_loop(s);
    plat_unlock();
    setModified();
}

/* ------------------------------------------------------------------ */
/* top left: position / sample panel                                   */

static int valueRow(int row, const char *label, const char *val)
{
    int y = row * RH;
    raised(0, y, LP_W, RH);
    text(4, y + 2, label, C_TEXT);
    sunken(VAL_X, y, VAL_W, RH, C_VALBG);
    text(VAL_X + VAL_W - 3 - textLen(val) * 8, y + 2, val, C_VALTXT);
    int d = 0, a;
    if ((a = arrowBtn(ARR1_X, y, ARR_W, RH, true))) d = a == 2 ? 10 : 1;
    if ((a = arrowBtn(ARR1_X + ARR_W, y, ARR_W, RH, false))) d = a == 2 ? -10 : -1;
    return d;
}

static void drawLeftPanel(void)
{
    char v[32];
    int d;
    Sample *s = cs();

    snprintf(v, sizeof v, "%04d", curOrder);
    if ((d = valueRow(0, "POS", v))) {
        if (!isPlaying()) { curOrder = clampi(curOrder + d, 0, song.numOrders - 1); }
    }
    snprintf(v, sizeof v, "%04d", song.orders[curOrder]);
    if ((d = valueRow(1, "PATTERN", v))) {
        song.orders[curOrder] = (uint8_t)clampi(song.orders[curOrder] + d, 0, MAX_PATTERNS - 1);
        setModified();
    }
    snprintf(v, sizeof v, "%04d", song.numOrders);
    if ((d = valueRow(2, "LENGTH", v))) {
        song.numOrders = clampi(song.numOrders + d, 1, MAX_ORDERS);
        if (curOrder >= song.numOrders) curOrder = song.numOrders - 1;
        setModified();
    }
    int ft = s->finetune < 8 ? s->finetune : s->finetune - 16;
    snprintf(v, sizeof v, "%+d", ft);
    if (ft == 0) snprintf(v, sizeof v, "0");
    if ((d = valueRow(3, "FINETUNE", v))) {
        ft = clampi(ft + (d > 0 ? 1 : -1), -8, 7);
        s->finetune = (uint8_t)(ft & 15);
        setModified();
    }
    snprintf(v, sizeof v, "%02X", curSample + 1);
    if ((d = valueRow(4, "SAMPLE", v))) {
        curSample = clampi(curSample + (d > 0 ? (d > 1 ? 10 : 1) : (d < -1 ? -10 : -1)), 0, NUM_SAMPLES - 1);
        selStart = selEnd = -1;
    }
    snprintf(v, sizeof v, "%02X", s->volume);
    if ((d = valueRow(5, "VOLUME", v))) { s->volume = (uint8_t)clampi(s->volume + d, 0, 64); setModified(); }
    snprintf(v, sizeof v, "%05X", s->length);
    if ((d = valueRow(6, "LENGTH", v))) {
        int step = abs(d) > 1 ? 0x100 : 2;
        smpSetLength(s->length + (d > 0 ? step : -step));
    }
    snprintf(v, sizeof v, "%05X", s->loopLength > 2 ? s->loopStart : 0);
    if ((d = valueRow(7, "REPEAT", v)) && s->length > 2) {
        plat_lock();
        int step = abs(d) > 1 ? 0x100 : 2;
        int32_t ll = s->loopLength > 2 ? s->loopLength : s->length;
        int32_t ls = s->loopLength > 2 ? s->loopStart : 0;
        ls = clampi(ls + (d > 0 ? step : -step), 0, s->length - 4);
        if (ls + ll > s->length) ll = s->length - ls;
        s->loopStart = ls; s->loopLength = ll;
        sample_fix_loop(s);
        plat_unlock();
        setModified();
    }
    snprintf(v, sizeof v, "%05X", s->loopLength > 2 ? s->loopLength : 2);
    if ((d = valueRow(8, "REPLEN", v)) && s->length > 2) {
        plat_lock();
        int step = abs(d) > 1 ? 0x100 : 2;
        int32_t ls = s->loopLength > 2 ? s->loopStart : 0;
        int32_t ll = s->loopLength > 2 ? s->loopLength : 2;
        ll = clampi(ll + (d > 0 ? step : -step), 2, s->length - ls);
        s->loopStart = ls; s->loopLength = ll;
        sample_fix_loop(s);
        plat_unlock();
        setModified();
    }
    snprintf(v, sizeof v, "%d", replayer_bpm());
    if ((d = valueRow(9, "TEMPO (BPM)", v))) {
        plat_lock();
        replayer_set_bpm(replayer_bpm() + d);
        plat_unlock();
    }
    snprintf(v, sizeof v, "%d", cfg.editSkip);
    if ((d = valueRow(10, "EDIT SKIP", v))) cfg.editSkip = clampi(cfg.editSkip + (d > 0 ? 1 : -1), 0, 16);
}

static void onClearChoice(int c) { onClear(c); }

static void drawButtons(void)
{
    int x0 = BTN_X, x1 = BTN_X + BTN_W;
    bool songPlay = liveMode == PLAY_SONG, patPlay = liveMode == PLAY_PATTERN;

    if (button(x0, 0 * RH, BTN_W, RH, "PLAY", songPlay)) startPlay(PLAY_SONG, false);
    if (button(x1, 0 * RH, BTN_W, RH, "STOP", false)) stopPlay();
    if (button(x0, 1 * RH, BTN_W, RH, "PATTERN", patPlay && !editMode)) startPlay(PLAY_PATTERN, false);
    if (button(x1, 1 * RH, BTN_W, RH, "CLEAR", false))
        ask("CLEAR WHAT?", NULL, onClearChoice, 4, "SONG", "SAMPLES", "ALL", "CANCEL");
    if (button(x0, 2 * RH, BTN_W, RH, "EDIT", editMode && !isPlaying())) { editMode = !editMode; if (isPlaying()) stopPlay(); }
    if (button(x1, 2 * RH, BTN_W, RH, "EDIT OP.", overlay == OV_EDITOP)) overlay = overlay == OV_EDITOP ? OV_NONE : OV_EDITOP;
    if (button(x0, 3 * RH, BTN_W, RH, "RECORD", editMode && isPlaying())) { editMode = true; startPlay(PLAY_PATTERN, false); }
    if (button(x1, 3 * RH, BTN_W, RH, "SAMPLER", screen == SCR_SAMPLER)) { screen = screen == SCR_SAMPLER ? SCR_PATTERN : SCR_SAMPLER; showHelp = false; }
    if (button(x0, 4 * RH, BTN_W, RH, "INS POS", false)) {
        if (song.numOrders < MAX_ORDERS) {
            memmove(&song.orders[curOrder + 1], &song.orders[curOrder], (size_t)(MAX_ORDERS - 1 - curOrder));
            song.numOrders++;
            setModified();
        }
    }
    if (button(x1, 4 * RH, BTN_W, RH, "DEL POS", false)) {
        if (song.numOrders > 1) {
            memmove(&song.orders[curOrder], &song.orders[curOrder + 1], (size_t)(MAX_ORDERS - 1 - curOrder));
            song.orders[MAX_ORDERS - 1] = 0;
            song.numOrders--;
            if (curOrder >= song.numOrders) curOrder = song.numOrders - 1;
            setModified();
        }
    }
    if (button(x0, 5 * RH, BTN_W, RH, "LOAD MOD", false)) openModuleDialog();
    if (button(x1, 5 * RH, BTN_W, RH, "SAVE MOD", false)) saveModule(false);
    if (button(x0, 6 * RH, BTN_W, RH, "LOAD SMP", false)) openSampleDialog();
    if (button(x1, 6 * RH, BTN_W, RH, "SAVE SMP", false)) saveSampleDialog();
    if (button(x0, 7 * RH, BTN_W, RH, "SAVE AS", false)) saveModule(true);
    if (button(x1, 7 * RH, BTN_W, RH, "EXP. WAV", false)) exportWavDialog();
    if (button(x0, 8 * RH, BTN_W, RH, "SETUP", overlay == OV_SETUP)) overlay = overlay == OV_SETUP ? OV_NONE : OV_SETUP;
    if (button(x1, 8 * RH, BTN_W, RH, "HELP", showHelp)) { showHelp = !showHelp; showAbout = false; }
    if (button(x0, 9 * RH, BTN_W, RH, "KILL SMP", false)) {
        char l[64];
        snprintf(l, sizeof l, "KILL SAMPLE %02X?", curSample + 1);
        ask(l, NULL, onKillSample, 2, "YES", "NO", NULL, NULL);
    }
    if (button(x1, 9 * RH, BTN_W, RH, "UNMUTE", false)) memset(chanMuted, 0, sizeof chanMuted);
    if (button(x0, 10 * RH, BTN_W, RH, "POS ED.", overlay == OV_POSED)) overlay = overlay == OV_POSED ? OV_NONE : OV_POSED;
    if (button(x1, 10 * RH, BTN_W, RH, "QUIT", false)) {
        if (app_request_quit()) plat_quit();
    }
}

/* ------------------------------------------------------------------ */
/* scopes                                                              */

static void drawScopes(void)
{
    const int cols = 4, sw = SCOPE_W / cols;
    for (int ch = 0; ch < NUM_CH; ch++) {
        int cx = SCOPE_X + (ch % cols) * sw, cy = (ch / cols) * 60;
        int ww = (ch % cols == cols - 1) ? SCOPE_W - (cols - 1) * sw : sw;
        int hh = (ch / cols == 1) ? TOP_H - 60 : 60;
        raised(cx, cy, ww, hh);
        int ix = cx + 2, iy = cy + 2, iw = ww - 4, ih = hh - 4;
        fill(ix, iy, iw, ih, S_BG);
        int mid = iy + ih / 2;
        hline(ix, mid, iw, S_MID);
        bool muted = chanMuted[ch];
        uint32_t wc = muted ? S_MUTED : S_WAVE;
        char lbl[8];
        snprintf(lbl, sizeof lbl, "%d", ch + 1);
        text(ix + 2, iy + 2, lbl, muted ? 0x804030 : 0x5A6070);
        if (muted) text(ix + iw - 42, iy + 2, "MUTE", 0xC05030);

        if (snapOk && !muted) {
            ScopeVoice *v = &snap.v[ch];
            if (v->active && v->smp >= 0 && v->smp < NUM_SAMPLES) {
                const Sample *s = &song.smp[v->smp];
                const Sample *ls = (v->regSmp >= 0 && v->regSmp < NUM_SAMPLES) ? &song.smp[v->regSmp] : NULL;
                if (s->data) {
                    int prevY = mid;
                    for (int x = 0; x < iw; x++) {
                        int32_t p = v->pos + x;
                        int sv = 0;
                        if (p < v->len) {
                            int32_t idx = v->start + p;
                            if (idx >= 0 && idx < s->length) sv = s->data[idx];
                        } else if (ls && ls->data && v->regLen > 2) {
                            int32_t idx = v->regStart + (p - v->len) % v->regLen;
                            if (idx >= 0 && idx < ls->length) sv = ls->data[idx];
                        }
                        int y = mid - sv * v->volume * (ih / 2 - 1) / (64 * 128);
                        int y0 = prevY < y ? prevY : y, y1 = prevY < y ? y : prevY;
                        if (x == 0) y0 = y1 = y;
                        fill(ix + x, y0, 1, y1 - y0 + 1, wc);
                        prevY = y;
                    }
                }
            }
        }
        if (inputOn && hit(cx, cy, ww, hh)) {
            if (ms.lPress) { ms.lPress = false; chanMuted[ch] = !chanMuted[ch]; }
            else if (ms.rPress) {
                ms.rPress = false;
                bool solo = !chanMuted[ch];
                for (int i = 0; i < NUM_CH; i++) if (i != ch && !chanMuted[i]) solo = false;
                for (int i = 0; i < NUM_CH; i++) chanMuted[i] = solo ? false : (i != ch);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* edit op / setup overlays (replace the scopes)                        */

static void ovTitle(const char *t)
{
    raised(SCOPE_X, 0, SCOPE_W, TOP_H);
    fill(SCOPE_X + 2, 2, SCOPE_W - 4, RH - 2, 0x6A6C78);
    text(SCOPE_X + 6, 3, t, 0xFFFFFF);
}

static void drawEditOp(void)
{
    ovTitle("EDIT OP.");
    if (button(W - 60, 0, 58, RH, "CLOSE", false)) overlay = OV_NONE;
    int y = RH + 4, x = SCOPE_X + 6;
    text(x, y + 2, "TRANSPOSE:", C_TEXT);
    if (button(x + 92, y, 150, RH, transposeAll ? "ALL SAMPLES" : "CURRENT SAMPLE", false)) transposeAll = !transposeAll;
    y += RH + 4;
    static const char *lbl[3] = { "TRACK", "PATTERN", "SONG" };
    static const int amt[4] = { -12, -1, 1, 12 };
    static const char *albl[4] = { "-12", "-1", "+1", "+12" };
    for (int sc = 0; sc < 3; sc++) {
        text(x, y + 2, lbl[sc], C_TEXT);
        for (int k = 0; k < 4; k++)
            if (button(x + 80 + k * 48, y, 46, RH, albl[k], false)) transpose(sc, amt[k]);
        y += RH + 1;
    }
    y += 4;
    text(x, y + 2, "TRACK", C_TEXT);
    if (button(x + 80, y, 60, RH, "CUT", false)) trackOp(0);
    if (button(x + 142, y, 60, RH, "COPY", false)) trackOp(1);
    if (button(x + 204, y, 60, RH, "PASTE", false)) trackOp(2);
    y += RH + 1;
    text(x, y + 2, "PATTERN", C_TEXT);
    if (button(x + 80, y, 60, RH, "CUT", false)) patternOp(0);
    if (button(x + 142, y, 60, RH, "COPY", false)) patternOp(1);
    if (button(x + 204, y, 60, RH, "PASTE", false)) patternOp(2);
    y += RH + 1;
    text(x, y + 2, "EFFECTS", C_TEXT);
    if (button(x + 80, y, 60, RH, "CUT", false)) cmdOp(0);
    if (button(x + 142, y, 60, RH, "COPY", false)) cmdOp(1);
    if (button(x + 204, y, 60, RH, "PASTE", false)) cmdOp(2);
}

static void setupRow(int y, const char *label, const char *val, int *delta, bool *toggle)
{
    int x = SCOPE_X + 6;
    text(x, y + 2, label, C_TEXT);
    if (delta) {
        sunken(x + 168, y, 72, RH, C_VALBG);
        text(x + 168 + 72 - 3 - textLen(val) * 8, y + 2, val, C_VALTXT);
        int a;
        *delta = 0;
        if ((a = arrowBtn(x + 242, y, ARR_W, RH, true))) *delta = 1;
        if ((a = arrowBtn(x + 242 + ARR_W, y, ARR_W, RH, false))) *delta = -1;
    } else if (toggle) {
        if (button(x + 168, y, 116, RH, val, false)) *toggle = !*toggle;
    }
}

static void drawSetup(void)
{
    ovTitle("SETUP");
    if (button(W - 60, 0, 58, RH, "CLOSE", false)) { overlay = OV_NONE; cfgSave(); }
    char v[32];
    int y = RH + 3, d;
    snprintf(v, sizeof v, "%d%%", cfg.stereoSep);
    setupRow(y, "STEREO SEPARATION", v, &d, NULL);
    if (d) cfg.stereoSep = clampi(cfg.stereoSep + d * 5, 0, 100);
    y += RH + 1;
    snprintf(v, sizeof v, "%dX", cfg.amp);
    setupRow(y, "AMPLIFICATION", v, &d, NULL);
    if (d) cfg.amp = clampi(cfg.amp + d, 1, 16);
    y += RH + 1;
    bool t = cfg.interpolation;
    setupRow(y, "INTERPOLATION", t ? "LINEAR" : "OFF (AMIGA)", NULL, &t);
    cfg.interpolation = t;
    y += RH + 1;
    t = cfg.a500Filter;
    setupRow(y, "AMIGA MODEL FILTER", t ? "A500" : "A1200", NULL, &t);
    cfg.a500Filter = t;
    y += RH + 1;
    t = cfg.ledEmulation;
    setupRow(y, "LED FILTER (E0X)", t ? "EMULATED" : "IGNORED", NULL, &t);
    cfg.ledEmulation = t;
    y += RH + 1;
    t = cfg.panning;
    setupRow(y, "8XX / E8X PANNING", t ? "ON" : "OFF", NULL, &t);
    cfg.panning = t;
    y += RH + 1;
    t = cfg.saveFormat == 1;
    setupRow(y, "SAVE FORMAT", t ? "ALWAYS 8CHN" : "AUTO 4/8 CH", NULL, &t);
    cfg.saveFormat = t ? 1 : 0;
    y += RH + 1;
    t = cfg.followPlay;
    setupRow(y, "FOLLOW PLAYBACK", t ? "ON" : "OFF", NULL, &t);
    cfg.followPlay = t;
}

static void drawPosEd(void)
{
    ovTitle("POSITION EDITOR");
    if (button(W - 60, 0, 58, RH, "CLOSE", false)) overlay = OV_NONE;
    const int vis = 9, lx = SCOPE_X + 4, ly = RH + 2, lw = SCOPE_W - 8;
    sunken(lx, ly, lw, vis * 11 + 4, 0x0A0C12);
    int first = curOrder - vis / 2;
    for (int i = 0; i < vis; i++) {
        int o = first + i;
        int y = ly + 2 + i * 11;
        if (o < 0 || o >= song.numOrders) continue;
        bool cur = o == curOrder;
        if (cur) fill(lx + 1, y, lw - 2, 11, P_BAR);
        textf(lx + 6, y + 2, cur ? 0xFFFFFF : P_ROWNUM, "POS %03d", o);
        int pat = song.orders[o];
        textf(lx + 80, y + 2, cur ? P_CURSOR : P_NOTE, "PATTERN %02d", pat);
        int notes = 0;
        for (int r = 0; r < NUM_ROWS; r++) for (int c = 0; c < NUM_CH; c++) if (song.pat[pat][r][c].note) notes++;
        textf(lx + 184, y + 2, P_DIM + 0x303030, notes ? "%4d NOTES" : "    EMPTY", notes);
        if (inputOn && hit(lx, y, lw, 11) && ms.lPress) {
            ms.lPress = false;
            if (!isPlaying()) curOrder = o;
        }
    }
    if (inputOn && hit(lx, ly, lw, vis * 11 + 4) && ms.wheel && !isPlaying())
        curOrder = clampi(curOrder - ms.wheel, 0, song.numOrders - 1);
}

/* ------------------------------------------------------------------ */
/* info strip                                                          */

static void drawInfo(void)
{
    int y = INFO_Y;
    raised(0, y, W, RH);
    text(4, y + 2, "SONGNAME:", C_TEXT);
    textField(80, y, 20 * 8 + 6, RH, song.name, 20);
    text(250, y + 2, "SAMPLENAME:", C_TEXT);
    char *nm = cs()->name;
    textField(340, y, 22 * 8 + 6, RH, nm, 22);
    uint64_t played = plat_audio_played_frames();
    int secs = 0;
    if (isPlaying() && played > playStartFrame) secs = (int)((played - playStartFrame) / 48000);
    int spd = (snapOk && snap.mode != PLAY_STOPPED) ? snap.speed : replayer_speed();
    int bpm = (snapOk && snap.mode != PLAY_STOPPED) ? snap.bpm : replayer_bpm();
    textf(528, y + 2, C_TEXT, "BPM:%d SPD:%d", bpm, spd);
    textf(W - 46, y + 2, C_TEXT, "%02d:%02d", secs / 60 % 100, secs % 60);

    y += RH;
    raised(0, y, W, RH);
    text(4, y + 2, "STATUS:", C_TEXT);
    if (statusErr && ((now - statusTime) / 250) % 2 == 0 && now - statusTime < 2000) fill(64, y + 1, 300, RH - 2, 0xD0A0A0);
    text(68, y + 2, status, statusErr ? 0x801010 : C_TEXT);
    if (now - statusTime > 4000 && strcmp(status, "ALL RIGHT")) { snprintf(status, sizeof status, "ALL RIGHT"); statusErr = false; }

    textf(384, y + 2, C_TEXT, "OCT:%d", octave);
    int mx = 448;
    const char *mode = "IDLE";
    uint32_t mc = C_TEXTDIM;
    if (recording()) { mode = "REC"; mc = 0x902000; }
    else if (editMode) { mode = "EDIT"; mc = C_RED; }
    else if (liveMode == PLAY_SONG) { mode = "PLAY"; mc = C_GREEN; }
    else if (liveMode == PLAY_PATTERN) { mode = "PATT"; mc = C_GREEN; }
    text(mx, y + 2, "MODE:", C_TEXT);
    fill(mx + 40, y + 1, 36, RH - 2, mc);
    text(mx + 42, y + 2, mode, 0xFFFFFF);
    bool led = snapOk ? snap.led : false;
    text(536, y + 2, "LED", C_TEXT);
    fill(562, y + 3, 7, 5, led && cfg.ledEmulation ? 0xFF3020 : 0x502020);
    if (!plat_audio_ok()) text(584, y + 2, "NO AUDIO!", 0x901010);
    else textf(584, y + 2, C_TEXT, "%s", song.extendedNotes ? "EXT.NOTES" : (song_uses_upper_channels() ? "8 CHANNELS" : "4 CH COMPAT"));
}

/* ------------------------------------------------------------------ */
/* pattern editor                                                      */

static const int colX[6] = { 0, 28, 36, 48, 56, 64 };
static const int colW[6] = { 24, 8, 8, 8, 8, 8 };

static void hexDigit(int x, int y, int v, uint32_t c) { glyph(x, y, (unsigned)"0123456789ABCDEF"[v & 15], c); }

static void drawPattern(void)
{
    int pat = viewPattern();
    int crow = (isPlaying() && snapOk && snap.mode != PLAY_STOPPED && cfg.followPlay) ? snap.row : curRow;
    int playRow = (isPlaying() && snapOk && snap.mode != PLAY_STOPPED && snap.pattern == pat) ? snap.row : -1;

    /* header */
    int y0 = PAT_Y;
    fill(0, y0, W, PAT_HDR_H, 0x1A1D28);
    textf(2, y0 + 2, P_ROW16, "P%02d", pat);
    for (int ch = 0; ch < NUM_CH; ch++) {
        int x = ROWNUM_W + ch * CH_W;
        bool muted = chanMuted[ch];
        fill(x + 1, y0 + 1, CH_W - 2, PAT_HDR_H - 2, muted ? 0x402020 : (ch == curCh ? 0x30385A : 0x23283A));
        textf(x + 4, y0 + 2, muted ? 0xA06050 : 0xB8C0D4, muted ? "%d MUTE" : "CH %d", ch + 1);
        /* VU from voice volume */
        if (snapOk && snap.v[ch].active && !muted) {
            int vw = snap.v[ch].volume * 28 / 64;
            fill(x + CH_W - 32, y0 + 4, vw, 4, vw >= 27 ? 0xF06040 : (vw >= 20 ? 0xE8C850 : 0x60D070));
        }
        if (inputOn && hit(x, y0, CH_W, PAT_HDR_H)) {
            if (ms.lPress) { ms.lPress = false; chanMuted[ch] = !chanMuted[ch]; }
            else if (ms.rPress) {
                ms.rPress = false;
                bool solo = !chanMuted[ch];
                for (int i = 0; i < NUM_CH; i++) if (i != ch && !chanMuted[i]) solo = false;
                for (int i = 0; i < NUM_CH; i++) chanMuted[i] = solo ? false : (i != ch);
            }
        }
    }

    int ry0 = PAT_Y + PAT_HDR_H;
    fill(0, ry0, W, H - ry0, P_BG);
    for (int i = 0; i < VIS_ROWS; i++) {
        int r = crow - CENTER_ROW + i;
        int y = ry0 + i * ROW_H;
        if (r < 0 || r >= NUM_ROWS) continue;
        uint32_t bg = (r % 4 == 0) ? P_BEAT : P_BG;
        if (r == playRow && !(i == CENTER_ROW)) bg = P_PLAYROW;
        if (i == CENTER_ROW) bg = recording() ? P_BAR_REC : (editMode ? P_BAR_EDIT : P_BAR);
        fill(0, y, W, ROW_H, bg);
        char rn[4];
        snprintf(rn, sizeof rn, "%02d", r);
        text(8, y + 1, rn, (r % 16 == 0) ? P_ROW16 : P_ROWNUM);
        for (int ch = 0; ch < NUM_CH; ch++) {
            const Cell *c = &song.pat[pat][r][ch];
            int x = ROWNUM_W + ch * CH_W + 6;
            if (i == CENTER_ROW && ch == curCh && screen == SCR_PATTERN)
                fill(x + colX[curCol] - 1, y, colW[curCol] + 2, ROW_H, P_CURSOR);
            bool cur = (i == CENTER_ROW && ch == curCh);
            uint32_t dim = (i == CENTER_ROW) ? 0x7884A8 : P_DIM;
            uint32_t ncol = c->note ? P_NOTE : dim;
            if (cur && curCol == 0) ncol = 0;
            text(x, y + 1, c->note ? note_name(c->note) : "---", ncol);
            uint32_t scol = c->sample ? P_SMP : dim;
            hexDigit(x + colX[1], y + 1, c->sample >> 4, (cur && curCol == 1) ? 0 : scol);
            hexDigit(x + colX[2], y + 1, c->sample & 15, (cur && curCol == 2) ? 0 : scol);
            uint32_t fcol = (c->cmd || c->param) ? P_FX : dim;
            hexDigit(x + colX[3], y + 1, c->cmd, (cur && curCol == 3) ? 0 : fcol);
            hexDigit(x + colX[4], y + 1, c->param >> 4, (cur && curCol == 4) ? 0 : fcol);
            hexDigit(x + colX[5], y + 1, c->param & 15, (cur && curCol == 5) ? 0 : fcol);
        }
    }
    for (int ch = 0; ch < NUM_CH; ch++) vline(ROWNUM_W + ch * CH_W, ry0, H - ry0, P_SEP);

    /* mouse: click to place cursor, wheel to scroll */
    if (inputOn && hit(0, ry0, W, H - ry0)) {
        if (ms.lPress) {
            ms.lPress = false;
            int i = (ms.y - ry0) / ROW_H;
            int r = crow - CENTER_ROW + i;
            if (ms.x >= ROWNUM_W) {
                int ch = clampi((ms.x - ROWNUM_W) / CH_W, 0, NUM_CH - 1);
                int lx = ms.x - (ROWNUM_W + ch * CH_W + 6);
                int col = 0;
                for (int k = 0; k < 6; k++) if (lx >= colX[k] - 2) col = k;
                curCh = ch;
                curCol = col;
            }
            if (!isPlaying() && r >= 0 && r < NUM_ROWS) curRow = r;
        }
        if (ms.wheel && !isPlaying()) curRow = clampi(curRow - ms.wheel * 2, 0, NUM_ROWS - 1);
    }
}

/* ------------------------------------------------------------------ */
/* sampler                                                             */

#define SW_X 0
#define SW_Y (PAT_Y + 12)
#define SW_W W
#define SW_H 236

static int32_t xToOff(int x)
{
    int32_t o = viewStart + (int32_t)((int64_t)(x - SW_X - 1) * viewLen / (SW_W - 2));
    return clampi(o, 0, cs()->length);
}

static int offToX(int32_t o)
{
    if (viewLen <= 0) return SW_X + 1;
    return SW_X + 1 + (int)((int64_t)(o - viewStart) * (SW_W - 2) / viewLen);
}

static void zoomAt(int32_t center, double f)
{
    Sample *s = cs();
    if (s->length <= 0) return;
    int32_t nl = (int32_t)(viewLen * f);
    if (nl < 32) nl = 32;
    if (nl > s->length) nl = s->length;
    int32_t ns = center - (int32_t)((int64_t)(center - viewStart) * nl / (viewLen > 0 ? viewLen : 1));
    viewStart = clampi(ns, 0, s->length - nl);
    viewLen = nl;
}

static void drawSampler(void)
{
    Sample *s = cs();
    if (samplerFor != curSample || viewStart + viewLen > s->length || viewLen <= 0) {
        samplerFor = curSample;
        viewStart = 0;
        viewLen = s->length;
        if (selEnd > s->length) selStart = selEnd = -1;
    }
    int y0 = PAT_Y;
    raised(0, y0, W, 12);
    char t[96];
    snprintf(t, sizeof t, "SAMPLER  -  SAMPLE %02X: ", curSample + 1);
    int x = text(4, y0 + 2, t, C_TEXT);
    textRaw(x, y0 + 2, s->name, 22, C_TEXT);
    if (button(W - 60, y0, 58, 12, "EXIT", false)) screen = SCR_PATTERN;

    /* waveform */
    fill(SW_X, SW_Y, SW_W, SW_H, 0x000000);
    frameRect(SW_X, SW_Y, SW_W, SW_H, 0x30343E);
    int mid = SW_Y + SW_H / 2;
    int ih = SW_H - 2;
    bool loop = s->loopLength > 2 && s->data;
    if (s->data && s->length > 0 && viewLen > 0) {
        int xs = haveSel() ? offToX(selStart) : -1, xe = haveSel() ? offToX(selEnd) : -1;
        if (haveSel()) fill(xs, SW_Y + 1, (xe > xs ? xe - xs : 1), ih, 0x1A2A60);
        if (loop) {
            int lx0 = offToX(s->loopStart), lx1 = offToX(s->loopStart + s->loopLength);
            for (int xx = lx0; xx < lx1; xx++) if (xx > SW_X && xx < SW_X + SW_W - 1) vline(xx, SW_Y + 1, 3, 0x2A5A2A);
        }
        hline(SW_X + 1, mid, SW_W - 2, 0x24283A);
        for (int px = 0; px < SW_W - 2; px++) {
            int32_t a = viewStart + (int32_t)((int64_t)px * viewLen / (SW_W - 2));
            int32_t b = viewStart + (int32_t)((int64_t)(px + 1) * viewLen / (SW_W - 2));
            if (b <= a) b = a + 1;
            if (a >= s->length) break;
            if (b > s->length) b = s->length;
            int mn = 127, mxv = -128;
            for (int32_t k = a; k < b; k++) { int v = s->data[k]; if (v < mn) mn = v; if (v > mxv) mxv = v; }
            int ya = mid - mxv * (ih / 2 - 2) / 128, yb = mid - mn * (ih / 2 - 2) / 128;
            fill(SW_X + 1 + px, ya, 1, yb - ya + 1, 0xFFD25C);
        }
        if (loop) {
            int lx0 = offToX(s->loopStart), lx1 = offToX(s->loopStart + s->loopLength);
            vline(lx0, SW_Y + 1, ih, 0x40E060);
            vline(lx1, SW_Y + 1, ih, 0x40E060);
            fill(lx0 - 3, SW_Y + 1, 7, 9, 0x40E060);
            fill(lx1 - 3, SW_Y + 1, 7, 9, 0x40E060);
        }
        /* play positions */
        if (snapOk)
            for (int ch = 0; ch < NUM_CH; ch++) {
                ScopeVoice *v = &snap.v[ch];
                if (v->active && v->smp == curSample) {
                    int px = offToX(v->start + v->pos);
                    if (px > SW_X && px < SW_X + SW_W - 1) vline(px, SW_Y + 1, ih, 0xFFFFFF);
                }
            }
    } else {
        textCenter(SW_X, SW_Y, SW_W, SW_H, "SAMPLE IS EMPTY - LOAD ONE WITH \"LOAD SMP\" OR DRAG A FILE HERE", 0x707890);
    }

    /* mouse in waveform */
    if (inputOn && s->data && s->length > 0 && hit(SW_X, SW_Y, SW_W, SW_H)) {
        if (ms.lPress) {
            ms.lPress = false;
            int32_t o = xToOff(ms.x);
            smpDrag = 1;
            if (loop && ms.y < SW_Y + 14) {
                int lx0 = offToX(s->loopStart), lx1 = offToX(s->loopStart + s->loopLength);
                if (abs(ms.x - lx0) <= 5) smpDrag = 2;
                else if (abs(ms.x - lx1) <= 5) smpDrag = 3;
            }
            if (smpDrag == 1) { selAnchor = o; selStart = selEnd = o; }
            activeId = -77;
        }
        if (ms.wheel) zoomAt(xToOff(ms.x), ms.wheel > 0 ? 0.5 : 2.0);
        if (ms.rPress) { ms.rPress = false; selStart = selEnd = -1; }
    }
    if (activeId == -77 && ms.lDown && smpDrag && s->data) {
        int32_t o = xToOff(ms.x);
        if (smpDrag == 1) {
            selStart = o < selAnchor ? o : selAnchor;
            selEnd = o < selAnchor ? selAnchor : o;
        } else {
            plat_lock();
            int32_t ls = s->loopStart, le = s->loopStart + s->loopLength;
            if (smpDrag == 2) ls = clampi(o & ~1, 0, le - 4);
            else le = clampi((o + 1) & ~1, ls + 4, s->length);
            s->loopStart = ls;
            s->loopLength = le - ls;
            sample_fix_loop(s);
            plat_unlock();
            setModified();
        }
    }
    if (!ms.lDown) smpDrag = 0;

    /* info line */
    int iy = SW_Y + SW_H + 1;
    fill(0, iy, W, RH, 0x1A1D28);
    if (haveSel())
        textf(4, iy + 2, 0xC8D0E0, "RANGE %05X-%05X (%d BYTES)", selStart, selEnd, selEnd - selStart);
    else
        text(4, iy + 2, "NO RANGE - DRAG TO MARK", 0x8088A0);
    if (loop) textf(400, iy + 2, 0x80E090, "LOOP %05X-%05X", s->loopStart, s->loopStart + s->loopLength);
    else text(400, iy + 2, "NO LOOP", 0x8088A0);
    textf(560, iy + 2, 0xC8D0E0, "VIEW %05X+%05X", viewStart, viewLen);

    /* buttons */
    int by = iy + RH + 2, bw = 90;
    static const char *b1[8] = { "PLAY WAVE", "PLAY RANGE", "STOP", "SHOW ALL", "SHOW RNG", "ZOOM IN", "ZOOM OUT", "SEL ALL" };
    static const char *b2[8] = { "CUT", "COPY", "PASTE", "CROP", "REVERSE", "NORMALIZE", "FADE IN", "FADE OUT" };
    static const char *b3[8] = { "LOOP", "CENTER", "VOL 50%", "VOL 200%", "UPSAMPLE", "DOWNSAMP.", "KILL SMP", "EXIT" };
    for (int k = 0; k < 8; k++) {
        if (button(k * bw, by, bw, RH + 2, b1[k], k == 0 && false)) {
            switch (k) {
                case 0: if (s->data) jam(octave * 12 + 1 + 12); break;
                case 1:
                    if (haveSel()) { plat_lock(); replayer_play_sample_range(curCh, curSample, selStart, selEnd, octave * 12 + 13); plat_unlock(); }
                    break;
                case 2: plat_lock(); replayer_stop_voices(); plat_unlock(); if (isPlaying()) stopPlay(); break;
                case 3: viewStart = 0; viewLen = s->length; break;
                case 4: if (haveSel() && selEnd - selStart >= 32) { viewStart = selStart; viewLen = selEnd - selStart; } break;
                case 5: zoomAt(viewStart + viewLen / 2, 0.5); break;
                case 6: zoomAt(viewStart + viewLen / 2, 2.0); break;
                case 7: if (s->length) { selStart = 0; selEnd = s->length; } break;
            }
        }
        if (button(k * bw, by + RH + 3, bw, RH + 2, b2[k], false)) {
            switch (k) {
                case 0: smpCut(true); break;
                case 1: smpCopy(); break;
                case 2: smpPaste(); break;
                case 3: smpCrop(); break;
                case 4: smpProcess(0); break;
                case 5: smpProcess(1); break;
                case 6: smpProcess(2); break;
                case 7: smpProcess(3); break;
            }
        }
        if (button(k * bw, by + 2 * (RH + 3), bw, RH + 2, b3[k], k == 0 && loop)) {
            switch (k) {
                case 0: smpToggleLoop(); break;
                case 1: smpProcess(4); break;
                case 2: smpProcess(5); break;
                case 3: smpProcess(6); break;
                case 4: smpResample(true); break;
                case 5: smpResample(false); break;
                case 6: {
                    char l[64];
                    snprintf(l, sizeof l, "KILL SAMPLE %02X?", curSample + 1);
                    ask(l, NULL, onKillSample, 2, "YES", "NO", NULL, NULL);
                } break;
                case 7: screen = SCR_PATTERN; break;
            }
        }
    }
    int ty = by + 3 * (RH + 3) + 2;
    fill(0, ty, W, H - ty, 0x1A1D28);
    text(4, ty + 2, "TASTI NOTA = SUONA IL CAMPIONE.  TRASCINA I MARCATORI VERDI PER IL LOOP.", 0x8088A0);
    text(4, ty + 14, "ROTELLINA = ZOOM,  TASTO DESTRO = TOGLIE LA SELEZIONE.", 0x8088A0);
}

/* ------------------------------------------------------------------ */
/* help / about                                                        */

static const char *helpLeft[] = {
    "\x01 TASTI PRINCIPALI",
    "SPAZIO        STOP / EDIT ON-OFF",
    "ALT DESTRO    SUONA CANZONE   (F6)",
    "CTRL DESTRO   SUONA PATTERN   (F7)",
    "SHIFT DESTRO  REGISTRA        (F8)",
    "SHIFT+F6/F7   SUONA DALLA RIGA",
    "F1 / F2       OTTAVA BASSA / ALTA",
    "Z S X D C ... NOTE OTTAVA BASSA",
    "Q 2 W 3 E ... NOTE OTTAVA ALTA",
    "0-9 A-F       CAMPIONE / EFFETTO",
    "FRECCE        MUOVI IL CURSORE",
    "TAB/SHIFT+TAB CANALE SUCC./PREC.",
    "PAG SU/GIU    16 RIGHE SU / GIU",
    "F9 F10 F11 F12 RIGA 0 / 16 / 32 / 48",
    "CANC          CANCELLA NOTA O EFF.",
    "SHIFT+CANC    CANCELLA TUTTA LA CELLA",
    "BACKSPACE     ELIMINA RIGA SOPRA",
    "INS           INSERISCI RIGA VUOTA",
    "  (+SHIFT = SU TUTTI I CANALI)",
    "SHIFT+F3/4/5  TAGLIA/COPIA/INC. TRACCIA",
    "ALT+F3/4/5    TAGLIA/COPIA/INC. PATTERN",
    "CTRL+F3/4/5   TAGLIA/COPIA/INC. EFFETTI",
    "CTRL+Z / Y    ANNULLA / RIPRISTINA",
    "CTRL+SX/DX    PATTERN -/+",
    "ALT+SX/DX     POSIZIONE -/+",
    "CTRL+SU/GIU   CAMPIONE -/+  (NUM +/-)",
    "CTRL+0..9     EDIT SKIP",
    "ALT+1..8      MUTE CANALE 1..8",
    "CTRL+O / S    APRI / SALVA MODULO",
    "CTRL+L        CARICA CAMPIONE",
    "ALT+INVIO     SCHERMO INTERO",
    "ESC           CHIUDE PANNELLI",
    "POS ED.       ELENCO POSIZIONI",
    NULL
};
static const char *helpRight[] = {
    "\x01 EFFETTI (COME PROTRACKER)",
    "0XY ARPEGGIO      8XX PANNING",
    "1XX PORTA SU      9XX OFFSET CAMP.",
    "2XX PORTA GIU'    AXY VOLUME SLIDE",
    "3XX PORTA A NOTA  BXX SALTA A POS.",
    "4XY VIBRATO       CXX VOLUME (0-40)",
    "5XY 3 + VOL SLIDE DXX FINE PATTERN",
    "6XY 4 + VOL SLIDE EXY COMANDI ESTESI",
    "7XY TREMOLO       FXX VELOC./BPM",
    "",
    "E0X FILTRO LED    E1X/E2X FINE PORTA",
    "E3X GLISSANDO     E4X FORMA VIBRATO",
    "E5X FINETUNE      E6X LOOP PATTERN",
    "E7X FORMA TREMOLO E8X PANNING",
    "E9X RIPETI NOTA   EAX/EBX FINE VOLUME",
    "ECX TAGLIA NOTA   EDX RITARDA NOTA",
    "EEX RITARDA PATT. EFX INVERTI LOOP",
    "",
    "\x01 MOUSE",
    "CLIC SULL'OSCILLOSCOPIO O SULLA",
    "TESTATA DEL CANALE = MUTE,",
    "TASTO DESTRO = SOLO.",
    "TASTO DESTRO SULLE FRECCE = VELOCE.",
    "TRASCINA UN FILE .MOD O UN CAMPIONE",
    "(WAV/IFF/AIFF/RAW) NELLA FINESTRA.",
    "",
    "\x01 SALVATAGGIO",
    "SE I CANALI 5-8 SONO VUOTI IL MODULO",
    "VIENE SALVATO COME M.K. (4 CANALI,",
    "COMPATIBILE CON PROTRACKER), ALTRI-",
    "MENTI COME 8CHN (FASTTRACKER/OPENMPT).",
    NULL
};

static void drawHelp(void)
{
    int y0 = PAT_Y;
    fill(0, y0, W, H - y0, 0x10121A);
    frameRect(0, y0, W, H - y0, 0x3A4060);
    for (int i = 0; helpLeft[i]; i++) {
        const char *s = helpLeft[i];
        bool title = s[0] == '\x01';
        text(8, y0 + 6 + i * 10, title ? s + 1 : s, title ? P_ROW16 : 0xC8D0E0);
    }
    for (int i = 0; helpRight[i]; i++) {
        const char *s = helpRight[i];
        bool title = s[0] == '\x01';
        text(372, y0 + 6 + i * 10, title ? s + 1 : s, title ? P_ROW16 : 0xC8D0E0);
    }
    if (button(W - 68, y0 + 2, 64, RH, "CLOSE", false)) showHelp = false;
    if (button(W - 136, y0 + 2, 64, RH, "ABOUT", false)) { showHelp = false; showAbout = true; }
}

static void drawAbout(void)
{
    int y0 = PAT_Y;
    fill(0, y0, W, H - y0, 0x10121A);
    frameRect(0, y0, W, H - y0, 0x3A4060);
    int y = y0 + 40;
    textCenter(0, y, W, 10, "P T 8", P_ROW16); y += 20;
    textCenter(0, y, W, 10, "TRACKER IN STILE PROTRACKER 2.3D - 8 CANALI", 0xE0E4F0); y += 14;
    textCenter(0, y, W, 10, "VERSIONE " APP_VERSION, 0xA0A8C0); y += 30;
    textCenter(0, y, W, 10, "REPLAYER BASATO SULLA LOGICA DI PROTRACKER 2.3D", 0xA0A8C0); y += 12;
    textCenter(0, y, W, 10, "(COMPRESE LE SUE STRANEZZE), ESTESO A 8 CANALI.", 0xA0A8C0); y += 12;
    textCenter(0, y, W, 10, "CARICA MOD M.K./FLT4/FLT8/8CHN/6CHN/OCTA/CD81 E 15 CAMPIONI.", 0xA0A8C0); y += 30;
    textCenter(0, y, W, 10, "OMAGGIO AGLI AUTORI DI PROTRACKER E A TUTTA LA SCENA AMIGA.", 0x8088A0); y += 12;
    textCenter(0, y, W, 10, "FONT 8X8 DI DANIEL HEPPER (PUBBLICO DOMINIO).", 0x8088A0);
    if (button(W - 68, y0 + 2, 64, RH, "CLOSE", false)) showAbout = false;
}

static void drawDialog(void)
{
    int w = 380, h = 74;
    int x = (W - w) / 2, y = (H - h) / 2;
    fill(x + 4, y + 4, w, h, 0x000000);
    raised(x, y, w, h);
    raised(x + 3, y + 3, w - 6, h - 6);
    textCenter(x, y + 10, w, 10, dlg.line1, C_TEXT);
    textCenter(x, y + 24, w, 10, dlg.line2, C_TEXT);
    int bw = 80, gap = 8;
    int total = dlg.nbtn * bw + (dlg.nbtn - 1) * gap;
    int bx = x + (w - total) / 2;
    for (int i = 0; i < dlg.nbtn; i++) {
        if (button(bx + i * (bw + gap), y + h - 24, bw, 14, dlg.btn[i], false)) { dlgChoose(i); break; }
    }
}

/* ------------------------------------------------------------------ */
/* frame                                                               */

void app_frame(uint32_t *fb)
{
    FB = fb;
    now = plat_ms();
    plat_lock();
    liveMode = replayer_mode();
    plat_unlock();
    snapOk = replayer_snapshot(plat_audio_played_frames(), &snap);
    if (isPlaying() && snapOk && snap.mode != PLAY_STOPPED && cfg.followPlay) {
        if (snap.mode == PLAY_SONG) curOrder = clampi(snap.order, 0, song.numOrders - 1);
        curRow = snap.row;
    }
    if (curOrder >= song.numOrders) curOrder = song.numOrders - 1;

    /* end text editing when clicking elsewhere */
    if (teBuf && ms.lPress && !(ms.x >= teX && ms.x < teX + teW && ms.y >= teY && ms.y < teY + RH))
        teBuf = NULL;

    inputOn = !dlg.active;
    fill(0, 0, W, H, C_FACE);
    drawLeftPanel();
    drawButtons();
    if (overlay == OV_EDITOP) drawEditOp();
    else if (overlay == OV_SETUP) drawSetup();
    else if (overlay == OV_POSED) drawPosEd();
    else drawScopes();
    drawInfo();
    if (showHelp) drawHelp();
    else if (showAbout) drawAbout();
    else if (screen == SCR_SAMPLER) drawSampler();
    else drawPattern();

    if (dlg.active) {
        inputOn = true;
        drawDialog();
    }

    int titleState = (modified ? 1 : 0) + (int)(strlen(songPath) * 2);
    if (titleState != lastTitleState) { lastTitleState = titleState; updateTitle(); }

    ms.lPress = ms.rPress = false;
    ms.wheel = 0;
    if (!ms.lDown && !ms.rDown) activeId = 0;
}

/* ------------------------------------------------------------------ */
/* input                                                               */

void app_mouse_move(int x, int y) { ms.x = x; ms.y = y; }

void app_mouse_button(int b, bool down)
{
    if (b == 0) { if (down && !ms.lDown) ms.lPress = true; ms.lDown = down; }
    else { if (down && !ms.rDown) ms.rPress = true; ms.rDown = down; }
}

void app_mouse_wheel(int d) { ms.wheel += d; }

void app_char(unsigned ch)
{
    if (!teBuf || ch < 32 || ch == 127) return;
    if (ch > 255) ch = '?';
    int len = (int)strlen(teBuf);
    if (len >= teMax) return;
    memmove(teBuf + teCur + 1, teBuf + teCur, (size_t)(len - teCur + 1));
    teBuf[teCur++] = (char)ch;
    setModified();
}

/* scancode -> semitone offset from the low octave's C (-1 = not a note key) */
static int noteKey(int sc)
{
    switch (sc) {
        case 0x2C: return 0;  case 0x1F: return 1;  case 0x2D: return 2;  case 0x20: return 3;
        case 0x2E: return 4;  case 0x2F: return 5;  case 0x22: return 6;  case 0x30: return 7;
        case 0x23: return 8;  case 0x31: return 9;  case 0x24: return 10; case 0x32: return 11;
        case 0x33: return 12; case 0x26: return 13; case 0x34: return 14; case 0x27: return 15;
        case 0x35: return 16;
        case 0x10: return 12; case 0x03: return 13; case 0x11: return 14; case 0x04: return 15;
        case 0x12: return 16; case 0x13: return 17; case 0x06: return 18; case 0x14: return 19;
        case 0x07: return 20; case 0x15: return 21; case 0x08: return 22; case 0x16: return 23;
        case 0x17: return 24; case 0x0A: return 25; case 0x18: return 26; case 0x0B: return 27;
        case 0x19: return 28; case 0x1A: return 29; case 0x0D: return 30; case 0x1B: return 31;
    }
    return -1;
}

static int hexKey(int vk)
{
    if (vk >= '0' && vk <= '9') return vk - '0';
    if (vk >= 'A' && vk <= 'F') return vk - 'A' + 10;
    if (vk >= K_NUMPAD0 && vk <= K_NUMPAD9) return vk - K_NUMPAD0;
    return -1;
}

static void textEditKey(int vk)
{
    int len = (int)strlen(teBuf);
    switch (vk) {
        case K_LEFT: if (teCur > 0) teCur--; break;
        case K_RIGHT: if (teCur < len) teCur++; break;
        case K_HOME: teCur = 0; break;
        case K_END: teCur = len; break;
        case K_BACK:
            if (teCur > 0) { memmove(teBuf + teCur - 1, teBuf + teCur, (size_t)(len - teCur + 1)); teCur--; setModified(); }
            break;
        case K_DELETE:
            if (teCur < len) { memmove(teBuf + teCur, teBuf + teCur + 1, (size_t)(len - teCur)); setModified(); }
            break;
        case K_RETURN: case K_ESCAPE: case K_TAB:
            teBuf = NULL;
            break;
    }
}

void app_key_down(int vk, int sc, bool ext, bool repeat, int mods)
{
    bool shift = mods & KMOD_SHIFT, ctrl = mods & KMOD_CTRL, alt = mods & KMOD_ALT;

    if (dlg.active) {
        if (vk == K_RETURN) dlgChoose(0);
        else if (vk == K_ESCAPE) dlgChoose(dlg.nbtn - 1);
        else if (vk == 'Y' && dlg.nbtn == 2) dlgChoose(0);
        else if (vk == 'N' && dlg.nbtn == 2) dlgChoose(1);
        return;
    }
    if (teBuf) { textEditKey(vk); return; }

    /* transport keys (ProTracker style) */
    if (vk == K_MENU && ext) { if (!repeat) startPlay(PLAY_SONG, false); return; }        /* right Alt */
    if (vk == K_CONTROL && ext) { if (!repeat) startPlay(PLAY_PATTERN, false); return; }  /* right Ctrl */
    if (vk == K_SHIFT && sc == 0x36) {                                                     /* right Shift */
        if (!repeat) { editMode = true; startPlay(PLAY_PATTERN, false); }
        return;
    }
    if (vk == K_SHIFT || vk == K_CONTROL || vk == K_MENU) return;

    switch (vk) {
        case K_ESCAPE:
            if (showHelp) showHelp = false;
            else if (showAbout) showAbout = false;
            else if (overlay) { if (overlay == OV_SETUP) cfgSave(); overlay = OV_NONE; }
            else if (screen == SCR_SAMPLER) screen = SCR_PATTERN;
            return;
        case K_SPACE:
            if (repeat) return;
            if (isPlaying()) stopPlay();
            else editMode = !editMode;
            return;
        case K_F1: octave = 1; return;
        case K_F2: octave = 2; return;
        case K_F3: case K_F4: case K_F5: {
            int op = vk - K_F3;
            if (shift) trackOp(op);
            else if (alt) patternOp(op);
            else if (ctrl) cmdOp(op);
            else if (vk == K_F5 && !repeat) startPlay(PLAY_SONG, false);
            return;
        }
        case K_F6: if (!repeat) startPlay(PLAY_SONG, shift); return;
        case K_F7: if (!repeat) startPlay(PLAY_PATTERN, shift); return;
        case K_F8:
            if (repeat) return;
            if (isPlaying()) stopPlay();
            else { editMode = true; startPlay(PLAY_PATTERN, false); }
            return;
        case K_F9: case K_F10: case K_F11: case K_F12:
            if (!isPlaying()) curRow = (vk - K_F9) * 16;
            return;
    }

    if (ctrl && !alt) {
        switch (vk) {
            case 'Z': undoStep(false); return;
            case 'Y': undoStep(true); return;
            case 'O': openModuleDialog(); return;
            case 'S': saveModule(shift); return;
            case 'L': openSampleDialog(); return;
            case 'E': exportWavDialog(); return;
            case 'Q': if (app_request_quit()) plat_quit(); return;
            case K_LEFT: case K_RIGHT:
                song.orders[curOrder] = (uint8_t)clampi(song.orders[curOrder] + (vk == K_RIGHT ? 1 : -1), 0, MAX_PATTERNS - 1);
                setModified();
                return;
            case K_UP: case K_DOWN:
                curSample = clampi(curSample + (vk == K_UP ? 1 : -1), 0, NUM_SAMPLES - 1);
                selStart = selEnd = -1;
                return;
        }
        if (vk >= '0' && vk <= '9') { cfg.editSkip = vk - '0'; return; }
        return;
    }
    if (alt && !ctrl) {
        if (vk >= '1' && vk <= '8') { chanMuted[vk - '1'] = !chanMuted[vk - '1']; return; }
        if (vk == K_LEFT || vk == K_RIGHT) {
            if (!isPlaying()) curOrder = clampi(curOrder + (vk == K_RIGHT ? 1 : -1), 0, song.numOrders - 1);
            return;
        }
        if (vk == K_RETURN) return;   /* handled by the platform (fullscreen) */
        if (vk == K_DELETE) {
            if (editMode) { int p = viewPattern(); undoBegin(); undoSave(p); song.pat[p][editRow()][curCh].cmd = 0; song.pat[p][editRow()][curCh].param = 0; advanceRow(cfg.editSkip); }
            return;
        }
        return;
    }

    switch (vk) {
        case K_UP: if (!isPlaying()) curRow = (curRow - 1) & 63; return;
        case K_DOWN: if (!isPlaying()) curRow = (curRow + 1) & 63; return;
        case K_PRIOR: if (!isPlaying()) curRow = clampi(curRow - 16, 0, 63); return;
        case K_NEXT: if (!isPlaying()) curRow = clampi(curRow + 16, 0, 63); return;
        case K_HOME: if (!isPlaying()) curRow = 0; return;
        case K_END: if (!isPlaying()) curRow = 63; return;
        case K_LEFT:
            if (--curCol < 0) { curCol = 5; curCh = (curCh + NUM_CH - 1) % NUM_CH; }
            return;
        case K_RIGHT:
            if (++curCol > 5) { curCol = 0; curCh = (curCh + 1) % NUM_CH; }
            return;
        case K_TAB:
            if (shift) { if (curCol == 0) curCh = (curCh + NUM_CH - 1) % NUM_CH; }
            else curCh = (curCh + 1) % NUM_CH;
            curCol = 0;
            return;
        case K_ADD: curSample = clampi(curSample + 1, 0, NUM_SAMPLES - 1); selStart = selEnd = -1; return;
        case K_SUBTRACT: curSample = clampi(curSample - 1, 0, NUM_SAMPLES - 1); selStart = selEnd = -1; return;
        case K_DELETE:
            if (screen == SCR_SAMPLER) { smpCut(false); return; }
            deleteCell(shift);
            return;
        case K_BACK: deleteRowAbove(shift); return;
        case K_INSERT: insertRow(shift); return;
        case K_RETURN: return;
    }

    if (repeat) return;

    /* note entry / hex entry */
    if (screen == SCR_PATTERN && curCol > 0 && editMode) {
        int h = hexKey(vk);
        if (h >= 0) { putHex(h); return; }
        return;
    }
    int nk = noteKey(sc);
    if (nk >= 0) {
        int note = octave * 12 + 1 + nk;
        if (note > NOTE_PT_MAX) return;
        putNote(note);
    }
}

void app_drop_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *b = baseName(path);
    bool isMod = (dot && (!strcmp(dot, ".mod") || !strcmp(dot, ".MOD") || !strcmp(dot, ".Mod"))) ||
                 !strncmp(b, "mod.", 4) || !strncmp(b, "MOD.", 4);
    if (isMod) {
        if (modified) {
            snprintf(pendingPath, sizeof pendingPath, "%s", path);
            ask("SONG HAS UNSAVED CHANGES.", "LOAD MODULE ANYWAY?", onDiscardLoad, 2, "YES", "NO", NULL, NULL);
        } else loadModuleNow(path);
    } else {
        loadSampleNow(path);
    }
}

void app_open_file(const char *path) { app_drop_file(path); }

bool app_request_quit(void)
{
    if (!modified) { cfgSave(); return true; }
    cfgSave();
    ask("THE SONG HAS UNSAVED CHANGES.", "REALLY QUIT PT8?", onQuit, 2, "QUIT", "CANCEL", NULL, NULL);
    return false;
}

void app_init(void)
{
    song_init();
    replayer_init(48000);
    undoBuf = (UndoEnt *)malloc(sizeof(UndoEnt) * UNDO_MAX);
    redoBuf = (UndoEnt *)malloc(sizeof(UndoEnt) * UNDO_MAX);
    cfgLoad();
    now = plat_ms();
    setStatus("ALL RIGHT", false);
}

void app_shutdown(void)
{
    cfgSave();
}
