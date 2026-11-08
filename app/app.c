/* SPDX-License-Identifier: MIT
 * BFMXR for the FM-1: bfxr.net's sound maker on the keyboard. The engine and the generators are bfxr.c; this is the
 * screen, the controls and the sound's way out. Part of fm1-base's unity build (after the platform: gfx.c and
 * announce.c are in scope); bfxr.c is a separate unit at -O2 (fm1.toml o2).
 *
 *   PRESET          a kind of sound (PICKUP/COIN .. RANDOMIZE): a new one of that kind, played
 *   EDIT            another new sound of the same kind
 *   FX              mutate the sound a little
 *   SELECT          scroll the list of 32 parameters; K1 .. K4 change the four marked rows
 *   ALGO            the wave
 *   PLAY, the keys  play it (C4 at the sound's own pitch, the other keys transpose it; keys: up to 4 at once);
 *                   OCT- / OCT+ the octave
 *   SAVE            keep the sound (it is back at the next power-on); MASTER the volume
 *   SAVE + a white key   store the sound in that slot (16 slots, one a white key)
 *   SEL + a white key    recall the slot's sound and play it
 *   GLO             the settings: play on change, voices, fast knobs, start-up, MIDI in channel
 * The slots go to and from a computer over USB SysEx (the site's Library page): see "the slots over USB" below. */
#include "fastmath.h"
#include "bfxr.h"
#include "splash_data.h"                         /* the start-up animation and sound (tools/gen_splash.py) */

/* the colours: bfxr2's tan ground, cream, maroon */
#define K_BG RGB(231, 209, 167)
#define K_ROW RGB(214, 194, 158)
#define K_TRACK RGB(204, 189, 161)
#define K_INK RGB(30, 16, 12)
#define K_LABEL RGB(94, 48, 40)
#define K_BAR RGB(102, 57, 49)
#define K_SOFT RGB(147, 116, 98)

#define SET_MAGIC 0x42465831u            /* "BFX1": the settings record; another magic loads a fresh sound */
typedef struct {
    uint32_t magic;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t gen;                         /* the kind of sound last made (GEN_*) */
    uint8_t sel;                         /* the first of the four rows K1 .. K4 change */
    uint8_t slot;                        /* the slot last stored or recalled, 1 .. 16 (0: none) */
    float p[BP_COUNT];                   /* the sound */
    /* the GLO menu's settings (0.6; a record from before them loads with the defaults) */
    uint8_t play_on_change;              /* 1: a change plays once the knobs rest */
    uint8_t voices;                      /* 1 .. NVOICES sounds at once (1: one at a time, as bfxr.net) */
    uint8_t fast_knobs;                  /* 1: a fast turn moves 3x */
    uint8_t startup;                     /* 0 animation and sound, 1 animation only, 2 off */
    uint8_t midi_ch;                     /* MIDI in: 0 every channel, 1 .. 16 one */
    uint8_t cfg_rsv[3];
} settings_t;
#define SET_OLD_SIZE ((uint32_t)__builtin_offsetof(settings_t, play_on_change))   /* the record before the menu's settings */
static settings_t set;
static uint8_t set_dirty;

/* the slots: 16 sounds, one a white key (left to right), kept in flash */
#define NSLOTS 16u
#define BANK_MAGIC 0x424E4B31u           /* "BNK1" */
typedef struct {
    uint32_t magic;
    uint32_t used;                       /* bit k: slot k holds a sound */
    float p[NSLOTS][BP_COUNT];
} bank_t;
static bank_t bank;

/* ------------------------------------------------------------------ sound --- */
/* NVOICES sounds at once: a key or a MIDI note each its own (a chord), the oldest cut when all play. PLAY, EDIT, FX,
 * PRESET and a change's preview stop the others first, as bfxr.net plays one sound at a time. The main loop asks
 * through a ring (it writes, the audio interrupt reads); MIDI notes start in the interrupt itself. Each block the
 * render times itself: past BX_LOAD_MAX of the block's time it cuts its oldest sound (heavy chords: TAN with
 * HARMONICS), so the audio never runs late. */
#define NVOICES 4u
#define BX_LOAD_MAX 70u                          /* % of a block's time */
static bfxr_voice_t bx_voice[NVOICES];
static uint32_t bx_age[NVOICES], bx_clock;
static float bx_buf[APP_BLOCK], bx_one[APP_BLOCK];
typedef struct { float pitch; uint8_t mono; } bx_req_t;
static bx_req_t bx_ring[8];
static volatile uint32_t bx_head, bx_tail;       /* main loop writes head, the audio interrupt tail */
static volatile int32_t bx_playing;              /* sounds playing now */
static volatile uint32_t bx_load, bx_cuts;       /* the last block's time in %, the sounds cut for time */
static uint32_t bx_seed = 0x2545F491u;
static float bx_gain;
static volatile uint32_t sp_snd = SPLASH_SND_N;  /* the start-up sound: the next sample (SPLASH_SND_N: none) */

static void play_semitones_mode(int32_t st, int mono)
{
    uint32_t h = bx_head;
    if (h - bx_tail >= 8u)
        return;                                  /* (eight asked in one block: the rest are dropped) */
    bx_ring[h & 7u].pitch = fm_exp2f((float)st / 12.0f);
    bx_ring[h & 7u].mono = (uint8_t)mono;
    bx_head = h + 1u;
}
static void play_semitones(int32_t st) { play_semitones_mode(st, 0); }   /* a key: beside what plays */

