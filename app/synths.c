/* SPDX-License-Identifier: MIT
 * BFMXR's preset synths: bfxr2's synths (https://github.com/increpare/bfxr2, MIT, Copyright (c) 2021 Stephen
 * Lavelle) rewritten for the FM-1: in float (the FM-1 has no double), and in time order, a block at a time, so a
 * sound renders into app.c's buffer while the FM-1 keeps running. Each follows bfxr2's audio/NAME_DSP.js line by
 * line; tools/check_synths.c compares every one with bfxr2's own output (tools/gen_synths.js). */
#include "synths.h"
#include "synth_tables.h"
#include "fastmath.h"

/* the heavier maths called, not copied into every loop (the FM-1's flash is the limit: the synths at -O2 would
 * otherwise fill it) */
#define NOINL __attribute__((noinline))
#ifdef __clang__                                 /* run once per sound: small, not fast */
#define COLD __attribute__((minsize, cold))
#else
#define COLD __attribute__((cold))
#endif
static NOINL float s_powf(float x, float y) { return fm_powf(x, y); }
static NOINL float s_expf(float x) { return fm_expf(x); }
static NOINL float s_exp2f(float x) { return fm_exp2f(x); }
static NOINL float s_sinf(float x) { return fm_sinf(x); }
static NOINL float s_cosf(float x) { return fm_cosf(x); }
static NOINL float s_sin_turns(float x) { return fm_sin_turns(x); }
static NOINL float s_sqrtf(float x) { return fm_sqrtf(x); }
#define fm_powf s_powf
#define fm_expf s_expf
#define fm_exp2f s_exp2f
#define fm_sinf s_sinf
#define fm_cosf s_cosf
#define fm_sin_turns s_sin_turns
#define fm_sqrtf s_sqrtf

#define RATE 44100.0f
#define TAU FM_TWO_PI
#define PI FM_PI

/* SoundDSP.rng's state, floor(seed * 4294967295), worked out exactly from a float seed in integers */
COLD void syn_rng_seed(syn_rng_t *r, float seed)
{
    fm_bits_t b;
    int32_t e, s;
    uint32_t m, a, low, xhi, xlo;
    b.f = seed;
    if (!(seed > 0.0f)) { r->s = 0; return; }
    if (seed >= 1.0f) { r->s = 0xFFFFFFFFu; return; }
    e = (int32_t)((b.u >> 23) & 0xFF) - 127;     /* seed = m * 2^(e-23) */
    m = (b.u & 0x7FFFFFu) | 0x800000u;
    s = e + 33;                                  /* X = seed * 2^56 = m << s, below 2^56 */
    if (s < 0) { r->s = 0; return; }
    a = s >= 24 ? m << (s - 24) : m >> (24 - s); /* X >> 24 */
    low = s >= 24 ? 0 : (m << s) & 0xFFFFFFu;    /* X & 0xFFFFFF */
    xhi = s == 0 ? 0 : s >= 32 ? m << (s - 32) : m >> (32 - s);
    xlo = s >= 32 ? 0 : m << s;
    /* seed * (2^32 - 1) = X/2^24 - X/2^56: its floor is a, less one when low/2^24 < X/2^56 */
    if (!(xhi < low || (xhi == low && xlo == 0)))
        a--;
    r->s = a;
}

/* ---- the parameters: bfxr2's SynthBase and PresetSynth ---- */
static int is_key(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static int locked(const syn_info_t *sy, uint32_t k) { return is_key(sy->p[k].key, "masterVolume"); }

COLD void syn_set(const syn_info_t *sy, float *p, uint32_t k, float v)
{
    const syn_param_t *d = &sy->p[k];
    v = syn_clamp(v, d->min, d->max);
    if (d->round || d->choices)
        v = fm_floorf(v + 0.5f);                 /* Math.round */
    p[k] = v;
}

COLD void syn_defaults(const syn_info_t *sy, float *p)
{
    for (uint32_t k = 0; k < sy->np; k++)
        p[k] = sy->p[k].def;
}

static COLD void reset_unlocked(const syn_info_t *sy, float *p)
{
    for (uint32_t k = 0; k < sy->np; k++)
        if (!locked(sy, k))
            p[k] = sy->p[k].def;
}

COLD void syn_recipe(const syn_info_t *sy, float *p, uint32_t r, syn_rng_t *rng)
{
    const syn_recipe_t *rc = &sy->r[r % sy->nr];
    reset_unlocked(sy, p);
    for (uint32_t i = 0; i < rc->nv; i++) {
        const syn_rv_t *v = &rc->v[i];
        float x = v->kind == SYN_RV_FIXED ? v->a : v->kind == SYN_RV_RANGE ? v->a + syn_rnd(rng) * (v->b - v->a) :
                  v->list[(uint32_t)(syn_rnd(rng) * (float)v->n) % v->n];
        if (!locked(sy, v->param))
            syn_set(sy, p, v->param, x);
    }
    for (uint32_t k = 0; k < sy->np; k++)        /* a new variation */
        if (is_key(sy->p[k].key, "seed"))
            p[k] = syn_rnd(rng);
}

COLD void syn_randomize(const syn_info_t *sy, float *p, syn_rng_t *rng)
{
    reset_unlocked(sy, p);
    for (uint32_t k = 0; k < sy->np; k++) {
        const syn_param_t *d = &sy->p[k];
        float x;
        if (locked(sy, k))
            continue;
        if (d->choices) {                        /* one of the choices, each as likely */
            float n = d->max - d->min + 1.0f;
            x = d->min + fm_floorf(syn_rnd(rng) * n);
            if (x > d->max)
                x = d->max;
        } else {
            x = d->min + syn_rnd(rng) * (d->max - d->min);
        }
        syn_set(sy, p, k, x);
    }
}

COLD void syn_mutate(const syn_info_t *sy, float *p, syn_rng_t *rng)
{
    for (uint32_t k = 0; k < sy->np; k++) {
        const syn_param_t *d = &sy->p[k];
        if (syn_rnd(rng) < 0.5f || locked(sy, k) || d->choices)
            continue;
        syn_set(sy, p, k, p[k] + (syn_rnd(rng) - 0.5f) * 0.1f * (d->max - d->min));
    }
}

COLD int syn_find(const syn_info_t *sy, const char *key)
{
    for (uint32_t k = 0; k < sy->np; k++)
        if (is_key(sy->p[k].key, key))
            return (int)k;
    return -1;
}

COLD int syn_choices(const syn_param_t *d, uint32_t v, char *out, uint32_t cap)
{
    const char *c = d->choices;
    uint32_t i = 0, n = 0;
    if (!c || !cap)
        return 0;
    while (*c && n < v) {                        /* to the v-th name */
        if (*c++ == '|')
            n++;
    }
    if (n != v || !*c)
        return 0;
    while (*c && *c != '|' && i + 1 < cap)
        out[i++] = *c++;
    out[i] = 0;
    return 1;
}

/* Math.round(duration * 44100) exactly, as bfxr2 works it out in double: the float's mantissa times 44100, in two
 * 32-bit halves (the FM-1's compiler has no 64-bit multiply) */
static COLD uint32_t syn_len(float duration)
{
    fm_bits_t b;
    uint32_t m, a, c, lo, hi, add;
    int32_t sh;
    b.f = duration;
    if (!(duration > 0.0f))
        return 0;
    m = (b.u & 0x7FFFFFu) | 0x800000u;
    sh = 23 - ((int32_t)((b.u >> 23) & 0xFF) - 127);    /* duration = m / 2^sh */
    if (sh <= 12 || sh > 62)
        return 0;                                    /* (over 2^11 s, or under a sample) */
    a = (m >> 12) * 44100u;                          /* m * 44100 = a * 2^12 + c, each below 2^28 */
    c = (m & 0xFFFu) * 44100u;
    lo = a << 12;
    hi = a >> 20;
    lo += c;
    hi += lo < c;
    add = sh >= 33 ? 0 : 1u << (sh - 1);             /* (+ a half: rounded) */
    if (sh >= 33)
        hi += 1u << (sh - 33);
    lo += add;
    hi += lo < add;
    return sh >= 32 ? hi >> (sh - 32) : (lo >> sh) | (hi << (32 - sh));
}

static void rng_skip(syn_rng_t *r, uint32_t n) { r->s += n * 0x6D2B79F5u; }   /* n draws on: mulberry32 counts */

/* a phase in turns kept in two floats (bfxr2 keeps some phases unwrapped in double and takes fractions of them:
 * sin(phase * 0.47)), and the sine of a multiple of it */
typedef struct { float hi, lo; } ff_t;
static inline void ff_add(ff_t *x, float b)
{
    float s = x->hi + b, v = s - x->hi, e = (x->hi - (s - v)) + (b - v);
    e += x->lo;
    x->hi = s + e;
    x->lo = e - (x->hi - s);
}
static inline void ff_split(float a, float *h, float *l)
{
    float c = 4097.0f * a;
    *h = c - (c - a);
    *l = a - *h;
}
static NOINL float ff_frac_mul(const ff_t *x, float k)     /* the fraction of x * k, 0 .. 1 */
{
    float p = x->hi * k, ah, al, bh, bl, e, f;
    ff_split(x->hi, &ah, &al);
    ff_split(k, &bh, &bl);
    e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    f = p - fm_floorf(p);
    f += e + x->lo * k;
    return f - fm_floorf(f);
}
static inline ff_t ff_two_prod(float a, float b)         /* a * b exactly */
{
    ff_t r;
    float ah, al, bh, bl;
    r.hi = a * b;
    ff_split(a, &ah, &al);
    ff_split(b, &bh, &bl);
    r.lo = ((ah * bh - r.hi) + ah * bl + al * bh) + al * bl;
    return r;
}
static inline int ff_ge(ff_t a, ff_t b) { return (a.hi - b.hi) + (a.lo - b.lo) >= 0.0f; }
static inline float ff_sin(const ff_t *x, float k) { return fm_sin_turns(ff_frac_mul(x, k)); }   /* sin(2 pi x k) */
static inline float ff_floor(const ff_t *x)
{
    float f = fm_floorf(x->hi);
    return x->hi == f && x->lo < 0 ? f - 1.0f : f;
}
#define ITAU (1.0f / TAU)

/* a slow sine, sin(2 pi f i / RATE + p0): exact at a block's first sample, turned a sample at a time from there */
typedef struct { float s, c, ws, wc; } lfo_t;
static inline void lfo_at(lfo_t *l, float f, float p0, uint32_t i)
{
    float x = f * (float)i / RATE + p0 * ITAU;
    x -= fm_floorf(x);
    l->s = fm_sin_turns(x);
    l->c = fm_sin_turns(x + 0.25f);
    l->ws = fm_sin_turns(f / RATE);
    l->wc = fm_sin_turns(f / RATE + 0.25f);
}
static inline float lfo_next(lfo_t *l)
{
    float v = l->s, sn = l->s * l->wc + l->c * l->ws;
    l->c = l->c * l->wc - l->s * l->ws;
    l->s = sn;
    return v;
}

/* an envelope worked out every 16 samples and drawn straight between (inside SoundDSP.finish's 5 ms fade, where the
 * curve is steepest, the difference is faded out) */
#define CTL_N 16u
typedef struct { uint32_t base; float a, b; } ctl_t;

/* ---- Whooshr: a moving air source; turbulent bands and a tonal edge sweep past the listener ---- */
typedef struct {
    syn_rng_t rng;
    uint32_t i, length;
    float size, air, whistle, movement, focus, flutter;
    float base, offset, beat, low, broad, phase;
} whooshr_t;

static COLD uint32_t whooshr_begin(void *st, const float *p)
{
    whooshr_t *w = st;
    float duration = syn_clamp(p[WHOOSHR_duration], 0.1f, 5.0f);
    w->length = syn_len(duration);
    syn_rng_seed(&w->rng, syn_clamp(p[WHOOSHR_seed], 0, 1));
    w->size = syn_clamp(p[WHOOSHR_size], 0, 1);
    w->air = syn_clamp(p[WHOOSHR_air], 0, 1);
    w->whistle = syn_clamp(p[WHOOSHR_whistle], 0, 1);
    w->movement = syn_clamp(p[WHOOSHR_movement], 0, 1);
    w->focus = syn_clamp(p[WHOOSHR_focus], 0, 1);
    w->flutter = syn_clamp(p[WHOOSHR_flutter], 0, 1);
    w->base = 190.0f * fm_exp2f((1.0f - w->size) * 3.1f);
    w->offset = syn_rnd(&w->rng) * TAU;
    w->beat = 8.0f + syn_rnd(&w->rng) * 10.0f;
    w->low = w->broad = 0;
    w->phase = w->offset;
    w->i = 0;
    return w->length;
}

static void whooshr_run(void *st, float *out, uint32_t n)
{
    whooshr_t *w = st;
    float inv = w->length > 1 ? 1.0f / (float)(w->length - 1) : 0.0f, sharp = 0.6f + w->focus * 5.0f;
    for (uint32_t k = 0; k < n; k++, w->i++) {
        float u = (float)w->i * inv, t = (float)w->i * (1.0f / RATE), position = (u - 0.5f) * 2.0f;
        float approach = 1.0f - w->movement * 0.7f * position;
        float envelope = fm_powf(fm_sinf(PI * u), sharp) / (1.0f + w->focus * position * position * 7.0f);
        float g = fm_sinf(TAU * w->beat * t + w->offset);
        float gust = 1.0f - w->flutter * 0.65f + w->flutter * 0.65f * g * g;
        float cutoff = fm_minf(12000.0f, (350.0f + 5500.0f * (1.0f - w->size)) * approach);
        float noise = syn_rnd(&w->rng) * 2.0f - 1.0f;
        w->low += (1.0f - fm_expf(-TAU * cutoff / RATE)) * (noise - w->low);
        w->broad += (1.0f - fm_expf(-TAU * cutoff * 0.14f / RATE)) * (noise - w->broad);
        w->phase += TAU * w->base * approach * (1.0f + w->flutter * 0.025f * fm_sinf(TAU * w->beat * t)) / RATE;
        if (w->phase > TAU)
            w->phase -= TAU;
        out[k] = ((w->low - w->broad) * w->air * 2.7f +
                  (fm_sinf(w->phase) + 0.12f * fm_sinf(w->phase * 2.0f)) * w->whistle * 0.45f) * envelope * gust;
    }
}

/* ---- Choirr: band-limited vocal sources, each singer through three vowel formants ---- */
#define CH_MAXV 12
typedef struct {
    syn_rng_t rng;
    uint32_t onset;
    float freq, initial, vib, phase, prev, prev2, coeff[3], y1[3], y2[3];
} choirr_voice_t;
typedef struct {
    uint32_t i, length, voices;
    float swell, breath, motion, gain, radii[3], squares[3], gains[3];
    choirr_voice_t v[CH_MAXV];
} choirr_t;


static COLD uint32_t choirr_begin(void *st, const float *p)
{
    static const int8_t CHORDS[5][4] = {{0, 0, 0, 0}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 7, 12, 19}, {0, 1, 6, 10}};
    static const uint8_t CHORD_N[5] = {1, 4, 4, 4, 4};
    static const float VOWELS[3][3] = {{350, 800, 2300}, {750, 1150, 2600}, {300, 2200, 3100}};
    choirr_t *c = st;
    syn_rng_t rng;
    float duration = syn_clamp(p[CHOIRR_duration], 0.15f, 5.0f), pitch, vowel, detune, blend;
    uint32_t harmony, vi, k;
    c->length = syn_len(duration);
    syn_rng_seed(&rng, syn_clamp(p[CHOIRR_seed], 0, 1));
    c->voices = (uint32_t)fm_floorf(syn_clamp(p[CHOIRR_voices], 1, 12) + 0.5f);
    pitch = 55.0f * fm_exp2f(syn_clamp(p[CHOIRR_pitch], 0, 1) * 4.0f);
    vowel = syn_clamp(p[CHOIRR_vowel], 0, 1);
    detune = syn_clamp(p[CHOIRR_detune], 0, 1);
    c->swell = syn_clamp(p[CHOIRR_swell], 0, 1);
    c->breath = syn_clamp(p[CHOIRR_breath], 0, 1);
    c->motion = syn_clamp(p[CHOIRR_motion], 0, 1);
    harmony = (uint32_t)fm_floorf(syn_clamp(p[CHOIRR_harmony], 0, 4) + 0.5f);
    vi = vowel < 0.5f ? 0 : 1;
    blend = vowel < 0.5f ? vowel * 2.0f : (vowel - 0.5f) * 2.0f;
    c->gain = 1.9f / fm_sqrtf((float)c->voices);
    c->radii[0] = fm_expf(-PI * 85.0f / RATE);
    c->radii[1] = fm_expf(-PI * 120.0f / RATE);
    c->radii[2] = fm_expf(-PI * 190.0f / RATE);
    for (k = 0; k < 3; k++) {
        c->squares[k] = c->radii[k] * c->radii[k];
        c->gains[k] = 1.0f - c->radii[k];
    }
    for (uint32_t v = 0; v < c->voices; v++) {   /* each singer's numbers, then its noise: its own place in the rng */
        choirr_voice_t *s = &c->v[v];
        float note = (float)CHORDS[harmony][v % CHORD_N[harmony]], scale;
        float lim = fm_minf((float)c->length * 0.1f, 3000.0f);
        s->freq = pitch * fm_exp2f(note / 12.0f + (syn_rnd(&rng) - 0.5f) * detune * 0.08f);
        scale = 0.92f + syn_rnd(&rng) * 0.16f;
        for (k = 0; k < 3; k++)
            s->coeff[k] = 2.0f * c->radii[k] *
                          fm_cosf(TAU * (VOWELS[vi][k] * (1.0f - blend) + VOWELS[vi + 1][k] * blend) * scale / RATE);
        s->initial = syn_rnd(&rng) * TAU;
        s->vib = 4.2f + syn_rnd(&rng) * 1.8f;
        s->onset = (uint32_t)fm_floorf(syn_rnd(&rng) * c->motion * lim + 0.5f);
        s->phase = syn_rnd(&rng);
        s->prev = s->prev2 = 0;
        for (k = 0; k < 3; k++)
            s->y1[k] = s->y2[k] = 0;
        s->rng = rng;
        rng_skip(&rng, s->onset < c->length ? c->length - s->onset : 0);
    }
    c->i = 0;
    return c->length;
}

