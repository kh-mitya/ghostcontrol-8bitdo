/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/* Switch Pro USB protocol: handshake to full 60 Hz reports (0x30), then
 * parse into ScePadData. eps[0] = IN, eps[1] = OUT. */
enum { HS_WAIT_81_01, HS_WAIT_81_02, HS_STREAMING };

/* 1: out was filled with controller state; 0: protocol traffic; <0: send failed */
int switch_handle_packet(int fd, struct usb_fs_endpoint *eps, const uint8_t *buf,
                         uint32_t len, int *hs, uint8_t *seq, ScePadData *out);
