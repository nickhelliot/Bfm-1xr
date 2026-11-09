/* SPDX-License-Identifier: MIT
 * BFMXR's preset synths: bfxr2's synths other than the classic Bfxr (https://github.com/increpare/bfxr2, MIT,
 * Copyright (c) 2021 Stephen Lavelle), each rewritten to make its sound in time order, a block at a time, so the FM-1
 * can render it into a small buffer (app.c "the preset synths' sound").
 *
 * A synth: begin(state, params) sets it up and gives the sound's length in samples; run(state, out, n) makes the next
 * n samples, before bfxr2's SoundDSP.finish (which app.c applies: the mean taken out, tanh, the volume, the fades).
 * The parameters are in the synth's table order (synth_tables.h, generated from bfxr2). */
#pragma once
#include <stdint.h>

typedef struct {
    const char *key;                     /* bfxr2's name */
    const char *label;                   /* on the screen */
    float def, min, max;
    const char *choices;                 /* a choice: "WOOD|GLASS|..." (its values min, min + 1, ...); 0 for a range */
    uint8_t round;                       /* bfxr2 rounds it to a whole number */
    const int8_t *values;                /* a choice's values when they are not min, min + 1, ... (nvalues of them) */
    uint8_t nvalues;
} syn_param_t;

enum { SYN_RV_RANGE, SYN_RV_FIXED, SYN_RV_PICK };
typedef struct {                         /* a parameter a recipe sets */
    uint8_t param, kind, n;              /* kind SYN_RV_*; n: the choices to pick from */
    float a, b;                          /* RANGE: a .. b; FIXED: a */
    const float *list;                   /* PICK: n values */
} syn_rv_t;
typedef struct { const char *name; const syn_rv_t *v; uint8_t nv; } syn_recipe_t;

typedef struct {
    const char *name;                    /* on the screen */
    const char *type;                    /* bfxr2's synth_type */
    uint8_t np;
    const syn_param_t *p;
    uint8_t nr;
    const syn_recipe_t *r;
    uint8_t randomize;                   /* 0 every slider at random; 1 a random recipe; 2 that, the length capped (Crittr);
                                            3 Transfxr's own */
} syn_info_t;

/* what a synth does to its whole sound after making it (bfxr2 works on its buffer), before SoundDSP.finish; and after */
typedef struct {
    float reverse;                       /* Riftr: the sound mixed with itself backwards */
    uint8_t necho;                       /* Jinglr: repeats of the sound, echo_off samples later, echo_gain loud */
    uint32_t echo_off[3];
    float echo_gain[3];
    uint8_t normalize;                   /* Riftr: after finishing, up to 4 times louder, to a peak of 0.9 */
    uint8_t plain;                       /* Transfxr: finished its own way, tanh alone (no mean taken out, no fades) */
    uint32_t fade_out;                   /* then faded over the last fade_out samples (0: none) */
    uint8_t raw;                         /* Footsteppr: its own level already (volume in it), only clipped to -1 .. 1 */
} syn_whole_t;

typedef struct {
    uint32_t (*begin)(void *st, const float *p);
    void (*run)(void *st, float *out, uint32_t n);
    void (*whole)(const float *p, uint32_t len, syn_whole_t *w);   /* (most have none: 0) */
} syn_engine_t;

#define SYN_MAXP 24u                     /* the most parameters a preset synth has (Transfxr: 23) */
#define SYN_STATE_BYTES 65536u           /* the largest synth state (shared with the Bfxr voices: app.c) */

/* bfxr2's seeded random numbers (SoundDSP.rng, mulberry32) */
typedef struct { uint32_t s; } syn_rng_t;
void syn_rng_seed(syn_rng_t *r, float seed);   /* exactly bfxr2's state for the seed */
static inline float syn_rnd(syn_rng_t *r)
{
    uint32_t t;
    r->s += 0x6D2B79F5u;
    t = (r->s ^ (r->s >> 15)) * (1u | r->s);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (float)((t ^ (t >> 14)) >> 8) * (1.0f / 16777216.0f);
}
static inline float syn_clamp(float v, float lo, float hi) { return v != v ? lo : v < lo ? lo : v > hi ? hi : v; }

#define SYN_SLOTS 24u
#include "synth_ids.h"                  /* Mixr's parameters (generated) */
#define SYN_BFXR_KINDS 7u                /* Mixr: the classic Bfxr's kinds of sound it can take (bfxr.net's generate_ buttons:
                                            pickup .. blip, GEN_PICKUP .. GEN_BLIP) */
uint32_t syn_mixr_kinds(uint32_t synth); /* a Mixr sound's kinds for synth (0: Bfxr): its recipes, besides 0 (any) */                    /* room in the saved records for this many preset synths */

extern const uint8_t SYN_N;                      /* the preset synths built in */
extern const syn_info_t SYN_INFO[];              /* (synth_tables.h) */
extern const syn_engine_t SYN_ENGINES[];         /* in SYN_INFO's order (synths.c) */

/* bfxr2's SynthBase / PresetSynth, for a sound p of synth sy (the volume, masterVolume, is never changed but by hand) */
void syn_set(const syn_info_t *sy, float *p, uint32_t k, float v);   /* set_param: kept in range, rounded where bfxr2 does */
void syn_defaults(const syn_info_t *sy, float *p);
void syn_recipe(const syn_info_t *sy, float *p, uint32_t r, syn_rng_t *rng);   /* generate_recipe */
void syn_randomize(const syn_info_t *sy, float *p, syn_rng_t *rng);           /* randomize_params */
void syn_mutate(const syn_info_t *sy, float *p, syn_rng_t *rng);              /* mutate_params */
int syn_find(const syn_info_t *sy, const char *key);                          /* a parameter's index, -1: none */
int syn_choices(const syn_param_t *d, float v, char *out, uint32_t cap);      /* the name of the choice of value v; 0: none */
float syn_choice_step(const syn_param_t *d, float v, int32_t dir);           /* the next (dir 1) or last (-1) choice's value */
