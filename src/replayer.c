/*
 * PT8 - replayer and mixer.
 *
 * The replay routine follows the logic of ProTracker 2.3D (including its
 * well-known quirks), extended to 8 channels. Sound output emulates Paula:
 * each voice has a current DMA block and latched "next block" registers,
 * which is what makes sample-swapping and loops behave like on an Amiga.
 */
#include "tracker.h"
#include "platform.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

Settings cfg = {
    .stereoSep = 30, .amp = 6, .interpolation = false, .a500Filter = false,
    .ledEmulation = true, .panning = true, .saveFormat = 0, .editSkip = 1, .followPlay = true,
    .audioBuffer = 512
};
bool chanMuted[NUM_CH];

typedef struct {
    int     smp;               /* sample of current DMA block, -1 = none */
    int32_t start, len;        /* bytes */
    int     regSmp;            /* latched registers (AUDxLC / AUDxLEN) */
    int32_t regStart, regLen;
    double  pos, delta;
    int     period, volume;
    bool    active;
} Voice;

typedef struct {
    uint16_t n_note;           /* period of note on this row, 0 = none */
    uint8_t  n_cmd, n_param;
    bool     n_rowEmpty;
    uint8_t  n_finetune;
    int      n_volume;
    int      n_smp;
    int32_t  n_start, n_length, n_loopstart, n_replen, n_wavestart;  /* start offsets in bytes, lengths in words */
    int      n_period, n_wantedperiod;
    uint8_t  n_toneportdirec, n_toneportspeed;
    uint8_t  n_vibratocmd, n_tremolocmd, n_wavecontrol, n_glissfunk;
    uint8_t  n_sampleoffset, n_pattpos, n_loopcount, n_funkoffset;
    int8_t   n_vibratopos, n_tremolopos;
    int      pan;              /* 0..255 */
} Chan;

static Voice voice[NUM_CH];
static Chan  chn[NUM_CH];

static int      outRate = 48000;
static int      mode = PLAY_STOPPED;
static int      speed = 6, bpm = 125, defaultBpm = 125;
static int      tick, row, order, pattern;
static int      playRow, playOrder, playPattern;   /* row currently sounding */
static int      pattDelTime, pattDelTime2, pBreakPos;
static bool     pBreakFlag, posJumpAssert, pendingStop;
static uint8_t  lowMask = 0xFF;
static bool     ledOn;
static bool     panOn;   /* 8xx/E8x panning active for this playback */
static double   samplesPerTick, tickRemain;
static uint64_t framesRendered;
static bool     exporting, songEnded;
static uint8_t  visited[MAX_ORDERS][NUM_ROWS];

/* output filters */
static double a500Coef, hpCoef;
static double lpL, lpR, hpInL, hpInR, hpOutL, hpOutR;
static double ledB0, ledB1, ledB2, ledA1, ledA2;
static double ledZ[2][2];

/* sync ring for UI (scopes, play position) */
typedef struct {
    uint64_t frame;
    int      mode, order, pattern, row, speed, bpm;
    bool     led;
    Voice    v[NUM_CH];
} Snap;
#define SNAP_COUNT 512
static Snap ring[SNAP_COUNT];
static int  ringHead, ringCount;

static const uint8_t vibTab[32] = {
    0, 24, 49, 74, 97,120,141,161,180,197,212,224,235,244,250,253,
  255,253,250,244,235,224,212,197,180,161,141,120, 97, 74, 49, 24
};
static const uint8_t funkTab[16] = { 0,5,6,7,8,10,11,13,16,19,22,26,32,43,64,128 };

static int perMin(void) { return song.extendedNotes ? 28 : 113; }
static int perMax(void) { return song.extendedNotes ? 3424 : 856; }

/* ---------------- Paula ---------------- */

static void paulaSetPeriod(int ch, int period)
{
    Voice *v = &voice[ch];
    v->period = period;
    if (period <= 0) { v->delta = 0; return; }   /* period 0: Paula output effectively silent */
    if (period < 20) period = 20;
    v->delta = (PAULA_PAL_CLK / period) / outRate;
}

static void paulaSetVolume(int ch, int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 64) vol = 64;
    voice[ch].volume = vol;
}

static void paulaSetData(int ch, int smp, int32_t start) { voice[ch].regSmp = smp; voice[ch].regStart = start; }
static void paulaSetLength(int ch, int32_t words) { voice[ch].regLen = words * 2; }

static void paulaStartDMA(int ch)
{
    Voice *v = &voice[ch];
    v->smp = v->regSmp;
    v->start = v->regStart;
    v->len = v->regLen;
    v->pos = 0;
    v->active = (v->smp >= 0 && v->len > 0);
}

/* ---------------- replayer helpers ---------------- */