static void choirr_run(void *st, float *out, uint32_t n)
{
    choirr_t *c = st;
    float sharp = 0.25f + c->swell * 2.7f;
    for (uint32_t k = 0; k < n; k++)
        out[k] = 0;
    for (uint32_t v = 0; v < c->voices; v++) {
        choirr_voice_t *s = &c->v[v];
        float span = (float)(c->length - s->onset);
        uint32_t first = c->i > s->onset ? c->i : s->onset;
        lfo_t vib, trem;
        ctl_t env = {0xFFFFFFFFu, 0, 0};
        if (first >= c->i + n)
            continue;
        lfo_at(&vib, s->vib, s->initial, first);
        lfo_at(&trem, 0.7f, s->initial, first);
        for (uint32_t k = first - c->i; k < n; k++) {
            uint32_t i = c->i + k;
            float step, source, resonant = 0, envelope;
            if (i - env.base >= CTL_N) {         /* the swell, every 16 samples */
                env.base = i - (i - s->onset) % CTL_N;
                env.a = fm_powf(fm_maxf(0.0f, fm_sinf(PI * (float)(env.base - s->onset) / span)), sharp);
                env.b = fm_powf(fm_maxf(0.0f, fm_sinf(PI * (float)(env.base + CTL_N - s->onset) / span)), sharp);
            }
            step = s->freq * (1.0f + c->motion * 0.009f * lfo_next(&vib)) / RATE;
            s->phase += step;
            if (s->phase >= 1.0f)
                s->phase -= 1.0f;
            source = 2.0f * s->phase - 1.0f;
            if (s->phase < step) {               /* PolyBLEP */
                float x = s->phase / step;
                source -= x + x - x * x - 1.0f;
            } else if (s->phase > 1.0f - step) {
                float x = (s->phase - 1.0f) / step;
                source -= x * x + x + x + 1.0f;
            }
            source = source * (1.0f - c->breath * 0.7f) + (syn_rnd(&s->rng) * 2.0f - 1.0f) * c->breath * 0.35f;
            for (uint32_t b = 0; b < 3; b++) {
                float x = c->gains[b] * (source - s->prev2) + s->coeff[b] * s->y1[b] - c->squares[b] * s->y2[b];
                s->y2[b] = s->y1[b];
                s->y1[b] = x;
                resonant += x * (b == 0 ? 1.2f : b == 1 ? 0.8f : 0.5f);
            }
            s->prev2 = s->prev;
            s->prev = source;
            envelope = env.a + (env.b - env.a) * (float)(i - env.base) * (1.0f / CTL_N);
            out[k] += (resonant + fm_sin_turns(s->phase) * 0.065f) * envelope * c->gain *
                      (0.9f + c->motion * 0.1f * lfo_next(&trem));
        }
    }
    c->i += n;
}

/* ---- Breathr: turbulent air coloured by direction; an obstructed airway flutters into a snore ---- */
#define BR_DELAY_MAX 4300u
typedef struct {
    syn_rng_t rng;
    uint32_t i, length, cycles, source, single, delay, held_every;
    float inward, effort, throat, rasp, flutter, space, slot, in_end, out_start, out_end, gain;
    float var[10], radius, r2, band_gain;
    float low, highpass, y1, y2, prev, prev2, wander, phase, coefficient, cutoff, held;
    float echo[BR_DELAY_MAX];            /* the output's last `delay` samples */
} breathr_t;

static COLD uint32_t breathr_begin(void *st, const float *p)
{
    breathr_t *b = st;
    float duration = syn_clamp(p[BREATHR_duration], 0.15f, 5.0f), inhale, hold;
    b->length = syn_len(duration);
    b->single = fm_floorf(syn_clamp(p[BREATHR_mode], 0, 1) + 0.5f) == 0.0f;
    b->inward = (1.0f - syn_clamp(p[BREATHR_direction], -1, 1)) / 2.0f;
    syn_rng_seed(&b->rng, syn_clamp(p[BREATHR_seed], 0, 1));
    b->cycles = b->single ? 1u : (uint32_t)fm_floorf(syn_clamp(p[BREATHR_cycles], 1, 10) + 0.5f);
    b->source = (uint32_t)fm_floorf(syn_clamp(p[BREATHR_source], 0, 2) + 0.5f);
    b->effort = syn_clamp(p[BREATHR_effort], 0, 1);
    inhale = syn_clamp(p[BREATHR_inhale], 0.1f, 0.9f);
    hold = syn_clamp(p[BREATHR_hold], 0, 0.7f);
    b->throat = syn_clamp(p[BREATHR_throat], 0, 1);
    b->rasp = syn_clamp(p[BREATHR_rasp], 0, 1);
    b->flutter = syn_clamp(p[BREATHR_flutter], 0, 1);
    b->space = syn_clamp(p[BREATHR_space], 0, 1);
    b->slot = (float)b->length / (float)b->cycles;
    b->in_end = (1.0f - hold) * inhale * 0.9f;
    b->out_start = b->in_end + hold * 0.9f + 0.025f;
    b->out_end = 0.965f;
    b->delay = (uint32_t)fm_floorf(RATE * (0.022f + b->space * 0.075f) + 0.5f);
    if (b->delay > BR_DELAY_MAX)
        b->delay = BR_DELAY_MAX;
    b->gain = 1.5f + b->effort * 1.5f;
    for (uint32_t k = 0; k < b->cycles; k++)
        b->var[k] = 0.88f + syn_rnd(&b->rng) * 0.24f;
    b->radius = fm_expf(-PI * 550.0f / RATE);
    b->r2 = b->radius * b->radius;
    b->band_gain = (1.0f - b->radius) * 1.6f;
    b->low = b->highpass = b->y1 = b->y2 = b->prev = b->prev2 = b->wander = 0;
    b->phase = syn_rnd(&b->rng) * TAU;
    b->coefficient = b->cutoff = b->held = 0;
    b->held_every = (uint32_t)fm_floorf(5.0f + b->throat * 22.0f + 0.5f);
    for (uint32_t k = 0; k < b->delay; k++)
        b->echo[k] = 0;
    b->i = 0;
    return b->length;
}

static void breathr_run(void *st, float *out, uint32_t n)
{
    breathr_t *b = st;
    for (uint32_t k = 0; k < n; k++, b->i++) {
        uint32_t i = b->i, cycle;
        float fi = (float)i, q = fm_floorf(fi / b->slot), position = (fi - q * b->slot) / b->slot, blend, local;
        float arch, shape, airflow, noise, excitation, turbulent, resonant, obstruction, flap, airway, tremor;
        float envelope, tissue, x;
        int inhaling = position < b->in_end, exhaling = position > b->out_start && position < b->out_end;
        cycle = q < (float)(b->cycles - 1u) ? (uint32_t)q : b->cycles - 1u;
        blend = b->single ? b->inward : inhaling ? 1.0f : 0.0f;
        local = b->single ? fi / fm_maxf(1.0f, (float)(b->length - 1u)) : inhaling ? position / b->in_end :
                exhaling ? (position - b->out_start) / (b->out_end - b->out_start) : 0.0f;
        arch = fm_powf(fm_maxf(0.0f, fm_sinf(PI * local)), b->single ? 0.7f + 0.1f * blend : inhaling ? 0.8f : 0.7f);
        shape = b->single ? (1.0f - 0.36f * local) * (1.0f - blend) + (0.8f + 0.2f * local) * blend :
                inhaling ? 0.8f + 0.2f * local : 1.0f - 0.36f * local;
        airflow = arch * shape * b->var[cycle];
        noise = syn_rnd(&b->rng) * 2.0f - 1.0f;
        b->wander += (noise - b->wander) * 0.0025f;
        if (b->source == 1 && i % b->held_every == 0)
            b->held = noise > 0 ? 0.8f : -0.8f;
        excitation = b->source == 1 ? b->held : noise;
        if ((i & 31u) == 0) {                    /* inhaled jets brighter; the mouth and chest soften the out-breath */
            float air = b->single ? (750.0f + b->effort * 1600.0f) * (1.0f - blend) + (2400.0f + b->effort * 2700.0f) * blend :
                        inhaling ? 2400.0f + b->effort * 2700.0f : 750.0f + b->effort * 1600.0f;
            float hz = air * (1.0f - b->throat * 0.38f) * (0.75f + airflow * 0.25f);
            b->cutoff = 1.0f - fm_expf(-TAU * hz / RATE);
            b->coefficient = 2.0f * b->radius *
                             fm_cosf(TAU * (b->single ? 510.0f + 490.0f * blend : inhaling ? 1000.0f : 510.0f) *
                                     (1.35f - b->throat * 0.6f) / RATE);
        }
        b->low += (excitation - b->low) * b->cutoff;
        b->highpass += (b->low - b->highpass) *
                       (1.0f - fm_expf(-TAU * (b->single ? 110.0f + 120.0f * blend : inhaling ? 230.0f : 110.0f) / RATE));
        turbulent = b->low - b->highpass;
        resonant = b->band_gain * (turbulent - b->prev2) + b->coefficient * b->y1 - b->r2 * b->y2;
        b->y2 = b->y1;
        b->y1 = resonant;
        b->prev2 = b->prev;
        b->prev = turbulent;
        b->phase += TAU * (24.0f + (1.0f - b->throat) * 44.0f) *
                    (1.0f + b->flutter * 0.3f * fm_sinf(TAU * 2.3f * fi / RATE) + b->wander * 0.9f) / RATE;
        obstruction = b->source == 2 ? 0.72f + b->rasp * 0.27f : b->rasp * 0.22f;
        x = fm_maxf(0.0f, fm_sinf(b->phase));
        flap = x * x * x;
        airway = 1.0f - obstruction + obstruction * flap;
        tremor = fm_maxf(0.2f, 1.0f + b->wander * b->flutter * 6.0f + b->flutter * 0.07f * fm_sinf(TAU * 7.3f * fi / RATE));
        envelope = airflow * tremor;
        if (b->source == 1)
            envelope = fm_floorf(envelope * 12.0f + 0.5f) / 12.0f;
        tissue = b->source == 2 ? (flap - 0.212f) * b->rasp * 0.3f * arch : 0.0f;
        x = ((turbulent * 0.85f + resonant * 0.8f) * airway + tissue) * envelope * b->gain;
        if (b->delay) {                          /* the room: the output `delay` samples ago */
            float *e = &b->echo[i % b->delay];
            if (i >= b->delay)
                x += *e * b->space * 0.38f;
            *e = x;
        }
        out[k] = x;
    }
}

/* ---- Birdr: two syringeal oscillators, beak resonances and a schedule of syllables ---- */
typedef struct { float start, active, shift, curvature, phase; } birdr_syl_t;
typedef struct {
    syn_rng_t rng;
    uint32_t i, length, voice, count, call;
    float pitch, sweep, arch, trill, trill_rate, duet, rasp, breath, noise_low;
    float radii[2], coeff[2], y1[2], y2[2];
    ff_t phase, second;                  /* in turns */
    birdr_syl_t syl[16];
} birdr_t;

