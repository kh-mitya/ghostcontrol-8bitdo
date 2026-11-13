/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mapper.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct gc_profile g_profile;

static const char *const in_names[GI_COUNT] = {
    "a", "b", "x", "y", "lb", "rb", "lt", "rt", "ls", "rs",
    "view", "menu", "xbox", "share", "up", "down", "left", "right"
};
static const char *const out_names[GO_COUNT] = {
    "none", "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2",
    "l3", "r3", "create", "options", "ps", "touchpad", "up", "down", "left", "right"
};
static const uint32_t out_bits[GO_COUNT] = {
    0, SCE_PAD_BUTTON_CROSS, SCE_PAD_BUTTON_CIRCLE, SCE_PAD_BUTTON_SQUARE,
    SCE_PAD_BUTTON_TRIANGLE, SCE_PAD_BUTTON_L1, SCE_PAD_BUTTON_R1, SCE_PAD_BUTTON_L2,
    SCE_PAD_BUTTON_R2, SCE_PAD_BUTTON_L3, SCE_PAD_BUTTON_R3, SCE_PAD_BUTTON_CREATE,
    SCE_PAD_BUTTON_OPTIONS, SCE_PAD_BUTTON_PS, SCE_PAD_BUTTON_TOUCH_PAD,
    SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_DOWN, SCE_PAD_BUTTON_LEFT, SCE_PAD_BUTTON_RIGHT
};

void mapper_defaults(struct gc_profile *p) {
    static const uint8_t map[GI_COUNT] = {
        GO_CROSS, GO_CIRCLE, GO_SQUARE, GO_TRIANGLE, GO_L1, GO_R1, GO_L2, GO_R2,
        GO_L3, GO_R3, GO_CREATE, GO_OPTIONS, GO_PS, GO_TOUCHPAD,
        GO_UP, GO_DOWN, GO_LEFT, GO_RIGHT
    };
    memset(p, 0, sizeof(*p));
    memcpy(p->map, map, sizeof(map));
    p->touch_combo = 1;
    p->trigger_threshold = 6;
}

/* ── View + Menu = touchpad click ─────────────────────────────────────────
 * Xbox pads have no key that reaches Capture. A lone View/Menu is held back
 * for COMBO_MS so the chord never leaks; a shorter tap is replayed. */
#define COMBO_MS   120
#define TAP_FRAMES 4

static int apply_combo(struct gc_combo *c, uint32_t *b, long now) {
    const uint32_t both = GI_BIT(GI_VIEW) | GI_BIT(GI_MENU);
    uint32_t held = *b & both;
    switch (c->mode) {
    case 0: if (held == both) c->mode = 2;                      /* idle */
            else if (held) { c->mode = 1; c->first = held; c->t0 = now; }
            break;
    case 1: if (held == both) c->mode = 2;                      /* one key, waiting */
            else if (!held) { c->tap = c->first; c->tap_frames = TAP_FRAMES; c->mode = 0; }
            else if (now - c->t0 >= COMBO_MS) c->mode = 3;
            break;
    default: if (!held) c->mode = 0;                            /* 2 chord, 3 single */
    }
    if (c->mode == 1 || c->mode == 2) *b &= ~both;
    if (c->mode == 0 && c->tap_frames > 0) { *b |= c->tap; c->tap_frames--; }
    return c->mode == 2;
}

/* Radial dead zone, rescaled so the stick still reaches full travel. */
static void deadzone(uint8_t *x, uint8_t *y, unsigned pct) {
    if (!pct) return;
    float fx = (*x - 127.5f) / 127.5f, fy = (*y - 127.5f) / 127.5f;
    float m = sqrtf(fx * fx + fy * fy), dz = pct / 100.0f;
    if (m <= dz) { *x = *y = 128; return; }
    float k = (m - dz) / (1.0f - dz) / m;
    float nx = fx * k * 127.5f + 127.5f, ny = fy * k * 127.5f + 127.5f;
    *x = (uint8_t)(nx < 0 ? 0 : nx > 255 ? 255 : nx + 0.5f);
    *y = (uint8_t)(ny < 0 ? 0 : ny > 255 ? 255 : ny + 0.5f);
}

void mapper_apply(const struct gc_profile *p, const struct gc_input *in,
                  struct gc_combo *combo, long now, ScePadData *out) {
    unsigned thr = p->trigger_threshold * 255u / 100u;
    uint32_t pressed = in->buttons & ~(GI_BIT(GI_LT) | GI_BIT(GI_RT));
    if (in->lt > thr) pressed |= GI_BIT(GI_LT);
    if (in->rt > thr) pressed |= GI_BIT(GI_RT);
    int touch = p->touch_combo && apply_combo(combo, &pressed, now);

    uint32_t buttons = touch ? SCE_PAD_BUTTON_TOUCH_PAD : 0;
    unsigned l2 = 0, r2 = 0;
    for (int i = 0; i < GI_COUNT; i++) {
        unsigned target = p->map[i] < GO_COUNT ? p->map[i] : GO_NONE;
        /* A trigger mapped to L2/R2 stays analog, pressed or not. */
        unsigned level = i == GI_LT ? in->lt : i == GI_RT ? in->rt :
                         (pressed & GI_BIT(i)) ? 255 : 0;
        if (target == GO_L2 && level > l2) l2 = level;
        if (target == GO_R2 && level > r2) r2 = level;
        if (pressed & GI_BIT(i)) buttons |= out_bits[target];
    }

    uint8_t s[4] = { in->lx, in->ly, in->rx, in->ry };
    if (p->swap_sticks) {
        uint8_t t0 = s[0], t1 = s[1];
        s[0] = s[2]; s[1] = s[3]; s[2] = t0; s[3] = t1;
    }
    for (int i = 0; i < 4; i++) if (p->invert[i]) s[i] = (uint8_t)(255 - s[i]);
    deadzone(&s[0], &s[1], p->deadzone[0]);
    deadzone(&s[2], &s[3], p->deadzone[1]);

    memset(out, 0, sizeof(*out));
    out->buttons = buttons;
    out->leftStick.x = s[0]; out->leftStick.y = s[1];
    out->rightStick.x = s[2]; out->rightStick.y = s[3];
    out->analogButtons.l2 = (uint8_t)l2;
    out->analogButtons.r2 = (uint8_t)r2;
    out->connected = 1;
    out->quat.w = 1.0f;
}