static void calcTickLen(void)
{
    if (bpm < 32) bpm = 32;
    if (bpm > 255) bpm = 255;
    samplesPerTick = outRate * 2.5 / bpm;
}

static void resetChannels(void)
{
    for (int i = 0; i < NUM_CH; i++) {
        Chan *c = &chn[i];
        memset(c, 0, sizeof(*c));
        c->n_smp = -1;
        c->n_replen = 1;
        c->n_rowEmpty = true;
        int k = i & 3;
        c->pan = (k == 0 || k == 3) ? 0 : 255;
    }
}

static void stopVoices(void)
{
    for (int i = 0; i < NUM_CH; i++) {
        memset(&voice[i], 0, sizeof(Voice));
        voice[i].smp = voice[i].regSmp = -1;
    }
}

static void loadSample(int ch, int si)
{
    Chan *x = &chn[ch];
    Sample *s = &song.smp[si];
    x->n_smp = si;
    x->n_start = 0;
    x->n_length = (s->data ? s->length : 0) / 2;
    x->n_finetune = s->finetune & 0x0F;
    x->n_volume = s->volume;
    int32_t repeat = s->loopStart / 2;
    int32_t replen = s->loopLength / 2;
    bool looped = s->loopLength > 2 && s->data;
    if (!looped) replen = 1;
    if (looped && repeat > 0) {
        x->n_loopstart = repeat * 2;
        x->n_wavestart = x->n_loopstart;
        x->n_length = repeat + replen;
    } else {
        x->n_loopstart = 0;
        x->n_wavestart = 0;
    }
    x->n_replen = replen;
    paulaSetVolume(ch, x->n_volume);
}

static void nextPosition(void)
{
    row = pBreakPos;
    pBreakPos = 0;
    posJumpAssert = false;
    if (mode == PLAY_SONG) {
        order = (order + 1) & 0x7F;
        if (order >= song.numOrders) {
            order = 0;
            if (exporting) songEnded = true;
        }
        pattern = song.orders[order];
    }
    if (row >= NUM_ROWS) row = 0;
}

static void checkMoreEffects(int ch);

static void setPeriod(int ch)
{
    Chan *x = &chn[ch];
    int i;
    if (!song.extendedNotes) {
        /* exact PT lookup: 37 entries, the last one is 0 (notes above B-3 become silent) */
        for (i = 0; i < 36; i++)
            if (x->n_note >= ptPeriodTable[i]) break;
        x->n_period = ptPeriodTable[x->n_finetune * 37 + i];
    } else {
        for (i = 0; i < NUM_NOTES; i++)
            if (x->n_note >= periodTable[0][i]) break;
        if (i >= NUM_NOTES) i = NUM_NOTES - 1;
        x->n_period = periodTable[x->n_finetune][i];
    }

    if (!(x->n_cmd == 0xE && (x->n_param >> 4) == 0xD)) {   /* not note delay */
        if (!(x->n_wavecontrol & 0x04)) x->n_vibratopos = 0;
        if (!(x->n_wavecontrol & 0x40)) x->n_tremolopos = 0;
        paulaSetData(ch, x->n_smp, x->n_start);
        paulaSetLength(ch, x->n_length);
        paulaSetPeriod(ch, x->n_period);
        paulaStartDMA(ch);
    }
    checkMoreEffects(ch);
}

static void setTonePorta(int ch)
{
    Chan *x = &chn[ch];
    const int16_t *t;
    int i = 0, cnt;
    if (!song.extendedNotes) { t = &ptPeriodTable[x->n_finetune * 37]; cnt = 37; }
    else { t = periodTable[x->n_finetune]; cnt = NUM_NOTES; }
    for (;;) {
        if (x->n_note >= t[i]) break;
        if (++i >= cnt) { i = cnt == 37 ? 35 : cnt - 1; break; }
    }
    if ((x->n_finetune & 8) && i > 0) i--;
    x->n_wantedperiod = t[i];
    x->n_toneportdirec = 0;
    if (x->n_period == x->n_wantedperiod) x->n_wantedperiod = 0;
    else if (x->n_period > x->n_wantedperiod) x->n_toneportdirec = 1;
}

static void tonePortNoChange(int ch)
{
    Chan *x = &chn[ch];
    if (!x->n_wantedperiod) return;
    if (x->n_toneportdirec) {
        x->n_period -= x->n_toneportspeed;
        if (x->n_period <= x->n_wantedperiod) { x->n_period = x->n_wantedperiod; x->n_wantedperiod = 0; }
    } else {
        x->n_period += x->n_toneportspeed;
        if (x->n_period >= x->n_wantedperiod) { x->n_period = x->n_wantedperiod; x->n_wantedperiod = 0; }
    }
    int per = x->n_period;
    if (x->n_glissfunk & 0x0F) {
        const int16_t *t;
        int i, cnt;
        if (!song.extendedNotes) { t = &ptPeriodTable[x->n_finetune * 37]; cnt = 36; }
        else { t = periodTable[x->n_finetune]; cnt = NUM_NOTES; }
        for (i = 0; i < cnt; i++) if (per >= t[i]) break;
        if (i >= cnt) i = cnt - 1;
        per = t[i];
    }
    paulaSetPeriod(ch, per);
}