static COLD uint32_t birdr_begin(void *st, const float *p)
{
    birdr_t *b = st;
    float gap, rhythm, variation, w[16], total = 0, offset = 0;
    uint32_t k;
    b->length = syn_len(syn_clamp(p[BIRDR_duration], 0.15f, 5.0f));
    syn_rng_seed(&b->rng, syn_clamp(p[BIRDR_seed], 0, 1));
    b->voice = (uint32_t)fm_floorf(syn_clamp(p[BIRDR_voice], 0, 4) + 0.5f);
    b->count = (uint32_t)fm_floorf(syn_clamp(p[BIRDR_syllables], 1, 16) + 0.5f);
    gap = syn_clamp(p[BIRDR_gap], 0, 0.85f);
    b->pitch = 180.0f * fm_exp2f(syn_clamp(p[BIRDR_pitch], 0, 1) * 4.8f);
    b->sweep = syn_clamp(p[BIRDR_sweep], -1, 1);
    b->arch = syn_clamp(p[BIRDR_arch], -1, 1);
    b->trill = syn_clamp(p[BIRDR_trill], 0, 1);
    b->trill_rate = 5.0f + syn_clamp(p[BIRDR_trill_rate], 0, 1) * 65.0f;
    b->duet = syn_clamp(p[BIRDR_duet], 0, 1);
    b->rasp = syn_clamp(p[BIRDR_rasp], 0, 1);
    b->breath = syn_clamp(p[BIRDR_breath], 0, 1);
    rhythm = syn_clamp(p[BIRDR_rhythm], 0, 1);
    variation = syn_clamp(p[BIRDR_variation], 0, 1);
    for (k = 0; k < b->count; k++) {             /* a contour repeated, the answers alternating, drifting */
        w[k] = 1.0f + rhythm * (syn_rnd(&b->rng) - 0.5f) * 1.1f;
        total += w[k];
    }
    for (k = 0; k < b->count; k++) {
        float length = w[k] / total * (float)b->length;
        b->syl[k].start = offset;
        b->syl[k].active = fm_maxf(1.0f, length * (1.0f - gap));
        b->syl[k].shift = variation * ((k % 2u ? -0.6f : 0.15f) + (syn_rnd(&b->rng) - 0.5f) * 0.2f);
        b->syl[k].curvature = 1.0f + variation * (syn_rnd(&b->rng) - 0.5f) * 0.5f;
        b->syl[k].phase = syn_rnd(&b->rng) * TAU;
        offset += length;
    }
    b->phase.hi = syn_rnd(&b->rng);
    b->second.hi = syn_rnd(&b->rng);
    b->phase.lo = b->second.lo = 0;
    b->noise_low = 0;
    b->call = 0;
    b->radii[0] = fm_expf(-PI * 260.0f / RATE);
    b->radii[1] = fm_expf(-PI * 480.0f / RATE);
    b->coeff[0] = 2.0f * b->radii[0] * fm_cosf(TAU * (b->voice == 3 ? 850.0f : 1550.0f) / RATE);
    b->coeff[1] = 2.0f * b->radii[1] * fm_cosf(TAU * (b->voice == 3 ? 1850.0f : 3200.0f) / RATE);
    b->y1[0] = b->y1[1] = b->y2[0] = b->y2[1] = 0;
    b->i = 0;
    return b->length;
}

static void birdr_run(void *st, float *out, uint32_t n)
{
    birdr_t *b = st;
    for (uint32_t k = 0; k < n; k++, b->i++) {
        birdr_syl_t *y;
        float local, u, attack, release, envelope, tremor, contour, irregular, f, noise, modulation, source;
        float resonant = 0, su, pulse, ph;
        while (b->call < b->count - 1u && (float)b->i >= b->syl[b->call + 1u].start)
            b->call++;
        y = &b->syl[b->call];
        local = (float)b->i - y->start;
        u = fm_minf(1.0f, local / y->active);
        attack = fm_maxf(35.0f, y->active * (b->voice == 4 ? 0.18f : 0.065f));
        release = fm_maxf(70.0f, y->active * (b->voice == 3 ? 0.4f : 0.2f));
        envelope = fm_maxf(0.0f, fm_minf(1.0f, fm_minf(local / attack, (y->active - local) / release)));
        su = fm_sinf(PI * u);
        envelope *= b->voice == 4 ? fm_powf(su, 0.6f) : 0.88f + 0.12f * su;
        {
            float tt = b->trill_rate * local / RATE + y->phase * ITAU;   /* in turns */
            tremor = fm_sin_turns(tt - fm_floorf(tt));
        }
        contour = b->sweep * (u - 0.5f) * 1.8f + b->arch * su * y->curvature + y->shift;
        irregular = b->rasp * (0.035f * ff_sin(&b->phase, 0.47f) + 0.022f * ff_sin(&b->second, 0.31f));
        f = fm_minf(10500.0f, b->pitch * fm_exp2f(contour + b->trill * 0.2f * tremor + irregular));
        ff_add(&b->phase, f / RATE);
        ff_add(&b->second, fm_minf(11500.0f, f * (1.12f + 0.15f * su + b->duet * 0.35f)) / RATE);
        noise = syn_rnd(&b->rng) * 2.0f - 1.0f;
        b->noise_low += (noise - b->noise_low) * 0.13f;
        modulation = b->voice == 1 ? b->duet * 1.15f * ff_sin(&b->second, 1.0f) : 0.0f;
        ph = ff_frac_mul(&b->phase, 1.0f);
        source = fm_sin_turns(ph + modulation * ITAU);
        if (b->voice == 2)
            source = 0.7f * fm_sin_turns(ph + 0.8f * fm_sin_turns(ph) * ITAU) + 0.23f * ff_sin(&b->phase, 2.0f) +
                     0.12f * ff_sin(&b->phase, 3.0f);
        if (b->voice == 3)
            source = 0.55f * fm_sin_turns(ph + 1.3f * fm_sin_turns(ph) * ITAU) + 0.3f * ff_sin(&b->phase, 0.5f) +
                     b->rasp * b->noise_low;
        if (b->voice == 4)
            source = 0.88f * fm_sin_turns(ph) + 0.1f * ff_sin(&b->phase, 2.0f);
        source += b->duet * 0.28f * ff_sin(&b->second, 1.0f) +
                  b->rasp * 0.2f * ff_sin(&b->phase, 0.5f) * (0.65f + 0.35f * ff_sin(&b->phase, 0.19f));
        for (uint32_t c = 0; c < 2; c++) {
            float x = (1.0f - b->radii[c]) * source + b->coeff[c] * b->y1[c] - b->radii[c] * b->radii[c] * b->y2[c];
            b->y2[c] = b->y1[c];
            b->y1[c] = x;
            resonant += x;
        }
        if (b->voice == 2 || b->voice == 3)
            source = source * 0.62f + resonant * 0.85f;
        source = source * (1.0f - b->breath * 0.45f) + b->breath * (noise - b->noise_low) * 0.25f;
        pulse = 1.0f - b->trill * 0.35f + b->trill * 0.35f * tremor;
        out[k] = source * envelope * pulse * 0.65f;
    }
}

/* ---- Bouncr: the object's and the surface's modes, struck by the same short contact, bounce after bounce ---- */
#define BO_MAXH 20
typedef struct {
    syn_rng_t rng;
    uint32_t start, end;
    float strength, contact, tone, absorption, low, rub, absorbed;
    float freq[8], phase[8], gain[8], decay[8];
} bouncr_hit_t;
typedef struct {
    uint32_t i, length, nhits, material, surface;
    float stiffness, spin, force, tail;
    bouncr_hit_t h[BO_MAXH];
} bouncr_t;

static COLD uint32_t bouncr_begin(void *st, const float *p)
{
    static const struct { float pitch, decay, hard, ratios[4]; } OBJ[5] = {
        {0.55f, 0.041f, 0.22f, {1, 1.97f, 3.17f, 4.8f}}, {1, 0.030f, 0.64f, {1, 2.43f, 4.13f, 6.27f}},
        {2.5f, 0.16f, 1, {1, 1.47f, 2.71f, 4.09f}}, {3.3f, 0.10f, 0.95f, {1, 2.76f, 5.4f, 8.13f}},
        {0.72f, 0.025f, 0.86f, {1, 1.91f, 3.47f, 5.31f}}};
    static const struct { float pitch, decay, hard, ring, ratios[4]; } SURF[6] = {
        {260, 0.022f, 0.95f, 0.30f, {1, 1.61f, 2.83f, 4.41f}}, {135, 0.075f, 0.6f, 0.72f, {1, 2.18f, 3.72f, 5.41f}},
        {330, 0.26f, 0.93f, 0.85f, {1, 1.59f, 2.32f, 3.79f}}, {710, 0.18f, 1, 0.70f, {1, 1.71f, 3.04f, 4.93f}},
        {78, 0.017f, 0.16f, 0.18f, {1, 1.83f, 2.81f, 4.18f}}, {90, 0.016f, 0.035f, 0.15f, {1, 2.11f, 3.43f, 4.72f}}};
    bouncr_t *o = st;
    syn_rng_t rng;
    float duration = syn_clamp(p[BOUNCR_duration], 0.2f, 5.0f), bounce, gravity, size, hardness;
    float transfer, decay_scale, base, time = 0.012f, gap;
    uint32_t count;
    o->length = syn_len(duration);
    syn_rng_seed(&rng, syn_clamp(p[BOUNCR_seed], 0, 1));
    o->material = (uint32_t)fm_floorf(syn_clamp(p[BOUNCR_material], 0, 4) + 0.5f);
    o->surface = (uint32_t)fm_floorf(syn_clamp(p[BOUNCR_surface], 0, 5) + 0.5f);
    count = (uint32_t)fm_floorf(syn_clamp(p[BOUNCR_count], 1, 20) + 0.5f);
    bounce = syn_clamp(p[BOUNCR_bounce], 0, 1);
    gravity = syn_clamp(p[BOUNCR_gravity], 0, 1);
    size = syn_clamp(p[BOUNCR_size], 0, 1);
    hardness = syn_clamp(p[BOUNCR_hardness], 0, 1);
    o->spin = syn_clamp(p[BOUNCR_spin], 0, 1);
    o->force = syn_clamp(p[BOUNCR_force], 0, 1);
    o->tail = syn_clamp(p[BOUNCR_tail], 0, 1);
    o->stiffness = fm_sqrtf(OBJ[o->material].hard * SURF[o->surface].hard) * (0.18f + hardness * 0.82f);
    transfer = 0.15f + SURF[o->surface].hard * 0.85f;
    decay_scale = (0.3f + o->tail * 2.7f) * (0.75f + o->force * 0.5f);
    base = (95.0f + 790.0f * (1.0f - size) * (1.0f - size)) * OBJ[o->material].pitch;
    gap = fm_minf(duration * 0.42f, 0.18f + 0.45f * (1.0f - gravity));
    o->nhits = 0;
    for (uint32_t hit = 0; hit < count; hit++) {
        bouncr_hit_t *h = &o->h[o->nhits];
        float od, sd, detune;
        uint32_t start = (uint32_t)fm_floorf(time * RATE + 0.5f), m;
        if (start >= o->length)
            break;
        h->start = start;
        h->strength = (0.18f + o->force * 0.82f) * fm_powf(0.53f + bounce * 0.43f, (float)hit) *
                      (0.94f + syn_rnd(&rng) * 0.06f);
        h->contact = (0.0008f + (1.0f - o->stiffness) * 0.013f) / (0.65f + o->force * 0.8f);
        od = OBJ[o->material].decay * decay_scale * (0.3f + transfer * 0.7f) * (0.75f + bounce * 0.35f);
        sd = SURF[o->surface].decay * decay_scale * (0.8f + size * 0.5f);
        detune = 1.0f + (syn_rnd(&rng) - 0.5f) * (0.025f + o->spin * 0.07f);
        for (m = 0; m < 4; m++) {
            float mf = (float)m;
            h->freq[2 * m] = fm_minf(RATE * 0.42f, base * OBJ[o->material].ratios[m] * detune);
            h->decay[2 * m] = od / (1.0f + mf * (0.2f + (1.0f - hardness) * 0.4f));
            h->gain[2 * m] = transfer * 0.72f * fm_powf(o->stiffness + 0.18f, mf * 0.48f) / (1.0f + mf * 0.8f);
            h->freq[2 * m + 1] = fm_minf(RATE * 0.42f, SURF[o->surface].pitch * (1.6f - size) *
                                         SURF[o->surface].ratios[m] * (0.97f + syn_rnd(&rng) * 0.06f));
            h->decay[2 * m + 1] = sd / (1.0f + mf * 0.48f);
            h->gain[2 * m + 1] = SURF[o->surface].ring * 0.72f * fm_powf(o->stiffness + 0.12f, mf * 0.6f) / (1.0f + mf);
        }
        for (m = 0; m < 8; m++) {                /* a soft contact cannot excite modes faster than its pulse */
            float x = h->freq[m] * h->contact * 0.4f;
            h->gain[m] /= 1.0f + x * x;
            h->phase[m] = 0;
        }
        {
            float span = fm_maxf(h->contact * 10.0f, fm_maxf(od, sd) * 7.0f) * RATE, c = fm_floorf(span);
            uint32_t len = (uint32_t)(c < span ? c + 1.0f : c);
            h->end = start + len < o->length ? start + len : o->length;
        }
        h->low = h->rub = h->absorbed = 0;
        h->tone = 1.0f - fm_expf(-TAU * (100.0f + o->stiffness * 11000.0f) / RATE);
        h->absorption = 1.0f - fm_expf(-TAU * (100.0f + SURF[o->surface].hard * SURF[o->surface].hard * 14000.0f) / RATE);
        h->rng = rng;
        rng_skip(&rng, 2u * (h->end - h->start));        /* (its noise and its scrape: two a sample) */
        o->nhits++;
        time += gap;
        gap = fm_maxf(0.006f, gap * (0.4f + bounce * 0.54f) * (1.0f - o->spin * 0.12f));
    }
    o->i = 0;
    return o->length;
}

