/*
 * TIN TANK - the parameter table shared by the engine, the plugin and the
 * tests.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * module.json's chain_params must agree with this table; scripts/metadata.py
 * checks keys, order, ranges and defaults at build time. The original has two
 * knobs and so does this: MIX (0..1) and DECAY (stored in seconds, so the
 * standard knob grid prints it itself). DECAY starts at 0.7 s: below that
 * the three widely spaced chains give one echo at 67 ms and almost no tail,
 * which is no longer a reverb (0.1.0 went down to 0.2 s).
 */
#ifndef DV_PARAMS_H
#define DV_PARAMS_H

enum {
    DV_P_MIX,     /* equal-power dry/wet, knob 1 */
    DV_P_DECAY,   /* broadband T60 of the loop, knob 2 */
    DV_P_COUNT
};

typedef struct {
    const char *key;
    float min, max, def;
    int integer;
} dv_param_info;

static const dv_param_info DV_PARAMS[DV_P_COUNT] = {
    {"mix",   0.0f, 1.0f, 0.35f, 0},
    {"decay", 0.7f, 3.0f, 1.4f,  0},
};

#endif