/* ── Text form ────────────────────────────────────────────────────────── */
static const char *const opt_names[] = {
    "touch_combo", "swap_sticks", "invert_left_x", "invert_left_y",
    "invert_right_x", "invert_right_y", "deadzone_left", "deadzone_right",
    "trigger_threshold"
};
enum { OPT_COUNT = sizeof(opt_names) / sizeof(opt_names[0]) };

static uint8_t *opt_ptr(struct gc_profile *p, int i, int *lo, int *hi) {
    *lo = 0; *hi = 1;
    switch (i) {
    case 0: return &p->touch_combo;
    case 1: return &p->swap_sticks;
    case 2: case 3: case 4: case 5: return &p->invert[i - 2];
    case 6: case 7: *hi = 50; return &p->deadzone[i - 6];
    default: *lo = 1; *hi = 99; return &p->trigger_threshold;
    }
}

size_t mapper_format(const struct gc_profile *p, char *buf, size_t size) {
    size_t n = 0;
#define PUT(...) do { int w = snprintf(buf + n, n < size ? size - n : 0, __VA_ARGS__); \
                      if (w > 0) n += (size_t)w; } while (0)
    PUT("# ghostcontrol-8bitdo mapping: Xbox input = DualSense button\n");
    if (p->preset[0]) PUT("preset = %s\n", p->preset);
    for (int i = 0; i < GI_COUNT; i++)
        PUT("%s = %s\n", in_names[i], out_names[p->map[i] < GO_COUNT ? p->map[i] : 0]);
    for (int i = 0; i < OPT_COUNT; i++) {
        int lo, hi;
        PUT("%s = %u\n", opt_names[i], *opt_ptr((struct gc_profile *)p, i, &lo, &hi));
    }
#undef PUT
    return n < size ? n : (size ? size - 1 : 0);
}

/* Preset names become file names: letters (any script), digits, space, - _ */
int mapper_valid_name(const char *name) {
    size_t n = strlen(name);
    if (!n || n > PRESET_NAME_MAX || name[0] == ' ' || name[n - 1] == ' ') return 0;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
        if (!(*c >= 0x80 || (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
              (*c >= 'A' && *c <= 'Z') || *c == ' ' || *c == '-' || *c == '_'))
            return 0;
    return 1;
}

static int find(const char *const *names, int count, const char *s) {
    for (int i = 0; i < count; i++) if (strcmp(names[i], s) == 0) return i;
    return -1;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';
    return s;
}

int mapper_parse(const char *text, struct gc_profile *out) {
    struct gc_profile p;
    char line[128];
    int lineno = 0;
    mapper_defaults(&p);
    while (*text) {
        size_t len = strcspn(text, "\n");
        lineno++;
        if (len >= sizeof(line)) return lineno;
        memcpy(line, text, len); line[len] = '\0';
        text += len + (text[len] == '\n');
        char *c = strchr(line, '#');
        if (c) *c = '\0';
        char *s = trim(line);
        if (!*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) return lineno;
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        int i;
        if (strcmp(key, "preset") == 0) {
            if (*val && !mapper_valid_name(val)) return lineno;
            snprintf(p.preset, sizeof(p.preset), "%s", val);
        } else if ((i = find(in_names, GI_COUNT, key)) >= 0) {
            int o = find(out_names, GO_COUNT, val);
            if (o < 0) return lineno;
            p.map[i] = (uint8_t)o;
        } else if ((i = find(opt_names, OPT_COUNT, key)) >= 0) {
            char *end;
            long v = strtol(val, &end, 10);
            int lo, hi;
            uint8_t *dst = opt_ptr(&p, i, &lo, &hi);
            if (!*val || *end || v < lo || v > hi) return lineno;
            *dst = (uint8_t)v;
        } else {
            return lineno;
        }
    }
    *out = p;
    return 0;
}

int mapper_read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

int mapper_write_file(const char *path, const char *data, size_t len) {
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, data, len) == (ssize_t)len;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int mapper_load(const char *path) {
    char buf[4096];
    mapper_defaults(&g_profile);
    if (mapper_read_file(path, buf, sizeof(buf)) < 0) return 0;
    return mapper_parse(buf, &g_profile);
}

int mapper_save(const char *path) {
    char buf[4096];
    size_t n = mapper_format(&g_profile, buf, sizeof(buf));
    return mapper_write_file(path, buf, n);
}