static void bouncr_run(void *st, float *out, uint32_t n)
{
    bouncr_t *o = st;
    float scatter_k = o->surface == 4 ? 0.6f : o->surface == 0 ? 0.16f : 0.04f;
    for (uint32_t k = 0; k < n; k++)
        out[k] = 0;
    for (uint32_t hi = 0; hi < o->nhits; hi++) {
        bouncr_hit_t *h = &o->h[hi];
        uint32_t a = o->i > h->start ? o->i : h->start, z = o->i + n < h->end ? o->i + n : h->end;
        float ek[8], e[8], bend_k, bend_e, cn_k, cn_e, sc_k, sc_e, sr_k, sr_e, t0;
        if (a >= z)
            continue;
        /* the decays exp(-t / d), exact at the block's first sample and stepped from there */
        t0 = (float)(a - h->start) / RATE;
        for (uint32_t m = 0; m < 8; m++) {
            e[m] = fm_expf(-t0 / h->decay[m]);
            ek[m] = fm_expf(-1.0f / (RATE * h->decay[m]));
        }
        bend_e = fm_expf(-t0 / (h->contact * 2.5f));
        bend_k = fm_expf(-1.0f / (RATE * h->contact * 2.5f));
        cn_e = fm_expf(-t0 / h->contact);
        cn_k = fm_expf(-1.0f / (RATE * h->contact));
        sc_e = fm_expf(-t0 / (0.014f + o->tail * 0.035f));
        sc_k = fm_expf(-1.0f / (RATE * (0.014f + o->tail * 0.035f)));
        sr_e = fm_expf(-t0 / (0.025f + o->spin * 0.08f));
        sr_k = fm_expf(-1.0f / (RATE * (0.025f + o->spin * 0.08f)));
        for (uint32_t i = a; i < z; i++) {
            float t = (float)(i - h->start) / RATE, attack = fm_minf(1.0f, t / (0.00025f + h->contact * 0.3f));
            float noise = syn_rnd(&h->rng) * 2.0f - 1.0f, body = 0, bend, cn, sc, sr, rr;
            h->low += (noise - h->low) * h->tone;
            h->rub += (noise - h->rub) * 0.08f;
            bend = o->material == 0 ? 1.0f + (0.2f + o->force * 0.65f) * bend_e : 1.0f;
            for (uint32_t m = 0; m < 8; m++) {
                h->phase[m] += h->freq[m] * bend / RATE;      /* in turns */
                if (h->phase[m] >= 1.0f)
                    h->phase[m] -= 1.0f;
                body += fm_sin_turns(h->phase[m]) * h->gain[m] * e[m];
                e[m] *= ek[m];
            }
            cn = h->low * (0.18f + o->stiffness * 0.6f) * cn_e;
            sc = scatter_k * h->low * sc_e;
            rr = syn_rnd(&h->rng);
            {
                float tt = (110.0f + rr * 30.0f) * t;
                sr = h->rub * o->spin * 0.6f * sr_e * (0.65f + 0.35f * fm_sin_turns(tt - fm_floorf(tt)));
            }
            h->absorbed += (body + cn + sc + sr - h->absorbed) * h->absorption;
            out[i - o->i] += h->absorbed * h->strength * attack;
            bend_e = fm_flush(bend_e * bend_k);
            cn_e = fm_flush(cn_e * cn_k);
            sc_e *= sc_k;
            sr_e *= sr_k;
            for (uint32_t m = 0; m < 8; m++)
                e[m] = fm_flush(e[m]);
        }
    }
    o->i += n;
}

/* ---- Swarmr: moving emitters that fall into step as a flock, or scatter into a cloud ---- */
#define SW_MAXA 32
typedef struct {
    syn_rng_t rng;
    uint32_t start;
    float pulse_offset, initial, detune, drift_phase, drift_rate, center, gain, pulse_speed;
    float phase, filtered, wing_noise;
} swarmr_agent_t;
typedef struct {
    uint32_t i, frames, count, kind;
    float agitation, size, movement, base;
    swarmr_agent_t a[SW_MAXA];
} swarmr_t;

static COLD uint32_t swarmr_begin(void *st, const float *p)
{
    swarmr_t *w = st;
    syn_rng_t rng;
    float speed, cohesion, scatter, pulse_rate, gain;
    w->frames = syn_len(syn_clamp(p[SWARMR_duration], 0.25f, 5.0f));
    w->count = (uint32_t)fm_floorf(syn_clamp(p[SWARMR_count], 3, 32) + 0.5f);
    w->kind = (uint32_t)fm_floorf(syn_clamp(p[SWARMR_kind], 0, 5) + 0.5f);
    speed = syn_clamp(p[SWARMR_speed], 0, 1);
    cohesion = syn_clamp(p[SWARMR_cohesion], 0, 1);
    w->agitation = syn_clamp(p[SWARMR_agitation], 0, 1);
    w->size = syn_clamp(p[SWARMR_size], 0, 1);
    w->movement = syn_clamp(p[SWARMR_movement], 0, 1);
    scatter = syn_clamp(p[SWARMR_scatter], 0, 1);
    syn_rng_seed(&rng, syn_clamp(p[SWARMR_seed], 0, 1));
    w->base = 90.0f * fm_exp2f((1.0f - w->size) * 3.8f);
    pulse_rate = 2.0f + speed * 17.0f;
    gain = 0.44f / fm_sqrtf((float)w->count);
    for (uint32_t k = 0; k < w->count; k++) {
        swarmr_agent_t *g = &w->a[k];
        g->start = (uint32_t)fm_floorf(syn_rnd(&rng) * scatter * (float)w->frames * 0.42f);
        g->pulse_offset = syn_rnd(&rng) * (1.0f - cohesion);
        g->initial = syn_rnd(&rng) * TAU;
        g->detune = fm_exp2f((syn_rnd(&rng) - 0.5f) * (0.08f + 0.8f * (1.0f - cohesion)));
        g->drift_phase = syn_rnd(&rng) * TAU;
        g->drift_rate = 0.4f + syn_rnd(&rng) * 2.0f;
        g->center = 0.4f + syn_rnd(&rng) * 0.2f;
        g->gain = gain * (0.8f + syn_rnd(&rng) * 0.4f);
        g->pulse_speed = pulse_rate * (1.0f + (syn_rnd(&rng) - 0.5f) * (1.0f - cohesion) * 0.3f);
        g->phase = g->initial;
        g->filtered = g->wing_noise = 0;
        g->rng = rng;
        rng_skip(&rng, g->start < w->frames ? w->frames - g->start : 0);
    }
    w->i = 0;
    return w->frames;
}

static void swarmr_run(void *st, float *out, uint32_t n)
{
    swarmr_t *w = st;
    for (uint32_t k = 0; k < n; k++)
        out[k] = 0;
    for (uint32_t ai = 0; ai < w->count; ai++) {
        swarmr_agent_t *g = &w->a[ai];
        float span = (float)(w->frames - g->start);
        uint32_t first = w->i > g->start ? w->i : g->start;
        lfo_t ld, la;
        ctl_t shape = {0xFFFFFFFFu, 0, 0};
        if (first >= w->i + n)
            continue;
        lfo_at(&ld, g->drift_rate, g->drift_phase, first);
        lfo_at(&la, g->drift_rate * 1.73f, g->initial, first);
        for (uint32_t k = first - w->i; k < n; k++) {
            uint32_t i = w->i + k;
            float t, age, travel, jitter, beat, wing, positive, neg, stroke, f, envelope = 1, signal, noise;
            float turbulence, airflow, window, drift, s1, c1, s2, s3;
            t = (float)i * (1.0f / RATE);
            age = (float)(i - g->start) / span;
            travel = 1.0f + w->movement * 0.55f * (1.0f - 2.0f * age);
            drift = lfo_next(&ld);
            jitter = 1.0f + w->agitation * 0.12f * drift;
            beat = t * g->pulse_speed + g->pulse_offset;
            beat -= fm_floorf(beat);
            wing = fm_sin_turns(beat);
            positive = fm_maxf(0.0f, wing);
            neg = fm_maxf(0.0f, -wing);
            stroke = positive * positive * positive + 0.32f * neg * neg * neg * neg * neg;
            f = w->base * g->detune * travel * jitter;
            if (w->kind == 1) {
                float p2 = positive * positive;
                f *= 1.3f + 2.2f * (1.0f - beat);
                envelope = p2 * p2 * p2;
            } else if (w->kind == 2)
                f *= 0.32f;
            else if (w->kind == 3) {
                float arrival = (float)(i - g->start) * g->pulse_speed / RATE, paired, x;
                f *= 2.3f;
                envelope = fm_maxf(beat < 0.2f ? fm_powf(1.0f - beat / 0.2f, 5.0f) : 0.0f,
                                   arrival < 0.2f ? fm_powf(1.0f - arrival / 0.2f, 5.0f) : 0.0f);
                paired = beat - 0.24f - w->agitation * 0.035f * drift;
                if (paired >= 0 && paired < 0.16f) {
                    x = 1.0f - paired / 0.16f;
                    envelope += 0.62f * x * x * x * x;
                }
            } else if (w->kind == 4)
                f *= 0.75f;
            else if (w->kind == 5)
                f *= 0.28f;
            g->phase += TAU * f / RATE;
            if (g->phase > TAU)
                g->phase -= TAU;
            noise = syn_rnd(&g->rng) * 2.0f - 1.0f;
            g->filtered += 0.08f * (noise - g->filtered);
            g->wing_noise += (noise - g->wing_noise) * (0.16f + 0.32f * (1.0f - w->size));
            turbulence = g->wing_noise - g->filtered;
            airflow = 1.0f + w->agitation * 0.4f * lfo_next(&la);
            {                                    /* sin and cos of the phase, its harmonics from them */
                float r = g->phase > PI ? g->phase - TAU : g->phase, rc = r + 0.5f * PI;
                s1 = fm_sin_pi(r);
                c1 = fm_sin_pi(rc >= PI ? rc - TAU : rc);
                s2 = 2.0f * s1 * c1;
                s3 = s1 * (3.0f - 4.0f * s1 * s1);
            }
            switch (w->kind) {
            case 0:
                signal = (s1 + 0.22f * s2 + 0.12f * s3) * (0.08f + 0.4f * stroke);
                signal += (turbulence * 3.4f + g->filtered * 0.6f) * (0.12f + 1.4f * stroke) * airflow;
                break;
            case 1:
                signal = (s1 + 0.2f * s2) * envelope;
                break;
            case 2: {                            /* the motor below the blade-pass tone; the blades' wakes broadband */
                float bl = 0.5f + 0.5f * s3, b2 = bl * bl, blade = b2 * b2 * bl;
                float s6 = 2.0f * s3 * c1 * (4.0f * c1 * c1 - 3.0f);
                signal = (0.62f * s1 + 0.28f * s3 + 0.15f * s6) * (0.75f + 0.25f * wing);
                signal += (turbulence * 2.1f + noise * 0.12f) * (0.25f + blade) * airflow;
                break;
            }
            case 3:
                signal = (noise * 0.65f + s1 * 0.5f) * envelope;
                break;
            case 4:
                signal = s1 * (0.75f + 0.25f * wing) + 0.12f * s2;
                break;
            default:
                signal = (g->filtered * 2.5f + s1 * 0.25f + noise * 0.07f) * (0.7f + 0.3f * wing);
            }
            if (w->kind == 3) {
                float d = (age - g->center) * 3.0f;
                window = fm_minf(1.0f, fm_minf((float)(i - g->start) / (RATE * 0.002f),
                                               (float)(w->frames - 1u - i) / (RATE * 0.005f))) /
                         (1.0f + w->movement * d * d * 4.0f);
            } else {                             /* the window and the fly-by, every 16 samples */
                if (i - shape.base >= CTL_N) {
                    float a0, a1, d0, d1;
                    shape.base = i - (i - g->start) % CTL_N;
                    a0 = (float)(shape.base - g->start) / span;
                    a1 = (float)(shape.base + CTL_N - g->start) / span;
                    d0 = (a0 - g->center) * 3.0f;
                    d1 = (a1 - g->center) * 3.0f;
                    shape.a = fm_powf(fm_maxf(0.0f, fm_sinf(PI * a0)), 0.7f) / (1.0f + w->movement * d0 * d0 * 4.0f);
                    shape.b = fm_powf(fm_maxf(0.0f, fm_sinf(PI * a1)), 0.7f) / (1.0f + w->movement * d1 * d1 * 4.0f);
                }
                window = shape.a + (shape.b - shape.a) * (float)(i - shape.base) * (1.0f / CTL_N);
            }
            out[k] += signal * window * g->gain;
        }
    }
    w->i += n;
}

/* ---- Machinr: rotating parts, friction and resonant contacts ---- */
typedef struct {
    syn_rng_t rng;
    uint32_t i, count, mech, turn, latch1, latch2, door;
    float speed, load, rough, loose, base, tooth_rate, attack, release, decay, ring_a, ring_b, contact_k;
    float a1, a2, b1, b2, slow_noise, friction, contact, event_strength, rotor_offset, prev_tooth;
    ff_t phase, teeth;                   /* the rotor in turns; the teeth counted */
} machinr_t;

static COLD uint32_t machinr_begin(void *st, const float *p)
{
    machinr_t *m = st;
    float scale, ring_f, size = p[MACHINR_size];
    uint32_t c = syn_len(p[MACHINR_duration]);
    m->count = c > 2u ? c : 2u;
    syn_rng_seed(&m->rng, p[MACHINR_seed]);
    m->mech = (uint32_t)p[MACHINR_mechanism];
    m->speed = p[MACHINR_speed];
    m->load = p[MACHINR_load];
    m->rough = p[MACHINR_roughness];
    m->loose = p[MACHINR_looseness];
    scale = fm_exp2f((0.5f - size) * 2.6f);
    m->base = (35.0f + 230.0f * m->speed * m->speed) * scale * (1.0f - 0.32f * m->load);
    m->tooth_rate = (5.0f + m->speed * 48.0f) * (1.0f - m->load * 0.28f);
    if (m->mech == 3)
        m->tooth_rate = 1.3f + m->speed * 8.0f;
    if (m->mech == 4)
        m->tooth_rate = 7.0f + m->speed * 30.0f;
    if (m->mech == 7)
        m->tooth_rate = 4.0f + m->speed * 17.0f;
    m->attack = fm_maxf(0.003f, p[MACHINR_startTime]) * RATE;
    m->release = fm_maxf(0.006f, p[MACHINR_stopTime]) * RATE;
    ring_f = fm_minf(8500.0f, (650.0f + 1100.0f * (1.0f - size)) * scale);
    m->decay = fm_expf(-1.0f / (RATE * (0.007f + 0.035f * m->loose)));
    m->ring_a = 2.0f * m->decay * fm_cosf(TAU * ring_f / RATE);
    m->ring_b = 2.0f * m->decay * fm_cosf(TAU * ring_f * 0.63f / RATE);
    m->contact_k = fm_expf(-1.0f / (RATE * (0.007f + 0.025f * m->load)));
    m->latch1 = (uint32_t)fm_floorf(RATE * 0.008f);
    m->latch2 = (uint32_t)fm_floorf((float)m->count * (0.19f + 0.12f * m->load));
    m->door = (uint32_t)fm_floorf((float)m->count * 0.88f);
    m->a1 = m->a2 = m->b1 = m->b2 = m->slow_noise = m->friction = m->contact = 0;
    m->phase.hi = m->phase.lo = m->teeth.hi = m->teeth.lo = 0;
    m->prev_tooth = -1;
    m->turn = 0;
    m->event_strength = 1;
    m->rotor_offset = syn_rnd(&m->rng) * TAU;
    m->i = 0;
    return m->count;
}