static void voice_start(float pitch, int mono)   /* audio interrupt */
{
    uint32_t i, pick = 0, k;
    bfxr_sound_t s;
    uint32_t nv = set.voices >= 1u && set.voices <= NVOICES ? set.voices : NVOICES;
    if (mono || nv == 1u)
        for (i = 0; i < NVOICES; i++)
            bx_voice[i].active = 0;
    for (i = nv; i < NVOICES; i++)               /* (fewer voices set: the others stop) */
        bx_voice[i].active = 0;
    for (i = 0; i < nv; i++) {                   /* a free voice, else the oldest */
        if (!bx_voice[i].active) {
            pick = i;
            break;
        }
        if (bx_age[i] < bx_age[pick])
            pick = i;
    }
    for (k = 0; k < BP_COUNT; k++)
        s.p[k] = set.p[k];
    bx_seed = bx_seed * 1664525u + 1013904223u;
    bfxr_start(&bx_voice[pick], &s, pitch, bx_seed);
    bx_age[pick] = ++bx_clock;
}

static void midi_in(void)                /* audio interrupt: notes from USB / TRS play the sound, a chord too */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if ((st & 0xF0u) == 0x90u && d2 && (!set.midi_ch || (st & 15u) + 1u == set.midi_ch))
            voice_start(fm_exp2f((float)((int32_t)d1 - 60) / 12.0f), 0);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, v, t0 = plat_ticks24(), dt, budget;
    int32_t playing = 0;
    float gain = (float)master_q12 / 4096.0f * 8000000.0f;   /* 1.0 is near full scale, as in a browser */
    midi_in();
    while (bx_tail != bx_head) {
        bx_req_t r = bx_ring[bx_tail & 7u];
        voice_start(r.pitch, r.mono);
        bx_tail = bx_tail + 1u;
    }
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    for (i = 0; i < n; i++)
        bx_buf[i] = 0;
    for (v = 0; v < NVOICES; v++) {
        if (!bx_voice[v].active)
            continue;
        bfxr_render(&bx_voice[v], bx_one, n);
        for (i = 0; i < n; i++)
            bx_buf[i] += bx_one[i];
        playing += bx_voice[v].active;
    }
    if (sp_snd < SPLASH_SND_N) {                 /* the start-up sound, under the sounds */
        uint32_t p = sp_snd;
        for (i = 0; i < n && p < SPLASH_SND_N; i++, p++)
            bx_buf[i] += (float)SPLASH_SND[p] * (1.0f / 32768.0f);
        sp_snd = p;
    }
    for (i = 0; i < n; i++) {
        float s;
        bx_gain += (gain - bx_gain) * 0.004f;              /* ~5 ms: turning MASTER does not click */
        s = fm_clampf(bx_buf[i], -1.0f, 1.0f) * bx_gain;   /* (a chord can add up past full scale) */
        lr[2 * i] = (int32_t)s;
        lr[2 * i + 1] = (int32_t)s;
    }
    dt = plat_ticks24() - t0;
    budget = n * (PLAT_TICKS_PER_US * 1000000u / 44100u);   /* the block's time, in ticks */
    bx_load = budget ? dt * 100u / budget : 0;
    if (bx_load > BX_LOAD_MAX && playing > 1) {  /* too slow: the oldest sound goes */
        uint32_t old = NVOICES;
        for (v = 0; v < NVOICES; v++)
            if (bx_voice[v].active && (old == NVOICES || bx_age[v] < bx_age[old]))
                old = v;
        if (old < NVOICES) {
            bx_voice[old].active = 0;
            playing--;
            bx_cuts++;
        }
    }
    bx_playing = playing;
}

void app_panic(void)
{
    uint32_t v;
    for (v = 0; v < NVOICES; v++)
        bx_voice[v].active = 0;
    bx_tail = bx_head;
}

/* --------------------------------------------------------------- settings --- */
static void cfg_defaults(void)
{
    set.play_on_change = 1;
    set.voices = NVOICES;
    set.fast_knobs = 1;
    set.startup = 0;
    set.midi_ch = 0;
    set.cfg_rsv[0] = set.cfg_rsv[1] = set.cfg_rsv[2] = 0;
}

static void settings_fresh(void)
{
    bfxr_sound_t s;
    uint32_t k;
    set.magic = SET_MAGIC;
    set.octave = 0;
    set.gen = GEN_PICKUP;
    set.sel = 0;
    set.slot = 0;
    cfg_defaults();
    bfxr_generate(&s, GEN_PICKUP);
    for (k = 0; k < BP_COUNT; k++)
        set.p[k] = s.p[k];
}

static void clamp_sound(float *p)                /* a stored or received sound, kept in range */
{
    uint32_t k;
    for (k = 0; k < BP_COUNT; k++) {
        bfxr_sound_t s;
        bfxr_set(&s, k, p[k]);
        p[k] = s.p[k];
    }
}

