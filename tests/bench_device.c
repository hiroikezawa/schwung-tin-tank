/*
 * TIN TANK - CPU benchmark meant to run ON the Move (static aarch64 binary).
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Runs the engine as the chain host does - 128-frame int16 blocks at
 * 44.1 kHz - and reports mean and worst block time. Normal priority: it
 * measures the code, it does not compete with Move's realtime threads (pin it
 * with taskset 0x7).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dv_engine.h"

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static dv_engine e;

int main(void) {
    const struct { const char *name; float mix, decay; int sweep; } cfg[] = {
        {"defaults", 0.35f, 1.4f, 0},
        {"max decay, wet", 1.0f, 3.0f, 0},
        {"DECAY swept", 0.5f, 1.0f, 1},
    };
    int16_t buf[256];
    uint32_t rng = 1;
    for (unsigned c = 0; c < sizeof(cfg) / sizeof(cfg[0]); ++c) {
        dv_engine_init(&e, 44100.0f);
        dv_engine_set(&e, DV_P_MIX, cfg[c].mix);
        dv_engine_set(&e, DV_P_DECAY, cfg[c].decay);
        dv_engine_snap(&e);
        double worst = 0, total = 0;
        const int warm = 400, n = 3445 * 2;   /* ~20 s measured */
        for (int i = 0; i < warm + n; ++i) {
            for (int k = 0; k < 256; ++k) {
                rng = rng * 1664525u + 1013904223u;
                buf[k] = (int16_t)((int32_t)rng >> 18);
            }
            if (cfg[c].sweep) dv_engine_set(&e, DV_P_DECAY, 0.7f + 2.3f * (float)(i % 345) / 345.0f);
            double t0 = now_us();
            dv_engine_process(&e, buf, 128);
            double dt = now_us() - t0;
            if (i < warm) continue;
            total += dt;
            if (dt > worst) worst = dt;
        }
        printf("%-16s mean %6.1f us (%4.1f %% of 2902 us)  worst %6.1f us\n", cfg[c].name, total / n,
               100.0 * total / n / 2902.0, worst);
    }
    printf("instance size %.1f KB\n", sizeof(dv_engine) / 1024.0);
    return 0;
}
