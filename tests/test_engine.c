/*
 * TIN TANK - engine and plugin tests (native).
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Offline impulse-response and signal tests: decay time against the knob,
 * where the first reflections land, how the echo density builds, frequency
 * response, low end, MIX exactness, stability at the longest DECAY, smooth
 * knob moves, state save/restore, block-size independence and instance
 * reuse. `--quick` shortens the long stability run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv_engine.h"
#include "host/audio_fx_api_v2.h"
#include "signals.h"

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); } else { printf("FAIL "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

#define N_MAX (SIG_FS * 70)
static dv_engine eng;
static float xin[N_MAX], yout[N_MAX];
static int16_t pcm[N_MAX * 2];

/* Run a mono signal through `e` in blocks of `block` at (mix, decay); the
 * mono output (left) lands in yout. */
static void run(dv_engine *e, const float *x, int n, float mix, float decay, int block, int fresh) {
    if (fresh) dv_engine_init(e, SIG_FS);
    dv_engine_set(e, DV_P_MIX, mix);
    dv_engine_set(e, DV_P_DECAY, decay);
    if (fresh) dv_engine_snap(e);
    for (int i = 0; i < n; ++i) {
        float v = fmaxf(-1.0f, fminf(1.0f, x[i]));
        pcm[2 * i] = pcm[2 * i + 1] = (int16_t)lrintf(v * 32767.0f);
    }
    for (int i = 0; i < n; i += block) dv_engine_process(e, pcm + 2 * i, i + block <= n ? block : n - i);
    for (int i = 0; i < n; ++i) yout[i] = pcm[2 * i] * (1.0f / 32768.0f);
}

static double rms(const float *x, int a, int b) {
    double s = 0;
    for (int i = a; i < b; ++i) s += (double)x[i] * x[i];
    return sqrt(s / (b - a));
}

static float peak(const float *x, int a, int b) {
    float p = 0;
    for (int i = a; i < b; ++i) p = fmaxf(p, fabsf(x[i]));
    return p;
}

static double db(double v) { return 20.0 * log10(v + 1e-20); }

/* Steady-state wet gain for a sine (Goertzel at that frequency, after the
 * loop has filled). */
static double sine_gain(float f, float decay) {
    const int n = SIG_FS * 6;
    for (int i = 0; i < n; ++i) xin[i] = 0.25f * sinf(2.0f * 3.14159265f * f * i / SIG_FS);
    run(&eng, xin, n, 1.0f, decay, 128, 1);
    double re = 0, im = 0;
    const int a = SIG_FS * 4;
    for (int i = a; i < n; ++i) {
        double ph = 2.0 * M_PI * f * i / SIG_FS;
        re += yout[i] * cos(ph);
        im += yout[i] * sin(ph);
    }
    return 2.0 * sqrt(re * re + im * im) / (n - a) / 0.25;
}

static void test_decay_time(void) {
    const float settings[] = {0.7f, 1.0f, 1.4f, 2.0f, 3.0f};
    const int n = SIG_FS * 12;
    sig_make(xin, n, 0, 0.9f);
    for (unsigned k = 0; k < sizeof(settings) / sizeof(settings[0]); ++k) {
        run(&eng, xin, n, 1.0f, settings[k], 128, 1);
        float t = sig_t60(yout, n, SIG_FS);
        CHECK(fabsf(t - settings[k]) < 0.2f * settings[k], "DECAY %.1f s -> measured broadband T60 %.2f s", settings[k], t);
    }
    /* the shortest DECAY is still a reverb, not one echo: the tail after
     * the first 100 ms carries a real share of the wet energy (0.1.0's
     * 0.2 s minimum left 2 %: MIX 100 % sounded like the reverb vanished) */
    run(&eng, xin, n, 1.0f, DV_PARAMS[DV_P_DECAY].min, 128, 1);
    double first = 0, all = 0;
    for (int i = 0; i < n; ++i) {
        double v = (double)yout[i] * yout[i];
        all += v;
        if (i < SIG_FS / 10) first += v;
    }
    CHECK(first / all < 0.75, "DECAY minimum %.1f s: %.0f %% of the wet energy after the first 100 ms",
          DV_PARAMS[DV_P_DECAY].min, 100.0 * (1.0 - first / all));
}