static void tonePortamento(int ch)
{
    Chan *x = &chn[ch];
    if (x->n_param) { x->n_toneportspeed = x->n_param; x->n_param = 0; }
    tonePortNoChange(ch);
}

static void portaUp(int ch)
{
    Chan *x = &chn[ch];
    x->n_period -= x->n_param & lowMask;
    lowMask = 0xFF;
    if (x->n_period < perMin()) x->n_period = perMin();
    paulaSetPeriod(ch, x->n_period);
}

static void portaDown(int ch)
{
    Chan *x = &chn[ch];
    x->n_period += x->n_param & lowMask;
    lowMask = 0xFF;
    if (x->n_period > perMax()) x->n_period = perMax();
    paulaSetPeriod(ch, x->n_period);
}

static void arpeggio(int ch)
{
    Chan *x = &chn[ch];
    int m = tick % 3;
    if (m == 0) { paulaSetPeriod(ch, x->n_period); return; }
    int add = (m == 1) ? (x->n_param >> 4) : (x->n_param & 0x0F);
    int i;
    if (!song.extendedNotes) {
        /* PT reads past the end of the finetune row (into the 0 terminator and the next row) */
        const int16_t *t = &ptPeriodTable[x->n_finetune * 37];
        for (i = 0; i < 37; i++) {
            if (x->n_period >= t[i]) { paulaSetPeriod(ch, t[i + add]); return; }
        }
        return;
    }
    const int16_t *t = periodTable[x->n_finetune];
    for (i = 0; i < NUM_NOTES; i++) if (x->n_period >= t[i]) break;
    if (i >= NUM_NOTES) return;
    int j = i + add;
    if (j >= NUM_NOTES) j = NUM_NOTES - 1;
    paulaSetPeriod(ch, t[j]);
}

static void vibrato2(int ch)
{
    Chan *x = &chn[ch];
    int pos = ((uint8_t)x->n_vibratopos >> 2) & 0x1F;
    int d;
    switch (x->n_wavecontrol & 3) {
        case 0: d = vibTab[pos]; break;
        case 1: { int p8 = (pos << 3) & 0xFF; d = (x->n_vibratopos < 0) ? 255 - p8 : p8; } break;
        default: d = 255; break;
    }
    d = (d * (x->n_vibratocmd & 15)) >> 7;
    paulaSetPeriod(ch, x->n_period + ((x->n_vibratopos < 0) ? -d : d));
    x->n_vibratopos = (int8_t)(x->n_vibratopos + ((x->n_vibratocmd >> 2) & 0x3C));
}

static void vibrato(int ch)
{
    Chan *x = &chn[ch];
    if (x->n_param) {
        if (x->n_param & 0x0F) x->n_vibratocmd = (x->n_vibratocmd & 0xF0) | (x->n_param & 0x0F);
        if (x->n_param & 0xF0) x->n_vibratocmd = (x->n_param & 0xF0) | (x->n_vibratocmd & 0x0F);
    }
    vibrato2(ch);
}

static void tremolo(int ch)
{
    Chan *x = &chn[ch];
    if (x->n_param) {
        if (x->n_param & 0x0F) x->n_tremolocmd = (x->n_tremolocmd & 0xF0) | (x->n_param & 0x0F);
        if (x->n_param & 0xF0) x->n_tremolocmd = (x->n_param & 0xF0) | (x->n_tremolocmd & 0x0F);
    }
    int pos = ((uint8_t)x->n_tremolopos >> 2) & 0x1F;
    int d;
    switch ((x->n_wavecontrol >> 4) & 3) {
        case 0: d = vibTab[pos]; break;
        case 1: { int p8 = (pos << 3) & 0xFF; d = (x->n_vibratopos < 0) ? 255 - p8 : p8; } break; /* PT bug: uses vibrato pos */
        default: d = 255; break;
    }
    d = (d * (x->n_tremolocmd & 15)) >> 6;
    int vol = x->n_volume + ((x->n_tremolopos < 0) ? -d : d);
    paulaSetVolume(ch, vol);
    x->n_tremolopos = (int8_t)(x->n_tremolopos + ((x->n_tremolocmd >> 2) & 0x3C));
}

static void volumeSlide(int ch)
{
    Chan *x = &chn[ch];
    int up = x->n_param >> 4;
    if (up) { x->n_volume += up; if (x->n_volume > 64) x->n_volume = 64; }
    else { x->n_volume -= x->n_param & 0x0F; if (x->n_volume < 0) x->n_volume = 0; }
    paulaSetVolume(ch, x->n_volume);
}