void app_init(void)
{
    uint32_t k;
    app_panic();                                 /* (silence: a restart in the simulator keeps the old RAM) */
    if (plat_store_load(OBJ_BANK, &bank, sizeof bank) != (int)sizeof bank || bank.magic != BANK_MAGIC) {
        bank.magic = BANK_MAGIC;
        bank.used = 0;
    }
    bank.used &= (1u << NSLOTS) - 1u;
    for (k = 0; k < NSLOTS; k++)
        clamp_sound(bank.p[k]);
    int got = plat_store_load(OBJ_SETTINGS, &set, sizeof set);
    if (got == (int)SET_OLD_SIZE)                /* (saved before the menu: its settings at their defaults) */
        cfg_defaults();
    if ((got != (int)sizeof set && got != (int)SET_OLD_SIZE) || set.magic != SET_MAGIC ||
        set.octave < -2 || set.octave > 2 || set.gen >= GEN_COUNT || set.sel > BP_COUNT - 4 || set.slot > NSLOTS) {
        settings_fresh();
        return;
    }
    clamp_sound(set.p);
    if (set.play_on_change > 1u || set.voices < 1u || set.voices > NVOICES || set.fast_knobs > 1u ||
        set.startup > 2u || set.midi_ch > 16u)
        cfg_defaults();
}

static void sound_get(bfxr_sound_t *s)
{
    uint32_t k;
    for (k = 0; k < BP_COUNT; k++)
        s->p[k] = set.p[k];
}
static void sound_put(const bfxr_sound_t *s)
{
    uint32_t k;
    for (k = 0; k < BP_COUNT; k++)
        set.p[k] = s->p[k];
    set_dirty = 1;
}

/* --------------------------------------------------------------------- UI --- */
static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint32_t edit_at;                    /* a parameter changed: played once the knobs rest */
    int32_t playing;                    /* as last drawn */
    uint8_t save_used;                   /* SAVE is held and a white key stored a slot: no plain save on release */
    uint8_t menu, menu_row;              /* the GLO menu is open; its row */
    uint8_t redraw;
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void num_text(float v, char *b)   /* -0.45, 0.30, 1.00 */
{
    int32_t c = (int32_t)(fm_fabsf(v) * 100.0f + 0.5f);
    if (v < 0 && c)
        *b++ = '-';
    *b++ = (char)('0' + c / 100 % 10);
    *b++ = '.';
    *b++ = (char)('0' + c / 10 % 10);
    *b++ = (char)('0' + c % 10);
    *b = 0;
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void value_text(uint32_t k, char *t)
{
    if (k == BP_WAVE) {
        t[0] = 0;
        text_cat(t, BFXR_WAVE_NAMES[(uint32_t)set.p[BP_WAVE] % BFXR_NWAVES]);
    } else {
        num_text(set.p[k], t);
    }
}

static void say_param(uint32_t k)
{
    char t[16];
    value_text(k, t);
    app_say(BFXR_PARAMS[k].label, t);
}

static void play_now(void) { play_semitones_mode(12 * set.octave, 1); }   /* one sound, as bfxr.net */

static void generate(uint32_t gen)
{
    bfxr_sound_t s;
    set.gen = (uint8_t)(gen % GEN_COUNT);
    bfxr_seed(plat_ms() ^ (ui.keys << 7));
    bfxr_generate(&s, set.gen);
    sound_put(&s);
    app_say(BFXR_GEN_NAMES[set.gen], "");
    play_now();
}

static void edit_param(uint32_t k, int32_t e)
{
    bfxr_sound_t s;
    const bfxr_param_t *d = &BFXR_PARAMS[k];
    sound_get(&s);
    if (k == BP_WAVE) {
        int32_t w = ((int32_t)s.p[k] + e) % BFXR_NWAVES;
        bfxr_set(&s, k, (float)(w < 0 ? w + BFXR_NWAVES : w));
    } else {
        int32_t a = e < 0 ? -e : e;
        float step = (d->max - d->min) * 0.01f * (a >= 3 && set.fast_knobs ? 3.0f : 1.0f);   /* fast turns: 3x */
        bfxr_set(&s, k, s.p[k] + (float)e * step);
    }
    sound_put(&s);
    say_param(k);
    ui.edit_at = plat_ms();
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave)
        return;
    set.octave = (int8_t)o;
    set_dirty = 1;
    t[0] = o < 0 ? '-' : '+';
    t[1] = (char)('0' + (o < 0 ? -o : o));
    t[2] = 0;
    app_say("OCTAVE", t);
}

/* ------------------------------------------------------- the start-up animation --- */
/* tools/gen_splash.py's frames: one palette, each frame the XOR of its colour indices with the frame before, LZ4
 * block-compressed. Its first frame is the boot screen (app_splash, before the app starts); app_ui_init plays the
 * rest at SPLASH_FRAME_MS a frame with the sound, then the editor. Any key or button skips it. */
static uint8_t sp_idx[SPLASH_W * SPLASH_H] PLAT_POOL;   /* the frame shown: colour indices */
static uint8_t sp_tmp[SPLASH_W * SPLASH_H] PLAT_POOL;   /* the next frame's difference */
static int32_t sp_frame = -1;                    /* the frame in sp_idx (-1: none) */
static uint32_t sp_t0, sp_keys, sp_btn;
static uint8_t sp_on;                            /* the animation is playing */

static uint32_t lz4_block(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t cap)   /* bytes out, 0: bad */
{
    const uint8_t *e = s + n;
    uint32_t o = 0;
    while (s < e) {
        uint32_t tok = *s++, lit = tok >> 4, m, off;
        if (lit == 15u) {
            uint32_t b;
            do {
                if (s >= e)
                    return 0;
                b = *s++;
                lit += b;
            } while (b == 255u);
        }
        if (lit > cap - o || lit > (uint32_t)(e - s))
            return 0;
        while (lit) {
            d[o++] = *s++;
            lit--;
        }
        if (s >= e)
            break;                               /* (the last sequence: literals only) */
        if (e - s < 2)
            return 0;
        off = (uint32_t)s[0] | (uint32_t)s[1] << 8;
        s += 2;
        m = tok & 15u;
        if (m == 15u) {
            uint32_t b;
            do {
                if (s >= e)
                    return 0;
                b = *s++;
                m += b;
            } while (b == 255u);
        }
        m += 4u;
        if (!off || off > o || m > cap - o)
            return 0;
        while (m) {
            d[o] = d[o - off];
            o++;
            m--;
        }
    }
    return o;
}

