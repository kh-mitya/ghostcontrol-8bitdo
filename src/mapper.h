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

/* Profiles are shared by all controllers; each controller remembers which
 * one it uses. Names come from a fixed list of symbols (the page draws one
 * per name), so nobody has to type, and a name fits a console notification. */
#define PROFILE_NAME_MAX 15
#define PROFILE_DEFAULT  "Default"   /* built in, never a file */
#define PROFILES_MAX     12
extern const char *const mapper_profile_list[PROFILES_MAX];

/* A mapping. Inputs are kept by position (Xbox names: a = bottom face
 * button), so a profile means the same on Xbox and Switch layouts. */
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

void mapper_defaults(struct gc_profile *p);
/* Xbox + RB = next profile, Xbox + LB = previous, one per controller. */
struct gc_guide { int mode, tap; long t0; uint32_t prev; };

/* Turn controller state into DualSense input. */
void mapper_apply(const struct gc_profile *p, const struct gc_input *in,
                  struct gc_combo *combo, long now_ms, ScePadData *out);
/* Watch for Xbox + LB/RB in *buttons and hide it from the game; returns +1
 * (next profile), -1 (previous) or 0. No adapter combo uses the Xbox button
 * (they are all View + something), so it cannot change the adapter's mode.
 * A lone Xbox press is held back until it is clear it is not a combo: a tap
 * is replayed, a hold passes through after 400 ms. */
int mapper_profile_combo(struct gc_guide *g, uint32_t *buttons, long now_ms);

/* Text form of a profile. parse: 0 ok, else the failing line
 * number; *p is only changed on success. */
size_t mapper_format(const struct gc_profile *p, char *buf, size_t size);
int mapper_parse(const char *text, struct gc_profile *p);
/* Index of a profile name in mapper_profile_list, -1 if it is not one. */
int mapper_profile_index(const char *name);
/* Saved profiles in list order (PROFILE_DEFAULT not included). */
int mapper_profile_names(const char *dir, const char *names[PROFILES_MAX]);
/* <dir>/<name>.ini, or the defaults for PROFILE_DEFAULT; -1 when missing. */
int mapper_profile_load(const char *dir, const char *name, struct gc_profile *p);
int mapper_profile_save(const char *dir, const char *name, const struct gc_profile *p);
/* Raw text file helpers. */
int mapper_read_file(const char *path, char *buf, size_t size);
int mapper_write_file(const char *path, const char *data, size_t len);
