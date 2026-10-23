/*
 * TIN TANK - test signals and measurements shared by the tests, the demo
 * renderer and the benchmark.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 */
#ifndef DV_SIGNALS_H
#define DV_SIGNALS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define SIG_FS 44100

static uint32_t sig_rng = 12345;
static inline float sig_noise(void) {   /* xorshift, uniform in [-1, 1) */
    sig_rng ^= sig_rng << 13;
    sig_rng ^= sig_rng >> 17;
    sig_rng ^= sig_rng << 5;
    return (float)(int32_t)sig_rng * (1.0f / 2147483648.0f);
}

/* Fill a mono float signal. kind: 0 impulse, 1 hi-hat, 2 snare, 3 short
 * synth note, 4 drone, 5 white noise. Level is peak (0..1). */
static void sig_make(float *x, int n, int kind, float level) {
    memset(x, 0, sizeof(float) * (size_t)n);
    sig_rng = 12345;
    if (kind == 0) {
        x[0] = level;
        return;
    }
    float hp = 0.0f, prev = 0.0f, bp1 = 0.0f, bp2 = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = (float)i / SIG_FS;
        float v = 0.0f;
        switch (kind) {
        case 1: {   /* hi-hat: high-passed noise, 60 ms decay */
            float w = sig_noise();
            hp = 0.75f * (hp + w - prev);
            prev = w;
            v = t < 0.3f ? hp * expf(-t / 0.06f) * 1.6f : 0.0f;
            break;
        }
        case 2: {   /* snare: 190 Hz body + band-passed noise */
            float w = sig_noise();
            bp1 += 0.25f * (w - bp1);
            bp2 += 0.05f * (bp1 - bp2);
            float body = sinf(2.0f * 3.14159265f * 190.0f * t) * expf(-t / 0.05f);
            v = t < 0.5f ? 0.6f * body + 1.4f * (bp1 - bp2) * expf(-t / 0.12f) : 0.0f;
            break;
        }
        case 3: {   /* short synth note: 220 Hz saw, 250 ms */
            float ph = fmodf(220.0f * t, 1.0f);
            float env = t < 0.005f ? t / 0.005f : t < 0.25f ? 1.0f : fmaxf(0.0f, 1.0f - (t - 0.25f) / 0.02f);
            v = 0.7f * (2.0f * ph - 1.0f) * env;
            break;
        }
        case 4: {   /* drone: 110 Hz + 165 Hz + 330 Hz, held */
            v = 0.45f * sinf(2.0f * 3.14159265f * 110.0f * t) + 0.3f * sinf(2.0f * 3.14159265f * 165.0f * t) +
                0.2f * sinf(2.0f * 3.14159265f * 330.0f * t);
            break;
        }
        case 5:
            v = sig_noise();
            break;
        }
        x[i] = v;
    }
    float peak = 1e-9f;
    for (int i = 0; i < n; ++i) peak = fmaxf(peak, fabsf(x[i]));
    for (int i = 0; i < n; ++i) x[i] *= level / peak;
}

/* T60 from the Schroeder backward integral of e[0..n), fitted between -5
 * and -35 dB and extrapolated (T30 x 2). Returns -1 if it never gets there. */
static float sig_t60(const float *e, int n, float fs) {
    static double edc[SIG_FS * 20];
    if (n > SIG_FS * 20) n = SIG_FS * 20;
    double acc = 0;
    for (int i = n - 1; i >= 0; --i) {
        acc += (double)e[i] * e[i];
        edc[i] = acc;
    }
    if (edc[0] <= 0) return -1;
    int i5 = -1, i35 = -1;
    for (int i = 0; i < n; ++i) {
        double db = 10.0 * log10(edc[i] / edc[0] + 1e-30);
        if (i5 < 0 && db <= -5.0) i5 = i;
        if (db <= -35.0) { i35 = i; break; }
    }
    if (i5 < 0 || i35 < 0) return -1;
    return 2.0f * (float)(i35 - i5) / fs;
}

#endif