static void sp_apply(uint32_t f)                 /* frame f over the one before it */
{
    uint32_t i, n = SPLASH_W * SPLASH_H;
    if (f >= SPLASH_FRAMES)
        return;
    if (lz4_block(SPLASH_LZ4 + SPLASH_OFF[f], SPLASH_OFF[f + 1] - SPLASH_OFF[f], sp_tmp, n) != n)
        return;                                  /* (a bad frame: the one before stays) */
    for (i = 0; i < n; i++)
        sp_idx[i] ^= sp_tmp[i];
    sp_frame = (int32_t)f;
}

static void sp_draw(void)
{
    uint32_t i, n = SPLASH_W * SPLASH_H;
    cv_begin(SPLASH_W, SPLASH_H, 0);
    for (i = 0; i < n; i++)
        cv_px[i] = swap16(SPLASH_PAL[sp_idx[i] & 31u]);
    cv_blit(0, SPLASH_Y);
}

static void sp_first(void)                       /* frame 0 on a black screen */
{
    uint32_t i;
    for (i = 0; i < SPLASH_W * SPLASH_H; i++)
        sp_idx[i] = 0;
    lcd_fill(0, 0, 240, 240, 0);
    sp_apply(0);
    sp_draw();
}

void app_splash(void) { sp_first(); }

static void sp_end(void)
{
    uint32_t k;
    sp_on = 0;
    for (k = 0; k < NE; k++)                     /* (turns during it are not edits) */
        (void)plat_enc((int)k);
    lcd_fill(0, 0, 240, 240, K_BG);
    ui.keys = plat_keys();                       /* (a key held to skip it plays nothing) */
    ui.btn = plat_buttons();
    ui.redraw = 1;
}

static void sp_frame_tick(void)                  /* app_frame while it plays */
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), now = plat_ms(), target;
    if ((keys & ~sp_keys) || (btn & ~sp_btn)) {  /* a key or button pressed: skip it */
        sp_snd = SPLASH_SND_N;
        sp_end();
        return;
    }
    sp_keys = keys;
    sp_btn = btn;
    target = (now - sp_t0) / SPLASH_FRAME_MS;
    if (target >= SPLASH_FRAMES) {
        sp_end();
        return;
    }
    if ((int32_t)target > sp_frame) {
        while ((int32_t)target > sp_frame && sp_frame + 1 < (int32_t)SPLASH_FRAMES)
            sp_apply((uint32_t)sp_frame + 1u);   /* (every difference in turn: a late frame catches up) */
        sp_draw();
    }
    plat_leds(0, 0);
}

/* ------------------------------------------------------------------ slots --- */
static int32_t key_slot(uint32_t k)              /* the slot of key k (a white key), -1 for a black key */
{
    static const uint8_t WK[NSLOTS] = PLAT_WHITE_KEYS;
    uint32_t i;
    for (i = 0; i < NSLOTS; i++)
        if (WK[i] == k)
            return (int32_t)i;
    return -1;
}

static uint32_t slot_keys(void)                  /* the white keys whose slots hold a sound: their LEDs */
{
    static const uint8_t WK[NSLOTS] = PLAT_WHITE_KEYS;
    uint32_t i, m = 0;
    for (i = 0; i < NSLOTS; i++)
        if (bank.used >> i & 1u)
            m |= 1u << WK[i];
    return m;
}

static int bank_save(void) { return plat_store_save(OBJ_BANK, &bank, sizeof bank); }

static void slot_store(uint32_t i)
{
    char t[4] = {(char)('0' + (i + 1) / 10), (char)('0' + (i + 1) % 10), 0, 0};
    uint32_t k;
    if (t[0] == '0') {
        t[0] = t[1];
        t[1] = 0;
    }
    for (k = 0; k < BP_COUNT; k++)
        bank.p[i][k] = set.p[k];
    bank.used |= 1u << i;
    set.slot = (uint8_t)(i + 1);
    set_dirty = 1;
    app_say(bank_save() == 0 ? "STORED IN SLOT" : "STORE FAILED", t);
}

static void slot_recall(uint32_t i)
{
    char t[4] = {(char)('0' + (i + 1) / 10), (char)('0' + (i + 1) % 10), 0, 0};
    uint32_t k;
    if (t[0] == '0') {
        t[0] = t[1];
        t[1] = 0;
    }
    if (!(bank.used >> i & 1u)) {
        app_say("SLOT EMPTY", t);
        return;
    }
    for (k = 0; k < BP_COUNT; k++)
        set.p[k] = bank.p[i][k];
    set.slot = (uint8_t)(i + 1);
    set_dirty = 1;
    app_say("SLOT", t);
    play_now();
}

