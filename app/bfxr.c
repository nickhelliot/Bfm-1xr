/* SPDX-License-Identifier: MIT
 * BFMXR for the FM-1: bfxr2's classic Bfxr engine (js/audio/Bfxr_DSP.js) played sample by sample, and its sound
 * generators (js/synths/Bfxr.js). Line for line with the JavaScript, quirks included; see bfxr.h for the credits.
 * Checked against the JavaScript's output on a computer: tools/check_engine.c. */
#include "bfxr.h"
#include "fastmath.h"

#define SR 44100.0f
#define TWO_PI 6.28318531f

const bfxr_param_t BFXR_PARAMS[BP_COUNT] = {
    {"VOLUME", 0, 1, 0.5f, 0},
    {"WAVE", 0, BFXR_NWAVES - 1, 0, 0},
    {"ATTACK", 0, 1, 0, 4},
    {"SUSTAIN", 0, 1, 0.3f, 2},
    {"PUNCH", 0, 1, 0, 2},
    {"DECAY", 0.03f, 1, 0.4f, 0},
    {"COMPRESSION", 0, 1, 0, 0},
    {"FREQUENCY", 0, 1, 0.3f, 4},
    {"FREQ SLIDE", -0.5f, 0.5f, 0, 0},
    {"DELTA SLIDE", -1, 1, 0, 7},
    {"FREQ CUTOFF", 0, 0.99f, 0, 0},
    {"VIBRATO DEPTH", 0, 1, 0, 3},
    {"VIBRATO SPEED", 0, 1, 0, 0},
    {"JUMP REPEAT", 0, 1, 0, 0},
    {"JUMP 1 AMOUNT", -1, 1, 0, 0},
    {"JUMP 1 ONSET", 0, 1, 0, 0},
    {"JUMP 2 AMOUNT", -1, 1, 0, 0},
    {"JUMP 2 ONSET", 0, 1, 0, 0},
    {"HARMONICS", 0, 1, 0, 3},
    {"HARM FALLOFF", 0, 1, 0, 2},
    {"SQUARE DUTY", 0, 0.99f, 0, 0},
    {"DUTY SWEEP", -1, 1, 0, 3},
    {"REPEAT SPEED", 0, 1, 0, 0},
    {"FLANGER OFFSET", -1, 1, 0, 3},
    {"FLANGER SWEEP", -1, 1, 0, 3},
    {"LOWPASS", 0.01f, 1, 1, 3},
    {"LOWPASS SWEEP", -1, 1, 0, 0},
    {"LOWPASS RESO", 0, 1, 0, 0},
    {"HIGHPASS", 0, 1, 0, 5},
    {"HIGHPASS SWEEP", -1, 1, 0, 0},
    {"BIT CRUSH", 0, 1, 0, 4},
    {"CRUSH SWEEP", -1, 1, 0, 5},
};
/* (bfxr2's randomize powers: its lpFilterSweep, hpFilterSweep and slide name no parameter, so they apply to none) */

const char *const BFXR_WAVE_NAMES[BFXR_NWAVES] = {"SQUARE", "SAW", "SIN", "WHITE", "TRIANGLE", "RASP", "TAN",
                                                 "WHISTLE", "BREAKER", "BITNOISE", "FMSYN", "VOICE"};
const char *const BFXR_GEN_NAMES[GEN_COUNT] = {"PICKUP/COIN", "LASER/SHOOT", "EXPLOSION", "POWERUP",
                                               "HIT/HURT", "JUMP", "BLIP/SELECT", "RANDOMIZE"};

/* the AKWF single-cycle waves (Adventure Kid Waveforms, CC0), 256 samples, unsigned 16-bit */
#include "akwf_tables.h"

/* bfxr2's seeded random numbers (SoundDSP.rng, mulberry32) */
static float rnd_next(uint32_t *st)
{
    uint32_t t;
    *st += 0x6D2B79F5u;
    t = (*st ^ (*st >> 15)) * (1u | *st);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (float)((t ^ (t >> 14)) >> 8) * (1.0f / 16777216.0f);   /* 24 bits: exact in a float, < 1 */
}

static inline float lerpf(float a, float b, float t) { return a + t * (b - a); }

