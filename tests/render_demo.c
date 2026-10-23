/*
 * TIN TANK - offline renders for listening and inspection.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 *   render_demo <outdir>
 *     ir_decay_<s>.wav           impulse response, MIX 100 %, DECAY 0.7 /
 *                                1.4 / 3.0 s (peak-normalised to -1 dBFS)
 *     <signal>_dry.wav           the five test signals
 *     <signal>_wet.wav           ... at MIX 100 %, DECAY 1.4 s
 *     <signal>_mix.wav           ... at the defaults (MIX 35 %, DECAY 1.4 s)
 *     snare_decay_sweep.wav      snare every 0.5 s, DECAY swept 0.7 -> 3 -> 0.7 s
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv_engine.h"
#include "signals.h"

#define LEN (SIG_FS * 8)

static dv_engine e;
static float x[LEN];
static int16_t pcm[LEN * 2];

static void write_wav(const char *path, const int16_t *data, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    unsigned bytes = (unsigned)frames * 4;
    unsigned hdr[] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, SIG_FS, SIG_FS * 4, 0x00100004,
                      0x61746164, bytes};
    fwrite(hdr, 4, 11, f);
    fwrite(data, 2, (size_t)frames * 2, f);
    fclose(f);
}

static void run(float mix, float decay, int sweep) {
    dv_engine_init(&e, SIG_FS);
    dv_engine_set(&e, DV_P_MIX, mix);
    dv_engine_set(&e, DV_P_DECAY, decay);
    dv_engine_snap(&e);
    for (int i = 0; i < LEN; ++i) pcm[2 * i] = pcm[2 * i + 1] = (int16_t)lrintf(fmaxf(-1, fminf(1, x[i])) * 32767.0f);
    for (int i = 0; i < LEN; i += 128) {
        if (sweep) {
            float ph = (float)i / LEN * 2.0f;
            dv_engine_set(&e, DV_P_DECAY, 0.7f + 2.3f * (ph < 1 ? ph : 2 - ph));
        }
        dv_engine_process(&e, pcm + 2 * i, 128);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    const float decays[] = {0.7f, 1.4f, 3.0f};
    for (int d = 0; d < 3; ++d) {
        sig_make(x, LEN, 0, 0.9f);
        run(1.0f, decays[d], 0);
        int peak = 1;
        for (int i = 0; i < LEN * 2; ++i) peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
        for (int i = 0; i < LEN * 2; ++i) pcm[i] = (int16_t)lrintf(pcm[i] * (29204.0f / peak));
        snprintf(path, sizeof path, "%s/ir_decay_%.1f.wav", dir, decays[d]);
        write_wav(path, pcm, LEN);
    }
    const char *names[] = {"impulse", "hihat", "snare", "synth", "drone"};
    for (int k = 1; k <= 4; ++k) {
        sig_make(x, LEN, k, 0.7f);
        if (k == 4)   /* the drone stops at 4 s so its tail can be heard */
            for (int i = SIG_FS * 4; i < LEN; ++i) x[i] = 0.0f;
        run(0.0f, 1.4f, 0);
        snprintf(path, sizeof path, "%s/%s_dry.wav", dir, names[k]);
        write_wav(path, pcm, LEN);
        run(1.0f, 1.4f, 0);
        snprintf(path, sizeof path, "%s/%s_wet.wav", dir, names[k]);
        write_wav(path, pcm, LEN);
        run(DV_PARAMS[DV_P_MIX].def, DV_PARAMS[DV_P_DECAY].def, 0);
        snprintf(path, sizeof path, "%s/%s_mix.wav", dir, names[k]);
        write_wav(path, pcm, LEN);
    }
    /* snare every half second through a DECAY sweep */
    static float one[SIG_FS / 2];
    sig_make(one, SIG_FS / 2, 2, 0.7f);
    for (int i = 0; i < LEN; ++i) x[i] = one[i % (SIG_FS / 2)];
    run(0.5f, 0.7f, 1);
    snprintf(path, sizeof path, "%s/snare_decay_sweep.wav", dir);
    write_wav(path, pcm, LEN);
    printf("renders in %s\n", dir);
    return 0;
}