static void machinr_run(void *st, float *out, uint32_t n)
{
    machinr_t *m = st;
    for (uint32_t k = 0; k < n; k++, m->i++) {
        uint32_t i = m->i;
        float t = (float)i * (1.0f / RATE), progress = (float)i / (float)(m->count - 1u);
        float engage = fm_minf(1.0f, (float)i / m->attack), stop = fm_minf(1.0f, (float)(m->count - 1u - i) / m->release);
        float envelope = engage * stop, noise = syn_rnd(&m->rng) * 2.0f - 1.0f, spin, wobble, tooth, kick = 0;
        float ra, rb, rattle, rotor, sample, ph1;
        m->slow_noise += (noise - m->slow_noise) * 0.0015f;
        m->friction += (noise - m->friction) * (0.09f + 0.5f * m->rough);
        spin = (0.18f + 0.82f * engage) * (0.2f + 0.8f * stop);
        if (m->mech == 6)
            spin *= 1.0f - 0.7f * progress;
        wobble = 1.0f + m->rough * (0.085f * fm_sinf(TAU * 7.3f * t + m->rotor_offset) + m->slow_noise * 1.2f);
        ff_add(&m->phase, m->base * spin * wobble / RATE);
        ff_add(&m->teeth, m->tooth_rate * spin * wobble / RATE);
        tooth = ff_floor(&m->teeth);
        if (tooth != m->prev_tooth) {
            m->prev_tooth = tooth;
            m->turn++;
            m->event_strength = 0.45f + syn_rnd(&m->rng) * 0.55f;
            kick = (0.15f + m->loose * 0.7f) * m->event_strength;
            if (m->mech == 3)
                kick = 0.75f;
            if (m->mech == 4)
                kick *= syn_rnd(&m->rng) > m->rough * 0.22f ? 1.0f : 0.1f;
            m->contact = m->event_strength;
        }
        m->contact *= m->contact_k;
        if (m->mech == 2)                        /* a shutter's two latches */
            kick = i == m->latch1 || i == m->latch2 ? 1.9f : 0.0f;
        if (m->mech == 7 && i == m->door)        /* the door strikes as it closes */
            kick += 3.2f;
        ra = kick * 0.14f + m->ring_a * m->a1 - m->decay * m->decay * m->a2;
        rb = kick * 0.11f + m->ring_b * m->b1 - m->decay * m->decay * m->b2;
        m->a2 = m->a1;
        m->a1 = ra;
        m->b2 = m->b1;
        m->b1 = rb;
        rattle = m->turn % 2u ? ra : rb;
        ph1 = ff_frac_mul(&m->phase, 1.0f);
        rotor = fm_sin_turns(ph1) + 0.35f * ff_sin(&m->phase, 2.0f) + 0.12f * ff_sin(&m->phase, 5.0f);
        switch (m->mech) {
        case 1:                                  /* teeth rubbing and slipping under load */
            sample = 0.11f * rotor + 0.55f * rattle + m->friction * (0.05f + 0.18f * m->rough) +
                     0.11f * m->load * fm_sin_turns(ff_frac_mul(&m->phase, 3.1f) + 3.0f * ff_sin(&m->phase, 0.017f) * ITAU) *
                         (0.5f + 0.5f * fm_sinf(TAU * 1.7f * t));
            break;
        case 2:
            sample = 1.05f * (ra + rb) + 0.08f * rotor * fm_expf(-progress * 9.0f) +
                     m->friction * 0.13f * fm_expf(-progress * 12.0f);
            break;
        case 3:
            sample = rattle * 1.35f + 0.014f * rotor + m->friction * m->contact * 0.18f;
            break;
        case 4:
            sample = 0.28f * rotor * (0.3f + m->contact) + 0.4f * m->contact * m->friction +
                     0.21f * ff_sin(&m->phase, 0.5f) * (0.4f + m->load) + 0.18f * rattle;
            break;
        case 5: {
            float position = 0.55f + 0.45f * fm_sinf(TAU * (1.1f + m->speed * 3.0f) * t);
            sample = (0.25f * ff_sin(&m->phase, 3.1f + m->load) + 0.09f * ff_sin(&m->phase, 6.2f)) * position +
                     0.05f * rattle + m->friction * (0.015f + 0.035f * m->rough);
            break;
        }
        case 6:
            sample = 0.13f * fm_sin_turns(ff_frac_mul(&m->phase, 1.9f) + 2.0f * fm_sinf(m->teeth.hi * 0.38f) * ITAU) +
                     0.53f * rattle + 0.08f * rotor + m->friction * (0.015f + 0.09f * m->rough);
            break;
        case 7: {
            float creak = fm_sin_turns(ff_frac_mul(&m->phase, 0.62f) + 4.0f * ff_sin(&m->phase, 0.023f) * ITAU);
            sample = 0.27f * creak * (0.55f + 0.45f * fm_sinf(m->teeth.hi * 1.4f)) + 0.45f * rattle +
                     0.14f * ff_sin(&m->phase, 0.31f) + m->friction * m->rough * 0.1f;
            break;
        }
        default:
            sample = 0.24f * rotor + 0.09f * ff_sin(&m->phase, 8.0f) * (0.25f + m->load) +
                     rattle * (0.1f + m->loose * 0.3f) + m->friction * (0.015f + m->rough * 0.15f);
        }
        out[k] = sample * envelope;
    }
}

/* ---- Crittr: nonverbal calls, pulsed breath through three moving throat resonances ---- */
typedef struct {
    syn_rng_t rng;
    uint32_t i, length, voice, calls;
    float pitch, morph, breath, growl, flutter, contour, slot, active, attack, release, duty, flutter_step;
    float freqs[3], radii[3], r2[3], gains[3], coeff[3], y1[3], y2[3], motion[12];
    float flutter_phase, prev, prev2, noise_low;
    ff_t phase;                          /* in turns */
} crittr_t;

static COLD uint32_t crittr_begin(void *st, const float *p)
{
    static const float ANAT[8][3] = {{550, 1260, 2550}, {820, 2100, 3900}, {360, 950, 1840}, {1100, 2800, 4700},
                                     {430, 1500, 3200}, {670, 1740, 3550}, {430, 1150, 2450}, {700, 1900, 3100}};
    static const float DUTY[8] = {0.24f, 0.12f, 0.3f, 0.07f, 0.38f, 0.1f, 0.2f, 0.28f};
    static const float RAD[3] = {100, 160, 230};
    crittr_t *c = st;
    float size, scale;
    c->length = syn_len(syn_clamp(p[CRITTR_duration], 0.15f, 5.0f));
    syn_rng_seed(&c->rng, syn_clamp(p[CRITTR_seed], 0, 1));
    c->voice = (uint32_t)fm_floorf(syn_clamp(p[CRITTR_voice], 0, 7) + 0.5f);
    c->pitch = 45.0f * fm_exp2f(5.0f * syn_clamp(p[CRITTR_pitch], 0, 1));
    size = syn_clamp(p[CRITTR_size], 0, 1);
    c->morph = syn_clamp(p[CRITTR_morph], 0, 1);
    c->breath = syn_clamp(p[CRITTR_breath], 0, 1);
    c->growl = syn_clamp(p[CRITTR_growl], 0, 1);
    c->flutter = syn_clamp(p[CRITTR_flutter], 0, 1);
    c->contour = syn_clamp(p[CRITTR_contour], -1, 1);
    c->calls = (uint32_t)fm_floorf(syn_clamp(p[CRITTR_calls], 1, 12) + 0.5f);
    c->slot = (float)c->length / (float)c->calls;
    c->active = c->slot * (1.0f - syn_clamp(p[CRITTR_gap], 0, 0.85f));
    c->attack = fm_maxf(40.0f, c->active * (c->voice == 6 ? 0.025f : c->voice == 7 ? 0.13f : 0.09f));
    c->release = fm_maxf(80.0f, c->active * 0.28f);
    scale = fm_exp2f((0.5f - size) * 2.8f);
    for (uint32_t k = 0; k < 3; k++) {
        c->freqs[k] = fm_minf(10000.0f, ANAT[c->voice][k] * scale);
        c->radii[k] = fm_expf(-PI * RAD[k] / RATE);
        c->r2[k] = c->radii[k] * c->radii[k];
        c->gains[k] = (1.0f - c->radii[k]) * 0.95f;
        c->coeff[k] = c->y1[k] = c->y2[k] = 0;
    }
    for (uint32_t k = 0; k < c->calls; k++)
        c->motion[k] = syn_rnd(&c->rng) * 2.0f - 1.0f;
    c->flutter_step = (7.0f + c->flutter * 38.0f) / RATE;   /* in turns */
    c->duty = DUTY[c->voice];
    c->phase.hi = syn_rnd(&c->rng);
    c->phase.lo = 0;
    c->flutter_phase = syn_rnd(&c->rng);
    c->prev = c->prev2 = c->noise_low = 0;
    c->i = 0;
    return c->length;
}

static void crittr_run(void *st, float *out, uint32_t n)
{
    crittr_t *c = st;
    for (uint32_t k = 0; k < n; k++, c->i++) {
        uint32_t i = c->i, call;
        float fi = (float)i, q = fm_floorf(fi / c->slot), local, position, envelope, tremor, bend, cycle, pulse;
        float noise, excitation, resonant = 0, sub, throat, trill, artic, s1;
        call = q < (float)(c->calls - 1u) ? (uint32_t)q : c->calls - 1u;
        local = fi - (float)call * c->slot;
        position = fm_minf(1.0f, local / c->active);
        envelope = fm_maxf(0.0f, fm_minf(1.0f, fm_minf(local / c->attack, (c->active - local) / c->release)));
        tremor = fm_sin_turns(c->flutter_phase);
        c->flutter_phase += c->flutter_step;
        if (c->flutter_phase >= 1.0f)
            c->flutter_phase -= 1.0f;
        bend = c->contour * ((position - 0.5f) * 1.9f + c->motion[call] * 0.3f);
        if (c->voice == 6)
            bend += 0.5f * fm_expf(-position * 13.0f) - position * 0.3f;
        if (c->voice == 7)
            bend += 0.5f * fm_sinf(PI * fm_minf(1.0f, position * 1.35f)) - 0.3f * position;
        ff_add(&c->phase, c->pitch * fm_expf(bend * 0.69314718056f) * (1.0f + c->flutter * 0.045f * tremor) / RATE);
        cycle = ff_frac_mul(&c->phase, 1.0f);
        pulse = cycle < c->duty ? 0.5f - 0.5f * fm_cosf(TAU * cycle / c->duty) : 0.0f;
        noise = syn_rnd(&c->rng) * 2.0f - 1.0f;
        c->noise_low += (noise - c->noise_low) * 0.06f;
        excitation = (pulse - c->duty * 0.5f) * 3.4f;
        s1 = fm_sin_turns(cycle);
        switch (c->voice) {
        case 1: excitation += 0.22f * fm_sin_turns(ff_frac_mul(&c->phase, 2.0f) + 0.8f * s1 * ITAU); break;
        case 2: excitation *= 0.7f + 0.3f * ff_sin(&c->phase, 0.5f); break;
        case 3: excitation *= 0.6f + 0.4f * ff_sin(&c->phase, 1.47f); break;
        case 4: excitation = 0.48f * s1 + excitation * 0.3f; break;
        case 5: excitation += 0.2f * ff_sin(&c->phase, 2.71f); break;
        case 6:
            excitation = excitation * (0.85f + 0.15f * ff_sin(&c->phase, 0.5f)) + noise * 0.65f * fm_expf(-position * 20.0f);
            break;
        case 7: excitation += 0.18f * ff_sin(&c->phase, 2.0f) + 0.09f * ff_sin(&c->phase, 3.0f); break;
        default: break;
        }
        excitation = excitation * (1.0f - c->breath * 0.8f) + noise * c->breath * 0.9f;
        if ((i & 31u) == 0) {                    /* the resonances move at a control rate */
            float opening = c->morph * (fm_sinf(PI * position) + 0.3f * c->motion[call]);
            c->coeff[0] = 2.0f * c->radii[0] * fm_cosf(TAU * fm_minf(11000.0f, c->freqs[0] * (1.0f + opening * 0.65f)) / RATE);
            c->coeff[1] = 2.0f * c->radii[1] * fm_cosf(TAU * fm_minf(11000.0f, c->freqs[1] * (1.0f - opening * 0.23f)) / RATE);
            c->coeff[2] = 2.0f * c->radii[2] * fm_cosf(TAU * fm_minf(11000.0f, c->freqs[2] * (1.0f + opening * 0.17f)) / RATE);
            if (c->voice == 6 || c->voice == 7) {    /* a bark opens fast; a meow goes /m/, /a/, /u/ */
                float mouth = c->voice == 6 ? fm_expf(-position * 5.0f) : fm_powf(fm_sinf(PI * position), 1.4f);
                c->coeff[0] = 2.0f * c->radii[0] *
                              fm_cosf(TAU * fm_minf(11000.0f, c->freqs[0] * (0.58f + c->morph * mouth * 0.95f)) / RATE);
                c->coeff[1] = 2.0f * c->radii[1] *
                              fm_cosf(TAU * fm_minf(11000.0f, c->freqs[1] * (1.0f - c->morph * position * 0.58f)) / RATE);
            }
        }
        for (uint32_t b = 0; b < 3; b++) {
            float x = c->gains[b] * (excitation - c->prev2) + c->coeff[b] * c->y1[b] - c->r2[b] * c->y2[b];
            c->y2[b] = c->y1[b];
            c->y1[b] = x;
            resonant += x * (b == 0 ? 1.15f : b == 1 ? 0.85f : 0.5f);
        }
        c->prev2 = c->prev;
        c->prev = excitation;
        sub = c->growl * (0.28f * ff_sin(&c->phase, 0.5f) + 0.1f * ff_sin(&c->phase, 1.0f / 3.0f)) *
              (0.85f + c->noise_low * 0.7f);
        throat = resonant * 1.65f + 0.12f * (1.0f - c->breath) * s1 + sub;
        trill = 1.0f - c->flutter * 0.42f + c->flutter * 0.42f * tremor;
        artic = c->voice == 6 ? fm_expf(-position * 4.5f) : c->voice == 7 ? 0.75f + 0.25f * fm_sinf(PI * position) : 1.0f;
        out[k] = throat * envelope * trill * artic;
    }
}