/* float-float arithmetic (Dekker / Knuth; no fused multiply-add, no reassociation: built without fast-math) */
static inline bfxr_ff_t ff(float a) { bfxr_ff_t r = {a, 0.0f}; return r; }
static inline bfxr_ff_t ff_quick(float s, float e)
{
    bfxr_ff_t r;
    r.hi = s + e;
    r.lo = e - (r.hi - s);
    return r;
}
static inline bfxr_ff_t ff_add(bfxr_ff_t x, bfxr_ff_t y)
{
    float s = x.hi + y.hi, bb = s - x.hi, e = (x.hi - (s - bb)) + (y.hi - bb);
    return ff_quick(s, e + x.lo + y.lo);
}
static inline void ff_split(float a, float *h, float *l)
{
    float c = 4097.0f * a;
    *h = c - (c - a);
    *l = a - *h;
}
static inline bfxr_ff_t ff_mul(bfxr_ff_t x, bfxr_ff_t y)
{
    float p = x.hi * y.hi, ah, al, bh, bl, e;
    ff_split(x.hi, &ah, &al);
    ff_split(y.hi, &bh, &bl);
    e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return ff_quick(p, e + x.hi * y.lo + x.lo * y.hi);
}
static inline bfxr_ff_t ff_div_f(bfxr_ff_t x, float d)   /* x / d */
{
    float q = x.hi / d, ph, pl, dh, dl, p, e;
    ff_split(q, &ph, &pl);
    ff_split(d, &dh, &dl);
    p = q * d;
    e = ((ph * dh - p) + ph * dl + pl * dh) + pl * dl;
    return ff_quick(q, ((x.hi - p) - e + x.lo) / d);
}
static inline int ff_gt(bfxr_ff_t a, bfxr_ff_t b) { return a.hi > b.hi || (a.hi == b.hi && a.lo > b.lo); }
static inline float ff_f(bfxr_ff_t a) { return a.hi + a.lo; }

/* ------------------------------------------------------------------ the engine (audio interrupt) --- */
static void clamp_total_length(float *p)
{
    float total = p[BP_ATTACK] + p[BP_SUSTAIN] + p[BP_DECAY];
    if (total < 0.18f) {
        float m = 0.18f / total;
        p[BP_ATTACK] *= m;
        p[BP_SUSTAIN] *= m;
        p[BP_DECAY] *= m;
    }
}