static void updateFunk(int ch)
{
    Chan *x = &chn[ch];
    int sp = x->n_glissfunk >> 4;
    if (!sp) return;
    x->n_funkoffset = (uint8_t)(x->n_funkoffset + funkTab[sp]);
    if (!(x->n_funkoffset & 128)) return;
    x->n_funkoffset = 0;
    if (x->n_smp < 0) return;
    Sample *s = &song.smp[x->n_smp];
    if (!s->data) return;
    int32_t end = x->n_loopstart + x->n_replen * 2;
    int32_t w = x->n_wavestart + 1;
    if (w >= end) w = x->n_loopstart;
    x->n_wavestart = w;
    if (w >= 0 && w < s->length) s->data[w] = (int8_t)(-1 - s->data[w]);
}

static void doRetrig(int ch)
{
    Chan *x = &chn[ch];
    paulaSetData(ch, x->n_smp, x->n_start);
    paulaSetLength(ch, x->n_length);
    paulaStartDMA(ch);
    paulaSetData(ch, x->n_smp, x->n_loopstart);
    paulaSetLength(ch, x->n_replen);
}

static void eCommands(int ch)
{
    Chan *x = &chn[ch];
    int p = x->n_param & 0x0F;
    switch (x->n_param >> 4) {
        case 0x0: ledOn = !(p & 1); break;
        case 0x1: if (tick == 0) { lowMask = 0x0F; portaUp(ch); } break;
        case 0x2: if (tick == 0) { lowMask = 0x0F; portaDown(ch); } break;
        case 0x3: x->n_glissfunk = (x->n_glissfunk & 0xF0) | p; break;
        case 0x4: x->n_wavecontrol = (x->n_wavecontrol & 0xF0) | p; break;
        case 0x5: x->n_finetune = (uint8_t)p; break;
        case 0x6:
            if (tick) break;
            if (!p) { x->n_pattpos = (uint8_t)row; break; }
            if (!x->n_loopcount) x->n_loopcount = (uint8_t)p;
            else if (--x->n_loopcount == 0) break;
            pBreakPos = x->n_pattpos;
            pBreakFlag = true;
            break;
        case 0x7: x->n_wavecontrol = (uint8_t)((p << 4) | (x->n_wavecontrol & 0x0F)); break;
        case 0x8: if (panOn && tick == 0) x->pan = p * 17; break;
        case 0x9:
            if (!p) break;
            if (tick == 0 && x->n_note) break;
            if (tick % p == 0) doRetrig(ch);
            break;
        case 0xA: if (tick == 0) { x->n_volume += p; if (x->n_volume > 64) x->n_volume = 64; paulaSetVolume(ch, x->n_volume); } break;
        case 0xB: if (tick == 0) { x->n_volume -= p; if (x->n_volume < 0) x->n_volume = 0; paulaSetVolume(ch, x->n_volume); } break;
        case 0xC: if (tick == p) { x->n_volume = 0; paulaSetVolume(ch, 0); } break;
        case 0xD:
            if (tick == p && x->n_note) { paulaSetPeriod(ch, x->n_period); doRetrig(ch); }
            break;
        case 0xE: if (tick == 0 && pattDelTime2 == 0) pattDelTime = p + 1; break;
        case 0xF:
            if (tick) break;
            x->n_glissfunk = (uint8_t)((p << 4) | (x->n_glissfunk & 0x0F));
            if (p) updateFunk(ch);
            break;
    }
}

static void checkMoreEffects(int ch)
{
    Chan *x = &chn[ch];
    switch (x->n_cmd) {
        case 0x9: {
            if (x->n_param) x->n_sampleoffset = x->n_param;
            int32_t off = (int32_t)x->n_sampleoffset << 7;
            if ((int16_t)off < (int16_t)x->n_length) { x->n_length -= off; x->n_start += off << 1; }
            else x->n_length = 1;
        } break;
        case 0xB:
            order = (x->n_param - 1) & 0xFF;
            pBreakPos = 0;
            posJumpAssert = true;
            break;
        case 0xD: {
            int r = (x->n_param >> 4) * 10 + (x->n_param & 0x0F);
            pBreakPos = (r > 63) ? 0 : r;
            posJumpAssert = true;
        } break;
        case 0xE: eCommands(ch); break;
        case 0xF:
            if (x->n_param == 0) { pendingStop = true; break; }
            if (x->n_param < 32) { speed = x->n_param; tick = 0; }
            else { bpm = x->n_param; calcTickLen(); }
            break;
        case 0xC: x->n_volume = x->n_param > 64 ? 64 : x->n_param; paulaSetVolume(ch, x->n_volume); break;
        case 0x8:
            if (panOn) x->pan = x->n_param;
            paulaSetPeriod(ch, x->n_period);
            break;
        default: paulaSetPeriod(ch, x->n_period); break;
    }
}