/* ------------------------------------------------------- the slots over USB --- */
/* SysEx, all 7-bit, F0 7D 42 46 ("BF", 7D: the non-commercial ID; the screen reader's is 7D 53 52) cmd .. F7:
 *   computer -> FM-1   01                       list: the FM-1 answers 02 for each slot, then 03
 *                      04 SLOT SOUND            write SLOT (0 .. 15; 16: the sound being edited); answered by 05
 *                      06 SLOT                  clear SLOT (0 .. 15); answered by 05
 *   FM-1 -> computer   02 SLOT USED SOUND       SLOT 0 .. 15, then 16 (the sound being edited); USED 0 / 1
 *                      03 VERSION NSLOTS        the end of a list (protocol version 1)
 *                      05 SLOT STATUS           0 done, 1 refused (a bad slot or length), 2 the flash write failed
 * SOUND: the 32 parameters in bfxr.h's order, each a float's 32 bits in five 7-bit bytes, low bits first (160 bytes).
 * The FM-1 sends a list a frame at a time as the computer reads (app_frame). */
#define BSX_SOUND (BP_COUNT * 5u)
static const uint8_t BSX_HEAD[4] = {0xF0, 0x7D, 0x42, 0x46};
static uint8_t bsx_frame[8 + BSX_SOUND];
static int32_t bsx_next = -1;                     /* the list being sent: the next slot (-1 none, NSLOTS + 1 the end) */

static void bsx_put_sound(uint8_t *d, const float *p)
{
    uint32_t k;
    for (k = 0; k < BP_COUNT; k++) {
        union { float f; uint32_t u; } b;
        b.f = p[k];
        d[5 * k] = (uint8_t)(b.u & 0x7Fu);
        d[5 * k + 1] = (uint8_t)(b.u >> 7 & 0x7Fu);
        d[5 * k + 2] = (uint8_t)(b.u >> 14 & 0x7Fu);
        d[5 * k + 3] = (uint8_t)(b.u >> 21 & 0x7Fu);
        d[5 * k + 4] = (uint8_t)(b.u >> 28 & 0x0Fu);
    }
}
static void bsx_get_sound(const uint8_t *d, float *p)
{
    uint32_t k;
    for (k = 0; k < BP_COUNT; k++) {
        union { float f; uint32_t u; } b;
        b.u = (uint32_t)d[5 * k] | (uint32_t)d[5 * k + 1] << 7 | (uint32_t)d[5 * k + 2] << 14 |
              (uint32_t)d[5 * k + 3] << 21 | (uint32_t)(d[5 * k + 4] & 0x0Fu) << 28;
        p[k] = b.f;
    }
    clamp_sound(p);
}
static uint32_t bsx_begin(uint8_t cmd)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        bsx_frame[i] = BSX_HEAD[i];
    bsx_frame[4] = cmd;
    return 5;
}
static void bsx_reply(uint32_t slot, uint32_t status)
{
    uint32_t n = bsx_begin(0x05);
    bsx_frame[n++] = (uint8_t)slot;
    bsx_frame[n++] = (uint8_t)status;
    bsx_frame[n++] = 0xF7;
    (void)plat_sysex_send(bsx_frame, n);
}

/* main loop: the next frames of a list, as many as the computer has room for now */
static void bsx_pump(void)
{
    while (bsx_next >= 0) {
        uint32_t n;
        if (bsx_next <= (int32_t)NSLOTS) {
            uint32_t i = (uint32_t)bsx_next;
            n = bsx_begin(0x02);
            bsx_frame[n++] = (uint8_t)i;
            bsx_frame[n++] = (uint8_t)(i == NSLOTS ? 1u : bank.used >> i & 1u);
            bsx_put_sound(bsx_frame + n, i == NSLOTS ? set.p : bank.p[i]);
            n += BSX_SOUND;
        } else {
            n = bsx_begin(0x03);
            bsx_frame[n++] = 1;
            bsx_frame[n++] = NSLOTS;
        }
        bsx_frame[n++] = 0xF7;
        if (plat_sysex_room() < n)
            return;                              /* (the rest at the next frame, as the computer reads) */
        if (plat_sysex_send(bsx_frame, n) != 0) {
            bsx_next = -1;                        /* no computer: the list is dropped */
            return;
        }
        bsx_next = bsx_next <= (int32_t)NSLOTS ? bsx_next + 1 : -1;
    }
}