static void reset(bfxr_voice_t *v, int total)
{
    float *p = v->p;
    float minf = fm_powf(p[BP_MINFREQ], 0.4f) * p[BP_FREQ];
    float r2 = 1.0f / v->pitch;                  /* the period scales as 1 / the frequency */
    v->fps = ff_div_f(ff(100.0f * r2), p[BP_FREQ] * p[BP_FREQ] + 0.001f);
    v->fmax = ff_div_f(ff(100.0f * r2), minf * minf + 0.001f);
    v->pj_reached = 0;
    v->pj2_reached = 0;
    if (total) {
        int32_t i;
        float win;
        v->master_vol = p[BP_VOLUME] * p[BP_VOLUME];
        v->wave = (int32_t)p[BP_WAVE];
        if (p[BP_SUSTAIN] < 0.01f)
            p[BP_SUSTAIN] = 0.01f;
        clamp_total_length(p);
        v->punch = p[BP_PUNCH];
        v->phase = 0;
        v->min_freq = p[BP_MINFREQ];
        v->muted = 0;
        v->overtones = p[BP_HARM] * 10;
        v->overtone_fall = p[BP_HARM_FALL];
        v->comp = 1 / (1 + 4 * p[BP_COMPRESS]);
        v->filters = p[BP_LPF] != 1.0f || p[BP_HPF] != 0.0f;
        v->vib_phase = ff(0);
        v->vib_speed = p[BP_VIB_SPEED] * p[BP_VIB_SPEED] * 0.01f;
        v->vib_amp = p[BP_VIB_DEPTH] * 0.5f;
        v->env_stage = 0;
        v->env_time = 0;
        v->len0 = p[BP_ATTACK] * p[BP_ATTACK] * 100000.0f;
        v->len1 = p[BP_SUSTAIN] * p[BP_SUSTAIN] * 100000.0f;
        v->len2 = p[BP_DECAY] * p[BP_DECAY] * 100000.0f + 10;
        v->stage_len = v->len0;
        v->full_len = v->len0 + v->len1 + v->len2;
        v->bc_sweep = -p[BP_CRUSH_SWEEP] / v->full_len;
        v->bc_phase = 0;
        v->bc_last = 0;
        v->over0 = 1.0f / v->len0;
        v->over1 = 1.0f / v->len1;
        v->over2 = 1.0f / v->len2;
        v->flanger = p[BP_FLANGE_OFF] != 0.0f || p[BP_FLANGE_SWEEP] != 0.0f;
        v->flanger_delta = p[BP_FLANGE_SWEEP] * p[BP_FLANGE_SWEEP] * p[BP_FLANGE_SWEEP] * 0.2f;
        v->flanger_pos = 0;
        v->onebit_state = 1 << 14;
        v->onebit = 0;
        for (i = 0; i < 1024; i++)
            v->flanger_buf[i] = 0;
        for (i = 0; i < 32; i++)
            v->noise[i] = rnd_next(&v->rng) * 2.0f - 1.0f;
        for (i = 0; i < 32; i++)
            v->lonoise[i] = (i % 8 == 0) ? rnd_next(&v->rng) * 2.0f - 1.0f : v->lonoise[i - 1];
        v->pj_repeat_len = lerpf(v->full_len, SR / 50, p[BP_PJ_REPEAT]) + 32;
        win = v->pj_repeat_len > 0 ? v->pj_repeat_len : v->full_len;
        v->pj_amt = p[BP_PJ1_AMT] > 0 ? 1.0f - p[BP_PJ1_AMT] * p[BP_PJ1_AMT] * 0.9f
                                      : 1.0f + p[BP_PJ1_AMT] * p[BP_PJ1_AMT] * 10.0f;
        v->pj2_amt = p[BP_PJ2_AMT] > 0 ? 1.0f - p[BP_PJ2_AMT] * p[BP_PJ2_AMT] * 0.9f
                                       : 1.0f + p[BP_PJ2_AMT] * p[BP_PJ2_AMT] * 10.0f;
        v->pj_cur = 0;
        v->pj_ts = p[BP_PJ1_ONSET] == 1.0f ? 0 : p[BP_PJ1_ONSET] * win + 32;
        v->pj2_ts = p[BP_PJ2_ONSET] == 1.0f ? 0 : p[BP_PJ2_ONSET] * win + 32;
        if (v->wave == 9) {                      /* BITNOISE: frequency and cutoff moved up the range */
            float sf = 0.5f + p[BP_FREQ], mf = 0.495f + p[BP_MINFREQ] / 0.99f;
            v->fps = ff_div_f(ff(100.0f * r2), sf * sf + 0.001f);
            v->fmax = ff_div_f(ff(100.0f * r2), mf * mf + 0.001f);
        }
    }
    v->slide = ff_add(ff(1.0f), ff(-p[BP_SLIDE] * p[BP_SLIDE] * p[BP_SLIDE] * 0.01f));   /* (1 - x, kept exact) */
    v->accel = ff(-p[BP_DSLIDE] * p[BP_DSLIDE] * p[BP_DSLIDE] * 0.000001f);
    v->flanger_offset = p[BP_FLANGE_OFF] * p[BP_FLANGE_OFF] * 1020.0f;
    if (p[BP_FLANGE_OFF] < 0)
        v->flanger_offset = -v->flanger_offset;
    v->bc_freq = 1 - (p[BP_CRUSH] > 0 ? fm_powf(p[BP_CRUSH], 1.0f / 3.0f) : 0);
    /* bfxr2 writes `if ((params.waveType)|0 == 0)`, read as waveType | (0 == 0): always true */
    v->duty = 0.5f - p[BP_DUTY] * 0.5f;
    v->duty_sweep = -p[BP_DUTY_SWEEP] * 0.00005f;
    v->lp_cut = p[BP_LPF] * p[BP_LPF] * p[BP_LPF] * 0.1f;
    v->lp_delta = 1.0f + p[BP_LPF_SWEEP] * 0.0001f;
    v->lp_damp = 5.0f / (1.0f + p[BP_LPF_RES] * p[BP_LPF_RES] * 20.0f) * (0.01f + v->lp_cut);
    if (v->lp_damp > 0.8f)
        v->lp_damp = 0.8f;
    v->lp_damp = 1.0f - v->lp_damp;
    v->lp_on = p[BP_LPF] != 1.0f;
    v->lp_pos = 0;
    v->lp_dpos = 0;
    v->hp_pos = 0;
    v->hp_cut = p[BP_HPF] * p[BP_HPF] * 0.1f;
    v->hp_delta = 1.0f + p[BP_HPF_SWEEP] * 0.0003f;
    v->reset_period = lerpf(v->full_len, SR / 10, p[BP_REPEAT]);
    v->reset_cur = 0;
}