static void checkEffects(int ch)
{
    Chan *x = &chn[ch];
    updateFunk(ch);
    if (!x->n_cmd && !x->n_param) { paulaSetPeriod(ch, x->n_period); return; }
    switch (x->n_cmd) {
        case 0x0: arpeggio(ch); break;
        case 0x1: portaUp(ch); break;
        case 0x2: portaDown(ch); break;
        case 0x3: tonePortamento(ch); break;
        case 0x4: vibrato(ch); break;
        case 0x5: tonePortNoChange(ch); volumeSlide(ch); break;
        case 0x6: vibrato2(ch); volumeSlide(ch); break;
        case 0xE: eCommands(ch); break;
        default:
            paulaSetPeriod(ch, x->n_period);
            if (x->n_cmd == 0x7) tremolo(ch);
            else if (x->n_cmd == 0xA) volumeSlide(ch);
            break;
    }
}

static void setRegs(int ch)
{
    Chan *x = &chn[ch];
    if (x->n_note) {
        if (x->n_cmd == 0xE && (x->n_param >> 4) == 0x5) {
            x->n_finetune = x->n_param & 0x0F;
            setPeriod(ch);
        } else if (x->n_cmd == 0x3 || x->n_cmd == 0x5) {
            setTonePorta(ch);
            checkMoreEffects(ch);
        } else if (x->n_cmd == 0x9) {
            checkMoreEffects(ch);
            setPeriod(ch);
        } else {
            setPeriod(ch);
        }
    } else {
        checkMoreEffects(ch);
    }
}

static void playVoice(int ch)
{
    Chan *x = &chn[ch];
    const Cell *cell = &song.pat[pattern][row][ch];
    if (x->n_rowEmpty) paulaSetPeriod(ch, x->n_period);
    x->n_note  = cell->note ? (uint16_t)periodTable[0][cell->note - 1] : 0;
    x->n_cmd   = cell->cmd & 0x0F;
    x->n_param = cell->param;
    x->n_rowEmpty = !(cell->note | cell->sample | cell->cmd | cell->param);
    if (cell->sample >= 1 && cell->sample <= NUM_SAMPLES) loadSample(ch, cell->sample - 1);
    setRegs(ch);
}

static void doTick(void)
{
    if (mode == PLAY_STOPPED) return;

    if (++tick >= speed) {
        tick = 0;
        if (pattDelTime2 == 0) {
            if (exporting && mode == PLAY_SONG) {
                bool inLoop = false;
                for (int i = 0; i < NUM_CH; i++) if (chn[i].n_loopcount) inLoop = true;
                if (!inLoop && visited[order][row]) { songEnded = true; return; }
                visited[order][row] = 1;
            }
            playRow = row; playOrder = order; playPattern = pattern;
            for (int i = 0; i < NUM_CH; i++) playVoice(i);
            for (int i = 0; i < NUM_CH; i++) {
                paulaSetData(i, chn[i].n_smp, chn[i].n_loopstart);
                paulaSetLength(i, chn[i].n_replen);
            }
        } else {
            for (int i = 0; i < NUM_CH; i++) checkEffects(i);
        }

        row++;
        if (pattDelTime) { pattDelTime2 = pattDelTime; pattDelTime = 0; }
        if (pattDelTime2) { if (--pattDelTime2) row--; }
        if (pBreakFlag) { pBreakFlag = false; row = pBreakPos; pBreakPos = 0; }
        if (row >= NUM_ROWS || posJumpAssert) nextPosition();
    } else {
        for (int i = 0; i < NUM_CH; i++) checkEffects(i);
        if (posJumpAssert) nextPosition();
    }

    if (pendingStop) {
        pendingStop = false;
        mode = PLAY_STOPPED;
        if (exporting) songEnded = true;
        else stopVoices();
    }
}

/* ---------------- public API ---------------- */

static void designLed(void)
{
    /* 2-pole Butterworth-ish low-pass at ~3090 Hz (LED filter) */
    double fc = 3090.53, q = 0.660;
    double w0 = 2 * M_PI * fc / outRate;
    double alpha = sin(w0) / (2 * q), cw = cos(w0);
    double a0 = 1 + alpha;
    ledB0 = ((1 - cw) / 2) / a0;
    ledB1 = (1 - cw) / a0;
    ledB2 = ledB0;
    ledA1 = (-2 * cw) / a0;
    ledA2 = (1 - alpha) / a0;
}

void replayer_init(int rate)
{
    outRate = rate;
    a500Coef = 1.0 - exp(-2 * M_PI * 4420.97 / rate);
    hpCoef = exp(-2 * M_PI * 5.2 / rate);
    designLed();
    resetChannels();
    stopVoices();
    bpm = defaultBpm;
    calcTickLen();
}

