/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Read-only identification of a supported controller:
 *   GC_KIND_SWITCH  057e:2009 (8BitDo adapter in Switch mode, Switch Pro):
 *                   HID 03/00/00, IN 0x81, OUT 0x01/0x02, 64-byte packets;
 *   GC_KIND_GIP     Xbox One / Series by wire: vendor interface ff/47/d0.
 * Fail-closed: anything else, or anything that also carries a storage/hub
 * interface, is left alone. Nothing here detaches, resets, claims or writes. */
enum { GC_KIND_NONE = 0, GC_KIND_SWITCH = 1, GC_KIND_GIP = 2 };
struct gc_usb_identity { unsigned kind, iface_index, in_ep, out_ep; uint16_t vid, pid; };

enum { GC_USB_NO = 0, GC_USB_YES = 1, GC_USB_INVALID = -1, GC_USB_STORAGE = -2 };

static inline int gc_usb_parse_config(const uint8_t *p, size_t n, unsigned want,
                                      struct gc_usb_identity *result) {
    unsigned seen[256] = {0}, count = 0, index = 0, last_alt = 0;
    unsigned declared = 0, actual = 0, in = 0, out = 0;
    int have_iface = 0, match = 0, found = 0;
    if (!p || !result || n < 9 || p[0] != 9 || p[1] != 2 ||
        ((unsigned)p[2] | ((unsigned)p[3] << 8)) != n || !p[4] || !p[5])
        return GC_USB_INVALID;
    for (size_t pos = 9; pos < n;) {
        if (n - pos < 2 || p[pos] < 2 || p[pos] > n - pos) return GC_USB_INVALID;
        const uint8_t *d = p + pos;
        if (d[1] == 4) {                               /* interface */
            if (d[0] < 9 || (have_iface && actual != declared)) return GC_USB_INVALID;
            if (match && in && out && !found) {
                found = 1; result->iface_index = index; result->in_ep = in; result->out_ep = out;
            }
            if (d[5] == 8 || d[5] == 9) return GC_USB_STORAGE;
            if (!seen[d[2]]) {
                if (d[3] != 0) return GC_USB_INVALID;
                seen[d[2]] = ++count;
            } else if (index != seen[d[2]] - 1 || d[3] <= last_alt) {
                return GC_USB_INVALID;
            }
            last_alt = d[3]; index = seen[d[2]] - 1;
            declared = d[4]; actual = 0; in = out = 0; have_iface = 1;
            match = d[3] == 0 &&
                    ((want == GC_KIND_SWITCH && d[5] == 3 && d[6] == 0 && d[7] == 0) ||
                     (want == GC_KIND_GIP && d[5] == 0xff && d[6] == 0x47 && d[7] == 0xd0));
        } else if (d[1] == 5) {                        /* endpoint */
            if (!have_iface || d[0] < 7 || !(d[2] & 15) || (d[2] & 0x70)) return GC_USB_INVALID;
            actual++;
            unsigned packet = d[4] | ((unsigned)d[5] << 8), type = d[3] & 3;
            if (match && want == GC_KIND_SWITCH && packet == 64 && (type == 2 || type == 3)) {
                if (d[2] == 0x81) in = d[2];
                if (d[2] == 0x01 || d[2] == 0x02) out = d[2];
            } else if (match && want == GC_KIND_GIP && type == 3 && packet && packet <= 64) {
                if ((d[2] & 0x80) && !in) in = d[2];
                if (!(d[2] & 0x80) && !out) out = d[2];
            }
        } else if (d[1] == 1 || d[1] == 2) return GC_USB_INVALID;
        pos += d[0];
    }
    if (!have_iface || actual != declared || count != p[4]) return GC_USB_INVALID;
    if (match && in && out && !found) {
        found = 1; result->iface_index = index; result->in_ep = in; result->out_ep = out;
    }
    return found ? GC_USB_YES : GC_USB_NO;
}

/* Returns the controller kind (GC_KIND_*), GC_KIND_NONE for anything else. */
unsigned gc_usb_inspect(int fd, struct gc_usb_identity *id);