static void test_early_reflections(void) {
    const int n = SIG_FS * 3;
    sig_make(xin, n, 0, 0.9f);
    run(&eng, xin, n, 1.0f, 1.4f, 128, 1);
    int first = -1;
    float p = peak(yout, 0, n);
    for (int i = 0; i < n; ++i)
        if (fabsf(yout[i]) > 0.05f * p) { first = i; break; }
    float ms = first * 1000.0f / SIG_FS;
    CHECK(peak(yout, 0, SIG_FS * 60 / 1000) == 0.0f, "nothing before 60 ms (pre-delay of the shortest fed chain)");
    CHECK(ms > 66.0f && ms < 69.0f, "first reflection at %.2f ms (66.7 ms chain + filters)", ms);
    /* the echo grid then thickens: distinct arrivals (local maxima within
     * 20 dB of the window's loudest, 1 ms apart) per 150 ms window */
    int counts[4];
    for (int wdx = 0; wdx < 4; ++wdx) {
        int a = SIG_FS * (60 + wdx * 150) / 1000, b = a + SIG_FS * 150 / 1000, c = 0, last = -1000;
        float wp = peak(yout, a, b);
        for (int i = a + 1; i < b - 1; ++i) {
            float v = fabsf(yout[i]);
            if (v > 0.1f * wp && v >= fabsf(yout[i - 1]) && v >= fabsf(yout[i + 1]) && i - last > SIG_FS / 1000) {
                c++;
                last = i;
            }
        }
        counts[wdx] = c;
    }
    CHECK(counts[0] < counts[1] && counts[1] < counts[3], "echo density builds: arrivals per 150 ms %d %d %d %d",
          counts[0], counts[1], counts[2], counts[3]);
    printf("     first arrivals (ms):");
    int shown = 0, last = -1000;
    for (int i = 1; i < n - 1 && shown < 10; ++i) {
        float v = fabsf(yout[i]);
        if (v > 0.1f * p && v >= fabsf(yout[i - 1]) && v >= fabsf(yout[i + 1]) && i - last > SIG_FS / 500) {
            printf(" %.1f", i * 1000.0f / SIG_FS);
            shown++;
            last = i;
        }
    }
    printf("\n");
}

static void test_frequency(void) {
    const float freqs[] = {40, 60, 100, 200, 500, 1000, 2000, 4000, 6000, 8000, 10000, 12000, 15000};
    printf("     wet sine gain, DECAY 1.4 s (dB):");
    double g1k = sine_gain(1000.0f, 1.4f), g40 = 0, g12k = 0;
    for (unsigned k = 0; k < sizeof(freqs) / sizeof(freqs[0]); ++k) {
        double g = sine_gain(freqs[k], 1.4f);
        printf(" %g:%.1f", freqs[k], db(g));
        if (freqs[k] == 40) g40 = g;
        if (freqs[k] == 12000) g12k = g;
    }
    printf("\n");
    CHECK(db(g40) < db(g1k), "low end does not swell: 40 Hz %.1f dB below 1 kHz", db(g1k) - db(g40));
    CHECK(db(g12k) < db(g1k) - 10.0, "top end rolls off: 12 kHz %.1f dB below 1 kHz", db(g1k) - db(g12k));

    /* tone resonances at the longest DECAY: the worst sine between 80 Hz and
     * 2 kHz (1/6-octave steps plus a fine sweep) stays bounded */
    double worst = 0;
    float wf = 0;
    for (float f = 80.0f; f < 2000.0f; f *= 1.0145f) {
        double g = sine_gain(f, 3.0f);
        if (g > worst) { worst = g; wf = f; }
    }
    CHECK(db(worst) < 12.0, "worst resonance at DECAY 3 s: %.1f dB at %.0f Hz (bounded)", db(worst), wf);

    /* the tail darkens with each pass: the share of energy above 4 kHz in
     * an early window vs a late one */
    const int n = SIG_FS * 4;
    sig_make(xin, n, 0, 0.9f);
    run(&eng, xin, n, 1.0f, 3.0f, 128, 1);
    double hf[2], all[2];
    for (int wdx = 0; wdx < 2; ++wdx) {
        int a = wdx ? SIG_FS * 3 / 2 : SIG_FS * 6 / 100, b = a + SIG_FS / 4;
        double s = 0, d = 0;
        for (int i = a + 1; i < b; ++i) {
            double diff = yout[i] - yout[i - 1];  /* first difference ~ high band */
            d += diff * diff;
            s += (double)yout[i] * yout[i];
        }
        hf[wdx] = d;
        all[wdx] = s;
    }
    double early = 10 * log10(hf[0] / all[0]), late = 10 * log10(hf[1] / all[1]);
    CHECK(late < early - 3.0, "tail darkens: HF share %.1f dB early, %.1f dB after 1.5 s", early, late);
}