void bfxr_start(bfxr_voice_t *v, const bfxr_sound_t *s, float pitch, uint32_t seed)
{
    uint32_t k;
    for (k = 0; k < BP_COUNT; k++)
        v->p[k] = s->p[k];
    v->rng = seed;
    v->pitch = pitch > 0 ? pitch : 1.0f;
    reset(v, 1);
    v->length = v->full_len;
    v->n = 0;
    v->active = 1;
}

static inline float sine_approx(float pos)
{
    float s;
    pos = pos > 0.5f ? (pos - 1.0f) * TWO_PI : pos * TWO_PI;
    s = pos < 0 ? 1.27323954f * pos + 0.405284735f * pos * pos : 1.27323954f * pos - 0.405284735f * pos * pos;
    return s < 0 ? 0.225f * (s * -s - s) + s : 0.225f * (s * s - s) + s;
}

static inline float akwf(const uint16_t *t, float ratio)
{
    int32_t i = (int32_t)(ratio * 256.0f);
    return (float)t[i & 255] * (1.0f / 32768.0f) - 1.0f;
}

static float one_sample(bfxr_voice_t *v)
{
    float super = 0, mult;
    int32_t j;
    if (v->reset_period != 0) {
        v->reset_cur++;
        if (v->reset_cur >= v->reset_period) {
            v->reset_cur = 0;
            reset(v, 0);
        }
    }
    v->pj_cur++;
    if (v->pj_cur >= v->pj_repeat_len) {
        v->pj_cur = 0;
        if (v->pj_reached) {
            v->fps = ff_div_f(v->fps, v->pj_amt);
            v->pj_reached = 0;
        }
        if (v->pj2_reached) {
            v->fps = ff_div_f(v->fps, v->pj2_amt);
            v->pj2_reached = 0;
        }
    }
    if (!v->pj_reached && v->pj_cur >= v->pj_ts) {
        v->pj_reached = 1;
        v->fps = ff_mul(v->fps, ff(v->pj_amt));
    }
    if (!v->pj2_reached && v->pj_cur >= v->pj2_ts) {
        v->fps = ff_mul(v->fps, ff(v->pj2_amt));
        v->pj2_reached = 1;
    }
    v->slide = ff_add(v->slide, v->accel);
    v->fps = ff_mul(v->fps, v->slide);
    if (ff_gt(v->fps, v->fmax)) {
        v->fps = v->fmax;
        if (v->min_freq > 0)
            v->muted = 1;
    }
    {
        bfxr_ff_t pd = v->fps;
        float pf;
        if (v->vib_amp > 0) {
            static const bfxr_ff_t TWO_PI_FF = {6.28318548f, -1.74845553e-7f};
            v->vib_phase = ff_add(v->vib_phase, ff(v->vib_speed));
            if (v->vib_phase.hi >= 6.28318531f)  /* (sin is periodic: a small angle keeps the float's precision) */
                v->vib_phase = ff_add(v->vib_phase, (bfxr_ff_t){-TWO_PI_FF.hi, -TWO_PI_FF.lo});
            pd = ff_mul(v->fps, ff_add(ff(1.0f), ff(fm_sinf(ff_f(v->vib_phase)) * v->vib_amp)));
        }
        pf = pd.hi;
        if (pf < 8.0f)
            v->period = 8;
        else if (pf > 1.0e7f)                     /* (real periods stay under ~1e6) */
            v->period = 10000000;
        else {
            int32_t ip = (int32_t)pf;            /* floor of hi + lo: hi rounds, lo says which side */
            float frac = (pf - (float)ip) + pd.lo;
            v->period = frac < 0 ? ip - 1 : frac >= 1.0f ? ip + 1 : ip;
        }
    }
    if (v->wave == 0) {
        v->duty += v->duty_sweep;
        if (v->duty < 0)
            v->duty = 0.001f;
        else if (v->duty > 0.5f)
            v->duty = 0.5f;
    }
    if (++v->env_time > v->stage_len) {
        v->env_time = 0;
        switch (++v->env_stage) {
        case 1: v->stage_len = v->len1; break;
        case 2: v->stage_len = v->len2; break;
        }
    }
    {
        float env = 0;
        switch (v->env_stage) {
        case 0: env = v->env_time * v->over0; break;
        case 1: env = 1.0f + (1.0f - v->env_time * v->over1) * 2.0f * v->punch; break;
        case 2: env = 1.0f - v->env_time * v->over2; break;
        default: v->active = 0; break;
        }
        if (v->flanger) {
            v->flanger_offset += v->flanger_delta;
            v->flanger_int = (int32_t)v->flanger_offset;
            if (v->flanger_int < 0)
                v->flanger_int = -v->flanger_int;
            else if (v->flanger_int > 1023)
                v->flanger_int = 1023;
        }
        if (v->filters && v->hp_delta != 0) {
            v->hp_cut *= v->hp_delta;
            if (v->hp_cut < 0.00001f)
                v->hp_cut = 0.00001f;
            else if (v->hp_cut > 0.1f)
                v->hp_cut = 0.1f;
        }
        {
            const int32_t per = v->period;
            const float inv = 1.0f / (float)per;
            for (j = 0; j < 8; j++) {
                float sample = 0, strength = 1;
                int32_t k, tp;
                if (++v->phase >= per) {
                    v->phase -= per;
                    if (v->phase >= per)         /* (the period shrank by more than a sample) */
                        v->phase %= per;
                    switch (v->wave) {
                    case 3:
                        for (k = 0; k < 32; k++)
                            v->noise[k] = rnd_next(&v->rng) * 2.0f - 1.0f;
                        break;
                    case 6:
                        for (k = 0; k < 32; k++)
                            v->lonoise[k] = (k % 8 == 0) ? rnd_next(&v->rng) * 2.0f - 1.0f : v->lonoise[k - 1];
                        break;
                    case 9: {
                        int32_t feed = ((v->onebit_state >> 1) & 1) ^ (v->onebit_state & 1);
                        v->onebit_state = (v->onebit_state >> 1) | (feed << 14);
                        v->onebit = (float)((~v->onebit_state) & 1) - 0.5f;
                        break;
                    }
                    }
                }
                tp = v->phase;                   /* (phase * (k + 1)) % period, one harmonic after another */
                for (k = 0; (float)k <= v->overtones; k++) {
                    float ratio = (float)tp * inv;
                    switch (v->wave) {
                    case 0: sample += strength * (ratio < v->duty ? 0.5f : -0.5f); break;
                    case 1: sample += strength * (1.0f - ratio * 2.0f); break;
                    case 2: sample += strength * sine_approx(ratio); break;
                    case 3: sample += strength * v->noise[((int32_t)(ratio * 32.0f)) & 31]; break;
                    case 4: sample += strength * (fm_fabsf(1 - ratio * 2) - 1); break;
                    case 5: sample += strength * akwf(AKWF_ORGAN, ratio); break;
                    case 6:                       /* tan (period pi); at the pole itself JavaScript's is +1.6e16 */
                        sample += (tp * 2 == per ? 1.6e16f : fm_tanf(3.14159265f * (ratio > 0.5f ? ratio - 1.0f : ratio))) * strength;
                        break;
                    case 7: {
                        int32_t t20 = (int32_t)(((uint32_t)tp * 20u) % (uint32_t)per);   /* (< 2^31: period <= 1e7) */
                        sample += strength * (0.75f * sine_approx(ratio) + 0.25f * sine_approx((float)t20 * inv));
                        break;
                    }
                    case 8: sample += strength * (fm_fabsf(1 - ratio * ratio * 2) - 1); break;
                    case 9: sample += strength * v->onebit; break;
                    case 10: sample += strength * akwf(AKWF_FM, ratio); break;
                    default: sample += strength * akwf(AKWF_VOICE, ratio); break;
                    }
                    strength *= 1 - v->overtone_fall;
                    tp += v->phase;
                    if (tp >= per)
                        tp -= per;
                }
                if (v->filters) {
                    float old = v->lp_pos;
                    v->lp_cut *= v->lp_delta;
                    if (v->lp_cut < 0)
                        v->lp_cut = 0;
                    else if (v->lp_cut > 0.1f)
                        v->lp_cut = 0.1f;
                    if (v->lp_on) {
                        v->lp_dpos += (sample - v->lp_pos) * v->lp_cut;
                        v->lp_dpos *= v->lp_damp;
                    } else {
                        v->lp_pos = sample;
                        v->lp_dpos = 0;
                    }
                    v->lp_pos += v->lp_dpos;
                    v->hp_pos += v->lp_pos - old;
                    v->hp_pos *= 1.0f - v->hp_cut;
                    sample = v->hp_pos;
                }
                if (v->flanger) {
                    v->flanger_buf[v->flanger_pos & 1023] = sample;
                    sample += v->flanger_buf[(v->flanger_pos - v->flanger_int + 1024) & 1023];
                    v->flanger_pos = (v->flanger_pos + 1) & 1023;
                }
                super += sample;
            }
        }
        if (super > 8.0f)
            super = 8.0f;
        else if (super < -8.0f)
            super = -8.0f;
        else if (super != super)                 /* (a tan wave at its pole) */
            super = 0;
        v->bc_phase += v->bc_freq;
        if (v->bc_phase > 1) {
            v->bc_phase = 0;
            v->bc_last = super;
        }
        if (v->bc_sweep != 0) {
            mult = lerpf(1, 50 * v->bc_freq, fm_sqrtf(v->bc_freq));
            v->bc_freq = fm_clampf(v->bc_freq + mult * v->bc_sweep, 0.00001f, 1);
        }
        super = v->master_vol * env * v->bc_last * 0.125f;
    }
    if (v->comp != 1.0f && super != 0)
        super = super > 0 ? fm_powf(super, v->comp) : -fm_powf(-super, v->comp);
    if (v->muted)
        v->active = 0;
    return fm_clampf(super, -1, 1);
}

