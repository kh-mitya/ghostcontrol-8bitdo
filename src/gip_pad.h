/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/* Xbox One / Series controllers over USB (GIP), as Linux xpad drives them.
 * eps[0] = IN, eps[1] = OUT. The pad reports only when something changes,
 * so the caller keeps the last state and repeats it. */
struct gip_state {
    uint16_t vid, pid;
    uint8_t seq;
    uint8_t guide;
    long guide_armed;   /* ignore Guide until then: some pads send it at power-on */
};

void gip_start(int fd, struct usb_fs_endpoint *eps, struct gip_state *g, long now);
/* 1: *state was updated; 0: protocol traffic */
int gip_handle_packet(int fd, struct usb_fs_endpoint *eps, const uint8_t *buf,
                      uint32_t len, struct gip_state *g, ScePadData *state, long now);