void replayer_play(int m, int ord, int r, int pat)
{
    panOn = cfg.panning && !(song.ptModule && !song_uses_upper_channels());
    resetChannels();
    stopVoices();
    mode = m;
    order = ord & 0x7F;
    if (order >= song.numOrders) order = 0;
    row = (r >= 0 && r < NUM_ROWS) ? r : 0;
    pattern = (m == PLAY_SONG) ? song.orders[order] : pat;
    speed = 6;
    bpm = defaultBpm;
    calcTickLen();
    tick = speed - 1;
    playRow = row; playOrder = order; playPattern = pattern;
    pattDelTime = pattDelTime2 = pBreakPos = 0;
    pBreakFlag = posJumpAssert = pendingStop = false;
    lowMask = 0xFF;
    songEnded = false;
    memset(visited, 0, sizeof(visited));
    tickRemain = 0;
}

void replayer_stop(void)
{
    mode = PLAY_STOPPED;
    stopVoices();
}

void replayer_stop_voices(void) { stopVoices(); }

int  replayer_mode(void) { return mode; }
int  replayer_bpm(void) { return mode != PLAY_STOPPED ? bpm : defaultBpm; }
int  replayer_speed(void) { return mode != PLAY_STOPPED ? speed : 6; }
void replayer_current(int *o, int *p, int *r) { *o = playOrder; *p = playPattern; *r = playRow; }
uint64_t replayer_frames_rendered(void) { return framesRendered; }

void replayer_set_bpm(int b)
{
    if (b < 32) b = 32;
    if (b > 255) b = 255;
    defaultBpm = b;
    bpm = b;
    calcTickLen();
}

void replayer_set_speed(int s)
{
    if (s < 1) s = 1;
    if (s > 31) s = 31;
    speed = s;
}

void replayer_jam(int ch, int smp, int note)
{
    if (ch < 0 || ch >= NUM_CH || smp < 0 || smp >= NUM_SAMPLES || note < 1 || note > NUM_NOTES) return;
    Chan *x = &chn[ch];
    loadSample(ch, smp);
    x->n_note = (uint16_t)periodTable[0][note - 1];
    x->n_cmd = 0;
    x->n_param = 0;
    x->n_period = periodTable[x->n_finetune][note - 1];
    x->n_vibratopos = x->n_tremolopos = 0;
    paulaSetData(ch, x->n_smp, x->n_start);
    paulaSetLength(ch, x->n_length);
    paulaSetPeriod(ch, x->n_period);
    paulaStartDMA(ch);
    paulaSetData(ch, x->n_smp, x->n_loopstart);
    paulaSetLength(ch, x->n_replen);
}

void replayer_play_sample_range(int ch, int smp, int32_t start, int32_t end, int note)
{
    if (ch < 0 || ch >= NUM_CH || smp < 0 || smp >= NUM_SAMPLES) return;
    Sample *s = &song.smp[smp];
    if (!s->data || end <= start) return;
    if (note < 1 || note > NUM_NOTES) note = 37;
    Chan *x = &chn[ch];
    x->n_smp = smp;
    x->n_loopstart = 0;
    x->n_replen = 1;
    x->n_finetune = s->finetune;
    x->n_period = periodTable[s->finetune][note - 1];
    voice[ch].regSmp = smp;
    voice[ch].regStart = start & ~1;
    voice[ch].regLen = (end - start) & ~1;   /* end is exclusive */
    paulaSetVolume(ch, s->volume);
    paulaSetPeriod(ch, x->n_period);
    paulaStartDMA(ch);
    voice[ch].regLen = 0;
}

void replayer_invalidate_sample(int idx)
{
    for (int i = 0; i < NUM_CH; i++) {
        if (voice[i].smp == idx || voice[i].regSmp == idx) {
            voice[i].active = false;
            voice[i].smp = voice[i].regSmp = -1;
        }
        if (chn[i].n_smp == idx) { chn[i].n_smp = -1; chn[i].n_length = 0; chn[i].n_replen = 1; }
    }
    ringCount = 0;
}

void replayer_reset_sync(void) { ringCount = 0; }

static void pushSnap(void)
{
    Snap *s = &ring[ringHead];
    ringHead = (ringHead + 1) % SNAP_COUNT;
    if (ringCount < SNAP_COUNT) ringCount++;
    s->frame = framesRendered;
    s->mode = mode;
    s->order = playOrder;
    s->pattern = playPattern;
    s->row = playRow;
    s->speed = speed;
    s->bpm = bpm;
    s->led = ledOn;
    memcpy(s->v, voice, sizeof(voice));
}