static void test_mix(void) {
    const int n = SIG_FS * 2;
    sig_make(xin, n, 2, 0.8f);
    run(&eng, xin, n, 0.0f, 3.0f, 128, 1);
    int same = 1;
    for (int i = 0; i < n; ++i)
        if (yout[i] != (int16_t)lrintf(fmaxf(-1, fminf(1, xin[i])) * 32767.0f) / 32768.0f) { same = 0; break; }
    CHECK(same, "MIX 0 %%: output bit-identical to the input");

    sig_make(xin, n, 0, 0.9f);
    run(&eng, xin, n, 1.0f, 3.0f, 128, 1);
    CHECK(peak(yout, 0, SIG_FS * 60 / 1000) == 0.0f, "MIX 100 %%: no dry impulse in the output");

    /* stereo: a left-only signal leaves the right dry path empty at MIX 0
     * and gives identical wet on both sides at MIX 100 */
    dv_engine_init(&eng, SIG_FS);
    dv_engine_set(&eng, DV_P_MIX, 1.0f);
    dv_engine_snap(&eng);
    for (int i = 0; i < n; ++i) { pcm[2 * i] = (int16_t)(xin[i] * 30000); pcm[2 * i + 1] = 0; }
    for (int i = 0; i < n; i += 128) dv_engine_process(&eng, pcm + 2 * i, 128);
    int ident = 1;
    for (int i = 0; i < n; ++i) if (pcm[2 * i] != pcm[2 * i + 1]) { ident = 0; break; }
    CHECK(ident, "MIX 100 %%: mono wet, identical on L and R");

    /* equal power: a level-matched drone keeps its loudness through the sweep */
    const int m = SIG_FS * 6;
    sig_make(xin, m, 4, 0.5f);
    double dry = rms(xin, SIG_FS * 4, m), lo = 1e9, hi = 0;
    for (int k = 0; k <= 10; ++k) {
        run(&eng, xin, m, k / 10.0f, 1.4f, 128, 1);
        double r = rms(yout, SIG_FS * 4, m);
        lo = fmin(lo, r);
        hi = fmax(hi, r);
    }
    CHECK(db(hi / dry) < 3.5 && db(lo / dry) > -6.0, "drone through MIX 0..100 %%: level %+.1f..%+.1f dB vs dry",
          db(lo / dry), db(hi / dry));
}

static void test_level_vs_decay(void) {
    const int n = SIG_FS * 8;
    sig_make(xin, n, 5, 0.35f);
    double r0 = 0, r1 = 0;
    run(&eng, xin, n, 1.0f, 0.7f, 128, 1);
    r0 = rms(yout, SIG_FS * 4, n);
    run(&eng, xin, n, 1.0f, 3.0f, 128, 1);
    r1 = rms(yout, SIG_FS * 4, n);
    CHECK(db(r1 / r0) < 6.0, "noise in, DECAY 0.7 -> 3 s: wet level %+.1f dB (no runaway)", db(r1 / r0));
    sig_make(xin, n, 4, 0.5f);
    run(&eng, xin, n, 1.0f, 0.7f, 128, 1);
    r0 = rms(yout, SIG_FS * 4, n);
    run(&eng, xin, n, 1.0f, 3.0f, 128, 1);
    r1 = rms(yout, SIG_FS * 4, n);
    printf("     drone in, DECAY 0.7 -> 3 s: wet level %+.1f dB (tonal input meets the modes)\n", db(r1 / r0));
}

static void test_stability(int quick) {
    /* the hottest input there is: full-scale noise then a held full-scale
     * drone at the longest DECAY */
    const int n = SIG_FS * (quick ? 10 : 60);
    for (int i = 0; i < n; ++i) {
        float t = (float)i / SIG_FS;
        xin[i] = i < n / 2 ? sig_noise() : 0.99f * sinf(2.0f * 3.14159265f * 523.25f * t) * (t < n * 0.9f / SIG_FS);
    }
    run(&eng, xin, n, 1.0f, 3.0f, 128, 1);
    int finite = 1;
    for (int i = 0; i < n; ++i) if (!isfinite(yout[i])) finite = 0;
    float p_mid = peak(yout, n / 4, n / 2), p_end = peak(yout, n * 3 / 4, n * 9 / 10);
    CHECK(finite && eng.nan_resets == 0, "%d s full-scale noise + drone at DECAY 3 s: finite, no resets", n / SIG_FS);
    CHECK(p_end < 1.0f && p_mid < 1.0f, "peak bounded (noise %.2f, drone %.2f)", p_mid, p_end);
    /* the tail dies: after the input stops, -60 dB within ~1.5 x DECAY */
    for (int i = 0; i < n; ++i) xin[i] = i < SIG_FS ? sig_noise() * 0.7f : 0.0f;
    const int m = SIG_FS * 8;
    run(&eng, xin, m, 1.0f, 3.0f, 128, 1);
    double ref = rms(yout, SIG_FS / 2, SIG_FS);
    double late = rms(yout, SIG_FS * 6, SIG_FS * 7);
    CHECK(db(late / ref) < -60.0, "tail gone 5 s after the input stops (%.0f dB)", db(late / ref));
    CHECK(peak(yout, SIG_FS * 7, m) == 0.0f, "and reaches digital silence (no denormal hiss)");
}

