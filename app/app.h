/* SPDX-License-Identifier: MIT
 * BFMXR for the FM-1: bfxr.net's sound effects on the keyboard. The app's half of the contract with fm1-base
 * (base/device/unity.c, base/host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "BFMXR"
#define APP_SPLASH_RGB RGB(94, 48, 40)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_CANVAS_PX (240u * 240u)      /* the whole screen drawn at once */

/* the store: the app's settings (the sound being edited), the panel calibration (the platform's), the 16 slots */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BANK, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}

/* the slots over USB (the Library page): SysEx F0 7D 42 46 cmd .. F7, see app.c */
#define APP_SYSEX
int app_sysex(const uint8_t *p, uint32_t n);

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

/* the host simulator's checks (base/host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
