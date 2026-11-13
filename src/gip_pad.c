/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gip_pad.h"
#include "usb_io.h"
#include <string.h>

#define GIP_CMD_ANNOUNCE    0x02
#define GIP_CMD_POWER       0x05
#define GIP_CMD_VIRTUAL_KEY 0x07
#define GIP_CMD_INPUT       0x20
#define GIP_OPT_ACK_INTERNAL 0x30

static void send_seq(int fd, struct usb_fs_endpoint *eps, struct gip_state *g,
                     const uint8_t *pkt, uint32_t len) {
    uint8_t b[16];
    memcpy(b, pkt, len);
    b[2] = g->seq++;
    usb_send(fd, eps, b, len);
}

void gip_start(int fd, struct usb_fs_endpoint *eps, struct gip_state *g, long now) {
    static const uint8_t power_on[] = { GIP_CMD_POWER, 0x20, 0, 0x01, 0x00 };
    /* Xbox One S and Elite 2 after Bluetooth use (xpad). */
    static const uint8_t s_init[] = { GIP_CMD_POWER, 0x20, 0, 0x0f, 0x06 };
    /* Sent by Windows; some pads (PDP) report nothing without them (xpad). */
    static const uint8_t led_on[] = { 0x0a, 0x20, 0, 0x03, 0x00, 0x01, 0x14 };
    static const uint8_t auth_done[] = { 0x06, 0x20, 0, 0x02, 0x01, 0x00 };
    send_seq(fd, eps, g, power_on, sizeof(power_on));
    if (g->vid == 0x045e && (g->pid == 0x02ea || g->pid == 0x0b00))
        send_seq(fd, eps, g, s_init, sizeof(s_init));
    send_seq(fd, eps, g, led_on, sizeof(led_on));
    send_seq(fd, eps, g, auth_done, sizeof(auth_done));
    g->guide = 0;
    g->guide_armed = now + 1000;
}

static uint8_t axis(const uint8_t *p) {
    int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (uint8_t)((v + 32768) >> 8);
}

static uint8_t trigger(const uint8_t *p) {
    unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
    if (v > 1023) v = 1023;
    return (uint8_t)(v * 255u / 1023u);
}

static void parse_input(const uint8_t *b, uint32_t len, const struct gip_state *g, struct gc_input *o) {
    uint8_t b4 = b[4], b5 = b[5];
    uint32_t btn = 0;
    if (b4 & 0x04) btn |= GI_BIT(GI_MENU);
    if (b4 & 0x08) btn |= GI_BIT(GI_VIEW);
    if (b4 & 0x10) btn |= GI_BIT(GI_A);
    if (b4 & 0x20) btn |= GI_BIT(GI_B);
    if (b4 & 0x40) btn |= GI_BIT(GI_X);
    if (b4 & 0x80) btn |= GI_BIT(GI_Y);
    if (b5 & 0x01) btn |= GI_BIT(GI_UP);
    if (b5 & 0x02) btn |= GI_BIT(GI_DOWN);
    if (b5 & 0x04) btn |= GI_BIT(GI_LEFT);
    if (b5 & 0x08) btn |= GI_BIT(GI_RIGHT);
    if (b5 & 0x10) btn |= GI_BIT(GI_LB);
    if (b5 & 0x20) btn |= GI_BIT(GI_RB);
    if (b5 & 0x40) btn |= GI_BIT(GI_LS);
    if (b5 & 0x80) btn |= GI_BIT(GI_RS);
    /* Series X|S Share button (xpad: data[len - 26] bit 0) */
    if (g->vid == 0x045e && g->pid == 0x0b12 && len >= 44 && (b[len - 26] & 0x01))
        btn |= GI_BIT(GI_SHARE);
    if (g->guide) btn |= GI_BIT(GI_XBOX);
    o->buttons = btn;
    o->lt = trigger(b + 6);
    o->rt = trigger(b + 8);
    o->lx = axis(b + 10);
    o->ly = (uint8_t)(255u - axis(b + 12));   /* GIP: up is positive */
    o->rx = axis(b + 14);
    o->ry = (uint8_t)(255u - axis(b + 16));
}

int gip_handle_packet(int fd, struct usb_fs_endpoint *eps, const uint8_t *buf,
                      uint32_t len, struct gip_state *g, struct gc_input *state, long now) {
    if (len < 4) return 0;
    switch (buf[0]) {
    case GIP_CMD_INPUT:
        if (len < 18) return 0;
        parse_input(buf, len, g, state);
        return 1;
    case GIP_CMD_VIRTUAL_KEY:                  /* Guide (Xbox) button */
        if (len < 5) return 0;
        if (buf[1] == GIP_OPT_ACK_INTERNAL) {
            /* Unacknowledged, the pad repeats it and stops reporting Guide. */
            uint8_t ack[13] = { 0x01, 0x20, buf[2], 0x09, 0x00, GIP_CMD_VIRTUAL_KEY,
                                0x20, 0x02, 0, 0, 0, 0, 0 };
            usb_send(fd, eps, ack, sizeof(ack));
        }
        g->guide = (buf[4] & 0x03) && now >= g->guide_armed;
        state->buttons = (state->buttons & ~GI_BIT(GI_XBOX)) | (g->guide ? GI_BIT(GI_XBOX) : 0);
        return 1;
    case GIP_CMD_ANNOUNCE:                     /* pad reset itself: power it on again */
        gip_start(fd, eps, g, now);
        return 0;
    }
    return 0;
}
