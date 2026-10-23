/*
 * TIN TANK - the whole effect (see dv_engine.h for the structure).
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Where a number comes from:
 *   delays 7T/4, 4T/3, T with T = 50 ms, the 1 Hz triangle on the 4T/3
 *     chain, the 13.6 kHz 2nd-order Bessel input filters, the 13.25 kHz
 *     2nd-order Butterworth output filter, minor-loop gain A with global
 *     feedback -2A/N               US 8,204,240 (FIG. 3-6 and text)
 *   the delays agree with the PT2399 resistor fit (4.99k/3.24k/1.82k ->
 *     86.9/66.8/50.6 ms) and the 50 ms chip's ~13.5 MHz clock with the
 *     BTDR-3 datasheet's "maximum internal clock approximately 14 MHz"
 *   LFO depth +-0.55 ms            the patent's triangle (5 V peak, taken as
 *     +-2.5 V about pin 6's 2.5 V) through 220k into the 66.7 ms chip's
 *     ~0.77 mA pin current, with the PT2399 current fit
 *   input high-pass                the BTDR-3 application circuit's pre-EQ
 *     high-pass; its corner (120 Hz here) is not published
 *   encoder slew and reconstruction low-pass scaling with clock: estimated
 *     (a 1-bit converter's slope limit and bandwidth both scale with its bit
 *     rate, 44 Kbit / delay); the absolute values are chosen, not measured
 *   internal clip at 1.9x the input's full scale: the BTDR's 1.5 V peak
 *     input against the PT2399's 2 Vrms output ceiling
 *   wet shoulder from 0.85 to full scale: ours, only so a hot input cannot
 *     clip the int16 output hard
 */
#include <math.h>
#include <string.h>

#include "dv_engine.h"

#define DV_PI 3.14159265358979f

/* The three chains (milliseconds) and which ones are fed by the input. */
static const float k_delay_ms[DV_LINES] = {87.5f, 66.6667f, 50.0f};
static const int k_fed[DV_LINES] = {1, 1, 0};
#define DV_MOD_LINE 1
#define DV_LFO_HZ 1.0f
#define DV_LFO_MS 0.55f

#define DV_BITS 45056.0f        /* 44 Kbit RAM */
#define DV_SLEW_HZ_50 5000.0f   /* full-scale sine the 50 ms chip still tracks */
#define DV_REC_DIV 80.0f        /* reconstruction corner = bit rate / 80 */
#define DV_PRE_HZ 13600.0f
#define DV_POST_HZ 13250.0f
#define DV_HP_HZ 120.0f
#define DV_CLIP_KNEE 1.4f
#define DV_CLIP_CEIL 1.9f

/* Broadband T60 -> A: a pass of mean length L loses A, so A = 10^(-3 L / T60).
 * L is the mean chain length, corrected by tests/test_engine.c's measured
 * T60 so the knob reads what the tail does. */
#define DV_LREF 0.0610f
/* Wet gain: the loop's energy gain grows as 1/(1 - A^2); half of that rise is
 * taken back so a longer DECAY is a longer tail without being a much louder
 * one. Scaled so a snare at the default DECAY comes back about as loud as
 * it went in (a hi-hat, mostly above the chips' bandwidth, comes back ~8 dB
 * quieter: that is the band limit, not the gain). */
#define DV_WET_NORM 1.30f

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

float dv_decay_to_gain(float seconds) {
    seconds = clampf(seconds, DV_PARAMS[DV_P_DECAY].min, DV_PARAMS[DV_P_DECAY].max);
    return powf(10.0f, -3.0f * DV_LREF / seconds);
}

float dv_wet_gain(float a) {
    return DV_WET_NORM * sqrtf(sqrtf(1.0f - a * a));
}

/* 2nd-order low-pass by the bilinear transform, prewarped so the -3 dB point
 * lands on fc. The analog prototype is n / (c^2 u^2 + q c u + n), u the
 * bilinear variable, c = w / tan(pi fc / fs) with w the prototype's -3 dB
 * frequency. */
static void lowpass2(dv_biquad *f, float fc, float fs, float q, float n, float w) {
    float k = tanf(DV_PI * fminf(fc, 0.45f * fs) / fs);
    float c = w / k;
    float a0 = c * c + q * c + n;
    f->b0 = n / a0;
    f->b1 = 2.0f * n / a0;
    f->b2 = n / a0;
    f->a1 = (2.0f * n - 2.0f * c * c) / a0;
    f->a2 = (c * c - q * c + n) / a0;
    f->z1 = f->z2 = 0.0f;
}

