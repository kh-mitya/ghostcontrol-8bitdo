/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

/* Synchronous write on OUT (ep_index 1). Only called while no IN transfer is
 * pending, but completions name their endpoint, so others are skipped. */
static inline int usb_send(int fd, struct usb_fs_endpoint *eps, const uint8_t *data, uint32_t len) {
    void *bufs[1] = { (void *)data };
    uint32_t lens[1] = { len };
    struct usb_fs_endpoint *ep = &eps[1];
    ep->ppBuffer = bufs; ep->pLength = lens; ep->nFrames = 1;
    ep->timeout = 150; ep->flags = 0; ep->aFrames = 0; ep->status = 0;
    struct usb_fs_start start = { .ep_index = 1 };
    if (ioctl(fd, USB_FS_START, &start) != 0) return -errno;
    for (int w = 0; w < 20; w++) {
        struct usb_fs_complete c = { .ep_index = 1 };
        if (ioctl(fd, USB_FS_COMPLETE, &c) == 0) {
            if (c.ep_index == 1) return ep->status ? -EIO : 0;
            continue;
        }
        if (errno != EBUSY) return -errno;
        usleep(10000);
    }
    struct usb_fs_stop stop = { .ep_index = 1 };
    ioctl(fd, USB_FS_STOP, &stop);
    return -EBUSY;
}