int app_sysex(const uint8_t *p, uint32_t n)      /* the bytes between F0 and F7 */
{
    uint32_t slot;
    if (n < 4 || p[0] != 0x7D || p[1] != 0x42 || p[2] != 0x46)
        return 0;
    switch (p[3]) {
    case 0x01:
        bsx_next = 0;
        bsx_pump();
        return 1;
    case 0x04:
        if (n != 5 + BSX_SOUND || p[4] > NSLOTS) {
            bsx_reply(n > 4 ? p[4] : 0x7F, 1);
            return 1;
        }
        slot = p[4];
        if (slot == NSLOTS) {                    /* the sound being edited */
            bsx_get_sound(p + 5, set.p);
            set_dirty = 1;
            app_say("SOUND RECEIVED", "");
            play_now();
            bsx_reply(slot, 0);
        } else {
            bsx_get_sound(p + 5, bank.p[slot]);
            bank.used |= 1u << slot;
            bsx_reply(slot, bank_save() == 0 ? 0u : 2u);
            app_say("RECEIVED SLOT", "");
        }
        ui.redraw = 1;
        return 1;
    case 0x06:
        if (n != 5 || p[4] >= NSLOTS) {
            bsx_reply(n > 4 ? p[4] : 0x7F, 1);
            return 1;
        }
        bank.used &= ~(1u << p[4]);
        if (set.slot == p[4] + 1u)
            set.slot = 0;
        bsx_reply(p[4], bank_save() == 0 ? 0u : 2u);
        ui.redraw = 1;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- the GLO menu --- */
/* GLO opens and closes it (HOME closes it too): SELECT picks a row, K1 or ALGO changes it. Closing keeps the settings
 * (with the sound, as SAVE does). The keys, PLAY and the octave work as ever meanwhile. */
enum { MENU_POC, MENU_VOICES, MENU_FAST, MENU_STARTUP, MENU_MIDI, MENU_ROWS };
static const char *const MENU_LABEL[MENU_ROWS] = {"PLAY ON CHANGE", "VOICES", "FAST KNOBS", "START-UP", "MIDI IN"};

static void menu_value(uint32_t row, char *t)
{
    static const char *const SU[3] = {"ANIM + SOUND", "ANIMATION", "OFF"};
    t[0] = 0;
    switch (row) {
    case MENU_POC: text_cat(t, set.play_on_change ? "ON" : "OFF"); break;
    case MENU_VOICES:
        t[0] = (char)('0' + set.voices);
        t[1] = 0;
        text_cat(t, set.voices == 1u ? " (AS BFXR.NET)" : "");
        break;
    case MENU_FAST: text_cat(t, set.fast_knobs ? "ON" : "OFF"); break;
    case MENU_STARTUP: text_cat(t, SU[set.startup % 3u]); break;
    default:
        if (!set.midi_ch)
            text_cat(t, "ALL CHANNELS");
        else {
            text_cat(t, "CHANNEL ");
            if (set.midi_ch >= 10u) {
                char d[3] = {'1', (char)('0' + set.midi_ch - 10u), 0};
                text_cat(t, d);
            } else {
                char d[2] = {(char)('0' + set.midi_ch), 0};
                text_cat(t, d);
            }
        }
        break;
    }
}

static int32_t wrapi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? hi : v > hi ? lo : v; }

static void menu_change(int32_t e)
{
    char t[24];
    int32_t d = e > 0 ? 1 : -1;
    switch (ui.menu_row) {
    case MENU_POC: set.play_on_change ^= 1u; break;
    case MENU_VOICES: set.voices = (uint8_t)clamp((int32_t)set.voices + d, 1, NVOICES); break;
    case MENU_FAST: set.fast_knobs ^= 1u; break;
    case MENU_STARTUP: set.startup = (uint8_t)wrapi((int32_t)set.startup + d, 0, 2); break;
    default: set.midi_ch = (uint8_t)wrapi((int32_t)set.midi_ch + d, 0, 16); break;
    }
    set_dirty = 1;
    menu_value(ui.menu_row, t);
    app_say(MENU_LABEL[ui.menu_row], t);
}

static void menu_close(void)
{
    ui.menu = 0;
    if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
        set_dirty = 0;
        app_say("SETTINGS", "KEPT");
    } else {
        app_say("SETTINGS", "NOT SAVED");
    }
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    int32_t e;
    for (k = 0; k < NKEYS; k++) {                /* key 0 is F3 (MIDI 53); C4 plays the sound as made */
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit) && !ui.menu && (btn & (1u << B_SAVE | 1u << B_SEL))) {
            int32_t i = key_slot(k);             /* SAVE + a white key: store; SEL + a white key: recall */
            if (i >= 0 && (btn >> B_SAVE & 1u)) {
                slot_store((uint32_t)i);
                ui.save_used = 1;
            } else if (i >= 0)
                slot_recall((uint32_t)i);
        } else if ((keys & bit) && !(ui.keys & bit)) {
            int32_t note = 53 + (int32_t)k + 12 * set.octave;
            play_semitones(note - 60);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | 100u << 24, TO_USB);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            int32_t note = 53 + (int32_t)k + 12 * set.octave;
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_GLO || (ui.menu && k == B_HOME)) {
            if (ui.menu)
                menu_close();
            else {
                ui.menu = 1;
                app_say("SETTINGS", "");
            }
            ui.redraw = 1;
            continue;
        }
        if (ui.menu && k != B_PLAY && k != B_OCTDN && k != B_OCTUP)
            continue;                            /* (the menu open: the rest wait) */
        if (k == B_PLAY)
            play_now();
        else if (k == B_EDIT)
            generate(set.gen);
        else if (k == B_FX) {
            bfxr_sound_t s;
            sound_get(&s);
            bfxr_seed(plat_ms());
            bfxr_mutate(&s);
            sound_put(&s);
            app_say("MUTATED", "");
            play_now();
        } else if (k == B_OCTDN)
            octave_step(-1);
        else if (k == B_OCTUP)
            octave_step(1);
        else if (k == B_HOME) {
            set.sel = 0;
            ui.redraw = 1;
        } else if (k == B_SAVE)
            ui.save_used = 0;                    /* (it acts on release: a slot may be stored meanwhile) */
    }
    if (!ui.menu && !(btn >> B_SAVE & 1u) && (ui.btn >> B_SAVE & 1u) && !ui.save_used) {   /* SAVE tapped: keep it */
        if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
            set_dirty = 0;
            app_say("SAVED", "");
        } else {
            app_say("SAVE", "FAILED");
        }
    }
    if (!(btn >> B_SAVE & 1u) && (ui.btn >> B_SAVE & 1u) && ui.save_used &&
        plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
        set_dirty = 0;                           /* (a slot stored: the sound and its slot kept too) */
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (ui.menu) {                           /* the menu: SELECT a row, K1 / ALGO its value */
            if (k == EN_SELECT) {
                char t[24];
                ui.menu_row = (uint8_t)clamp((int32_t)ui.menu_row + e, 0, MENU_ROWS - 1);
                menu_value(ui.menu_row, t);
                app_say(MENU_LABEL[ui.menu_row], t);
            } else if (k == EN_K1 || k == EN_ALGO)
                menu_change(e);
            ui.redraw = 1;
            continue;
        }
        if (k == EN_SELECT) {
            set.sel = (uint8_t)clamp((int32_t)set.sel + e, 0, BP_COUNT - 4);
            say_param(set.sel);
        } else if (k == EN_PRESET) {
            int32_t g = ((int32_t)set.gen + e) % GEN_COUNT;
            generate((uint32_t)(g < 0 ? g + GEN_COUNT : g));
        } else if (k == EN_ALGO) {
            edit_param(BP_WAVE, e);
        } else {
            edit_param(set.sel + (k - EN_K1), e);
        }
        ui.redraw = 1;
    }
    if (ui.edit_at && plat_ms() - ui.edit_at >= 300u) {   /* as bfxr.net: a change is heard once you let go */
        ui.edit_at = 0;
        if (set.play_on_change)
            play_now();
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds(btn | (set_dirty ? 1u << B_SAVE : 0u) | (bx_playing ? LED_PLAY_GREEN : 0u) | (ui.menu ? 1u << B_GLO : 0u),
              (btn & (1u << B_SAVE | 1u << B_SEL)) ? slot_keys() | keys : keys);   /* SAVE / SEL held: the slots */
}