void bfxr_render(bfxr_voice_t *v, float *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        float s = 0;
        if (v->active && v->n < v->length) {
            s = one_sample(v);
            v->n++;
            if (!v->active)
                s = 0;                           /* (the sample a mute or the envelope's end is found on) */
        } else {
            v->active = 0;
        }
        out[i] = s;
    }
}

/* ------------------------------------------------------------------ the generators (main loop) --- */
static uint32_t gen_rng = 0x5EEDu;
void bfxr_seed(uint32_t seed) { gen_rng ^= seed * 2654435761u; }
static float rnd(void) { return rnd_next(&gen_rng); }

void bfxr_set(bfxr_sound_t *s, uint32_t k, float v)
{
    const bfxr_param_t *d = &BFXR_PARAMS[k % BP_COUNT];
    if (v != v)
        v = d->def;
    if (k == BP_WAVE)
        v = (float)(int32_t)(v + 0.5f);
    s->p[k % BP_COUNT] = fm_clampf(v, d->min, d->max);
}
#define SET(k, v) bfxr_set(s, (k), (v))
#define GET(k) (s->p[k])

void bfxr_defaults(bfxr_sound_t *s)
{
    uint32_t k;
    for (k = 0; k < BP_COUNT; k++)
        s->p[k] = BFXR_PARAMS[k].def;
}