bool replayer_snapshot(uint64_t played, PlaySnap *out)
{
    plat_lock();
    if (ringCount == 0) { plat_unlock(); return false; }
    Snap *best = NULL;
    for (int k = 0; k < ringCount; k++) {
        int i = (ringHead - 1 - k + SNAP_COUNT * 2) % SNAP_COUNT;
        if (ring[i].frame <= played) { best = &ring[i]; break; }
    }
    if (!best) best = &ring[(ringHead - ringCount + SNAP_COUNT * 2) % SNAP_COUNT];
    out->frame = best->frame;
    out->mode = best->mode;
    out->order = best->order;
    out->pattern = best->pattern;
    out->row = best->row;
    out->speed = best->speed;
    out->bpm = best->bpm;
    out->led = best->led;
    double dt = played > best->frame ? (double)(played - best->frame) : 0.0;
    for (int i = 0; i < NUM_CH; i++) {
        Voice t = best->v[i];
        if (t.active && t.delta > 0) {
            t.pos += t.delta * dt;
            bool latched = false;
            while (t.pos >= t.len) {
                if (latched) { t.pos = fmod(t.pos, t.len); break; }
                t.pos -= t.len;
                if (t.regLen <= 2 || t.regSmp < 0) { t.active = false; break; }
                t.smp = t.regSmp; t.start = t.regStart; t.len = t.regLen;
                latched = true;
            }
        }
        ScopeVoice *o = &out->v[i];
        o->smp = t.smp;
        o->start = t.start;
        o->len = t.len;
        o->pos = (int32_t)t.pos;
        o->regSmp = t.regSmp;
        o->regStart = t.regStart;
        o->regLen = t.regLen;
        o->delta = t.delta;
        o->volume = t.volume;
        o->active = t.active && t.smp >= 0;
    }
    plat_unlock();
    return true;
}

/* ---------------- mixer ---------------- */

#define MIX_CHUNK 1024
static float mixL[MIX_CHUNK], mixR[MIX_CHUNK];

static void mixVoices(int n)
{
    memset(mixL, 0, sizeof(float) * n);
    memset(mixR, 0, sizeof(float) * n);
    const double sep = cfg.stereoSep / 100.0;

    for (int ch = 0; ch < NUM_CH; ch++) {
        Voice *v = &voice[ch];
        if (!v->active || v->smp < 0 || v->delta <= 0) continue;
        double p = chn[ch].pan / 255.0;
        p = 0.5 + (p - 0.5) * sep;
        float vol = chanMuted[ch] ? 0.0f : v->volume / (64.0f * 128.0f);
        float gl = (float)((1.0 - p) * vol);
        float gr = (float)(p * vol);

        const Sample *s = &song.smp[v->smp];
        const int8_t *data = s->data;
        int32_t dlen = s->length;

        for (int i = 0; i < n; i++) {
            int32_t ip = (int32_t)v->pos;
            int32_t idx = v->start + ip;
            float smp = 0.0f;
            if (data && idx >= 0 && idx < dlen) {
                smp = data[idx];
                if (cfg.interpolation) {
                    float s2 = 0.0f;
                    if (ip + 1 < v->len) {
                        if (idx + 1 < dlen) s2 = data[idx + 1];
                    } else if (v->regLen > 2 && v->regSmp >= 0) {
                        const Sample *rs = &song.smp[v->regSmp];
                        if (rs->data && v->regStart < rs->length) s2 = rs->data[v->regStart];
                    }
                    smp += (s2 - smp) * (float)(v->pos - ip);
                }
            }
            mixL[i] += smp * gl;
            mixR[i] += smp * gr;
            v->pos += v->delta;
            if (v->pos >= v->len) {
                bool latched = false;
                do {
                    v->pos -= v->len;
                    if (v->regLen <= 2 || v->regSmp < 0) { v->active = false; break; }
                    v->smp = v->regSmp;
                    v->start = v->regStart;
                    v->len = v->regLen;
                    if (latched && v->pos >= v->len) v->pos = fmod(v->pos, v->len);
                    latched = true;
                } while (v->pos >= v->len);
                if (!v->active) break;
                s = &song.smp[v->smp];
                data = s->data;
                dlen = s->length;
            }
        }
    }
}

