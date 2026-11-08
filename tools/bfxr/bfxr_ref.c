/* SPDX-License-Identifier: GPL-3.0-only
 * Reference (double precision) C port of the classic Bfxr engine, to check the fixed-point port against.
 * It follows bfxr2 js/audio/Bfxr_DSP.js line by line, quirks included, and uses the same seeded random
 * generator as bfxr2's SoundDSP.rng, so its output can be compared with the JavaScript sample by sample.
 *
 * Bfxr_DSP.js is ported from Thomas Vian's SfxrSynth (Apache-2.0, Copyright 2010 Thomas Vian) and bfxr2 is
 * MIT (Copyright (c) 2021 Stephen Lavelle). The AKWF waves are Adventure Kid Waveforms (CC0).
 *
 *   cc -O2 -ffp-contract=off -I REF_DIR -o bfxr_ref bfxr_ref.c -lm
 *   ./bfxr_ref REF_DIR          compares every case_NNNN in REF_DIR (made by gen_ref.js) with this port */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "akwf_tables.h"

#define NPARAM 32
#define SAMPLE_RATE 44100.0

/* ---- bfxr2 SoundDSP.rng ---- */
static uint32_t rng_state;
static void rng_seed(double seed) { rng_state = (uint32_t)(uint64_t)floor(seed * 4294967295.0); }
static double rnd(void)
{
    rng_state += 0x6D2B79F5u;
    uint32_t t = (rng_state ^ (rng_state >> 15)) * (1u | rng_state);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

static double lerp(double a, double b, double t) { return a + t * (b - a); }
/* Math.min / Math.max propagate NaN */
static double jsmin(double a, double b) { return (a != a || b != b) ? NAN : (a < b ? a : b); }
static double jsmax(double a, double b) { return (a != a || b != b) ? NAN : (a > b ? a : b); }

typedef struct {
    int wave, muted, filters, flanger, lpOn, pjReached, pj2Reached, envStage;
    int32_t phase, periodTemp, flangerInt, oneBitState;
    double oneBitNoise, masterVolume, sustainPunch, minFreq, overtones, overtoneFalloff, comp;
    double vibPhase, vibSpeed, vibAmp, envVol, envTime, L0, L1, L2, attackLen, full, bcSweep, bcPhase, bcLast;
    double oL0, oL1, oL2, flangerDelta, flangerOffset, flangerPos, fps, fmax, pjRepeatLen, pjAmt, pj2Amt, pjCur, pjTs, pj2Ts;
    double slide, accel, bcFreq, duty, dutySweep, lpCut, lpDelta, lpDamp, lpPos, lpDeltaPos, hpPos, hpCut, hpDelta;
    double resetPeriod, resetCur, lpOld;
    double flangerBuf[1024], noise[32], loNoise[32];
} dsp_t;

static void clamp_total_length(double *p)
{
    double total = p[2] + p[3] + p[5];
    if (total < 0.18) {
        double m = 0.18 / total;
        p[2] *= m; p[3] *= m; p[5] *= m;
    }
}

static void reset(dsp_t *d, double *p, int total)
{
    d->fps = 100.0 / (p[7] * p[7] + 0.001);
    double minf = pow(p[10], 0.4) * p[7];
    d->fmax = 100.0 / (minf * minf + 0.001);
    d->pjReached = 0;
    d->pj2Reached = 0;
    if (total) {
        d->masterVolume = p[1] * p[1];
        d->wave = (int32_t)p[0];
        if (p[3] < 0.01) p[3] = 0.01;
        clamp_total_length(p);
        d->sustainPunch = p[4];
        d->phase = 0;
        d->minFreq = p[10];
        d->muted = 0;
        d->overtones = p[18] * 10;
        d->overtoneFalloff = p[19];
        d->comp = 1 / (1 + 4 * p[6]);
        d->filters = p[25] != 1.0 || p[28] != 0.0;
        d->vibPhase = 0.0;
        d->vibSpeed = p[12] * p[12] * 0.01;
        d->vibAmp = p[11] * 0.5;
        d->envVol = 0.0;
        d->envStage = 0;
        d->envTime = 0;
        d->L0 = p[2] * p[2] * 100000.0;
        d->L1 = p[3] * p[3] * 100000.0;
        d->L2 = p[5] * p[5] * 100000.0 + 10;
        d->attackLen = d->L0;
        d->full = d->L0 + d->L1 + d->L2;
        d->bcSweep = -p[31] / d->full;
        d->bcPhase = 0;
        d->bcLast = 0;
        d->oL0 = 1.0 / d->L0;
        d->oL1 = 1.0 / d->L1;
        d->oL2 = 1.0 / d->L2;
        d->flanger = p[23] != 0.0 || p[24] != 0.0;
        d->flangerDelta = p[24] * p[24] * p[24] * 0.2;
        d->flangerPos = 0;
        d->oneBitState = 1 << 14;
        d->oneBitNoise = 0;
        memset(d->flangerBuf, 0, sizeof d->flangerBuf);
        for (int i = 0; i < 32; i++) d->noise[i] = rnd() * 2.0 - 1.0;
        for (int i = 0; i < 32; i++) d->loNoise[i] = (i % 8 == 0) ? rnd() * 2.0 - 1.0 : d->loNoise[i - 1];
        d->pjRepeatLen = lerp(d->full, SAMPLE_RATE / 50, p[13]) + 32;
        double win = d->full;
        if (d->pjRepeatLen > 0) win = d->pjRepeatLen;
        d->pjAmt = p[14] > 0.0 ? 1.0 - p[14] * p[14] * 0.9 : 1.0 + p[14] * p[14] * 10.0;
        d->pj2Amt = p[16] > 0.0 ? 1.0 - p[16] * p[16] * 0.9 : 1.0 + p[16] * p[16] * 10.0;
        d->pjCur = 0;
        d->pjTs = p[15] == 1.0 ? 0 : p[15] * win + 32;
        d->pj2Ts = p[17] == 1.0 ? 0 : p[17] * win + 32;
        if (d->wave == 9) { /* Bitnoise: frequency_start 0..1, min_frequency_relative_to_starting_frequency 0..0.99 */
            double sf = p[7], mf = p[10];
            double deltaStart = (sf - 0.0) / (1.0 - 0.0), deltaMin = (mf - 0.0) / (0.99 - 0.0);
            sf = 0.5 + deltaStart;
            mf = 0.495 + deltaMin;
            d->fps = 100.0 / (sf * sf + 0.001);
            d->fmax = 100.0 / (mf * mf + 0.001);
        }
    }
    d->slide = 1.0 - p[8] * p[8] * p[8] * 0.01;
    d->accel = -p[9] * p[9] * p[9] * 0.000001;
    d->flangerOffset = p[23] * p[23] * 1020.0;
    if (p[23] < 0.0) d->flangerOffset = -d->flangerOffset;
    d->bcFreq = 1 - pow(p[30], 1.0 / 3.0);
    /* bfxr2 writes `if ((params.waveType)|0 == 0)`, which JavaScript reads as waveType | (0 == 0): always true */
    d->duty = 0.5 - p[20] * 0.5;
    d->dutySweep = -p[21] * 0.00005;
    d->lpCut = p[25] * p[25] * p[25] * 0.1;
    d->lpDelta = 1.0 + p[26] * 0.0001;
    d->lpDamp = 5.0 / (1.0 + p[27] * p[27] * 20.0) * (0.01 + d->lpCut);
    if (d->lpDamp > 0.8) d->lpDamp = 0.8;
    d->lpDamp = 1.0 - d->lpDamp;
    d->lpOn = p[25] != 1.0;
    d->lpPos = 0.0;
    d->lpDeltaPos = 0.0;
    d->hpPos = 0.0;
    d->hpCut = p[28] * p[28] * 0.1;
    d->hpDelta = 1.0 + p[29] * 0.0003;
    d->resetPeriod = lerp(d->full, SAMPLE_RATE / 10, p[22]);
    d->resetCur = 0;
}

static double sine_approx(double pos)
{
    pos = pos > 0.5 ? (pos - 1.0) * 6.28318531 : pos * 6.28318531;
    double s = pos < 0 ? 1.27323954 * pos + .405284735 * pos * pos : 1.27323954 * pos - 0.405284735 * pos * pos;
    return s < 0 ? .225 * (s * -s - s) + s : .225 * (s * s - s) + s;
}

/* Renders into a malloc'd float buffer; returns its length, or -1 when bfxr2 leaves its buffer unset. */
static long render(const double *params, double seed, float **out)
{
    dsp_t *d = calloc(1, sizeof *d);
    double p[NPARAM];
    memcpy(p, params, sizeof p);
    rng_seed(seed);
    reset(d, p, 1);

    double length = d->full;
    long buflen = (long)length;
    float *buf = calloc(buflen > 0 ? (size_t)buflen : 1, sizeof *buf);
    int finished = 0;
    long last_nonzero = -1;
    long i;
    for (i = 0; i < length; i++) {
        if (finished) { free(buf); free(d); return -1; }
        if (d->resetPeriod != 0) {
            d->resetCur++;
            if (d->resetCur >= d->resetPeriod) { d->resetCur = 0; reset(d, p, 0); }
        }
        d->pjCur++;
        if (d->pjCur >= d->pjRepeatLen) {
            d->pjCur = 0;
            if (d->pjReached) { d->fps /= d->pjAmt; d->pjReached = 0; }
            if (d->pj2Reached) { d->fps /= d->pj2Amt; d->pj2Reached = 0; }
        }
        if (!d->pjReached && d->pjCur >= d->pjTs) { d->pjReached = 1; d->fps *= d->pjAmt; }
        if (!d->pj2Reached && d->pjCur >= d->pj2Ts) { d->fps *= d->pj2Amt; d->pj2Reached = 1; }

        d->slide += d->accel;
        d->fps *= d->slide;
        if (d->fps > d->fmax) {
            d->fps = d->fmax;
            if (d->minFreq > 0.0) d->muted = 1;
        }
        double periodD = d->fps;
        if (d->vibAmp > 0.0) {
            d->vibPhase += d->vibSpeed;
            periodD = d->fps * (1.0 + sin(d->vibPhase) * d->vibAmp);
        }
        d->periodTemp = (int32_t)periodD;
        if (d->periodTemp < 8) d->periodTemp = 8;

        if (d->wave == 0) {
            d->duty += d->dutySweep;
            if (d->duty < 0.0) d->duty = 0.001;
            else if (d->duty > 0.5) d->duty = 0.5;
        }
        if (++d->envTime > d->attackLen) {
            d->envTime = 0;
            switch (++d->envStage) {
            case 1: d->attackLen = d->L1; break;
            case 2: d->attackLen = d->L2; break;
            }
        }
        switch (d->envStage) {
        case 0: d->envVol = d->envTime * d->oL0; break;
        case 1: d->envVol = 1.0 + (1.0 - d->envTime * d->oL1) * 2.0 * d->sustainPunch; break;
        case 2: d->envVol = 1.0 - d->envTime * d->oL2; break;
        case 3: d->envVol = 0.0; finished = 1; break;
        }
        if (d->flanger) {
            d->flangerOffset += d->flangerDelta;
            d->flangerInt = (int32_t)d->flangerOffset;
            if (d->flangerInt < 0) d->flangerInt = -d->flangerInt;
            else if (d->flangerInt > 1023) d->flangerInt = 1023;
        }
        if (d->filters && d->hpDelta != 0.0) {
            d->hpCut *= d->hpDelta;
            if (d->hpCut < 0.00001) d->hpCut = 0.00001;
            else if (d->hpCut > 0.1) d->hpCut = 0.1;
        }

        double super = 0.0;
        for (int j = 0; j < 8; j++) {
            d->phase++;
            if (d->phase >= d->periodTemp) {
                d->phase -= d->periodTemp;
                switch (d->wave) {
                case 3: for (int n = 0; n < 32; n++) d->noise[n] = rnd() * 2.0 - 1.0; break;
                case 6: for (int n = 0; n < 32; n++) d->loNoise[n] = (n % 8 == 0) ? rnd() * 2.0 - 1.0 : d->loNoise[n - 1]; break;
                case 9: {
                    int feed = ((d->oneBitState >> 1) & 1) ^ (d->oneBitState & 1);
                    d->oneBitState = (d->oneBitState >> 1) | (feed << 14);
                    d->oneBitNoise = (double)((~d->oneBitState) & 1) - 0.5;
                    break;
                }
                }
            }
            double sample = 0, strength = 1;
            double pt = (double)d->periodTemp;
            for (int k = 0; k <= d->overtones; k++) {
                int64_t tp = ((int64_t)d->phase * (k + 1)) % d->periodTemp;
                double ratio = (double)tp / pt;
                switch (d->wave) {
                case 0: sample += strength * (ratio < d->duty ? 0.5 : -0.5); break;
                case 1: sample += strength * (1.0 - ratio * 2.0); break;
                case 2: sample += strength * sine_approx(ratio); break;
                case 3: sample += strength * d->noise[((int)((double)tp * 32 / pt)) % 32]; break;
                case 4: sample += strength * (fabs(1 - ratio * 2) - 1); break;
                case 5: sample += strength * (AKWF_ORGAN[((int)((double)tp * 256 / pt)) % 256] / 32768.0 - 1); break;
                case 6: sample += tan(M_PI * (double)tp / pt) * strength; break;
                case 7: {
                    double v = 0.75 * sine_approx(ratio);
                    double r2 = (double)((tp * 20) % d->periodTemp) / pt;
                    v += 0.25 * sine_approx(r2);
                    sample += strength * v;
                    break;
                }
                case 8: sample += strength * (fabs(1 - ratio * ratio * 2) - 1); break;
                case 9: sample += strength * d->oneBitNoise; break;
                case 10: sample += strength * (AKWF_FM[((int)((double)tp * 256 / pt)) % 256] / 32768.0 - 1); break;
                case 11: sample += strength * (AKWF_VOICE[((int)((double)tp * 256 / pt)) % 256] / 32768.0 - 1); break;
                }
                strength *= (1 - d->overtoneFalloff);
            }
            if (d->filters) {
                d->lpOld = d->lpPos;
                d->lpCut *= d->lpDelta;
                if (d->lpCut < 0.0) d->lpCut = 0.0;
                else if (d->lpCut > 0.1) d->lpCut = 0.1;
                if (d->lpOn) {
                    d->lpDeltaPos += (sample - d->lpPos) * d->lpCut;
                    d->lpDeltaPos *= d->lpDamp;
                } else {
                    d->lpPos = sample;
                    d->lpDeltaPos = 0.0;
                }
                d->lpPos += d->lpDeltaPos;
                d->hpPos += d->lpPos - d->lpOld;
                d->hpPos *= 1.0 - d->hpCut;
                sample = d->hpPos;
            }
            if (d->flanger) {
                int pos = (int)d->flangerPos;
                d->flangerBuf[pos & 1023] = sample;
                sample += d->flangerBuf[(pos - d->flangerInt + 1024) & 1023];
                d->flangerPos = (pos + 1) & 1023;
            }
            super += sample;
        }
        if (super > 8.0) super = 8.0;
        else if (super < -8.0) super = -8.0;

        d->bcPhase += d->bcFreq;
        if (d->bcPhase > 1) { d->bcPhase = 0; d->bcLast = super; }
        double mult = lerp(1, 50 * d->bcFreq, sqrt(d->bcFreq));
        d->bcFreq = jsmax(jsmin(d->bcFreq + mult * d->bcSweep, 1), 0.00001);
        super = d->bcLast;

        super = d->masterVolume * d->envVol * super * 0.125;
        if (super > 0) super = pow(super, d->comp);
        else super = -pow(-super, d->comp);

        if (d->muted) { buflen = i < buflen ? i : buflen; break; }
        if (fabs(super) > 0.2e-2) last_nonzero = i;
        if (i < buflen) buf[i] = (float)jsmax(-1, jsmin(super, 1));
    }
    if (last_nonzero < buflen - 1) {
        if (last_nonzero < 10) last_nonzero = 10;
        if (last_nonzero + 1 < buflen) buflen = last_nonzero + 1;
    }
    *out = buf;
    free(d);
    return buflen;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: bfxr_ref REF_DIR\n"); return 2; }
    int total = 0, same_len = 0, bad = 0;
    double worst = 0;
    for (int n = 0;; n++) {
        char path[512];
        snprintf(path, sizeof path, "%s/case_%04d.txt", argv[1], n);
        FILE *f = fopen(path, "r");
        if (!f) break;
        double v[NPARAM + 1];
        for (int k = 0; k < NPARAM + 1; k++) if (fscanf(f, "%lf", &v[k]) != 1) { fprintf(stderr, "bad %s\n", path); return 2; }
        fclose(f);
        snprintf(path, sizeof path, "%s/case_%04d.f32", argv[1], n);
        f = fopen(path, "rb");
        fseek(f, 0, SEEK_END);
        long rl = ftell(f) / 4;
        fseek(f, 0, SEEK_SET);
        float *ref = malloc((size_t)rl * 4 + 4);
        if ((long)fread(ref, 4, (size_t)rl, f) != rl) return 2;
        fclose(f);
        float *mine = NULL;
        long ml = render(v, v[NPARAM], &mine);
        total++;
        double maxd = 0;
        long cmp = ml < rl ? ml : rl;
        for (long k = 0; k < cmp; k++) {
            double e = fabs((double)mine[k] - (double)ref[k]);
            if (e != e) e = 1e9;
            if (e > maxd) maxd = e;
        }
        int ok = ml == rl && maxd < 1e-5;
        if (ml == rl) same_len++;
        if (maxd > worst && ml == rl) worst = maxd;
        if (!ok) {
            bad++;
            if (bad <= 12) printf("case %04d wave %d: length %ld vs ref %ld, max diff %.3g\n", n, (int)v[0], ml, rl, maxd);
        }
        free(ref);
        free(mine);
    }
    printf("%d cases, %d with the same length, %d differ; worst difference among equal lengths %.3g\n", total, same_len, bad, worst);
    return bad ? 1 : 0;
}