void app_input_only(void)
{
    if (!sp_on)
        input();
}

/* the value as a bar: from the left for a range from 0, from the middle for one from -x to x. A bar from the left
 * reads straight off the number (0.43: 43 % of the bar), also where the range starts a little above 0 (DECAY 0.03,
 * LOWPASS 0.01) or ends below 1 (FREQ CUTOFF, SQUARE DUTY 0.99): the same number, the same length on every row */
static void value_bar(int32_t x, int32_t y, int32_t w, uint32_t k)
{
    const bfxr_param_t *d = &BFXR_PARAMS[k];
    float t = d->min < 0 ? (set.p[k] - d->min) / (d->max - d->min) : fm_clampf(set.p[k], 0, 1);
    int32_t f = (int32_t)(t * (float)w + 0.5f);
    cv_rect(x, y, w, 6, K_TRACK);
    if (d->min < 0) {
        int32_t mid = w / 2;
        if (f >= mid)
            cv_rect(x + mid, y, f - mid, 6, K_BAR);
        else
            cv_rect(x + f, y, mid - f, 6, K_BAR);
        cv_rect(x + mid, y - 1, 1, 8, K_LABEL);
    } else {
        cv_rect(x, y, f, 6, K_BAR);
    }
}

#define ROWS 9
#define ROW_H 20
#define LIST_Y 34

