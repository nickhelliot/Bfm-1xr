/* SPDX-License-Identifier: MIT
 * BFMXR for the FM-1: the classic Bfxr sound engine of bfxr2 (https://github.com/increpare/bfxr2, MIT, Copyright (c)
 * 2021 Stephen Lavelle), itself from Thomas Vian's SfxrSynth (Apache-2.0, Copyright 2010 Thomas Vian), rewritten to
 * play sample by sample instead of into a buffer. The parameters, their ranges and the sound generators are bfxr2's. */
#pragma once
#include <stdint.h>

/* the 32 parameters, in bfxr2's order (js/synths/Bfxr.js param_info) */
enum {
    BP_VOLUME, BP_WAVE, BP_ATTACK, BP_SUSTAIN, BP_PUNCH, BP_DECAY, BP_COMPRESS, BP_FREQ, BP_SLIDE, BP_DSLIDE,
    BP_MINFREQ, BP_VIB_DEPTH, BP_VIB_SPEED, BP_PJ_REPEAT, BP_PJ1_AMT, BP_PJ1_ONSET, BP_PJ2_AMT, BP_PJ2_ONSET,
    BP_HARM, BP_HARM_FALL, BP_DUTY, BP_DUTY_SWEEP, BP_REPEAT, BP_FLANGE_OFF, BP_FLANGE_SWEEP, BP_LPF,
    BP_LPF_SWEEP, BP_LPF_RES, BP_HPF, BP_HPF_SWEEP, BP_CRUSH, BP_CRUSH_SWEEP, BP_COUNT
};
#define BFXR_NWAVES 12

typedef struct {
    const char *label;           /* as bfxr.net shows it, short enough for the screen */
    float min, max, def;
    uint8_t power;               /* randomize: r^power (0: plain r) */
} bfxr_param_t;
extern const bfxr_param_t BFXR_PARAMS[BP_COUNT];
extern const char *const BFXR_WAVE_NAMES[BFXR_NWAVES];

/* the sound generators, bfxr.net's buttons */
enum { GEN_PICKUP, GEN_LASER, GEN_EXPLOSION, GEN_POWERUP, GEN_HIT, GEN_JUMP, GEN_BLIP, GEN_RANDOM, GEN_COUNT };
extern const char *const BFXR_GEN_NAMES[GEN_COUNT];

typedef struct { float p[BP_COUNT]; } bfxr_sound_t;

/* main loop: a sound's parameters */
void bfxr_defaults(bfxr_sound_t *s);
void bfxr_generate(bfxr_sound_t *s, uint32_t gen);   /* GEN_*: a new sound of that kind */
void bfxr_mutate(bfxr_sound_t *s);
void bfxr_set(bfxr_sound_t *s, uint32_t k, float v); /* clamped to the parameter's range (the wave: rounded) */
void bfxr_seed(uint32_t seed);                       /* the generators' random numbers */
void bfxr_reseed(uint32_t seed);                     /* ... from a known start (Mixr: the same sound again) */

/* a float-float number (hi + lo, ~48 bits): the FM-1's compiler has no double, and the sums made every sample (the
 * period and its slide, the vibrato's phase) drift audibly in a plain float */
typedef struct { float hi, lo; } bfxr_ff_t;

/* a playing sound: started and rendered by the audio interrupt only */
typedef struct {
    float p[BP_COUNT];           /* the sound's parameters as played (the engine adjusts the envelope's) */
    int32_t active, wave, muted, filters, flanger, lp_on, pj_reached, pj2_reached, env_stage;
    int32_t phase, period, flanger_int, onebit_state;
    uint32_t rng;
    float pitch;                 /* playback frequency ratio (keys: 2^(semitones / 12)) */
    float onebit, master_vol, punch, min_freq, overtones, overtone_fall, comp;
    float vib_speed, vib_amp, env_time, len0, len1, len2, stage_len, full_len, bc_sweep, bc_phase, bc_last;
    bfxr_ff_t fps, fmax, slide, accel, vib_phase;   /* per-sample sums: float-float, near JavaScript's double */
    float over0, over1, over2, flanger_delta, flanger_offset, pj_repeat_len, pj_amt, pj2_amt, pj_cur;
    float pj_ts, pj2_ts, bc_freq, duty, duty_sweep, lp_cut, lp_delta, lp_damp, lp_pos, lp_dpos;
    float hp_pos, hp_cut, hp_delta, reset_period, reset_cur, n, length;
    int32_t flanger_pos;
    float flanger_buf[1024], noise[32], lonoise[32];
} bfxr_voice_t;

void bfxr_start(bfxr_voice_t *v, const bfxr_sound_t *s, float pitch, uint32_t seed);
/* writes n samples (-1 .. 1); silence once the sound has ended (v->active 0) */
void bfxr_render(bfxr_voice_t *v, float *out, uint32_t n);
