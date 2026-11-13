/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* One controller state in Xbox terms, filled by every protocol (Switch
 * protocol through the 8BitDo adapter, GIP by wire). The mapper turns it into
 * DualSense input, so a profile works the same for every connection. */
enum gc_in {
    GI_A, GI_B, GI_X, GI_Y, GI_LB, GI_RB, GI_LT, GI_RT, GI_LS, GI_RS,
    GI_VIEW, GI_MENU, GI_XBOX, GI_SHARE, GI_UP, GI_DOWN, GI_LEFT, GI_RIGHT,
    GI_COUNT
};
#define GI_BIT(i) (1u << (i))

struct gc_input {
    uint32_t buttons;           /* GI_BIT(); LT/RT come from lt/rt instead */
    uint8_t lx, ly, rx, ry;     /* PlayStation orientation: 0 = left / up */
    uint8_t lt, rt;             /* 0..255; on/off (0/255) on the Switch protocol */
};