static void test_knob_moves(void) {
    /* DECAY swept end to end 8 times a second on a sustained tone: the
     * output's largest step stays that of a steady setting (no clicks, no
     * pitch jumps) */
    const int n = SIG_FS * 6;
    sig_make(xin, n, 4, 0.5f);
    run(&eng, xin, n, 1.0f, 3.0f, 128, 1);
    float steady = 0;
    for (int i = SIG_FS * 3 + 1; i < n; ++i) steady = fmaxf(steady, fabsf(yout[i] - yout[i - 1]));
    dv_engine_init(&eng, SIG_FS);
    dv_engine_set(&eng, DV_P_MIX, 1.0f);
    dv_engine_snap(&eng);
    for (int i = 0; i < n; ++i) pcm[2 * i] = pcm[2 * i + 1] = (int16_t)lrintf(xin[i] * 32767.0f);
    float moving = 0, prev = 0;
    for (int i = 0; i < n; i += 128) {
        float ph = fmodf((float)i / SIG_FS * 8.0f, 2.0f);
        dv_engine_set(&eng, DV_P_DECAY, 0.7f + 2.3f * (ph < 1 ? ph : 2 - ph));
        dv_engine_set(&eng, DV_P_MIX, (i / 128) % 2 ? 1.0f : 0.6f);
        dv_engine_process(&eng, pcm + 2 * i, 128);
        for (int k = 0; k < 128; ++k) {
            float v = pcm[2 * (i + k)] / 32768.0f;
            if (i >= SIG_FS * 3) moving = fmaxf(moving, fabsf(v - prev));
            prev = v;
        }
    }
    CHECK(moving < 1.3f * steady + 0.002f, "DECAY swept 8x/s and MIX toggled every block: largest step %.4f vs steady %.4f",
          moving, steady);
}

static void test_block_sizes(void) {
    const int n = SIG_FS * 2;
    sig_make(xin, n, 2, 0.8f);
    run(&eng, xin, n, 0.5f, 2.0f, 128, 1);
    static float ref[SIG_FS * 2];
    memcpy(ref, yout, sizeof(ref));
    const int blocks[] = {1, 7, 64, 333};
    for (unsigned k = 0; k < 4; ++k) {
        run(&eng, xin, n, 0.5f, 2.0f, blocks[k], 1);
        CHECK(!memcmp(ref, yout, sizeof(ref)), "host block %d gives the same output as 128", blocks[k]);
    }
}

