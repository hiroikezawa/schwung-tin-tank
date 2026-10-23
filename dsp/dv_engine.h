/*
 * TIN TANK - the whole effect.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * A model of the reverberator in US patent 8,204,240 (B. Neunaber, "Apparatus
 * and method for artificial reverberation", FIG. 3 / FIG. 4, N = 3): three
 * PT2399-style delay chains, each behind its own summing node, coupled by
 * equal minor-loop gains A and a global feedback of -2A/N, i.e. the loop
 * matrix A (I - 2/N J), a scaled Householder reflection. Chains 0 and 1 take
 * the input, chain 2 is fed back only; the outputs of chains 1 and 2 are the
 * two outputs, summed to mono.
 *
 *   in ─ HP ─┬─(+)─ sat ─ LP 13.6k ─ PT2399 87.5 ms ─────────┬ y0
 *            ├─(+)─ sat ─ LP 13.6k ─ PT2399 66.7 ms (1 Hz) ──┼ y1 ─┐
 *            │ (+)─ sat ─ LP 13.6k ─ PT2399 50.0 ms ─────────┼ y2 ─┴ sum ─ LP 13.25k ─ wet
 *            │  ^ node_i = x_i + A y_i - (2A/3)(y0 + y1 + y2)  │
 *            └──┴──────────────────────────────────────────────┘
 *
 * "PT2399" here is a delay whose encoder is slew limited (the chip converts
 * with a comparator and integrators into a 1-bit RAM, so it slope-overloads)
 * followed by a reconstruction low-pass; both scale with the chip's clock,
 * i.e. with its delay time. Everything is mono; the stereo host gets the same
 * wet signal on both sides and its own dry signal on each.
 *
 * All memory is inside the struct: nothing is allocated, nothing locked.
 */
#ifndef DV_ENGINE_H
#define DV_ENGINE_H

#include <stdint.h>

#include "dv_params.h"

#define DV_LINES 3
#define DV_BUF 8192          /* power of two, > 88.1 ms at 48 kHz */
#define DV_MASK (DV_BUF - 1)

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} dv_biquad;

typedef struct {
    float buf[DV_BUF];
    float len;           /* delay in samples */
    float slew;          /* largest step the encoder can make per sample */
    float enc;           /* encoder tracking state */
    float rec_k, rec;    /* reconstruction one-pole */
    dv_biquad pre;       /* input filter (Bessel 13.6 kHz) */
} dv_line;

typedef struct {
    float p[DV_P_COUNT];
    float fs;
    int w;               /* shared write position */

    dv_line line[DV_LINES];
    dv_biquad post;      /* output filter (Butterworth 13.25 kHz) */
    float hp_k, hp_x, hp_y;

    /* 1 Hz triangle on chain 1 */
    float lfo, lfo_inc, lfo_depth;

    /* smoothed controls */
    float a, a_t, a_k;   /* loop gain A (per sample) */
    float mix_s, mix_k;  /* mix (per block) */
    float g_dry, g_wet;  /* gains reached at the end of the last block */

    int nan_resets;
} dv_engine;

void dv_engine_init(dv_engine *e, float fs);
void dv_engine_set(dv_engine *e, int idx, float value);
void dv_engine_snap(dv_engine *e);   /* jump all smoothing to its target */
void dv_engine_process(dv_engine *e, int16_t *audio, int frames);

/* The loop gain A that gives a broadband T60 of `seconds`. */
float dv_decay_to_gain(float seconds);
/* The wet output gain used at loop gain A. */
float dv_wet_gain(float a);

#endif
