/* SPDX-License-Identifier: MIT
 * Compares BFMXR's preset synths (app/synths.c) with bfxr2's output (tools/gen_synths.js):
 *   cc -O2 -ffp-contract=off -Iapp -Ibase/include tools/check_synths.c app/synths.c -lm -o check_synths
 *   ./check_synths SREF_DIR [SYNTH ...]
 * Each case is rendered a block at a time, as on the FM-1, finished like bfxr2's SoundDSP.finish, and compared with
 * bfxr2's samples: the error's level against the sound's (dB, lower is closer) and the largest error. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "synths.h"

/* app.c's buffer: the sound as µ-law bytes, log2(1 + 255|x|) / 8 in 7 bits and a sign */
static float through_mulaw(float x)
{
    float a = fabsf(x) > 1 ? 1 : fabsf(x);
    int c = (int)(log2f(1.0f + 255.0f * a) * (127.0f / 8.0f) + 0.5f);
    float y = (exp2f((float)(c > 127 ? 127 : c) * (8.0f / 127.0f)) - 1.0f) / 255.0f;
    return x < 0 ? -y : y;
}

static float *load_f32(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    float *b;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(sz ? sz : 4);
    *n = (uint32_t)(fread(b, 4, sz / 4, f));
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    static unsigned char state[SYN_STATE_BYTES] __attribute__((aligned(8)));
    int bad = 0;
    if (argc < 2) { fprintf(stderr, "usage: check_synths SREF_DIR [SYNTH ...]\n"); return 2; }
    for (unsigned si = 0; si < SYN_N; si++) {
        const syn_info_t *sy = &SYN_INFO[si];
        double worst = -999, worst_peak = 0, worst_mu = -999;
        int cases = 0, wc = -1, only = argc <= 2;
        for (int a = 2; a < argc; a++) only |= !strcmp(argv[a], sy->type);
        if (!only) continue;
        for (int c = 0;; c++) {
            char path[512];
            float p[SYN_MAXP];
            uint32_t nref = 0, len;
            FILE *f;
            float *ref, *out;
            double mean = 0, es = 0, rs = 0, peak = 0, db, em = 0;
            snprintf(path, sizeof path, "%s/%s/case_%04d.txt", argv[1], sy->type, c);
            if (!(f = fopen(path, "r"))) break;
            for (unsigned k = 0; k < sy->np; k++) { double v = 0; if (fscanf(f, "%lf", &v) != 1) v = 0; p[k] = (float)v; }
            fclose(f);
            snprintf(path, sizeof path, "%s/%s/case_%04d.f32", argv[1], sy->type, c);
            ref = load_f32(path, &nref);
            len = SYN_ENGINES[si].begin(state, p);
            out = calloc(len ? len : 1, sizeof(float));
            for (uint32_t o = 0; o < len; o += 256)
                SYN_ENGINES[si].run(state, out + o, len - o < 256 ? len - o : 256);
            int wide = len * 2u <= 220500u;      /* app.c: up to 2.5 s kept as 16 bits, longer as µ-law bytes */
            syn_whole_t w;
            memset(&w, 0, sizeof w);
            if (SYN_ENGINES[si].whole)           /* the whole sound's own work, as app.c does it */
                SYN_ENGINES[si].whole(p, len, &w);
            if (w.reverse > 0)
                for (uint32_t i = 0; i < len / 2u; i++) {
                    float a = out[i], b = out[len - 1u - i];
                    out[i] = a * (1 - w.reverse) + b * w.reverse;
                    out[len - 1u - i] = b * (1 - w.reverse) + a * w.reverse;
                }
            for (uint32_t i = len; w.necho && i-- > 0;)
                for (uint32_t r = 0; r < w.necho; r++)
                    if (i >= w.echo_off[r])
                        out[i] += out[i - w.echo_off[r]] * w.echo_gain[r];
            for (uint32_t i = 0; i < len; i++) mean += isfinite(out[i]) ? out[i] : 0;
            mean /= len ? len : 1;
            {
                uint32_t fl = len / 2 < 220 ? len / 2 : 220;
                float vol = p[0] < 0 ? 0 : p[0] > 1 ? 1 : p[0];
                if (fl < 1) fl = 1;
                for (uint32_t i = 0; i < len; i++) {
                    double v = isfinite(out[i]) ? out[i] - mean : 0, fade = 1;
                    if ((double)i / fl < fade) fade = (double)i / fl;
                    if ((double)(len - 1 - i) / fl < fade) fade = (double)(len - 1 - i) / fl;
                    out[i] = (float)(tanh(v) * vol * 0.95 * fade);
                }
                if (w.normalize) {                   /* (Riftr: up to 4 times louder, to a peak of 0.9) */
                    double peak = 0;
                    for (uint32_t i = 0; i < len; i++) peak = fabs(out[i]) > peak ? fabs(out[i]) : peak;
                    if (peak > vol * 0.00001) {
                        double gain = vol * 0.9 / peak;
                        gain = gain < 1 ? 1 : gain > 4 ? 4 : gain;
                        for (uint32_t i = 0; i < len; i++) out[i] = (float)(out[i] * gain);
                    }
                }
            }
            if (len != nref) {
                printf("%s case %d: %u samples, bfxr2 %u\n", sy->type, c, len, nref);
                bad++;
            }
            for (uint32_t i = 0; i < len && i < nref; i++) {
                double e = out[i] - ref[i], vol = p[0] < 0 ? 0 : p[0] > 1 ? 1 : p[0];
                double x = vol > 0 ? out[i] / (vol * 0.95) : 0;
                double m = vol > 0 ? (wide ? (double)(int16_t)(x * 32767.0) / 32767.0 : through_mulaw((float)x)) * vol * 0.95 - ref[i] : 0;
                es += e * e; rs += (double)ref[i] * ref[i]; em += m * m;
                if (fabs(e) > peak) peak = fabs(e);
            }
            db = 10 * log10((es + 1e-30) / (rs + 1e-30));
            if (getenv("VERBOSE")) {
                uint32_t first = 0;
                while (first < len && first < nref && fabs(out[first] - ref[first]) < 0.01) first++;
                printf("  %s case %d: %.1f dB, %u samples, first error over 0.01 at %u\n", sy->type, c, db, len, first);
            }
            if (db > worst) { worst = db; wc = c; worst_peak = peak; }
            if (10 * log10((em + 1e-30) / (rs + 1e-30)) > worst_mu) worst_mu = 10 * log10((em + 1e-30) / (rs + 1e-30));
            cases++;
            free(ref); free(out);
        }
        printf("%-10s %3d cases, worst error %6.1f dB (case %d, largest %.4f); in the FM-1's buffer %5.1f dB%s\n",
               sy->type, cases, worst, wc, worst_peak, worst_mu, worst > -40 ? "  <- check by ear" : "");
    }
    return bad ? 1 : 0;
}
