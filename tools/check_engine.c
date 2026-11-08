/* SPDX-License-Identifier: MIT
 * The FM-1 engine (app/bfxr.c) against bfxr2's JavaScript, on a computer: every reference sound from
 * tools/gen_ref.js, played by the engine with the same parameters and random seed.
 *   cc -O2 -ffp-contract=off -I base/include -I app -o /tmp/check_engine tools/check_engine.c app/bfxr.c -lm
 *   /tmp/check_engine REF_DIR [WAV_DIR]    (WAV_DIR: each case as case_NNNN.wav, the engine's, to listen to)
 * Known difference: the TAN wave through a filter. TAN sends values near 1e16 into the filters, whose output is then
 * decided by rounding (float here, double in JavaScript): the same loudness, a slightly different crackle. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "bfxr.h"

/* what is heard: the loudness in windows of 1024 samples (within 1 dB, or both below -50 dB) and the pitch, as zero
 * crossings over 4096 samples (within 3 %, or within 4 of each other); a one-sample shift of a period passes, a wrong
 * sound does not */
static double win_db(const float *a, long na, long w, long len)
{
    double e = 0;
    long i;
    for (i = w; i < w + len; i++)
        e += i < na ? (double)a[i] * a[i] : 0;
    return 10 * log10(e / len + 1e-12);
}
static long win_zc(const float *a, long na, long w, long len)
{
    long z = 0, i;
    for (i = w > 0 ? w : 1; i < w + len; i++)
        z += ((i < na ? a[i] : 0) >= 0) != ((i - 1 < na ? a[i - 1] : 0) >= 0);
    return z;
}
static int heard_same(const float *a, long na, const float *b, long nb, int *where)
{
    long w, n = na > nb ? na : nb;
    for (w = 0; w + 1024 <= n; w += 1024) {
        double ea = win_db(a, na, w, 1024), eb = win_db(b, nb, w, 1024);
        if (ea < -50 && eb < -50)
            continue;
        if (fabs(ea - eb) > 1.0) {
            *where = (int)(w / 1024);
            return 0;
        }
    }
    for (w = 0; w + 4096 <= n; w += 1024) {
        long za = win_zc(a, na, w, 4096), zb = win_zc(b, nb, w, 4096);
        if (win_db(a, na, w, 4096) < -50 && win_db(b, nb, w, 4096) < -50)
            continue;
        if (labs(za - zb) > 4 && labs(za - zb) * 100 > 3 * (za + zb) / 2) {
            *where = (int)(w / 1024);
            return 0;
        }
    }
    return 1;
}

static void wav(const char *path, const float *x, long n)
{
    FILE *f = fopen(path, "wb");
    unsigned hdr[11] = {0x46464952, 36 + 2 * n, 0x45564157, 0x20746d66, 16, 0x00010001, 44100, 88200, 0x00100002,
                        0x61746164, 2 * n};
    long i;
    fwrite(hdr, 4, 11, f);
    for (i = 0; i < n; i++) {
        short s = (short)lrintf(fmaxf(-1, fminf(1, x[i])) * 32767);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static bfxr_voice_t v;
    int n, bad = 0, total = 0, exact = 0;
    double worst_rel = 0;
    for (n = 0;; n++) {
        char path[512];
        double q[33], e2 = 0, r2 = 0, maxd = 0;
        bfxr_sound_t s;
        float *ref, *mine;
        long rl, ml, k, cap;
        int i;
        FILE *f;
        snprintf(path, sizeof path, "%s/case_%04d.txt", argv[1], n);
        if (!(f = fopen(path, "r")))
            break;
        for (i = 0; i < 33; i++)
            if (fscanf(f, "%lf", &q[i]) != 1)
                return 2;
        fclose(f);
        /* gen_ref.js order: waveType, masterVolume, then as bfxr.h */
        s.p[BP_WAVE] = (float)q[0];
        s.p[BP_VOLUME] = (float)q[1];
        for (i = 2; i < BP_COUNT; i++)
            s.p[i] = (float)q[i];
        snprintf(path, sizeof path, "%s/case_%04d.f32", argv[1], n);
        f = fopen(path, "rb");
        fseek(f, 0, SEEK_END);
        rl = ftell(f) / 4;
        fseek(f, 0, SEEK_SET);
        ref = malloc(rl * 4 + 4);
        if ((long)fread(ref, 4, rl, f) != rl)
            return 2;
        fclose(f);
        bfxr_start(&v, &s, 1.0f, (uint32_t)(unsigned long long)floor(q[32] * 4294967295.0));
        cap = (long)v.length + 16;
        mine = calloc(cap, 4);
        for (ml = 0; ml < cap && v.active; ml += 1)
            bfxr_render(&v, mine + ml, 1);
        while (ml > 0 && fabsf(mine[ml - 1]) <= 0.2e-2f)   /* the JavaScript trims the silent end */
            ml--;
        for (k = 0; k < rl; k++) {
            double d = (k < ml ? mine[k] : 0) - ref[k];
            e2 += d * d;
            r2 += (double)ref[k] * ref[k];
            if (fabs(d) > maxd)
                maxd = fabs(d);
        }
        total++;
        {
            double rel = r2 > 0 ? sqrt(e2 / r2) : sqrt(e2);
            long dl = labs(ml - rl);
            int len_ok = dl <= 64 + rl / 200, where = -1;
            if (len_ok && rel < 0.02) {
                exact++;
                if (rel > worst_rel)
                    worst_rel = rel;
            } else if (!len_ok || !heard_same(mine, ml, ref, rl, &where)) {
                bad++;
                printf("case %04d wave %2d: length %ld vs %ld, error %.2f%% of the signal, heard differently from "
                       "window %d\n", n, (int)q[0], ml, rl, 100 * rel, where);
            }
        }
        if (argc > 2) {
            snprintf(path, sizeof path, "%s/case_%04d.wav", argv[2], n);
            wav(path, mine, ml);
        }
        free(ref);
        free(mine);
    }
    printf("%d cases: %d the same sample for sample (within 2%%, worst %.2f%%), %d the same to the ear (loudness and "
           "pitch), %d different\n", total, exact, 100 * worst_rel, total - exact - bad, bad);
    return bad ? 1 : 0;
}
