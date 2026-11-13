/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usb_guard.h"
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>
#include <sys/ioctl.h>

/* Vendors whose wired pads speak GIP: Microsoft, PDP, PowerA, Hori. */
static int gip_vendor(uint16_t vid) {
    return vid == 0x045e || vid == 0x0e6f || vid == 0x24c6 || vid == 0x20d6 || vid == 0x0f0d;
}

unsigned gc_usb_inspect(int fd, struct gc_usb_identity *id) {
    struct usb_device_descriptor dev = {0};
    struct usb_config_descriptor active = {0};
    struct gc_usb_identity selected = {0};
    int active_seen = 0, active_match = 0;
    if (ioctl(fd, USB_GET_DEVICE_DESC, &dev) != 0 || dev.bLength != 18 ||
        dev.bDescriptorType != 1 || !dev.bNumConfigurations || dev.bNumConfigurations > 8 ||
        dev.bDeviceClass == 8 || dev.bDeviceClass == 9)
        return GC_KIND_NONE;
    uint16_t vid = UGETW(dev.idVendor), pid = UGETW(dev.idProduct);
    /* One descriptor read is enough for everything else on the bus. The
     * 8BitDo adapter changes PID when it changes mode, which re-enumerates it. */
    unsigned want = vid == 0x057e && pid == 0x2009 ? GC_KIND_SWITCH :
                    gip_vendor(vid) ? GC_KIND_GIP : GC_KIND_NONE;
    if (want == GC_KIND_NONE) return GC_KIND_NONE;
    if (ioctl(fd, USB_GET_CONFIG_DESC, &active) != 0 ||
        active.bLength != 9 || active.bDescriptorType != 2) return GC_KIND_NONE;
    for (unsigned cfg = 0; cfg < dev.bNumConfigurations; ++cfg) {
        uint8_t bytes[1024] = {0};
        struct usb_gen_descriptor full = {0};
        struct gc_usb_identity cand = {0};
        full.ugd_data = bytes; full.ugd_maxlen = sizeof(bytes); full.ugd_config_index = cfg;
        if (ioctl(fd, USB_GET_FULL_DESC, &full) != 0 || full.ugd_actlen > sizeof(bytes))
            return GC_KIND_NONE;
        int r = gc_usb_parse_config(bytes, full.ugd_actlen, want, &cand);
        if (r < 0) return GC_KIND_NONE;                /* storage or malformed */
        if (bytes[5] == active.bConfigurationValue) {
            if (active_seen++ || bytes[4] != active.bNumInterface ||
                ((unsigned)bytes[2] | ((unsigned)bytes[3] << 8)) != (unsigned)UGETW(active.wTotalLength))
                return GC_KIND_NONE;
            if (r == GC_USB_YES) { selected = cand; active_match = 1; }
        }
    }
    if (active_seen != 1 || !active_match) return GC_KIND_NONE;
    struct usb_alt_interface alt = {0};
    alt.uai_interface_index = selected.iface_index;
    if (ioctl(fd, USB_GET_ALTINTERFACE, &alt) != 0 || alt.uai_alt_index != 0)
        return GC_KIND_NONE;
    selected.kind = want; selected.vid = vid; selected.pid = pid;
    *id = selected;
    return want;
}
