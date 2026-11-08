/* SPDX-License-Identifier: MIT
 * BFXR for the FM-1: bfxr.net's sound maker on the keyboard. The engine and the generators are bfxr.c; this is the
 * screen, the controls and the sound's way out. Part of fm1-base's unity build (after the platform: gfx.c and
 * announce.c are in scope); bfxr.c is a separate unit at -O2 (fm1.toml o2).
 *
 *   PRESET          a kind of sound (PICKUP/COIN .. RANDOMIZE): a new one of that kind, played
 *   EDIT            another new sound of the same kind
 *   FX              mutate the sound a little
 *   SELECT          scroll the list of 32 parameters; K1 .. K4 change the four marked rows
 *   ALGO            the wave
 *   PLAY, the keys  play it (C4 at the sound's own pitch, the other keys transpose it); OCT- / OCT+ the octave
 *   SAVE            keep the sound (it is back at the next power-on); MASTER the volume
 *   SAVE + a white key   store the sound in that slot (16 slots, one a white key)
 *   SEL + a white key    recall the slot's sound and play it
 * The slots go to and from a computer over USB SysEx (the site's Library page): see "the slots over USB" below. */
#include "fastmath.h"
#include "bfxr.h"

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
} settings_t;
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
/* the main loop asks for a sound (the parameters in set.p, a pitch); the audio interrupt starts it */
static bfxr_voice_t bx_voice;
static float bx_buf[APP_BLOCK];
static volatile uint32_t trig_seq, trig_done;
static volatile float trig_pitch = 1.0f;
static volatile int32_t bx_playing;
static uint32_t bx_seed = 0x2545F491u;
static float bx_gain;

static void play_semitones(int32_t st)
{
    trig_pitch = fm_exp2f((float)st / 12.0f);
    trig_seq++;
}

static void midi_in(void)                /* audio interrupt: notes from USB / TRS play the sound */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if ((st & 0xF0u) == 0x90u && d2)
            play_semitones((int32_t)d1 - 60);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    float gain = (float)master_q12 / 4096.0f * 8000000.0f;   /* 1.0 is near full scale, as in a browser */
    midi_in();
    if (trig_seq != trig_done) {
        bfxr_sound_t s;
        trig_done = trig_seq;
        for (i = 0; i < BP_COUNT; i++)
            s.p[i] = set.p[i];
        bx_seed = bx_seed * 1664525u + 1013904223u;
        bfxr_start(&bx_voice, &s, trig_pitch, bx_seed);
    }
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    bfxr_render(&bx_voice, bx_buf, n);
    bx_playing = bx_voice.active;
    for (i = 0; i < n; i++) {
        int32_t s;
        bx_gain += (gain - bx_gain) * 0.004f;              /* ~5 ms: turning MASTER does not click */
        s = (int32_t)(bx_buf[i] * bx_gain);
        lr[2 * i] = s;
        lr[2 * i + 1] = s;
    }
}

void app_panic(void)
{
    bx_voice.active = 0;
    trig_done = trig_seq;
}

/* --------------------------------------------------------------- settings --- */
static void settings_fresh(void)
{
    bfxr_sound_t s;
    uint32_t k;
    set.magic = SET_MAGIC;
    set.octave = 0;
    set.gen = GEN_PICKUP;
    set.sel = 0;
    set.slot = 0;
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
    if (plat_store_load(OBJ_BANK, &bank, sizeof bank) != (int)sizeof bank || bank.magic != BANK_MAGIC) {
        bank.magic = BANK_MAGIC;
        bank.used = 0;
    }
    bank.used &= (1u << NSLOTS) - 1u;
    for (k = 0; k < NSLOTS; k++)
        clamp_sound(bank.p[k]);
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC ||
        set.octave < -2 || set.octave > 2 || set.gen >= GEN_COUNT || set.sel > BP_COUNT - 4 || set.slot > NSLOTS) {
        settings_fresh();
        return;
    }
    clamp_sound(set.p);
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

static void play_now(void) { play_semitones(12 * set.octave); }

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
        float step = (d->max - d->min) * 0.01f * (a >= 3 ? 3.0f : 1.0f);   /* fast turns go 3x */
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
#define SX_SOUND (BP_COUNT * 5u)
static const uint8_t SX_HEAD[4] = {0xF0, 0x7D, 0x42, 0x46};
static uint8_t sx_frame[8 + SX_SOUND];
static int32_t sx_next = -1;                     /* the list being sent: the next slot (-1 none, NSLOTS + 1 the end) */