static uint32_t pick_wave(const uint8_t *w, uint32_t n) { return w[(uint32_t)(rnd() * (float)n) % n]; }
enum { W_SQUARE = 0, W_SAW = 1, W_SIN = 2, W_WHITE = 3, W_BITNOISE = 9, W_FMSYN = 10, W_VOICE = 11, W_WHISTLE = 7 };

static float centered(float min, float max, float centre)
{
    float r;
    if (rnd() < 0.5f) {
        r = rnd();
        return centre + r * r * (max - centre);
    }
    r = rnd();
    return centre - r * r * (centre - min);
}

/* bfxr2's rectify_params: keeps a randomized or mutated sound audible */
static void rectify(bfxr_sound_t *s)
{
    float r;
    SET(BP_FREQ, centered(0, 0.6f, GET(BP_WAVE) == W_VOICE ? 0.22f : BFXR_PARAMS[BP_FREQ].def));
    if (GET(BP_ATTACK) + GET(BP_SUSTAIN) + GET(BP_DECAY) < 0.2f) {
        SET(BP_SUSTAIN, 0.2f + rnd() * 0.3f);
        SET(BP_DECAY, 0.2f + rnd() * 0.3f);
    }
    r = rnd() * rnd();
    SET(BP_PUNCH, r * r);
    if ((GET(BP_FREQ) > 0.7f && GET(BP_SLIDE) > 0.2f) || (GET(BP_FREQ) < 0.2f && GET(BP_SLIDE) < -0.05f))
        SET(BP_SLIDE, -GET(BP_SLIDE));
    if (GET(BP_LPF) < 0.1f && GET(BP_LPF_SWEEP) < 0)
        SET(BP_LPF_SWEEP, -GET(BP_LPF_SWEEP) + 0.2f);
    if (GET(BP_WAVE) != W_SQUARE) {
        SET(BP_DUTY, BFXR_PARAMS[BP_DUTY].def);
        SET(BP_DUTY_SWEEP, BFXR_PARAMS[BP_DUTY_SWEEP].def);
    } else if (GET(BP_DUTY) > 0.7f && rnd() < 0.5f) {
        SET(BP_DUTY_SWEEP, -rnd() * 0.5f);
    }
}