static void test_plugin(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = 1;
    host.sample_rate = 44100;
    host.frames_per_block = 128;
    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&host);
    CHECK(api && api->api_version == 2, "move_audio_fx_init_v2 answers v2");
    void *a = api->create_instance("", NULL);
    char buf[2048];
    api->set_param(a, "mix", "0.8");
    api->set_param(a, "decay", "2.5");
    CHECK(api->get_param(a, "state", buf, sizeof(buf)) > 0, "state: %s", buf);
    void *b = api->create_instance("", NULL);
    api->set_param(b, "state", buf);
    char ma[32], mb[32], da[32], dbuf[32];
    api->get_param(a, "mix", ma, 32);
    api->get_param(b, "mix", mb, 32);
    api->get_param(a, "decay", da, 32);
    api->get_param(b, "decay", dbuf, 32);
    CHECK(!strcmp(ma, mb) && !strcmp(da, dbuf), "state restores into a new instance (mix %s decay %s)", mb, dbuf);
    api->set_param(b, "state", "{\"mix\":0.1}");
    api->get_param(b, "mix", mb, 32);
    CHECK(!strcmp(mb, "0.8000"), "a partial state is refused whole");
    api->set_param(b, "state", "{\"version\":1,\"mix\":5,\"decay\":-3}");
    api->get_param(b, "mix", mb, 32);
    api->get_param(b, "decay", dbuf, 32);
    CHECK(!strcmp(mb, "1.0000") && !strcmp(dbuf, "0.7000"), "out-of-range state is clamped (%s %s)", mb, dbuf);
    api->set_param(b, "decay", "nan");
    api->set_param(b, "decay", "abc");
    api->get_param(b, "decay", dbuf, 32);
    CHECK(!strcmp(dbuf, "0.7000"), "nan / garbage values are ignored");
    CHECK(api->get_param(a, "ui_hierarchy", buf, sizeof(buf)) > 0 && strstr(buf, "[\"mix\",\"decay\"]"),
          "ui_hierarchy: knobs MIX, DECAY");
    CHECK(api->get_param(a, "chain_params", buf, sizeof(buf)) > 0 && strstr(buf, "\"key\":\"mix\""), "chain_params served");
    CHECK(api->get_param(a, "chain_params", buf, 8) == -1 && buf[0] == 0, "too small a buffer answers -1 and empty");

    /* reuse: a destroyed, re-created instance starts silent with defaults */
    int16_t blk[256];
    for (int k = 0; k < 256; ++k) blk[k] = (int16_t)((k * 7919) % 20000 - 10000);
    for (int i = 0; i < 50; ++i) api->process_block(a, blk, 128);
    api->destroy_instance(a);
    void *c = api->create_instance("", NULL);
    memset(blk, 0, sizeof(blk));
    api->process_block(c, blk, 128);
    int silent = 1;
    for (int k = 0; k < 256; ++k) if (blk[k]) silent = 0;
    api->get_param(c, "mix", ma, 32);
    CHECK(c == a && silent && !strcmp(ma, "0.3500"), "a reused pool slot starts silent with defaults");
    void *all[8];
    int got = 0;
    for (int i = 0; i < 8; ++i) if ((all[i] = api->create_instance("", NULL))) got++;
    CHECK(got == 6, "pool of 8: 6 more instances, then NULL rather than a crash (%d)", got);
    for (int i = 0; i < 8; ++i) if (all[i]) api->destroy_instance(all[i]);
    api->destroy_instance(b);
    api->destroy_instance(c);

    host.sample_rate = 22050;
    CHECK(move_audio_fx_init_v2(&host) == NULL, "an unsupported rate is refused at init");
    host.sample_rate = 48000;
    CHECK(move_audio_fx_init_v2(&host) != NULL, "48 kHz accepted");
    host.sample_rate = 44100;
    move_audio_fx_init_v2(&host);
}

static void test_48k(void) {
    /* sample-rate independence: the first reflection and T60 at 48 kHz */
    static dv_engine e48;
    const int n = 48000 * 6;
    static int16_t p[48000 * 6 * 2];
    static float y[48000 * 6];
    dv_engine_init(&e48, 48000);
    dv_engine_set(&e48, DV_P_MIX, 1.0f);
    dv_engine_set(&e48, DV_P_DECAY, 1.4f);
    dv_engine_snap(&e48);
    memset(p, 0, sizeof(p));
    p[0] = p[1] = 29000;
    for (int i = 0; i < n; i += 128) dv_engine_process(&e48, p + 2 * i, 128);
    float pk = 0;
    for (int i = 0; i < n; ++i) { y[i] = p[2 * i] / 32768.0f; pk = fmaxf(pk, fabsf(y[i])); }
    int first = 0;
    while (first < n && fabsf(y[first]) < 0.05f * pk) first++;
    float t = sig_t60(y, n, 48000);
    CHECK(fabsf(first / 48.0f - 67.3f) < 2.0f && fabsf(t - 1.4f) < 0.21f, "48 kHz: first reflection %.2f ms, T60 %.2f s",
          first / 48.0f, t);
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "--quick");
    printf("== decay\n");
    test_decay_time();
    printf("== early reflections and density\n");
    test_early_reflections();
    printf("== frequency\n");
    test_frequency();
    printf("== mix\n");
    test_mix();
    printf("== level vs decay\n");
    test_level_vs_decay();
    printf("== stability\n");
    test_stability(quick);
    printf("== knob moves\n");
    test_knob_moves();
    printf("== block sizes\n");
    test_block_sizes();
    printf("== plugin\n");
    test_plugin();
    test_48k();
    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ALL ENGINE TESTS PASSED\n");
    return 0;
}