static void sx_put_sound(uint8_t *d, const float *p)
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
static void sx_get_sound(const uint8_t *d, float *p)
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
static uint32_t sx_begin(uint8_t cmd)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        sx_frame[i] = SX_HEAD[i];
    sx_frame[4] = cmd;
    return 5;
}
static void sx_reply(uint32_t slot, uint32_t status)
{
    uint32_t n = sx_begin(0x05);
    sx_frame[n++] = (uint8_t)slot;
    sx_frame[n++] = (uint8_t)status;
    sx_frame[n++] = 0xF7;
    (void)plat_sysex_send(sx_frame, n);
}

/* main loop: the next frames of a list, as many as the computer has room for now */
static void sx_pump(void)
{
    while (sx_next >= 0) {
        uint32_t n;
        if (sx_next <= (int32_t)NSLOTS) {
            uint32_t i = (uint32_t)sx_next;
            n = sx_begin(0x02);
            sx_frame[n++] = (uint8_t)i;
            sx_frame[n++] = (uint8_t)(i == NSLOTS ? 1u : bank.used >> i & 1u);
            sx_put_sound(sx_frame + n, i == NSLOTS ? set.p : bank.p[i]);
            n += SX_SOUND;
        } else {
            n = sx_begin(0x03);
            sx_frame[n++] = 1;
            sx_frame[n++] = NSLOTS;
        }
        sx_frame[n++] = 0xF7;
        if (plat_sysex_room() < n)
            return;                              /* (the rest at the next frame, as the computer reads) */
        if (plat_sysex_send(sx_frame, n) != 0) {
            sx_next = -1;                        /* no computer: the list is dropped */
            return;
        }
        sx_next = sx_next <= (int32_t)NSLOTS ? sx_next + 1 : -1;
    }
}

int app_sysex(const uint8_t *p, uint32_t n)      /* the bytes between F0 and F7 */
{
    uint32_t slot;
    if (n < 4 || p[0] != 0x7D || p[1] != 0x42 || p[2] != 0x46)
        return 0;
    switch (p[3]) {
    case 0x01:
        sx_next = 0;
        sx_pump();
        return 1;
    case 0x04:
        if (n != 5 + SX_SOUND || p[4] > NSLOTS) {
            sx_reply(n > 4 ? p[4] : 0x7F, 1);
            return 1;
        }
        slot = p[4];
        if (slot == NSLOTS) {                    /* the sound being edited */
            sx_get_sound(p + 5, set.p);
            set_dirty = 1;
            app_say("SOUND RECEIVED", "");
            play_now();
            sx_reply(slot, 0);
        } else {
            sx_get_sound(p + 5, bank.p[slot]);
            bank.used |= 1u << slot;
            sx_reply(slot, bank_save() == 0 ? 0u : 2u);
            app_say("RECEIVED SLOT", "");
        }
        ui.redraw = 1;
        return 1;
    case 0x06:
        if (n != 5 || p[4] >= NSLOTS) {
            sx_reply(n > 4 ? p[4] : 0x7F, 1);
            return 1;
        }
        bank.used &= ~(1u << p[4]);
        if (set.slot == p[4] + 1u)
            set.slot = 0;
        sx_reply(p[4], bank_save() == 0 ? 0u : 2u);
        ui.redraw = 1;
        return 1;
    }
    return 0;
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    int32_t e;
    for (k = 0; k < NKEYS; k++) {                /* key 0 is F3 (MIDI 53); C4 plays the sound as made */
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit) && (btn & (1u << B_SAVE | 1u << B_SEL))) {
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
    if (!(btn >> B_SAVE & 1u) && (ui.btn >> B_SAVE & 1u) && !ui.save_used) {   /* SAVE tapped: keep the sound */
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
        play_now();
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds(btn | (set_dirty ? 1u << B_SAVE : 0u) | (bx_playing ? LED_PLAY_GREEN : 0u),
              (btn & (1u << B_SAVE | 1u << B_SEL)) ? slot_keys() | keys : keys);   /* SAVE / SEL held: the slots */
}

void app_input_only(void) { input(); }

/* the value as a bar: from the left for a range from 0, from the middle for one from -x to x */
static void value_bar(int32_t x, int32_t y, int32_t w, uint32_t k)
{
    const bfxr_param_t *d = &BFXR_PARAMS[k];
    float t = (set.p[k] - d->min) / (d->max - d->min);
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
    lcd_fill(0, 0, 240, 240, K_BG);
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    input();
    sx_pump();
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
    else if (!strcmp(what, "playing"))
        *got = bx_playing;
    else if (!strcmp(what, "slots"))            /* the slots holding a sound, a bit each */
        *got = (int)bank.used;
    else if (!strcmp(what, "slot"))
        *got = set.slot;
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')   /* pN: parameter N x 100 */
        *got = (int)(set.p[atoi(what + 1) % BP_COUNT] * 100.0f + (set.p[atoi(what + 1) % BP_COUNT] < 0 ? -0.5f : 0.5f));
    else
        return 0;
    return 1;
}
#endif