static void randomize(bfxr_sound_t *s)
{
    uint32_t k, e;
    for (k = 0; k < BP_COUNT; k++) {
        const bfxr_param_t *d = &BFXR_PARAMS[k];
        float r = rnd(), base = r;
        int above = rnd() < 0.5f;
        if (k == BP_WAVE)
            continue;
        for (e = 1; e < d->power; e++)
            r *= base;
        if (d->min == d->def)
            above = 1;
        if (d->max == d->def)
            above = 0;
        s->p[k] = above ? d->def + (d->max - d->def) * r : d->def - (d->def - d->min) * r;
    }
    SET(BP_WAVE, (float)((uint32_t)(rnd() * 11.0f) % 11u));   /* (bfxr2 leaves VOICE out of a random wave) */
    if (rnd() < 0.5f)
        SET(BP_REPEAT, 0);
    SET(BP_MINFREQ, 0);
    SET(BP_COMPRESS, 0);
    rectify(s);
}

void bfxr_mutate(bfxr_sound_t *s)
{
    uint32_t k;
    if (rnd() < 0.1f) {                          /* now and then another wave instead */
        SET(BP_WAVE, (float)(((uint32_t)GET(BP_WAVE) + (uint32_t)(rnd() * 11.0f)) % BFXR_NWAVES));
        return;
    }
    for (k = 0; k < BP_COUNT; k++) {
        const bfxr_param_t *d = &BFXR_PARAMS[k];
        if (rnd() < 0.5f || k == BP_WAVE)
            continue;
        SET(k, GET(k) + (rnd() - 0.5f) * 0.1f * (d->max - d->min));
    }
    rectify(s);
}