/* ---- Zappr: seeded leader arcs splitting into branches, over an unstable mains field ---- */
#define ZA_MAXE 192
typedef struct {
    syn_rng_t rng;
    uint32_t start, len;
    float life, freq, gain, phase, low;
} zappr_event_t;
typedef struct {
    syn_rng_t rng;                       /* the field's (the crackle's) */
    uint32_t i, frames, nev;
    float duration, voltage, crackle, hum, spark, decay, hum_step, crackle_fall, crackle_p, tail;
    float phase, crackle_env, noise_low;
    zappr_event_t ev[ZA_MAXE];
} zappr_t;

static COLD uint32_t zappr_begin(void *st, const float *p)
{
    zappr_t *z = st;
    syn_rng_t rng;
    uint32_t arcs, k;
    float branching, spread;
    z->duration = syn_clamp(p[ZAPPR_duration], 0.12f, 5.0f);
    z->frames = syn_len(z->duration);
    arcs = (uint32_t)fm_floorf(syn_clamp(p[ZAPPR_arcs], 1, 24) + 0.5f);
    z->voltage = syn_clamp(p[ZAPPR_voltage], 0, 1);
    branching = syn_clamp(p[ZAPPR_branching], 0, 1);
    z->crackle = syn_clamp(p[ZAPPR_crackle], 0, 1);
    z->hum = syn_clamp(p[ZAPPR_hum], 0, 1);
    z->spark = syn_clamp(p[ZAPPR_spark], 0, 1);
    spread = syn_clamp(p[ZAPPR_spread], 0, 1);
    z->decay = syn_clamp(p[ZAPPR_decay], 0, 1);
    syn_rng_seed(&rng, syn_clamp(p[ZAPPR_seed], 0, 1));
    z->nev = 0;
    for (uint32_t arc = 0; arc < arcs; arc++) {  /* the arcs and their branches: when, how long, how high, how loud */
        float position = arc == 0 ? 0.004f : ((float)arc + syn_rnd(&rng) * 0.65f) / (float)arcs * z->duration *
                                             (0.03f + spread * 0.86f);
        float life = 0.008f + z->decay * 0.1f, freq = 400.0f + z->voltage * 4800.0f * (0.6f + syn_rnd(&rng) * 0.8f);
        uint32_t branches;
        zappr_event_t *e = &z->ev[z->nev++];
        e->start = (uint32_t)fm_floorf(position * RATE + 0.5f);
        e->life = life;
        e->freq = freq;
        e->gain = 0.55f + syn_rnd(&rng) * 0.4f;
        branches = (uint32_t)fm_floorf(branching * (2.0f + syn_rnd(&rng) * 5.0f) + 0.5f);
        for (uint32_t b = 0; b < branches && z->nev < ZA_MAXE; b++) {
            zappr_event_t *f = &z->ev[z->nev++];
            float at = position + life * (0.3f + syn_rnd(&rng) * 2.0f);
            f->start = (uint32_t)fm_floorf(at * RATE + 0.5f);
            f->life = life * (0.18f + syn_rnd(&rng) * 0.55f);
            f->freq = freq * (0.5f + syn_rnd(&rng) * 1.2f);
            f->gain = branching * (0.15f + syn_rnd(&rng) * 0.25f);
        }
    }
    z->rng = rng;                                /* the field: a noise, a crackle's chance, its strength if it cracks */
    z->crackle_p = z->crackle * (12.0f + z->voltage * 130.0f) / RATE;
    for (uint32_t i = 0; i < z->frames; i++) {   /* (its draws counted, for the arcs' place in the rng after it) */
        (void)syn_rnd(&rng);
        if (syn_rnd(&rng) < z->crackle_p)
            (void)syn_rnd(&rng);
    }
    for (k = 0; k < z->nev; k++) {
        zappr_event_t *e = &z->ev[k];
        float span = e->life * RATE * 6.0f, c = fm_floorf(span);
        uint32_t len = (uint32_t)(c < span ? c + 1.0f : c);
        e->len = e->start >= z->frames ? 0 : len < z->frames - e->start ? len : z->frames - e->start;
        e->phase = syn_rnd(&rng) * TAU;
        e->low = 0;
        e->rng = rng;
        rng_skip(&rng, e->len);
    }
    z->hum_step = TAU * (42.0f + z->voltage * 65.0f) / RATE;
    z->crackle_fall = fm_expf(-1.0f / (RATE * (0.001f + z->decay * 0.014f)));
    z->tail = 0.2f + z->decay * 0.6f;
    z->phase = z->crackle_env = z->noise_low = 0;
    z->i = 0;
    return z->frames;
}

static void zappr_run(void *st, float *out, uint32_t n)
{
    zappr_t *z = st;
    for (uint32_t k = 0; k < n; k++) {           /* the field and the crackle */
        uint32_t i = z->i + k;
        float t = (float)i / RATE, noise = syn_rnd(&z->rng) * 2.0f - 1.0f, field, r, x;
        z->noise_low += 0.075f * (noise - z->noise_low);
        z->phase += z->hum_step;
        if (z->phase > TAU)
            z->phase -= TAU;
        if (syn_rnd(&z->rng) < z->crackle_p)
            z->crackle_env = 0.15f + syn_rnd(&z->rng) * 0.55f;
        z->crackle_env *= z->crackle_fall;
        r = z->phase > PI ? z->phase - TAU : z->phase;
        x = fm_sin_pi(r);
        field = z->hum * (x + 0.28f * x * (3.0f - 4.0f * x * x) + 0.15f * fm_sinf(z->phase * 7.0f)) * 0.27f;
        out[k] = (field + z->crackle_env * (noise - z->noise_low) * z->crackle) * fm_minf(1.0f, t / 0.006f) *
                 fm_powf(fm_maxf(0.0f, 1.0f - t / z->duration), z->tail);
    }
    for (uint32_t ei = 0; ei < z->nev; ei++) {   /* the arcs, in their order */
        zappr_event_t *e = &z->ev[ei];
        uint32_t a = z->i > e->start ? z->i : e->start, end = e->start + e->len, b = z->i + n < end ? z->i + n : end;
        float dk, d, rs = 75.0f + z->voltage * 290.0f;
        if (a >= b)
            continue;
        d = fm_expf(-(float)(a - e->start) / RATE / e->life);
        dk = fm_expf(-1.0f / (RATE * e->life));
        for (uint32_t i = a; i < b; i++) {
            uint32_t j = i - e->start;
            float age = (float)j / RATE, noise = syn_rnd(&e->rng) * 2.0f - 1.0f, discharge, restrike, tt;
            e->phase += TAU * e->freq * (0.3f + 0.7f * d) / RATE;
            if (e->phase > TAU)
                e->phase -= TAU;
            e->low += 0.2f * (noise - e->low);
            discharge = (0.4f + z->spark * 0.5f) * (noise - e->low) +
                        fm_sinf(e->phase + fm_sinf(e->phase * 0.47f) * z->voltage * 2.0f) * (1.0f - z->spark * 0.65f);
            tt = age * rs;
            restrike = 0.65f + 0.35f * fm_maxf(0.0f, fm_sin_turns(tt - fm_floorf(tt)));
            out[i - z->i] += discharge * e->gain * d * restrike * fm_minf(1.0f, (float)j / 12.0f);
            d = fm_flush(d * dk);
        }
    }
    z->i += n;
}

/* ---- Signlr: packets of symbols, frequency or phase coded, with dropouts and radio echoes; or sonar pings ---- */
#define SI_HIST 9100u
typedef struct {
    syn_rng_t rng;
    uint32_t i, length, encoding, packets, delay_a, delay_b;
    float carrier, deviation, drift, interference, corruption, echo, symbol_samples, slot, active, edge, symbol_edge;
    float carrier0, het_step, heterodyne, bit, symbol_gain, phase_code, scramble, noise_low, radio_low;
    int32_t last_symbol, last_packet;
    uint32_t packet, symbol;             /* where it is, worked out exactly (bfxr2 does it in double) */
    ff_t symbols;                        /* symbols a second */
    ff_t phase;                          /* in turns */
    /* the pings */
    float ratios[4], weights[4], phases[4], ring, step, root, color, echo_g;
    uint32_t echo_d[4];
    int16_t hist[SI_HIST];               /* the output, for the echoes (16 bits: -8 .. 8) */
} signlr_t;

static COLD uint32_t signlr_begin(void *st, const float *p)
{
    signlr_t *g = st;
    float seed = syn_clamp(p[SIGNLR_seed], 0, 1), symbols, gap;
    g->length = syn_len(syn_clamp(p[SIGNLR_duration], 0.15f, 5.0f));
    syn_rng_seed(&g->rng, seed);
    g->encoding = (uint32_t)fm_floorf(syn_clamp(p[SIGNLR_encoding], 0, 4) + 0.5f);
    g->packets = (uint32_t)fm_floorf(syn_clamp(p[SIGNLR_packets], 1, 12) + 0.5f);
    g->drift = syn_clamp(p[SIGNLR_drift], -1, 1);
    g->echo = syn_clamp(p[SIGNLR_echo], 0, 1);
    g->deviation = syn_clamp(p[SIGNLR_deviation], 0, 1);
    gap = syn_clamp(p[SIGNLR_gap], 0, 0.85f);
    g->i = 0;
    if (g->encoding == 4) {                      /* pings: one struck resonator, shared by the whole scan */
        float duration = syn_clamp(p[SIGNLR_duration], 0.15f, 5.0f), fall = gap;
        static const float W0[4] = {0.8f, 0.06f, 0.03f, 0.02f}, WC[4] = {0, 0.28f, 0.17f, 0.1f};
        g->root = 170.0f * fm_exp2f(syn_clamp(p[SIGNLR_carrier], 0, 1) * 4.1f);
        g->color = g->deviation;
        g->ratios[0] = 1;
        g->ratios[1] = 2.01f + syn_rnd(&g->rng) * 0.12f;
        g->ratios[2] = 3.1f + syn_rnd(&g->rng) * 1.7f;
        g->ratios[3] = 5.2f + syn_rnd(&g->rng) * 2.2f;
        for (uint32_t h = 0; h < 4; h++) {
            g->weights[h] = W0[h] + g->color * WC[h];
            g->phases[h] = syn_rnd(&g->rng) * TAU;
        }
        g->ring = 0.045f + (1.0f - fall) * 0.6f;
        g->step = duration / (float)g->packets;
        g->echo_g = g->echo * 0.52f;
        for (uint32_t r = 1; r <= 3; r++)
            g->echo_d[r] = (uint32_t)fm_floorf(RATE * (0.062f + seed * 0.055f) * (float)r + 0.5f);
        return g->length;
    }
    g->carrier = 90.0f * fm_exp2f(syn_clamp(p[SIGNLR_carrier], 0, 1) * 5.7f);
    g->interference = syn_clamp(p[SIGNLR_interference], 0, 1);
    g->corruption = syn_clamp(p[SIGNLR_corruption], 0, 1);
    symbols = syn_clamp(p[SIGNLR_symbols], 0, 1);
    {
        ff_t x2 = ff_two_prod(symbols, symbols), m = ff_two_prod(176.0f, x2.hi);
        m.lo += 176.0f * x2.lo;
        g->symbols.hi = m.hi + 4.0f;
        g->symbols.lo = m.lo + ((4.0f - g->symbols.hi) + m.hi);
    }
    symbols = 4.0f + 176.0f * symbols * symbols;
    g->symbol_samples = RATE / symbols;
    g->packet = 0;
    g->symbol = 0;
    g->slot = (float)g->length / (float)g->packets;
    g->active = g->slot * (1.0f - gap);
    g->edge = fm_minf(RATE * 0.005f, g->active * 0.12f);
    g->symbol_edge = fm_minf(RATE * 0.0007f, g->symbol_samples * 0.06f);
    g->delay_a = (uint32_t)fm_floorf(RATE * (0.073f + seed * 0.041f) + 0.5f);
    g->delay_b = (uint32_t)fm_floorf((float)g->delay_a * 1.79f + 0.5f);
    g->phase.hi = syn_rnd(&g->rng);
    g->phase.lo = 0;
    g->last_symbol = g->last_packet = -1;
    g->bit = g->symbol_gain = 1;
    g->noise_low = g->radio_low = g->phase_code = g->scramble = 0;
    g->carrier0 = g->carrier * fm_exp2f(-g->drift * 0.5f);
    g->het_step = (g->carrier * 1.013f + 37.0f) / RATE;   /* in turns */
    g->heterodyne = syn_rnd(&g->rng);
    for (uint32_t k = 0; k < SI_HIST; k++)
        g->hist[k] = 0;
    return g->length;
}

static float signlr_ping(const signlr_t *g, uint32_t packet, uint32_t i)   /* a ping's sample i, before its echoes */
{
    float f = g->root * fm_exp2f(g->drift * (float)packet / (float)(g->packets > 1u ? g->packets - 1u : 1u));
    float t = (float)i / RATE, attack = 1.0f - fm_expf(-t / 0.0007f), sample = 0;
    float bend = g->color * 0.0008f * (1.0f - fm_expf(-t / 0.006f));
    for (uint32_t h = 0; h < 4; h++) {
        float fr = f * g->ratios[h], x;
        if (fr > 18000.0f)
            continue;
        x = fr * (float)i / RATE;                /* in turns: the whole turns dropped first */
        x = (x - fm_floorf(x)) + fr * bend + g->phases[h] * ITAU;
        sample += fm_sin_turns(x - fm_floorf(x)) * g->weights[h] * fm_expf(-t * (1.0f + (float)h * 1.4f) / g->ring);
    }
    return sample * attack * 0.7f;
}

