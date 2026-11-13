/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "switch_pad.h"
#include "usb_io.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int send_cmd(int fd, struct usb_fs_endpoint *eps, uint8_t a, uint8_t b) {
    uint8_t buf[2] = { a, b };
    return usb_send(fd, eps, buf, 2);
}

static int send_subcmd(int fd, struct usb_fs_endpoint *eps, uint8_t *seq,
                       uint8_t subcmd, uint8_t arg) {
    static const uint8_t rumble[8] = {0x00,0x01,0x40,0x40,0x00,0x01,0x40,0x40};
    uint8_t buf[12] = {0x01, *seq};
    memcpy(buf + 2, rumble, 8);
    buf[10] = subcmd; buf[11] = arg;
    int r = usb_send(fd, eps, buf, sizeof(buf));
    if (r == 0) *seq = (uint8_t)((*seq + 1) & 0x0f);
    return r;
}

static uint8_t stick(uint16_t v) { if (v > 4095) v = 4095; return (uint8_t)((v * 255u) / 4095u); }
/* Switch reports stick up as high values; PlayStation expects up = 0. */
static uint8_t stick_y(uint16_t v) { return (uint8_t)(255u - stick(v)); }

/* Buttons by position, in Xbox terms: Switch B (bottom) is Xbox A, and so on.
 * right: [3] in full reports, [1] in simple; shared: [4] / [2]; left: [5] / [8] */
static uint32_t buttons(uint8_t br, uint8_t bs, uint8_t bl) {
    uint32_t b = 0;
    if (br & 0x04) b |= GI_BIT(GI_A);
    if (br & 0x08) b |= GI_BIT(GI_B);
    if (br & 0x01) b |= GI_BIT(GI_X);
    if (br & 0x02) b |= GI_BIT(GI_Y);
    if (br & 0x40) b |= GI_BIT(GI_RB);
    if (bl & 0x40) b |= GI_BIT(GI_LB);
    if (bs & 0x01) b |= GI_BIT(GI_VIEW);     /* − */
    if (bs & 0x02) b |= GI_BIT(GI_MENU);     /* + */
    if (bs & 0x04) b |= GI_BIT(GI_RS);
    if (bs & 0x08) b |= GI_BIT(GI_LS);
    if (bs & 0x10) b |= GI_BIT(GI_XBOX);     /* Home */
    if (bs & 0x20) b |= GI_BIT(GI_SHARE);    /* Capture */
    return b;
}

/* Full report: [3] right buttons, [4] shared, [5] left buttons, [6..11] sticks */
static void parse_full(const uint8_t *b, struct gc_input *o) {
    uint8_t br = b[3], bs = b[4], bl = b[5];
    uint32_t btn = buttons(br, bs, bl);
    if (bl & 0x02) btn |= GI_BIT(GI_UP);
    if (bl & 0x01) btn |= GI_BIT(GI_DOWN);
    if (bl & 0x04) btn |= GI_BIT(GI_RIGHT);
    if (bl & 0x08) btn |= GI_BIT(GI_LEFT);
    o->buttons = btn;
    o->lx = stick((uint16_t)(b[6] | ((b[7] & 0xF) << 8)));
    o->ly = stick_y((uint16_t)((b[7] >> 4) | ((uint16_t)b[8] << 4)));
    o->rx = stick((uint16_t)(b[9] | ((b[10] & 0xF) << 8)));
    o->ry = stick_y((uint16_t)((b[10] >> 4) | ((uint16_t)b[11] << 4)));
    o->lt = (bl & 0x80) ? 255 : 0;           /* ZL/ZR are on/off */
    o->rt = (br & 0x80) ? 255 : 0;
}

/* Simple report, sent until the handshake switches the pad to full reports:
 * [1] right buttons, [2] shared, [3] hat, [4..7] sticks (8 bit), [8] L/ZL */
static void parse_simple(const uint8_t *b, struct gc_input *o) {
    static const uint32_t hat[8] = {
        GI_BIT(GI_UP), GI_BIT(GI_UP) | GI_BIT(GI_RIGHT), GI_BIT(GI_RIGHT),
        GI_BIT(GI_DOWN) | GI_BIT(GI_RIGHT), GI_BIT(GI_DOWN), GI_BIT(GI_DOWN) | GI_BIT(GI_LEFT),
        GI_BIT(GI_LEFT), GI_BIT(GI_UP) | GI_BIT(GI_LEFT) };
    uint8_t br = b[1], bs = b[2], bl = b[8];
    o->buttons = buttons(br, bs, bl) | (b[3] < 8 ? hat[b[3]] : 0);
    o->lx = b[4]; o->ly = (uint8_t)(255u - b[5]);
    o->rx = b[6]; o->ry = (uint8_t)(255u - b[7]);
    o->lt = (bl & 0x80) ? 255 : 0;
    o->rt = (br & 0x80) ? 255 : 0;
}

int switch_handle_packet(int fd, struct usb_fs_endpoint *eps, const uint8_t *buf,
                         uint32_t len, int *hs, uint8_t *seq, struct gc_input *out) {
    if (len == 0) return 0;
    switch (buf[0]) {
    case 0x30: case 0x00:                      /* full input report */
        if (len < 12 || (buf[0] == 0x00 && buf[1] == 0)) return 0;
        *hs = HS_STREAMING;
        parse_full(buf, out);
        return 1;
    case 0x3f:                                 /* simple input report */
        if (len < 9) return 0;
        parse_simple(buf, out);
        return 1;
    case 0x21:                                 /* subcommand reply carries input too */
        if (len < 13 || *hs != HS_STREAMING) return 0;
        parse_full(buf, out);
        return 1;
    case 0x81:                                 /* USB handshake */
        if (len < 2) return 0;
        if (*hs == HS_STREAMING) { *hs = HS_WAIT_81_01; return 0; }   /* controller restarted */
        if (buf[1] == 0x01 && *hs == HS_WAIT_81_01) {
            if (send_cmd(fd, eps, 0x80, 0x02)) return -1;
            *hs = HS_WAIT_81_02;
        } else if (buf[1] == 0x02 && *hs <= HS_WAIT_81_02) {
            if (send_cmd(fd, eps, 0x80, 0x04)) return -1;
            if (send_subcmd(fd, eps, seq, 0x40, 0x01) ||   /* IMU on       */
                send_subcmd(fd, eps, seq, 0x48, 0x01) ||   /* vibration on */
                send_subcmd(fd, eps, seq, 0x30, 0x01) ||   /* player LED 1 */
                send_subcmd(fd, eps, seq, 0x03, 0x30))     /* full reports */
                return -1;
            *hs = HS_STREAMING;
        }
        return 0;
    }
    return 0;
}