static inline float biquad(dv_biquad *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* Linear below the knee, then a tanh shoulder to the ceiling. */
static inline float soft_clip(float x, float knee, float ceil) {
    float m = fabsf(x);
    if (m <= knee) return x;
    float r = ceil - knee;
    float y = knee + r * tanhf((m - knee) / r);
    return x < 0.0f ? -y : y;
}

/* Cubic Hermite read `d` samples behind the write position. */
static inline float read_frac(const float *buf, int w, float d) {
    float pos = (float)w - d;
    int i = (int)floorf(pos);
    float t = pos - (float)i;
    float xm1 = buf[(i - 1) & DV_MASK], x0 = buf[i & DV_MASK];
    float x1 = buf[(i + 1) & DV_MASK], x2 = buf[(i + 2) & DV_MASK];
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

static void clear_state(dv_engine *e) {
    for (int i = 0; i < DV_LINES; ++i) {
        dv_line *l = &e->line[i];
        memset(l->buf, 0, sizeof(l->buf));
        l->enc = l->rec = 0.0f;
        l->pre.z1 = l->pre.z2 = 0.0f;
    }
    e->post.z1 = e->post.z2 = 0.0f;
    e->hp_x = e->hp_y = 0.0f;
    e->w = 0;
}

void dv_engine_init(dv_engine *e, float fs) {
    memset(e, 0, sizeof(*e));
    e->fs = fs;
    for (int i = 0; i < DV_LINES; ++i) {
        dv_line *l = &e->line[i];
        float sec = k_delay_ms[i] * 0.001f;
        float bitrate = DV_BITS / sec;
        l->len = sec * fs;
        l->slew = 2.0f * DV_PI * DV_SLEW_HZ_50 * (sec > 0 ? 0.050f / sec : 1.0f) / fs;
        l->rec_k = 1.0f - expf(-2.0f * DV_PI * (bitrate / DV_REC_DIV) / fs);
        /* Bessel: 3 / (s^2 + 3 s + 3), -3 dB at s = 1.3617 */
        lowpass2(&l->pre, DV_PRE_HZ, fs, 3.0f, 3.0f, 1.3617f);
    }
    /* Butterworth: 1 / (s^2 + sqrt2 s + 1), -3 dB at s = 1 */
    lowpass2(&e->post, DV_POST_HZ, fs, 1.41421356f, 1.0f, 1.0f);
    e->hp_k = expf(-2.0f * DV_PI * DV_HP_HZ / fs);
    e->lfo_inc = 4.0f * DV_LFO_HZ / fs;   /* triangle in [-1, 1]: 4 units per period */
    e->lfo_depth = DV_LFO_MS * 0.001f * fs;
    e->a_k = 1.0f - expf(-1.0f / (0.030f * fs));
    e->mix_k = 1.0f - expf(-128.0f / (0.020f * fs));
    for (int i = 0; i < DV_P_COUNT; ++i) dv_engine_set(e, i, DV_PARAMS[i].def);
    dv_engine_snap(e);
    clear_state(e);
}

void dv_engine_set(dv_engine *e, int idx, float value) {
    if (idx < 0 || idx >= DV_P_COUNT || !isfinite(value)) return;
    const dv_param_info *pi = &DV_PARAMS[idx];
    e->p[idx] = clampf(value, pi->min, pi->max);
    if (idx == DV_P_DECAY) e->a_t = dv_decay_to_gain(e->p[idx]);
}

static void mix_gains(float mix, float *dry, float *wet) {
    if (mix <= 0.0f) {
        *dry = 1.0f;
        *wet = 0.0f;
    } else if (mix >= 1.0f) {
        *dry = 0.0f;
        *wet = 1.0f;
    } else {
        *dry = cosf(0.5f * DV_PI * mix);
        *wet = sinf(0.5f * DV_PI * mix);
    }
}

void dv_engine_snap(dv_engine *e) {
    e->a = e->a_t;
    e->mix_s = e->p[DV_P_MIX];
    mix_gains(e->mix_s, &e->g_dry, &e->g_wet);
}

/* The mono wet signal for one input sample. */
static inline float tick(dv_engine *e, float x) {
    /* input conditioning: one-pole high-pass */
    float hp = e->hp_k * (e->hp_y + x - e->hp_x);
    e->hp_x = x;
    e->hp_y = hp;

    e->a += e->a_k * (e->a_t - e->a);
    float a = e->a;

    /* the triangle */
    e->lfo += e->lfo_inc;
    if (e->lfo > 1.0f) { e->lfo = 2.0f - e->lfo; e->lfo_inc = -e->lfo_inc; }
    else if (e->lfo < -1.0f) { e->lfo = -2.0f - e->lfo; e->lfo_inc = -e->lfo_inc; }

    const int w = e->w;
    float y[DV_LINES], sum = 0.0f;
    for (int i = 0; i < DV_LINES; ++i) {
        dv_line *l = &e->line[i];
        float raw = i == DV_MOD_LINE ? read_frac(l->buf, w, l->len + e->lfo * e->lfo_depth)
                                     : l->buf[(w - (int)(l->len + 0.5f)) & DV_MASK];
        l->rec += l->rec_k * (raw - l->rec);   /* reconstruction low-pass */
        y[i] = l->rec;
        sum += y[i];
    }
    const float g = (2.0f / DV_LINES) * a * sum;
    for (int i = 0; i < DV_LINES; ++i) {
        dv_line *l = &e->line[i];
        /* 1e-18: a constant far below audibility keeps every state out of
         * the denormal range once the tail has died */
        float node = (k_fed[i] ? hp : 0.0f) + a * y[i] - g + 1e-18f;
        float f = biquad(&l->pre, soft_clip(node, DV_CLIP_KNEE, DV_CLIP_CEIL));
        /* encoder: tracks the input at most `slew` per sample */
        float d = clampf(f - l->enc, -l->slew, l->slew);
        l->enc += d;
        l->buf[w] = l->enc;
    }
    e->w = (w + 1) & DV_MASK;
    return biquad(&e->post, 0.5f * (y[1] + y[2]));
}

static int state_ok(const dv_engine *e) {
    float s = e->post.z1 + e->post.z2 + e->hp_y;
    for (int i = 0; i < DV_LINES; ++i) {
        const dv_line *l = &e->line[i];
        s += l->enc + l->rec + l->pre.z1 + l->pre.z2;
    }
    return isfinite(s) && fabsf(s) < 1e6f;
}

void dv_engine_process(dv_engine *e, int16_t *audio, int frames) {
    while (frames > 0) {
        const int n = frames > 128 ? 128 : frames;
        /* mix: smoothed per block, the gains ramped linearly inside it */
        float target = e->p[DV_P_MIX];
        e->mix_s += e->mix_k * (target - e->mix_s);
        if (fabsf(target - e->mix_s) < 1e-4f) e->mix_s = target;
        float dry1, wet1;
        mix_gains(e->mix_s, &dry1, &wet1);
        const float dry0 = e->g_dry, wet0 = e->g_wet;
        const float wet_scale0 = dv_wet_gain(e->a);
        const float inv = 1.0f / (float)n;
        /* fully dry for the whole block: the input passes untouched (the
         * reverb still runs, so raising MIX brings in a tail) */
        const int bypass = dry0 == 1.0f && dry1 == 1.0f && wet0 == 0.0f && wet1 == 0.0f;

        for (int k = 0; k < n; ++k) {
            const float l = audio[2 * k] * (1.0f / 32768.0f);
            const float r = audio[2 * k + 1] * (1.0f / 32768.0f);
            /* the wet signal alone is kept under full scale, so a hot
             * input at a long DECAY rounds off instead of wrapping */
            const float wet = soft_clip(tick(e, 0.5f * (l + r)) * wet_scale0, 0.85f, 1.0f);
            if (bypass) continue;
            const float t = (float)(k + 1) * inv;
            const float gd = dry0 + (dry1 - dry0) * t;
            const float gw = wet0 + (wet1 - wet0) * t;
            const float ol = clampf((gd * l + gw * wet) * 32768.0f, -32768.0f, 32767.0f);
            const float or_ = clampf((gd * r + gw * wet) * 32768.0f, -32768.0f, 32767.0f);
            audio[2 * k] = (int16_t)lrintf(ol);
            audio[2 * k + 1] = (int16_t)lrintf(or_);
        }
        e->g_dry = dry1;
        e->g_wet = wet1;
        if (!state_ok(e)) {
            clear_state(e);
            e->nan_resets++;
        }
        audio += 2 * n;
        frames -= n;
    }
}