static void outputChunk(int16_t *out, int n)
{
    const double amp = cfg.amp / 16.0 * 32767.0;
    const bool led = ledOn && cfg.ledEmulation;
    for (int i = 0; i < n; i++) {
        double l = mixL[i], r = mixR[i];
        if (cfg.a500Filter) {
            lpL += a500Coef * (l - lpL);
            lpR += a500Coef * (r - lpR);
            l = lpL; r = lpR;
        } else {
            lpL = l; lpR = r;
        }
        if (led) {
            double yl = ledB0 * l + ledZ[0][0];
            ledZ[0][0] = ledB1 * l - ledA1 * yl + ledZ[0][1];
            ledZ[0][1] = ledB2 * l - ledA2 * yl;
            double yr = ledB0 * r + ledZ[1][0];
            ledZ[1][0] = ledB1 * r - ledA1 * yr + ledZ[1][1];
            ledZ[1][1] = ledB2 * r - ledA2 * yr;
            l = yl; r = yr;
        }
        /* DC blocking high-pass (~5 Hz), present on all Amigas */
        double ol = l - hpInL + hpCoef * hpOutL;
        double orr = r - hpInR + hpCoef * hpOutR;
        hpInL = l; hpInR = r; hpOutL = ol; hpOutR = orr;

        long li = lrint(ol * amp), ri = lrint(orr * amp);
        if (li > 32767) li = 32767;
        if (li < -32768) li = -32768;
        if (ri > 32767) ri = 32767;
        if (ri < -32768) ri = -32768;
        out[i * 2] = (int16_t)li;
        out[i * 2 + 1] = (int16_t)ri;
    }
}

void mixer_render(int16_t *out, int frames)
{
    while (frames > 0) {
        int n;
        if (mode != PLAY_STOPPED) {
            if (tickRemain <= 0.0) {
                doTick();
                pushSnap();
                tickRemain += samplesPerTick;
                if (exporting && songEnded) {
                    /* fill the rest with silence-free tail of the voices */
                    mode = PLAY_STOPPED;
                }
            }
            n = (int)ceil(tickRemain - 1e-9);
            if (n < 1) n = 1;
        } else {
            n = 256;
            pushSnap();
        }
        if (n > frames) n = frames;
        if (n > MIX_CHUNK) n = MIX_CHUNK;
        mixVoices(n);
        outputChunk(out, n);
        if (mode != PLAY_STOPPED) tickRemain -= n;
        out += n * 2;
        frames -= n;
        framesRendered += (uint64_t)n;
    }
}

/* ---------------- WAV export ---------------- */

static void w32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void w16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int export_wav(const char *path, char *msg, int msgsz)
{
    FILE *f = plat_fopen(path, "wb");
    if (!f) { snprintf(msg, msgsz, "CAN'T CREATE FILE"); return 0; }
    uint8_t hdr[44] = { 0 };
    fwrite(hdr, 1, 44, f);

    /* keep sample data intact (EFx modifies it during playback) */
    int8_t *backup[NUM_SAMPLES];
    for (int i = 0; i < NUM_SAMPLES; i++) {
        backup[i] = NULL;
        if (song.smp[i].data && song.smp[i].length > 0) {
            backup[i] = (int8_t *)malloc((size_t)song.smp[i].length);
            if (backup[i]) memcpy(backup[i], song.smp[i].data, (size_t)song.smp[i].length);
        }
    }

    double saveLp[2] = { lpL, lpR };
    exporting = true;
    replayer_play(PLAY_SONG, 0, 0, 0);
    static int16_t buf[1024 * 2];
    uint64_t total = 0;
    const uint64_t maxFrames = (uint64_t)outRate * 60 * 60;
    while (!songEnded && total < maxFrames) {
        mixer_render(buf, 1024);
        fwrite(buf, 4, 1024, f);
        total += 1024;
    }
    exporting = false;
    replayer_stop();
    ringCount = 0;
    lpL = saveLp[0]; lpR = saveLp[1];

    for (int i = 0; i < NUM_SAMPLES; i++) {
        if (backup[i]) {
            memcpy(song.smp[i].data, backup[i], (size_t)song.smp[i].length);
            free(backup[i]);
        }
    }

    uint32_t dataBytes = (uint32_t)(total * 4);
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); w32(f, 36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(f, 16); w16(f, 1); w16(f, 2); w32(f, (uint32_t)outRate);
    w32(f, (uint32_t)outRate * 4); w16(f, 4); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, dataBytes);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) { snprintf(msg, msgsz, "WRITE ERROR!"); return 0; }
    int secs = (int)(total / (uint64_t)outRate);
    snprintf(msg, msgsz, "EXPORTED %d:%02d OF AUDIO", secs / 60, secs % 60);
    return 1;
}

/* debugging / test hook */
void replayer_debug_voice(int ch, int *period, int *vol, int *active)
{
    *period = voice[ch].period;
    *vol = voice[ch].volume;
    *active = voice[ch].active;
}
void replayer_debug_tick(void) { doTick(); }
int replayer_debug_pos(int *o, int *r) { *o = playOrder; *r = playRow; return tick; }
void replayer_debug_mix(int frames)
{
    while (frames > 0) {
        int n = frames > MIX_CHUNK ? MIX_CHUNK : frames;
        mixVoices(n);
        frames -= n;
    }
}
double replayer_debug_ticklen(void) { return samplesPerTick; }