void bfxr_generate(bfxr_sound_t *s, uint32_t gen)
{
    static const uint8_t HIT[] = {W_WHITE, W_BITNOISE, W_SAW, W_SQUARE, W_VOICE};
    static const uint8_t JUMP[] = {W_SQUARE, W_SAW, W_FMSYN};
    static const uint8_t BLIP[] = {W_SQUARE, W_SAW, W_FMSYN, W_WHISTLE};
    bfxr_defaults(s);
    switch (gen) {
    case GEN_PICKUP:
        SET(BP_FREQ, 0.4f + rnd() * 0.5f);
        SET(BP_SUSTAIN, rnd() * 0.1f);
        SET(BP_DECAY, 0.1f + rnd() * 0.4f);
        SET(BP_PUNCH, 0.3f + rnd() * 0.3f);
        if (rnd() < 0.5f) {
            float num, den;
            SET(BP_PJ1_ONSET, 0.5f + rnd() * 0.2f);
            num = (float)(int32_t)(rnd() * 7) + 1;
            den = (float)(int32_t)(rnd() * 7) + num + 2;
            SET(BP_PJ1_AMT, num / den);
        }
        break;
    case GEN_LASER:
        SET(BP_WAVE, (float)(int32_t)(rnd() * 3));
        if (GET(BP_WAVE) == W_SIN && rnd() < 0.5f)
            SET(BP_WAVE, (float)(int32_t)(rnd() * 2));
        if (rnd() < 0.33f) {
            SET(BP_FREQ, 0.1f + rnd() * 0.5f);
            SET(BP_MINFREQ, rnd() * 0.1f);
            SET(BP_SLIDE, -0.35f - rnd() * 0.3f);
        } else {
            SET(BP_FREQ, 0.5f + rnd() * 0.5f);
            SET(BP_MINFREQ, GET(BP_FREQ) - 0.2f - rnd() * 0.6f);
            if (GET(BP_MINFREQ) < 0.2f)
                SET(BP_MINFREQ, 0.2f);
            SET(BP_SLIDE, -0.15f - rnd() * 0.2f);
        }
        if (GET(BP_FREQ) < 0.15f) {
            SET(BP_MINFREQ, 0);
            SET(BP_SLIDE, -0.1f - rnd() * 0.1f);
        }
        if (rnd() < 0.5f) {
            SET(BP_DUTY, rnd() * 0.5f);
            SET(BP_DUTY_SWEEP, rnd() * 0.2f);
        } else {
            SET(BP_DUTY, 0.4f + rnd() * 0.5f);
            SET(BP_DUTY_SWEEP, -rnd() * 0.7f);
        }
        SET(BP_SUSTAIN, 0.1f + rnd() * 0.2f);
        SET(BP_DECAY, rnd() * 0.4f);
        if (rnd() < 0.5f)
            SET(BP_PUNCH, rnd() * 0.3f);
        if (rnd() < 0.33f) {
            SET(BP_FLANGE_OFF, rnd() * 0.2f);
            SET(BP_FLANGE_SWEEP, -rnd() * 0.2f);
        }
        if (rnd() < 0.5f)
            SET(BP_HPF, rnd() * 0.3f);
        break;
    case GEN_EXPLOSION:
        SET(BP_WAVE, rnd() < 0.5f ? W_WHITE : W_BITNOISE);
        if (rnd() < 0.5f) {
            SET(BP_FREQ, 0.1f + rnd() * 0.4f);
            SET(BP_SLIDE, -0.1f + rnd() * 0.4f);
        } else {
            SET(BP_FREQ, 0.2f + rnd() * 0.7f);
            SET(BP_SLIDE, -0.2f - rnd() * 0.2f);
        }
        SET(BP_FREQ, GET(BP_FREQ) * GET(BP_FREQ));
        if (rnd() < 0.2f)
            SET(BP_SLIDE, 0);
        if (rnd() < 0.33f)
            SET(BP_REPEAT, 0.3f + rnd() * 0.5f);
        SET(BP_SUSTAIN, 0.1f + rnd() * 0.3f);
        SET(BP_DECAY, rnd() * 0.5f);
        SET(BP_PUNCH, 0.2f + rnd() * 0.6f);
        if (rnd() < 0.5f) {
            SET(BP_FLANGE_OFF, -0.3f + rnd() * 0.9f);
            SET(BP_FLANGE_SWEEP, -rnd() * 0.3f);
        }
        if (rnd() < 0.33f) {
            SET(BP_PJ1_ONSET, 0.6f + rnd() * 0.3f);
            SET(BP_PJ1_AMT, 0.8f - rnd() * 1.6f);
        }
        break;
    case GEN_POWERUP:
        if (rnd() < 0.5f)
            SET(BP_WAVE, W_SAW);
        else
            SET(BP_DUTY, rnd() * 0.6f);
        if (rnd() < 0.5f) {
            SET(BP_FREQ, 0.2f + rnd() * 0.3f);
            SET(BP_SLIDE, 0.1f + rnd() * 0.4f);
            SET(BP_REPEAT, 0.4f + rnd() * 0.4f);
        } else {
            SET(BP_FREQ, 0.2f + rnd() * 0.3f);
            SET(BP_SLIDE, 0.05f + rnd() * 0.2f);
            if (rnd() < 0.5f) {
                SET(BP_VIB_DEPTH, rnd() * 0.7f);
                SET(BP_VIB_SPEED, rnd() * 0.6f);
            }
        }
        SET(BP_SUSTAIN, rnd() * 0.4f);
        SET(BP_DECAY, 0.1f + rnd() * 0.4f);
        break;
    case GEN_HIT:
        SET(BP_WAVE, pick_wave(HIT, sizeof HIT));
        if (GET(BP_WAVE) == W_SQUARE)
            SET(BP_DUTY, rnd() * 0.6f);
        SET(BP_FREQ, 0.2f + rnd() * 0.6f);
        SET(BP_SLIDE, -0.3f - rnd() * 0.4f);
        SET(BP_SUSTAIN, rnd() * 0.1f);
        SET(BP_DECAY, 0.1f + rnd() * 0.2f);
        if (rnd() < 0.5f)
            SET(BP_HPF, rnd() * 0.3f);
        break;
    case GEN_JUMP:
        SET(BP_WAVE, pick_wave(JUMP, sizeof JUMP));
        SET(BP_DUTY, rnd() * 0.6f);
        SET(BP_FREQ, 0.3f + rnd() * 0.3f);
        SET(BP_SLIDE, 0.1f + rnd() * 0.2f);
        SET(BP_SUSTAIN, 0.1f + rnd() * 0.3f);
        SET(BP_DECAY, 0.1f + rnd() * 0.2f);
        if (rnd() < 0.5f)
            SET(BP_HPF, rnd() * 0.3f);
        if (rnd() < 0.5f)
            SET(BP_LPF, 1.0f - rnd() * 0.6f);
        break;
    case GEN_BLIP:
        SET(BP_WAVE, pick_wave(BLIP, sizeof BLIP));
        if (GET(BP_WAVE) == W_SQUARE)
            SET(BP_DUTY, rnd() * 0.6f);
        SET(BP_FREQ, 0.2f + rnd() * 0.4f);
        SET(BP_SUSTAIN, 0.1f + rnd() * 0.1f);
        SET(BP_DECAY, rnd() * 0.2f);
        SET(BP_HPF, 0.1f);
        break;
    default:
        randomize(s);
        break;
    }
}