static void draw(void)
{
    char t[32];
    uint32_t r, now = plat_ms();
    int32_t top = clamp((int32_t)set.sel - 2, 0, BP_COUNT - ROWS);
    cv_begin(240, 240, K_BG);
    /* the header: the name, the kind of sound, and PLAYING while it plays */
    cv_rect(0, 0, 240, 30, K_BAR);
    r = (uint32_t)cv_text(10, 5, &FONT_B, APP_NAME, K_BG);
    if (set.slot) {                              /* the slot last stored or recalled */
        char sl[8] = "SLOT ";
        sl[5] = (char)(set.slot >= 10 ? '1' : '0' + set.slot);
        sl[6] = (char)(set.slot >= 10 ? '0' + set.slot - 10 : 0);
        sl[7] = 0;
        cv_text((int32_t)r + 10, 10, &FONT_XS, sl, K_TRACK);
    }
    cv_text(236 - text_w(&FONT_XS, BFXR_GEN_NAMES[set.gen]), 4, &FONT_XS, BFXR_GEN_NAMES[set.gen], K_BG);
    if (ui.playing)
        cv_text(236 - text_w(&FONT_XS, "PLAYING"), 16, &FONT_XS, "PLAYING", K_TRACK);
    /* the parameters: the four K1 .. K4 change are marked */
    for (r = 0; r < ROWS; r++) {
        uint32_t k = (uint32_t)top + r;
        int32_t y = LIST_Y + (int32_t)r * ROW_H;
        int on = k >= set.sel && k < set.sel + 4u;
        if (on) {
            static const char *const KN[4] = {"K1", "K2", "K3", "K4"};
            cv_rect(0, y, 240, ROW_H - 2, K_ROW);
            cv_rect(3, y + 3, 20, 12, K_BAR);
            cv_text(13 - text_w(&FONT_XS, KN[k - set.sel]) / 2, y + 3, &FONT_XS, KN[k - set.sel], K_BG);
        }
        cv_text(28, y + 3, &FONT_XS, BFXR_PARAMS[k].label, on ? K_INK : K_LABEL);
        value_text(k, t);
        if (k == BP_WAVE)
            cv_text(236 - text_w(&FONT_XS, t), y + 3, &FONT_XS, t, K_BAR);
        else {
            value_bar(128, y + 6, 70, k);
            cv_text(236 - text_w(&FONT_XS, t), y + 3, &FONT_XS, t, on ? K_INK : K_SOFT);
        }
    }
    /* the GLO menu, over the list */
    if (ui.menu) {
        cv_rect(0, 32, 240, 182, K_BG);
        cv_text(10, 40, &FONT_B, "SETTINGS", K_BAR);
        for (r = 0; r < MENU_ROWS; r++) {
            int32_t y = 66 + (int32_t)r * 26;
            int on = r == ui.menu_row;
            if (on)
                cv_rect(0, y - 3, 240, 22, K_ROW);
            cv_text(10, y, &FONT_XS, MENU_LABEL[r], on ? K_INK : K_LABEL);
            menu_value(r, t);
            cv_text(232 - text_w(&FONT_XS, t), y, &FONT_XS, t, on ? K_BAR : K_SOFT);
        }
        cv_text(10, 198, &FONT_XS, "SELECT: ROW   K1: CHANGE   GLO: CLOSE", K_SOFT);
    }
    /* SAVE or SEL held: the 16 slots, as the white keys lie (filled: a sound stored) */
    if (ui.btn & (1u << B_SAVE | 1u << B_SEL)) {
        const char *what = (ui.btn >> B_SAVE & 1u) ? "STORE: PRESS A WHITE KEY" : "RECALL: PRESS A WHITE KEY";
        cv_rect(6, 44, 228, 160, K_BG);
        cv_rect(6, 44, 228, 1, K_BAR);
        cv_rect(6, 203, 228, 1, K_BAR);
        cv_text(120 - text_w(&FONT_XS, what) / 2, 54, &FONT_XS, what, K_INK);
        for (r = 0; r < NSLOTS; r++) {
            int32_t x = 12 + (int32_t)(r % 8u) * 27, y = 84 + (int32_t)(r / 8u) * 52;
            int used = bank.used >> r & 1u, cur = set.slot == r + 1u;
            char n[3] = {(char)(r + 1 >= 10 ? '1' : '0' + r + 1), (char)(r + 1 >= 10 ? '0' + r + 1 - 10 : 0), 0};
            if (cur)
                cv_rect(x - 2, y - 2, 28, 44, K_INK);
            cv_rect(x, y, 24, 40, used ? K_BAR : K_TRACK);
            cv_text(x + 12 - text_w(&FONT_XS, n) / 2, y + 12, &FONT_XS, n, used ? K_BG : K_SOFT);
        }
    }
    /* the scroll position */
    cv_rect(238, LIST_Y + top * (ROWS * ROW_H) / BP_COUNT, 2, ROWS * ROWS * ROW_H / BP_COUNT, K_SOFT);
    /* the footer: a message for a moment, else what the buttons do */
    cv_rect(0, 216, 240, 1, K_SOFT);
    if (now < ui.msg_until) {
        int32_t x = cv_text(8, 222, &FONT_XS, ui.msg[0], K_INK);
        cv_text(x + 8, 222, &FONT_XS, ui.msg[1], K_BAR);
    } else {
        char o[3] = {set.octave < 0 ? '-' : '+', (char)('0' + (set.octave < 0 ? -set.octave : set.octave)), 0};
        t[0] = 0;
        text_cat(t, "EDIT NEW  FX MUTATE  OCT ");
        text_cat(t, o);
        cv_text(8, 222, &FONT_XS, t, K_SOFT);
    }
    cv_blit(0, 0);
}

/* a screen reader that has just started listening hears where it is */
static void speak(void)
{
    char t[80];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, BFXR_GEN_NAMES[set.gen]);
        text_cat(t, ". ");
        text_cat(t, BFXR_PARAMS[set.sel].label);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

void app_ui_init(void)
{
    if (sp_frame != 0)                           /* (the host simulator shows no boot screen) */
        sp_first();
    sp_keys = plat_keys();
    sp_btn = plat_buttons();
    sp_t0 = plat_ms();
    sp_on = 1;
    sp_snd = set.startup == 0 ? 0u : SPLASH_SND_N;   /* the sound, from the start (START-UP: ANIM + SOUND) */
    if (set.startup == 2u)                       /* START-UP: OFF (the boot screen showed its first frame) */
        sp_end();
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    if (sp_on) {                                 /* the start-up animation */
        sp_frame_tick();
        bsx_pump();
        return;
    }
    input();
    bsx_pump();
    speak();
    if (ui.playing != bx_playing) {
        ui.playing = bx_playing;
        ui.redraw = 1;
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "gen"))
        *got = set.gen;
    else if (!strcmp(what, "wave"))
        *got = (int)set.p[BP_WAVE];
    else if (!strcmp(what, "sel"))
        *got = set.sel;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "playing"))           /* sounds playing now */
        *got = bx_playing;
    else if (!strcmp(what, "cuts"))              /* sounds cut for time */
        *got = (int)bx_cuts;
    else if (!strcmp(what, "slots"))            /* the slots holding a sound, a bit each */
        *got = (int)bank.used;
    else if (!strcmp(what, "slot"))
        *got = set.slot;
    else if (!strcmp(what, "splash"))            /* the start-up animation is playing */
        *got = sp_on;
    else if (!strcmp(what, "menu"))
        *got = ui.menu;
    else if (!strcmp(what, "voices"))
        *got = set.voices;
    else if (!strcmp(what, "poc"))
        *got = set.play_on_change;
    else if (!strcmp(what, "startup"))
        *got = set.startup;
    else if (!strcmp(what, "midich"))
        *got = set.midi_ch;
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')   /* pN: parameter N x 100 */
        *got = (int)(set.p[atoi(what + 1) % BP_COUNT] * 100.0f + (set.p[atoi(what + 1) % BP_COUNT] < 0 ? -0.5f : 0.5f));
    else
        return 0;
    return 1;
}
#endif