static void signlr_run(void *st, float *out, uint32_t n)
{
    signlr_t *g = st;
    if (g->encoding == 4) {                      /* the pings and their three echoes */
        float span = RATE * g->ring * 6.0f;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t i = g->i + k;
            float x = 0;
            for (uint32_t pk = 0; pk < g->packets; pk++) {
                uint32_t start = (uint32_t)fm_floorf((float)pk * g->step * RATE + 0.5f);
                float g_r = 1.0f;
                if (i < start)
                    break;
                for (uint32_t r = 0; r <= 3; r++) {
                    uint32_t d = r ? g->echo_d[r] : 0, j;
                    if (r)
                        g_r *= g->echo_g;
                    if (i < start + d)
                        break;
                    j = i - start - d;
                    if ((float)j < span && j < g->length - start)
                        x += signlr_ping(g, pk, j) * g_r;
                }
            }
            out[k] = x;
        }
        g->i += n;
        return;
    }
    {
        /* the carrier's drift: exact at the block's start, stepped from there */
        float carrier = g->carrier0 * fm_exp2f(g->drift * (float)g->i / (float)g->length);
        float dstep = fm_exp2f(g->drift / (float)g->length);
        for (uint32_t k = 0; k < n; k++, g->i++) {
            uint32_t i = g->i, packet, num;
            float local, pos, unit, f, noise, enc, pe, be, radio, sample, ph;
            int32_t symbol;
            /* the packet, i / (length / packets), and the symbol, (i - packet * slot) / (RATE / symbols): exactly */
            packet = i * g->packets / g->length;
            if (packet > g->packets - 1u)
                packet = g->packets - 1u;
            if (packet != g->packet) {
                g->packet = packet;
                g->symbol = 0;
            }
            num = i * g->packets - packet * g->length;          /* local * packets, a whole number */
            {
                ff_t a = ff_two_prod((float)num, g->symbols.hi);
                a.lo += (float)num * g->symbols.lo;
                while (ff_ge(a, ff_two_prod((float)(g->symbol + 1u), RATE * (float)g->packets)))
                    g->symbol++;
            }
            symbol = (int32_t)g->symbol;
            local = (float)num / (float)g->packets;
            pos = local - (float)symbol * g->symbol_samples;
            unit = pos / g->symbol_samples;
            if ((int32_t)packet != g->last_packet || symbol != g->last_symbol) {
                g->last_packet = (int32_t)packet;
                g->last_symbol = symbol;
                g->bit = symbol < 4 ? (((uint32_t)symbol + packet) % 2u ? 1.0f : -1.0f)   /* a sync word, then the payload */
                                    : (syn_rnd(&g->rng) < 0.5f ? -1.0f : 1.0f);
                g->symbol_gain = syn_rnd(&g->rng) < g->corruption * 0.85f ? 0.025f : 1.0f;
                g->phase_code = g->bit > 0 ? 0.0f : PI;
                g->scramble = g->corruption * (syn_rnd(&g->rng) * 2.0f - 1.0f);
            }
            f = carrier;
            if (g->encoding == 0)
                f *= fm_exp2f(g->deviation * g->bit * 0.7f + g->scramble * 0.2f);
            if (g->encoding == 2)
                f *= fm_exp2f(g->deviation * (1.0f - unit * 2.0f) + g->scramble * 0.2f);
            if (g->encoding == 3)
                f *= 1.0f + g->deviation * 0.3f * g->bit;
            ff_add(&g->phase, fm_minf(14000.0f, fm_maxf(30.0f, f)) / RATE);
            carrier *= dstep;
            g->heterodyne += g->het_step;
            if (g->heterodyne >= 1.0f)
                g->heterodyne -= 1.0f;
            noise = syn_rnd(&g->rng) * 2.0f - 1.0f;
            g->noise_low += (noise - g->noise_low) * 0.08f;
            ph = ff_frac_mul(&g->phase, 1.0f);
            if (g->encoding == 1)
                enc = fm_sin_turns(ph + g->phase_code * (0.25f + g->deviation * 0.75f) * ITAU);
            else if (g->encoding == 2)
                enc = fm_sin_turns(ph) * fm_expf(-unit * (5.0f + g->deviation * 5.0f));
            else if (g->encoding == 3)
                enc = fm_sin_turns(ph) * (0.62f + 0.38f * fm_sin_turns(ff_frac_mul(&g->phase, 0.037f) + g->bit * ITAU));
            else
                enc = fm_sin_turns(ph) + 0.08f * g->deviation * ff_sin(&g->phase, 2.0f);
            pe = fm_maxf(0.0f, fm_minf(1.0f, fm_minf(local / g->edge, (g->active - local) / g->edge)));
            be = g->encoding == 2 ? fm_minf(1.0f, pos / g->symbol_edge)
                                  : 0.88f + 0.12f * fm_minf(1.0f, fm_minf(pos / g->symbol_edge,
                                                                          (g->symbol_samples - pos) / g->symbol_edge));
            radio = (noise - g->noise_low) * 0.22f + fm_sin_turns(g->heterodyne) * 0.14f;
            sample = enc * 0.52f * g->symbol_gain * pe * be + radio * g->interference * (0.4f + 0.6f * pe);
            g->radio_low += (sample - g->radio_low) * 0.3f;   /* a narrow receiver: radio mode's static rolled off */
            if (g->encoding == 3)
                sample = g->radio_low;
            if (i >= g->delay_a)
                sample += (float)g->hist[(i - g->delay_a) % SI_HIST] * (8.0f / 32767.0f) * g->echo * 0.38f;
            if (i >= g->delay_b)
                sample -= (float)g->hist[(i - g->delay_b) % SI_HIST] * (8.0f / 32767.0f) * g->echo * 0.17f;
            g->hist[i % SI_HIST] = (int16_t)(fm_clampf(sample, -8.0f, 8.0f) * (32767.0f / 8.0f));
            out[k] = sample;
        }
    }
}

