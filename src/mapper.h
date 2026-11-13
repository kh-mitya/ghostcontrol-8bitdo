/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "input.h"
#include "gc_types.h"

/* DualSense targets a controller input can be mapped to. */
enum gc_out {
    GO_NONE, GO_CROSS, GO_CIRCLE, GO_SQUARE, GO_TRIANGLE, GO_L1, GO_R1, GO_L2, GO_R2,
    GO_L3, GO_R3, GO_CREATE, GO_OPTIONS, GO_PS, GO_TOUCHPAD, GO_UP, GO_DOWN,
    GO_LEFT, GO_RIGHT, GO_COUNT
};

enum { GC_PROFILE_ADAPTER, GC_PROFILE_WIRED, GC_PROFILE_COUNT };

struct gc_profile {
    uint8_t map[GI_COUNT];      /* enum gc_out per input */
    uint8_t touch_combo;        /* View + Menu together = touchpad */
    uint8_t swap_sticks;
    uint8_t invert[4];          /* left x, left y, right x, right y */
    uint8_t deadzone[2];        /* percent, left / right stick */
    uint8_t trigger_threshold;  /* percent of travel at which LT/RT count as pressed */
};

/* View + Menu chord detector, one per controller. */
struct gc_combo { int mode; uint32_t first, tap; int tap_frames; long t0; };

extern struct gc_profile g_profiles[GC_PROFILE_COUNT];

void mapper_defaults(struct gc_profile p[GC_PROFILE_COUNT]);
/* Turn controller state into DualSense input. */
void mapper_apply(const struct gc_profile *p, const struct gc_input *in,
                  struct gc_combo *combo, long now_ms, ScePadData *out);
/* Text form (the mapping file). parse: 0 ok, else the failing line number;
 * profiles are only changed on success. */
size_t mapper_format(const struct gc_profile p[GC_PROFILE_COUNT], char *buf, size_t size);
int mapper_parse(const char *text, struct gc_profile p[GC_PROFILE_COUNT]);
/* Load from / save to MAPPING_PATH. load: defaults when there is no file. */
int mapper_load(const char *path);
int mapper_save(const char *path);