/* atan2 for a synth's setup (Cephes' atanf: 1e-7) */
static COLD float s_atan2f(float y, float x)
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a, r = 0, z, t;
    if (ax == 0 && ay == 0)
        return 0;
    a = ay > ax ? ax / ay : ay / ax;             /* 0 .. 1 */
    if (a > 0.414213562f) {
        t = (a - 1.0f) / (a + 1.0f);
        r = 0.785398163f;
    } else
        t = a;
    z = t * t;
    r += (((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z - 3.33329491539e-1f) * z * t + t;
    if (ay > ax)
        r = 1.57079633f - r;
    if (x < 0)
        r = PI - r;
    return y < 0 ? -r : r;
}

/* ---- Pluckr: waveguide strings of a material, sharing a damped bridge, plucked into shape ---- */
#define PL_POOL 2900u                    /* the strings' delay lines */
#define PL_DRY 3120u                     /* the vibrato's look back */
typedef struct {
    uint32_t off, delay, index, start;
    float previous, low, allpass, allpass_in, fraction, feedback, wave_in[2], wave_out[2];
} pluckr_string_t;
typedef struct {
    uint32_t i, length, count, material, stages, dry_n;
    float brightness, coupling, tremolo, vibrato, speed, gain, bridge, depth;
    float average, filter, bright, dispersion;
    pluckr_string_t s[8];
    float buf[PL_POOL];
    float dry[PL_DRY];                   /* the strings' sum, for the vibrato */
} pluckr_t;

static COLD uint32_t pluckr_begin(void *st, const float *p)
{
    static const struct { float life, average, filter, bright, excite, dispersion; uint8_t stages; } M[6] = {
        {1, 0.5f, 0.18f, 0.8f, 1, 0, 0}, {1.45f, 0.14f, 0.65f, 0.35f, 1.5f, 0.2f, 1}, {0.62f, 0.55f, 0.12f, 0.5f, 0.6f, 0.6f, 1},
        {0.075f, 0.65f, 0.1f, 0.28f, 0.35f, 0.4f, 1}, {1.3f, 0.06f, 0.85f, 0.15f, 1.8f, -0.72f, 2},
        {0.95f, 0.25f, 0.4f, 0.5f, 1.1f, -0.45f, 2}};
    static const float NOTES[8] = {0, 7, 12, 16, 19, 24, 28, 31};
    pluckr_t *k = st;
    syn_rng_t rng;
    float pitch, damping, strum, pluck, inharmonic;
    uint32_t off = 0;
    k->length = syn_len(syn_clamp(p[PLUCKR_duration], 0.15f, 5.0f));
    syn_rng_seed(&rng, syn_clamp(p[PLUCKR_seed], 0, 1));
    k->count = (uint32_t)fm_floorf(syn_clamp(p[PLUCKR_strings], 1, 8) + 0.5f);
    k->material = (uint32_t)fm_floorf(syn_clamp(p[PLUCKR_material], 0, 5) + 0.5f);
    pitch = 55.0f * fm_exp2f(syn_clamp(p[PLUCKR_pitch], 0, 1) * 4.0f);
    damping = syn_clamp(p[PLUCKR_damping], 0, 1);
    k->brightness = syn_clamp(p[PLUCKR_brightness], 0, 1);
    k->coupling = syn_clamp(p[PLUCKR_coupling], 0, 1);
    strum = syn_clamp(p[PLUCKR_strum], 0, 1);
    pluck = syn_clamp(p[PLUCKR_pluck], 0, 1);
    inharmonic = syn_clamp(p[PLUCKR_inharmonic], 0, 1);
    k->tremolo = syn_clamp(p[PLUCKR_tremolo], 0, 1);
    k->vibrato = syn_clamp(p[PLUCKR_vibrato], 0, 1);
    k->speed = syn_clamp(p[PLUCKR_tremoloRate], 0.2f, 12.0f);
    k->average = M[k->material].average;
    k->filter = M[k->material].filter;
    k->bright = M[k->material].bright;
    k->dispersion = M[k->material].dispersion;
    k->stages = M[k->material].stages;
    k->gain = 0.65f / fm_sqrtf((float)k->count);
    for (uint32_t si = 0; si < k->count; si++) {
        pluckr_string_t *g = &k->s[si];
        float f = pitch * fm_exp2f(NOTES[si] / 12.0f + (syn_rnd(&rng) - 0.5f) * inharmonic * 0.07f);
        float period = RATE / f, omega = TAU / period, a = k->dispersion, low = 0, mean = 0, loc = 0.05f + pluck * 0.9f;
        float dd = (float)k->stages * 2.0f * s_atan2f((1.0f - a) * fm_sinf(omega / 2.0f), (1.0f + a) * fm_cosf(omega / 2.0f)) / omega;
        float desired = fm_maxf(2.0f, period - k->average - dd), ex = fm_minf(1.0f, (0.05f + k->brightness * 0.9f) * M[k->material].excite);
        uint32_t delay = (uint32_t)fm_maxf(2.0f, fm_floorf(desired));
        if (off + delay > PL_POOL)
            delay = PL_POOL - off;               /* (never, for the strings' pitches) */
        g->off = off;
        g->delay = delay;
        for (uint32_t i = 0; i < delay; i++) {   /* the pluck: shaped noise and a triangle at the plucked place */
            float x = (float)i / (float)delay, tri = x < loc ? x / loc : (1.0f - x) / (1.0f - loc);
            low += (syn_rnd(&rng) * 2.0f - 1.0f - low) * ex;
            k->buf[off + i] = low * 0.75f + (tri - 0.5f) * (1.0f - k->brightness) * 0.75f;
            mean += k->buf[off + i];
        }
        mean /= (float)delay;
        for (uint32_t i = 0; i < delay; i++)
            k->buf[off + i] -= mean;
        off += delay;
        g->index = 0;
        g->previous = g->low = g->allpass = g->allpass_in = 0;
        g->wave_in[0] = g->wave_in[1] = g->wave_out[0] = g->wave_out[1] = 0;
        g->fraction = (1.0f - (desired - (float)delay)) / (1.0f + (desired - (float)delay));
        g->feedback = fm_expf(-3.0f / (f * (0.07f + (1.0f - damping) * (1.0f - damping) * 4.0f) * M[k->material].life));
        g->start = (uint32_t)fm_floorf((float)si * strum * fm_minf(RATE * 0.11f, (float)k->length / (float)k->count * 0.65f) + 0.5f);
    }
    k->bridge = 0;
    k->depth = (fm_exp2f(k->vibrato * 0.75f / 12.0f) - 1.0f) * RATE / (TAU * k->speed);
    k->dry_n = (uint32_t)(k->depth * 2.0f) + 2u;
    if (k->dry_n > PL_DRY)
        k->dry_n = PL_DRY;
    k->i = 0;
    return k->length;
}

static void pluckr_run(void *st, float *out, uint32_t n)
{
    pluckr_t *k = st;
    for (uint32_t q = 0; q < n; q++, k->i++) {
        uint32_t i = k->i;
        float sum = 0, bridge_next = 0, motion, dry;
        float disp = k->dispersion + (k->material == 5 ? 0.22f * fm_sinf(TAU * 1.7f * (float)i / RATE) : 0.0f);
        for (uint32_t si = 0; si < k->count; si++) {
            pluckr_string_t *g = &k->s[si];
            float cur, avg, in, d;
            if (i < g->start)
                continue;
            cur = k->buf[g->off + g->index];
            avg = cur * (1.0f - k->average) + g->previous * k->average;
            g->previous = cur;
            g->low += (avg - g->low) * (k->filter + k->brightness * k->bright);
            in = g->low;
            d = g->fraction * in + g->allpass_in - g->fraction * g->allpass;
            g->allpass_in = in;
            g->allpass = d;
            for (uint32_t sg = 0; sg < k->stages; sg++) {
                float nx = disp * d + g->wave_in[sg] - disp * g->wave_out[sg];
                g->wave_in[sg] = d;
                g->wave_out[sg] = nx;
                d = nx;
            }
            k->buf[g->off + g->index] = g->feedback * (d * (1.0f - k->coupling * 0.025f) + k->bridge * k->coupling * 0.025f);
            if (++g->index == g->delay)
                g->index = 0;
            sum += cur;
            bridge_next += cur;
        }
        k->bridge = bridge_next / (float)k->count;
        {
            float x = k->speed * (float)i / RATE;
            motion = 0.5f - 0.5f * fm_sin_turns(x - fm_floorf(x) + 0.25f);
        }
        dry = sum * k->gain;
        if (k->vibrato <= 0) {
            out[q] = dry * (1.0f - k->tremolo * motion);
            continue;
        }
        k->dry[i % k->dry_n] = dry;              /* a smoothly moving delay bends the pitch */
        {
            float pos = fm_maxf(0.0f, (float)i - k->depth * 2.0f * motion), fr;
            uint32_t idx = (uint32_t)pos, nx = idx + 1u < k->length ? idx + 1u : k->length - 1u;
            fr = pos - (float)idx;
            if (nx > i)                          /* (the sample after this one: not made yet, and dry[] is causal) */
                nx = i;
            out[q] = (k->dry[idx % k->dry_n] * (1.0f - fr) + k->dry[nx % k->dry_n] * fr) * (1.0f - k->tremolo * motion);
        }
    }
}

/* ---- Rustlr: friction gathers tension and lets go in slips and folds; each material its own give and loss ---- */
#define RU_MAXG 2520                     /* grains and folds */
#define RU_ACTIVE 160                    /* sounding at once */
typedef struct { uint32_t rng, at; } rustlr_rec_t;   /* at: start << 12 | the grain's number */
typedef struct {
    syn_rng_t rng;
    uint16_t idx;
    uint8_t crease;
    uint32_t start, length, j, end;
    float strength, low, slow, flex, prev, decay, dk;
} rustlr_grain_t;
typedef struct {
    syn_rng_t rng;                       /* the friction bed's */
    uint32_t i, frames, kind, gesture, folds, count, ngr, next, nact;
    float motion, pressure, filter, slow_filter, flex_filter, soft, highpass, crease, stick, flex_hz, gain, density;
    float slip_loss, friction, friction_slow, shear, slip, motion_low, breakaway;
    rustlr_rec_t rec[RU_MAXG];           /* by start */
    rustlr_grain_t act[RU_ACTIVE];       /* by number */
} rustlr_t;

static const struct { float cutoff, soft, highpass, crease, stick, flex; } RU_MAT[6] = {
    {4200, 0.35f, 0.65f, 0.85f, 0.5f, 720}, {950, 1, 0.2f, 0.08f, 0.15f, 180}, {1900, 0.65f, 0.32f, 0.38f, 1, 330},
    {6400, 0.25f, 0.72f, 1.1f, 0.6f, 1150}, {12500, 0.06f, 0.92f, 1.35f, 0.3f, 2600}, {4500, 0.15f, 0.55f, 0.5f, 0.2f, 1500}};

/* grain g's numbers, drawn from its place in the rng: its start, length and strength (its noise follows) */
static void rustlr_grain(const rustlr_t *r, const float *centers, uint32_t g, syn_rng_t *rng, uint32_t *start,
                         uint32_t *length, float *strength, float grain_frames, float fold_len)
{
    float position;
    int32_t st;
    uint32_t len;
    if (g < r->count) {
        if (r->kind == 5)                        /* a zip's teeth follow the pull, with small defects */
            position = fm_powf(((float)g + 0.25f + syn_rnd(rng) * 0.5f) / (float)r->count, 1.35f - r->motion * 0.35f);
        else {
            float c = centers[(uint32_t)(syn_rnd(rng) * (float)(r->folds + 2u))], a = syn_rnd(rng), b = syn_rnd(rng);
            position = fm_maxf(0.0f, fm_minf(0.96f, c + (a + b - 1.0f) * (0.035f + r->density * 0.24f)));
        }
        len = (uint32_t)fm_maxf(6.0f, fm_floorf(grain_frames * (0.45f + syn_rnd(rng) * 1.1f) + 0.5f));
        st = (int32_t)fm_floorf(position * ((float)r->frames - (float)len) + 0.5f);
        *strength = r->gain * (0.16f + syn_rnd(rng) * 0.3f) * (r->kind == 5 ? 1.4f : 1.0f);
    } else {
        uint32_t fold = g - r->count;
        position = ((float)fold + 0.3f + syn_rnd(rng) * 0.45f) / (float)r->folds;
        len = (uint32_t)fm_maxf(8.0f, fm_floorf(fold_len * (0.65f + syn_rnd(rng) * 0.65f) + 0.5f));
        st = (int32_t)fm_floorf(position * ((float)r->frames - (float)len) + 0.5f);
        *strength = r->gain * (0.42f + syn_rnd(rng) * 0.45f) * r->crease;
    }
    *start = st > 0 ? (uint32_t)st : 0u;
    *length = len;
}

typedef struct { float centers[14], grain_frames, fold_len; } rustlr_shape_t;
static rustlr_shape_t ru_shape;          /* (the grains' shared numbers: kept beside the state) */

static COLD uint32_t rustlr_begin(void *st, const float *p)
{
    rustlr_t *r = st;
    syn_rng_t rng;
    float duration = syn_clamp(p[RUSTLR_duration], 0.08f, 3.0f), grain, brightness, cutoff, normalizer;
    uint32_t clusters, n;
    r->frames = syn_len(duration);
    r->kind = (uint32_t)fm_floorf(syn_clamp(p[RUSTLR_material], 0, 5) + 0.5f);
    r->gesture = (uint32_t)fm_floorf(syn_clamp(p[RUSTLR_gesture], 0, 3) + 0.5f);
    grain = syn_clamp(p[RUSTLR_grain], 0, 1);
    r->density = syn_clamp(p[RUSTLR_density], 0, 1);
    r->motion = syn_clamp(p[RUSTLR_motion], -1, 1);
    r->pressure = syn_clamp(p[RUSTLR_pressure], 0, 1);
    brightness = syn_clamp(p[RUSTLR_brightness], 0, 1);
    r->folds = (uint32_t)fm_floorf(syn_clamp(p[RUSTLR_folds], 1, 12) + 0.5f);
    syn_rng_seed(&rng, syn_clamp(p[RUSTLR_seed], 0, 1));
    r->soft = RU_MAT[r->kind].soft;
    r->highpass = RU_MAT[r->kind].highpass;
    r->crease = RU_MAT[r->kind].crease;
    r->stick = RU_MAT[r->kind].stick;
    r->flex_hz = RU_MAT[r->kind].flex;
    cutoff = RU_MAT[r->kind].cutoff * (0.22f + brightness * 1.18f) / (1.0f + grain * 0.65f) * (0.8f + r->pressure * 0.4f);
    r->filter = 1.0f - fm_expf(-TAU * cutoff / RATE);
    r->slow_filter = 1.0f - fm_expf(-TAU * fm_maxf(60.0f, cutoff * 0.13f) / RATE);
    r->flex_filter = 1.0f - fm_expf(-TAU * r->flex_hz * (0.7f + r->pressure * 0.8f) / RATE);
    ru_shape.grain_frames = fm_maxf(8.0f, fm_floorf((0.0007f + grain * grain * 0.023f) * (1.0f + r->soft) * RATE + 0.5f));
    ru_shape.fold_len = (0.003f + grain * 0.022f + r->soft * 0.016f) * RATE;
    r->count = (uint32_t)fm_maxf(8.0f, fm_floorf(duration * (48.0f + r->density * 780.0f) / (1.0f + grain * 1.4f) + 0.5f));
    if (r->count + r->folds > RU_MAXG)
        r->count = RU_MAXG - r->folds;
    clusters = r->folds + 2u;
    for (uint32_t c = 0; c < clusters; c++)
        ru_shape.centers[c] = ((float)c + 0.2f + syn_rnd(&rng) * 0.6f) / (float)clusters;
    normalizer = 1.0f / fm_sqrtf(1.0f + (float)r->count * ru_shape.grain_frames / (float)r->frames * 0.12f);
    r->gain = (0.24f + r->pressure * 0.76f) * normalizer * 2.0f;
    r->ngr = r->count + r->folds;
    for (uint32_t g = 0; g < r->ngr; g++) {      /* each grain's place in the rng, and when it starts */
        uint32_t start, length;
        float strength;
        r->rec[g].rng = rng.s;
        rustlr_grain(r, ru_shape.centers, g, &rng, &start, &length, &strength, ru_shape.grain_frames, ru_shape.fold_len);
        n = start < r->frames ? (length < r->frames - start ? length : r->frames - start) : 0u;
        rng_skip(&rng, n);
        r->rec[g].at = start << 12 | g;
    }
    for (uint32_t a = 1; a < r->ngr; a++) {      /* by start (then number): an insertion sort, nearly in order */
        rustlr_rec_t x = r->rec[a];
        uint32_t b = a;
        while (b > 0 && r->rec[b - 1u].at > x.at) {
            r->rec[b] = r->rec[b - 1u];
            b--;
        }
        r->rec[b] = x;
    }
    r->rng = rng;                                /* the bed */
    r->breakaway = 0.7f + syn_rnd(&r->rng) * 0.6f;
    r->slip_loss = fm_expf(-1.0f / ((0.0015f + r->soft * 0.017f) * (0.6f + r->pressure) * RATE));
    r->friction = r->friction_slow = r->shear = r->slip = r->motion_low = 0;
    r->next = r->nact = 0;
    r->i = 0;
    return r->frames;
}

static void rustlr_run(void *st, float *out, uint32_t n)
{
    rustlr_t *r = st;
    uint32_t b1 = r->i + n;
    for (uint32_t k = 0; k < n; k++)
        out[k] = 0;
    while (r->next < r->ngr && (r->rec[r->next].at >> 12) < b1) {   /* the grains starting in this block */
        rustlr_rec_t *rc = &r->rec[r->next++];
        uint32_t g = rc->at & 0xFFFu, a, start, length;
        rustlr_grain_t *x;
        float strength;
        syn_rng_t rng = {rc->rng};
        if (r->nact >= RU_ACTIVE)
            continue;                            /* (more at once than room for: dropped) */
        rustlr_grain(r, ru_shape.centers, g, &rng, &start, &length, &strength, ru_shape.grain_frames, ru_shape.fold_len);
        for (a = r->nact; a > 0 && r->act[a - 1u].idx > g; a--)
            r->act[a] = r->act[a - 1u];
        x = &r->act[a];
        r->nact++;
        x->rng = rng;
        x->idx = (uint16_t)g;
        x->crease = g >= r->count;
        x->start = start;
        x->length = length;
        x->end = start + (length < r->frames - start ? length : r->frames - start);
        x->j = 0;
        x->strength = strength;
        x->low = x->slow = x->flex = x->prev = 0;
        x->decay = 1.0f;                         /* exp(-age (5 - soft 2)), stepped */
        x->dk = fm_expf(-(5.0f - r->soft * 2.0f) / (float)length);
    }
    for (uint32_t a = 0; a < r->nact; a++) {     /* the grains sounding, in their order */
        rustlr_grain_t *x = &r->act[a];
        float attack = x->crease ? 0.025f + r->soft * 0.22f : 0.5f, inv = 1.0f / (float)x->length;
        float bend_k = RATE / (r->flex_hz * TAU) * (0.5f + r->soft);
        while (x->j < x->length && x->start + x->j < b1 && x->start + x->j < x->end) {
            uint32_t i = x->start + x->j;
            float noise = syn_rnd(&x->rng) * 2.0f - 1.0f, age = (float)x->j * inv, window, surface, v;
            x->low += (noise - x->low) * r->filter;
            x->slow += (x->low - x->slow) * r->slow_filter;
            x->flex += (x->low - x->flex) * r->flex_filter;
            if (x->crease)                       /* a fold bends for longer; a thin film lets go at once */
                window = fm_minf(1.0f, age / attack) * x->decay;
            else {
                float sn = fm_sinf(PI * age);
                window = sn * sn;
            }
            surface = x->low - x->slow * r->highpass;
            v = (surface + (x->crease ? (x->flex - x->prev) * bend_k : 0.0f)) * window * x->strength;
            out[i - r->i] += v;
            x->prev = x->flex;
            x->decay *= x->dk;
            x->j++;
        }
        if (x->j >= x->length || x->start + x->j >= x->end) {   /* done: out of the list */
            for (uint32_t b = a + 1u; b < r->nact; b++)
                r->act[b - 1u] = r->act[b];
            r->nact--;
            a--;
        }
    }
    for (uint32_t k = 0; k < n; k++, r->i++) {   /* the friction bed, loading and slipping; the gesture's shape */
        uint32_t i = r->i;
        float position = (float)i / (float)(r->frames - 1u), arch = fm_maxf(0.0f, fm_sinf(PI * position)), speed, bed, env, travel;
        r->friction += (syn_rnd(&r->rng) * 2.0f - 1.0f - r->friction) * r->filter;
        r->friction_slow += (r->friction - r->friction_slow) * r->slow_filter;
        r->motion_low += (syn_rnd(&r->rng) * 2.0f - 1.0f - r->motion_low) * 0.0012f;
        speed = (0.3f + fm_powf(arch, 0.6f)) * (0.7f + fm_minf(0.8f, fm_fabsf(r->motion_low) * 12.0f));
        r->shear += speed * (30.0f + r->density * 130.0f) / RATE / (0.6f + r->pressure * r->stick * 2.0f);
        if (r->shear > r->breakaway) {
            r->shear -= r->breakaway;
            r->slip += 0.45f + r->pressure * r->stick * 1.5f;
            r->breakaway = 0.65f + syn_rnd(&r->rng) * 0.8f;
        }
        r->slip *= r->slip_loss;
        bed = (r->friction - r->friction_slow * r->highpass) * r->gain * (0.07f + r->density * 0.12f) * (0.6f + r->soft * 1.3f) *
              speed * (0.45f + r->stick * r->slip + r->shear * 0.35f);
        if (r->gesture == 0)
            env = fm_powf(arch, 0.35f) * fm_expf(-position * 4.5f) * 2.0f;
        else if (r->gesture == 1)
            env = fm_powf(arch, 0.7f) * (0.55f + 0.45f * fm_fabsf(fm_sinf(TAU * position)));
        else if (r->gesture == 2)
            env = fm_powf(arch, 0.45f);
        else
            env = fm_powf(arch, 0.5f) * (0.2f + 0.8f * fm_fabsf(fm_sinf(PI * (float)r->folds * position)));
        travel = fm_expf(r->motion * (position - 0.5f) * 5.0f - fm_fabsf(r->motion) * 1.4f);
        out[k] = (out[k] + bed) * env * travel;
    }
}

const syn_engine_t SYN_ENGINES[] = {
    {whooshr_begin, whooshr_run},
    {choirr_begin, choirr_run},
    {breathr_begin, breathr_run},
    {birdr_begin, birdr_run},
    {bouncr_begin, bouncr_run},
    {swarmr_begin, swarmr_run},
    {machinr_begin, machinr_run},
    {crittr_begin, crittr_run},
    {zappr_begin, zappr_run},
    {signlr_begin, signlr_run},
    {pluckr_begin, pluckr_run},
    {rustlr_begin, rustlr_run},
};
_Static_assert(sizeof(SYN_ENGINES) / sizeof(SYN_ENGINES[0]) == SYN_NPRESET, "an engine for every synth");
_Static_assert(sizeof(whooshr_t) <= SYN_STATE_BYTES && sizeof(choirr_t) <= SYN_STATE_BYTES &&
               sizeof(breathr_t) <= SYN_STATE_BYTES && sizeof(birdr_t) <= SYN_STATE_BYTES &&
               sizeof(bouncr_t) <= SYN_STATE_BYTES && sizeof(swarmr_t) <= SYN_STATE_BYTES &&
               sizeof(machinr_t) <= SYN_STATE_BYTES && sizeof(crittr_t) <= SYN_STATE_BYTES &&
               sizeof(zappr_t) <= SYN_STATE_BYTES && sizeof(signlr_t) <= SYN_STATE_BYTES &&
               sizeof(pluckr_t) <= SYN_STATE_BYTES && sizeof(rustlr_t) <= SYN_STATE_BYTES, "synth state");
