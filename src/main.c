/* SPDX-License-Identifier: GPL-3.0-or-later
 * ghostcontrol-8bitdo — up to 4 controllers → virtual DualSense pads on PS5:
 * the 8BitDo USB Adapter 2 in Switch mode (057e:2009) and Xbox One / Series
 * pads by wire (GIP). Users are assigned as for a DualSense.
 *
 * One thread. The process sleeps except for:
 *   - a look at /dev every 2 s while a slot is free (one cached descriptor
 *     read per device on the external ports, nothing written);
 *   - the USB reports of the open controllers.
 * Stop it by launching it again or creating STOP_PATH: it then releases USB
 * and deletes its pad first. Killing it mid-transfer panicked the kernel.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <ps5/kernel.h>

#include "gc_types.h"
#include "usb_guard.h"
#include "switch_pad.h"
#include "gip_pad.h"
#include "usb_io.h"
#include "shellui_bind.h"
#include "mapper.h"
#include "web.h"
#include "shortcut.h"

#define VERSION   "ghostcontrol-8bitdo v0.4-dev"
#define DATA_DIR  "/data/ghostpad"
#define LOG_PATH  DATA_DIR "/ghostcontrol-8bitdo.log"
#define LOCK_PATH DATA_DIR "/gc_main.pid"   /* same lock as GhostControl 1.x */
#define STOP_PATH DATA_DIR "/stop"
#define PROFILES_DIR DATA_DIR "/profiles"
#define CTRL_DIR     DATA_DIR "/controllers"   /* profile and labels of each controller seen */

extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *user);
extern int32_t sceUserServiceGetForegroundUser(int32_t *user);
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t type);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data);
extern int32_t sceKernelSendNotificationRequest(int, void *req, size_t size, int);

static volatile sig_atomic_t g_stop;
static void on_term(int sig) { (void)sig; g_stop = 1; }

static int g_log_fd = -1;
static void gc_log(const char *fmt, ...) {
    char buf[256]; va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || g_log_fd < 0) return;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    write(g_log_fd, buf, (size_t)n);
}

static void notify(const char *text) {
    struct { char unk[45]; char message[3075]; } req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", text);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Sleep in short steps, answering the mapping page meanwhile; returns 1 once
 * a stop was requested. */
static int wait_or_stop(int ms) {
    for (; ms > 0 && !g_stop; ms -= 250) {
        web_poll(250);
        if (ms % 1000 < 250 && access(STOP_PATH, F_OK) == 0) g_stop = 1;
    }
    return g_stop;
}

/* ── Controller slots ─────────────────────────────────────────────────── */
#define MAX_SLOTS 4
#define REPEAT_MS 16       /* GIP reports only changes; the PS5 drops a pad
                            * whose state stops arriving, so repeat it */
#define WEB_POLL_MS 20   /* how often the mapping page is served while playing */

struct slot {
    int used, fd, out_open, pending, errors;
    struct gc_usb_identity id;
    char path[32];
    struct usb_fs_endpoint eps[2];   /* the kernel keeps pointers to these */
    void *bufp[1];
    uint32_t lenv[1];
    uint8_t buf[64];
    int hs; uint8_t seq;             /* Switch handshake */
    struct gip_state gip;
    struct gc_input in;              /* latest controller state */
    int have_state;
    int pad;                         /* virtual pad handle, -1 = none */
    int32_t user;                    /* bound user, -1 = the console asks */
    long last_rx, last_inject, retry_at;
    struct gc_combo combo;
    char ident[48];                  /* stable controller id, file name of its settings */
    char profile[PROFILE_NAME_MAX + 1];
    int nintendo;                    /* page shows Nintendo labels (Switch protocol only) */
    struct gc_profile map;           /* mapping of that profile */
    struct gc_guide guide;           /* Xbox + LB/RB profile switch */
};
static struct slot g_slots[MAX_SLOTS];

/* ── Virtual pad ──────────────────────────────────────────────────────────
 * scePadVirtualDeviceAddDevice() returns an error even when it creates the
 * pad; the pad's id only appears in the kernel log. /dev/klog has a single
 * reader, so it is held just for that moment. Without the log no pad is
 * made: a pad with an unknown id takes no input, cannot be deleted, and
 * leaves a "who is using this controller" dialog nothing can answer. */
static int g_pad_lost;       /* a pad exists whose id we never learned */
static long g_dialog_until;  /* an unbound pad's dialog may still be open */

static int klog_open(void) {
    for (int i = 0; i < 50; i++) {                       /* busy: up to 5 s */
        int fd = open("/dev/klog", O_RDONLY);
        if (fd >= 0) return fd;
        if (errno != EBUSY) break;
        usleep(100000);
    }
    /* Whoever holds /dev/klog (klogsrv) republishes it here. */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(3232) };
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0) return s;
    close(s);
    return -1;
}

static uint64_t parse_pad_added(const char *line) {
    if (!strstr(line, "DEVICE_ADDED") || !strstr(line, "subType:22") ||
        !strstr(line, "capabilityBattery:0")) return 0;
    const char *p = strstr(line, "DeviceId:0x");
    if (!p) p = strstr(line, "deviceId=0x");
    return p ? strtoull(p + 11, NULL, 16) : 0;
}

/* want_id: return the id of the first new virtual pad within timeout_ms.
 * Otherwise drop the backlog and return after quiet_ms of silence. */
static uint64_t klog_read(int fd, int timeout_ms, int want_id, int quiet_ms) {
    char buf[512], line[1024];
    size_t n = 0;
    long t0 = now_ms();
    int quiet = 0;
    while (now_ms() - t0 < timeout_ms) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, 50);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            quiet += 50;
            if (!want_id && quiet >= quiet_ms) break;
            continue;
        }
        quiet = 0;
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len <= 0) break;
        for (ssize_t i = 0; i < len; i++) {
            if (buf[i] == '\n' || n >= sizeof(line) - 1) {
                line[n] = '\0'; n = 0;
                uint64_t id = want_id ? parse_pad_added(line) : 0;
                if (id) return id;
            } else if (buf[i] != '\r') line[n++] = buf[i];
        }
    }
    return 0;
}

static int valid_user(int32_t u) { return u != -1 && u != 0; }

/* As with a DualSense: the first controller goes to the user at the screen;
 * every further one gets the console's own "who is using this controller"
 * dialog, answered with that controller. -1: let the console ask. */
static int32_t pick_user(void) {
    int32_t fg = -1;
    if (sceUserServiceGetForegroundUser(&fg) != 0 || !valid_user(fg))
        if (sceUserServiceGetInitialUser(&fg) != 0 || !valid_user(fg)) return -1;
    for (int s = 0; s < MAX_SLOTS; s++)
        if (g_slots[s].used && g_slots[s].pad >= 0) return -1;
    return fg;
}

static int pad_create(struct slot *s, int32_t user) {
    int kfd = klog_open();
    if (kfd < 0) {
        gc_log("kernel log unavailable (errno %d): no pad created", errno);
        notify(VERSION ": kernel log busy, retrying");
        return -1;
    }
    klog_read(kfd, 2000, 0, 300);          /* older ids are not ours */
    struct { int32_t size, user; int32_t pad[6]; } param = { sizeof(param), 1, {0} };
    int ret = scePadVirtualDeviceAddDevice(&param, 3 /* DualSense */);
    uint64_t id = klog_read(kfd, 5000, 1, 0);
    close(kfd);
    if (!id) {
        g_pad_lost = 1;
        gc_log("pad created (0x%08x) but its id never arrived: restart needed", (unsigned)ret);
        notify(VERSION ": virtual pad lost, launch the payload again");
        return -1;
    }
    s->pad = (int32_t)(id & 0xffffffffu);
    s->user = valid_user(user) && shellui_bind(id, user) == 0 ? user : -1;
    gc_log("%s: pad 0x%llx user 0x%x %s", s->path, (unsigned long long)id, (unsigned)user,
           s->user != -1 ? "bound" : "not bound: the console asks who is using it");
    return 0;
}

static void pad_delete(struct slot *s) {
    if (s->pad < 0) return;
    int r = -1;
    for (int i = 0; i < 10 && r != 0; i++) {
        r = scePadVirtualDeviceDeleteDevice(s->pad);
        if (r != 0) usleep(500000);
    }
    gc_log("%s: pad deleted (0x%08x)", s->path, (unsigned)r);
    s->pad = -1;
}

/* ── Per-controller mapping ───────────────────────────────────────────────
 * A controller is recognised again by its USB serial number when it has a
 * real one (Microsoft pads do), otherwise by model and physical port. The
 * 8BitDo adapter is one device to the console, so the mapping follows the
 * adapter, whichever pad is paired with it. */
static void make_ident(int fd, const struct gc_usb_identity *id, char *out, size_t size) {
    struct usb_device_info info;
    char serial[33] = "";
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, USB_GET_DEVICEINFO, &info) == 0) {
        size_t n = 0;
        int seen[128] = {0}, distinct = 0;
        for (const char *c = info.udi_serial; *c && n < sizeof(serial) - 1; c++) {
            if ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')) {
                if (!seen[(int)*c]++) distinct++;
                serial[n++] = *c;
            }
        }
        serial[n] = '\0';
        if (n < 6 || distinct < 3) serial[0] = '\0';   /* "000000000001" is not unique */
    }
    if (serial[0]) {
        snprintf(out, size, "%04x-%04x-%s", id->vid, id->pid, serial);
        return;
    }
    struct usb_device_port_path pp;
    memset(&pp, 0, sizeof(pp));
    size_t n = (size_t)snprintf(out, size, "%04x-%04x-port", id->vid, id->pid);
    if (ioctl(fd, USB_GET_DEV_PORT_PATH, &pp) == 0) {
        n += (size_t)snprintf(out + n, size - n, "%u", pp.udp_bus);
        for (unsigned i = 0; i < pp.udp_port_level && i < USB_DEVICE_PORT_PATH_MAX && n < size; i++)
            n += (size_t)snprintf(out + n, size - n, "-%u", pp.udp_port_no[i]);
    }
}

static void ctrl_path(const char *ident, char *out, size_t size) {
    snprintf(out, size, CTRL_DIR "/%s.ini", ident);
}

static void store_ctrl(const struct slot *s) {
    char path[128], text[96];
    ctrl_path(s->ident, path, sizeof(path));
    int n = snprintf(text, sizeof(text), "profile = %s\nlabels = %s\n", s->profile,
                     s->nintendo ? "nintendo" : "xbox");
    mkdir(CTRL_DIR, 0755);
    if (mapper_write_file(path, text, (size_t)n) != 0) gc_log("%s: cannot save its settings", s->path);
}

/* Switch the slot to a profile; a missing one falls back to Default. */
static void use_profile(struct slot *s, const char *name) {
    if (mapper_profile_load(PROFILES_DIR, name, &s->map) != 0) {
        name = PROFILE_DEFAULT;
        mapper_defaults(&s->map);
    }
    if (name != s->profile) snprintf(s->profile, sizeof(s->profile), "%s", name);
}

static void load_ctrl(struct slot *s) {
    char path[128], text[256], name[PROFILE_NAME_MAX + 1] = PROFILE_DEFAULT;
    ctrl_path(s->ident, path, sizeof(path));
    s->nintendo = 0;
    if (mapper_read_file(path, text, sizeof(text)) >= 0) {
        char *v = strstr(text, "profile = ");
        if (v) { v += 10; size_t n = strcspn(v, "\r\n"); if (n < sizeof(name)) { memcpy(name, v, n); name[n] = '\0'; } }
        s->nintendo = strstr(text, "labels = nintendo") != NULL;
    }
    use_profile(s, name);
}

/* Point every controller file that names profile `from` at `to` (rename or
 * delete), so a later profile with a reused name is not picked up by them. */
static void retarget_ctrl_files(const char *from, const char *to) {
    DIR *d = opendir(CTRL_DIR);
    struct dirent *e;
    char want[48];
    snprintf(want, sizeof(want), "profile = %s\n", from);
    while (d && (e = readdir(d)) != NULL) {
        char path[160], text[256];
        snprintf(path, sizeof(path), CTRL_DIR "/%s", e->d_name);
        if (mapper_read_file(path, text, sizeof(text)) < 0 || !strstr(text, want)) continue;
        char out[256];
        int n = snprintf(out, sizeof(out), "profile = %s\nlabels = %s\n", to,
                         strstr(text, "labels = nintendo") ? "nintendo" : "xbox");
        mapper_write_file(path, out, (size_t)n);
    }
    if (d) closedir(d);
}

static void cycle_profile(struct slot *s, int dir) {
    const char *names[PROFILES_MAX + 1];
    names[0] = PROFILE_DEFAULT;
    int n = 1 + mapper_profile_names(PROFILES_DIR, names + 1);
    int cur = 0;
    for (int i = 0; i < n; i++) if (strcmp(names[i], s->profile) == 0) cur = i;
    use_profile(s, names[(cur + dir + n) % n]);
    store_ctrl(s);
    char msg[96];
    snprintf(msg, sizeof(msg), "Controller %d: %s", (int)(s - g_slots) + 1, s->profile);
    notify(msg);
}

static void inject(struct slot *s, long now) {
    ScePadData p;
    struct gc_input in = s->in;
    int dir = mapper_profile_combo(&s->guide, &in.buttons, now);
    if (dir) cycle_profile(s, dir);
    mapper_apply(&s->map, &in, &s->combo, now, &p);
    scePadVirtualDeviceInsertData(s->pad, &p);
    s->last_inject = now;
}

/* ── USB ──────────────────────────────────────────────────────────────── */
static const char *kind_name(unsigned kind) {
    return kind == GC_KIND_SWITCH ? "8BitDo (Switch mode)" : "Xbox controller";
}

/* Identity check and detach happen on the same fd that is then used. */
static int open_controller(const char *path, struct gc_usb_identity *id) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    int iface;
    if (!gc_usb_inspect(fd, id) ||
        (iface = (int)id->iface_index, ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface) != 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int open_in(int fd, struct usb_fs_endpoint *eps, int ep_max, unsigned ep, unsigned *maxpkt) {
    struct usb_fs_init init = { .pEndpoints = eps, .ep_index_max = ep_max };
    if (ioctl(fd, USB_FS_INIT, &init) != 0) return -1;
    struct usb_fs_open o = { .ep_index = 0, .ep_no = ep, .max_bufsize = 64, .max_frames = 1 };
    if (ioctl(fd, USB_FS_OPEN, &o) != 0) {
        struct usb_fs_uninit u = {0};
        ioctl(fd, USB_FS_UNINIT, &u);
        return -1;
    }
    *maxpkt = o.max_packet_length;
    return 0;
}

static void slot_close(struct slot *s, const char *why) {
    struct usb_fs_stop stop = { .ep_index = 0 };
    struct usb_fs_close close_out = { .ep_index = 1 }, close_in = { .ep_index = 0 };
    struct usb_fs_uninit uninit = {0};
    ioctl(s->fd, USB_FS_STOP, &stop);
    if (s->out_open) ioctl(s->fd, USB_FS_CLOSE, &close_out);
    ioctl(s->fd, USB_FS_CLOSE, &close_in);
    ioctl(s->fd, USB_FS_UNINIT, &uninit);
    close(s->fd);
    gc_log("%s: closed (%s)", s->path, why);
    if (s->pad >= 0) {
        pad_delete(s);
        if (!g_stop) {
            char msg[96];
            snprintf(msg, sizeof(msg), VERSION ": controller %d disconnected", (int)(s - g_slots) + 1);
            notify(msg);
        }
    }
    memset(s, 0, sizeof(*s));
    s->fd = -1; s->pad = -1;
}

static int slot_open(struct slot *s, const char *path) {
    struct gc_usb_identity id = {0};
    unsigned maxpkt = 0;
    int fd = -1;
    memset(s, 0, sizeof(*s));
    s->fd = -1; s->pad = -1; s->user = -1;
    if ((fd = open_controller(path, &id)) < 0) return -1;
    if (id.kind == GC_KIND_SWITCH) {
        /* The console's HID driver takes the interface back once after the
         * first claim, so claim it, let go, and claim again (retrying the race). */
        struct usb_fs_uninit uninit = {0};
        if (open_in(fd, s->eps, 1, id.in_ep, &maxpkt) != 0) { close(fd); return -1; }
        ioctl(fd, USB_FS_UNINIT, &uninit);
        close(fd);
        fd = -1;
        for (int attempt = 0; attempt < 5 && fd < 0; attempt++) {
            memset(s->eps, 0, sizeof(s->eps));
            if ((fd = open_controller(path, &id)) < 0) return -1;
            if (open_in(fd, s->eps, 2, id.in_ep, &maxpkt) != 0) { close(fd); fd = -1; usleep(50000); }
        }
        if (fd < 0) return -1;
    } else {
        usleep(120000);                    /* let a hub settle after the detach */
        if (open_in(fd, s->eps, 2, id.in_ep, &maxpkt) != 0) { close(fd); return -1; }
    }
    s->fd = fd; s->id = id; s->used = 1;
    s->in.lx = s->in.ly = s->in.rx = s->in.ry = 128;   /* centred until the first report */
    make_ident(fd, &id, s->ident, sizeof(s->ident));
    load_ctrl(s);
    snprintf(s->path, sizeof(s->path), "%s", path);
    if (maxpkt == 0 || maxpkt > 64 || (id.kind == GC_KIND_SWITCH && maxpkt != 64)) {
        slot_close(s, "unexpected packet size");
        return -1;
    }
    struct usb_fs_open o = { .ep_index = 1, .ep_no = id.out_ep, .max_bufsize = 64, .max_frames = 1 };
    s->out_open = ioctl(fd, USB_FS_OPEN, &o) == 0;
    long now = now_ms();
    if (id.kind == GC_KIND_SWITCH) {
        static const uint8_t handshake[] = { 0x80, 0x02 }, hid_only[] = { 0x80, 0x04 };
        if (s->out_open) {
            usb_send(fd, s->eps, handshake, 2); usleep(30000);
            usb_send(fd, s->eps, hid_only, 2);  usleep(50000);
        }
        s->hs = HS_WAIT_81_01; s->seq = 1;
    } else {
        if (!s->out_open) { slot_close(s, "no OUT endpoint"); return -1; }
        s->gip.vid = id.vid; s->gip.pid = id.pid;
        gip_start(fd, s->eps, &s->gip, now);
    }
    s->eps[0].nFrames = 1;
    s->eps[0].timeout = id.kind == GC_KIND_SWITCH ? 200 : 0;   /* GIP may stay silent */
    s->eps[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
    s->last_rx = now_ms();
    gc_log("%s: %s %04x:%04x open, id %s, profile %s", path, kind_name(id.kind), id.vid, id.pid,
           s->ident, s->profile);
    char msg[96];
    snprintf(msg, sizeof(msg), VERSION ": controller %d (%s) found, press any button",
             (int)(s - g_slots) + 1, kind_name(id.kind));
    notify(msg);
    return 0;
}

/* Read-only look for new controllers; bus 0 is internal (ugen0.2 is the
 * BT/Wi-Fi radio) and address 1 is a root hub: neither is ever opened. */
static void scan(void) {
    static char failed[32];              /* say it once, not every 2 s */
    DIR *dir = opendir("/dev");
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL) {
        unsigned bus, addr; char extra, path[32];
        if (sscanf(e->d_name, "ugen%u.%u%c", &bus, &addr, &extra) != 2 ||
            (bus != 1 && bus != 2) || addr <= 1) continue;
        if ((size_t)snprintf(path, sizeof(path), "/dev/%s", e->d_name) >= sizeof(path)) continue;
        struct slot *free_slot = NULL;
        int busy = 0;
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (g_slots[i].used && strcmp(g_slots[i].path, path) == 0) busy = 1;
            if (!g_slots[i].used && !free_slot) free_slot = &g_slots[i];
        }
        if (busy) continue;
        if (!free_slot) break;
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        struct gc_usb_identity id;
        unsigned kind = gc_usb_inspect(fd, &id);
        close(fd);
        if (!kind) continue;
        if (slot_open(free_slot, path) == 0) failed[0] = '\0';
        else if (strcmp(failed, path) != 0) {
            gc_log("%s: cannot open the controller (errno %d)", path, errno);
            snprintf(failed, sizeof(failed), "%s", path);
        }
    }
    if (dir) closedir(dir);
}

/* The pad appears with the first button press, so a dialog it may raise
 * always has a live controller to answer it. */
static void maybe_create_pad(struct slot *s, long now) {
    if ((!s->in.buttons && s->in.lt < 128 && s->in.rt < 128) || g_pad_lost || now < s->retry_at) return;
    int32_t user = pick_user();
    if (user == -1 && now < g_dialog_until) return;   /* one console dialog at a time */
    if (pad_create(s, user) != 0) { s->retry_at = now_ms() + 10000; return; }
    long after = now_ms();
    if (s->user == -1) g_dialog_until = after + 15000;
    for (int i = 0; i < MAX_SLOTS; i++) g_slots[i].last_rx = after;   /* we were busy */
    char msg[96];
    snprintf(msg, sizeof(msg), VERSION ": controller %d ready", (int)(s - g_slots) + 1);
    notify(msg);
}

static void handle_report(struct slot *s, long now) {
    uint32_t len = s->lenv[0];
    if (s->id.kind == GC_KIND_SWITCH) {
        struct gc_input in = s->in;
        if (switch_handle_packet(s->fd, s->eps, s->buf, len, &s->hs, &s->seq, &in) <= 0) return;
        s->in = in;
    } else if (gip_handle_packet(s->fd, s->eps, s->buf, len, &s->gip, &s->in, now) <= 0) {
        return;
    }
    s->have_state = 1;
    if (s->pad < 0) maybe_create_pad(s, now);
    else inject(s, now);
}

/* One step for one controller; returns 1 when a report came in. */
static int slot_poll(struct slot *s, long now) {
    int got = 0;
    if (!s->pending) {
        s->bufp[0] = s->buf; s->lenv[0] = sizeof(s->buf);
        s->eps[0].ppBuffer = s->bufp; s->eps[0].pLength = s->lenv;
        s->eps[0].aFrames = 0; s->eps[0].status = 0;
        struct usb_fs_start start = { .ep_index = 0 };
        if (ioctl(s->fd, USB_FS_START, &start) == 0) s->pending = 1;
        else if (errno != EBUSY) { slot_close(s, "controller disconnected"); return 0; }
    }
    if (s->pending) {
        struct usb_fs_complete c = { .ep_index = 0 };
        if (ioctl(s->fd, USB_FS_COMPLETE, &c) == 0) {
            if (c.ep_index == 0) {
                s->pending = 0;
                if (s->eps[0].status == 0 && s->lenv[0] > 0) {
                    s->errors = 0; s->last_rx = now; got = 1;
                    handle_report(s, now);
                } else if (++s->errors >= 100) {
                    slot_close(s, "USB errors"); return 0;
                }
            }
        } else if (errno != EBUSY) {
            slot_close(s, "controller disconnected"); return 0;
        }
    }
    /* The Switch protocol never goes quiet: silence means the adapter
     * stalled (rest mode, hub reset). Let go and find it again. */
    if (s->id.kind == GC_KIND_SWITCH && now - s->last_rx > 5000) {
        slot_close(s, "no reports for 5 s"); return 0;
    }
    if (s->pad >= 0 && s->have_state && now - s->last_inject >= REPEAT_MS) inject(s, now);
    return got;
}

/* For the mapping page: what is connected, what is pressed right now, and
 * which profiles exist. */
size_t gc_status_json(char *buf, size_t size) {
    size_t n = 0;
#define PUT(...) do { int w = snprintf(buf + n, n < size ? size - n : 0, __VA_ARGS__); \
                      if (w > 0) n += (size_t)w; } while (0)
    PUT("{\"version\":\"" VERSION "\",\"controllers\":[");
    for (int i = 0, first = 1; i < MAX_SLOTS; i++) {
        const struct slot *s = &g_slots[i];
        if (!s->used) continue;
        PUT("%s{\"slot\":%d,\"id\":\"%s\",\"profile\":\"%s\",\"switch\":%d,\"nintendo\":%d,"
            "\"vid\":\"%04x\",\"pid\":\"%04x\",\"pad\":%d,\"buttons\":%u,\"lt\":%u,\"rt\":%u}",
            first ? "" : ",", i + 1, s->ident, s->profile, s->id.kind == GC_KIND_SWITCH, s->nintendo,
            s->id.vid, s->id.pid, s->pad >= 0, (unsigned)s->in.buttons, s->in.lt, s->in.rt);
        first = 0;
    }
    const char *names[PROFILES_MAX];
    int count = mapper_profile_names(PROFILES_DIR, names);
    PUT("],\"profiles\":[\"" PROFILE_DEFAULT "\"");
    for (int i = 0; i < count; i++) PUT(",\"%s\"", names[i]);
    PUT("]}");
#undef PUT
    return n < size ? n : size - 1;
}

static struct slot *slot_by(int slot, const char *id) {
    if (slot < 1 || slot > MAX_SLOTS) return NULL;
    struct slot *s = &g_slots[slot - 1];
    return s->used && strcmp(s->ident, id) == 0 ? s : NULL;
}

/* Reload every connected controller that uses profile `name`. */
static void reload_users(const char *name, const char *now_name) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        struct slot *s = &g_slots[i];
        if (!s->used || strcmp(s->profile, name) != 0) continue;
        use_profile(s, now_name);
    }
}

int gc_profile_get(const char *name, struct gc_profile *out) {
    return mapper_profile_load(PROFILES_DIR, name, out);
}

int gc_profile_apply(const char *name, const struct gc_profile *p) {
    if (mapper_profile_save(PROFILES_DIR, name, p) != 0) return -1;
    reload_users(name, name);
    return 0;
}

const char *gc_profile_new(int slot, const char *id, const struct gc_profile *p) {
    struct slot *s = slot_by(slot, id);
    if (!s) return NULL;
    const char *names[PROFILES_MAX];
    int count = mapper_profile_names(PROFILES_DIR, names);
    for (int i = 0; i < PROFILES_MAX; i++) {
        const char *name = mapper_profile_list[i];
        int taken = 0;
        for (int j = 0; j < count; j++) taken |= names[j] == name;
        if (taken) continue;
        if (mapper_profile_save(PROFILES_DIR, name, p) != 0) return NULL;
        use_profile(s, name);
        store_ctrl(s);
        return name;
    }
    return NULL;   /* all names taken */
}

int gc_profile_delete(const char *name) {
    char path[160];
    if (mapper_profile_index(name) < 0) return -1;
    snprintf(path, sizeof(path), PROFILES_DIR "/%s.ini", name);
    if (unlink(path) != 0) return -1;
    reload_users(name, PROFILE_DEFAULT);
    retarget_ctrl_files(name, PROFILE_DEFAULT);
    return 0;
}

int gc_profile_rename(const char *from, const char *to) {
    char a[160], b[160];
    if (mapper_profile_index(from) < 0 || mapper_profile_index(to) < 0) return -1;
    snprintf(a, sizeof(a), PROFILES_DIR "/%s.ini", from);
    snprintf(b, sizeof(b), PROFILES_DIR "/%s.ini", to);
    if (access(b, F_OK) == 0 || rename(a, b) != 0) return -1;
    reload_users(from, to);
    retarget_ctrl_files(from, to);
    return 0;
}

int gc_controller_set(int slot, const char *id, const char *profile, int nintendo) {
    struct slot *s = slot_by(slot, id);
    if (!s) return -1;
    if (profile) {
        if (strcmp(profile, PROFILE_DEFAULT) != 0 && mapper_profile_index(profile) < 0) return -1;
        use_profile(s, profile);
    }
    if (nintendo >= 0) s->nintendo = nintendo;
    store_ctrl(s);
    return 0;
}

void gc_web_ready(void) {
    gc_log("mapping page: port %d ready", WEB_PORT);
}

int main(void) {
    mkdir(DATA_DIR, 0755);
    int lock = open(LOCK_PATH, O_RDWR | O_CREAT, 0600);
    if (lock < 0) { notify(VERSION ": cannot create lock file"); return 1; }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        /* Already running: ask that instance to let go of USB, then take over. */
        int st = open(STOP_PATH, O_WRONLY | O_CREAT, 0600);
        if (st >= 0) close(st);
        notify(VERSION ": restarting");
        int locked = 0;
        for (int i = 0; i < 200 && !locked; i++) {       /* up to 20 s */
            usleep(100000);
            locked = flock(lock, LOCK_EX | LOCK_NB) == 0;
        }
        if (!locked) { notify(VERSION ": previous instance did not stop"); return 1; }
    }
    unlink(STOP_PATH);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);              /* a browser may hang up mid-answer */
    char pid[16];
    int n = snprintf(pid, sizeof(pid), "%d", (int)getpid());
    if (ftruncate(lock, 0) == 0) pwrite(lock, pid, (size_t)n, 0);

    rename(LOG_PATH, LOG_PATH ".prev");
    g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    gc_log(VERSION " started");

    uint8_t caps[16];
    memset(caps, 0xff, sizeof(caps));
    kernel_set_ucred_authid(getpid(), 0x3800000000010003l);
    kernel_set_ucred_caps(getpid(), caps);
    sceUserServiceInitialize(NULL);
    int r1 = scePadInit(), r2 = scePadSetProcessPrivilege(1);
    if (r1 || r2) gc_log("scePadInit 0x%08x, privilege 0x%08x", (unsigned)r1, (unsigned)r2);
    if (web_start(WEB_PORT) != 0)
        gc_log("mapping page: port %d busy (errno %d), retrying", WEB_PORT, errno);
    int sc = shortcut_install();
    if (sc < 0) gc_log("home-screen shortcut failed (0x%08x)", (unsigned)sc);
    else gc_log(sc ? "home-screen shortcut installed" : "home-screen shortcut registered");
    notify(VERSION " started");

    for (int i = 0; i < MAX_SLOTS; i++) { g_slots[i].fd = -1; g_slots[i].pad = -1; }
    long last_scan = now_ms() - 2000, last_stop_check = 0, last_web = 0;
    while (!g_stop) {
        long now = now_ms();
        int active = 0;
        for (int i = 0; i < MAX_SLOTS; i++) active += g_slots[i].used;
        if (now - last_scan >= 2000 && active < MAX_SLOTS) {
            last_scan = now;
            scan();
            active = 0;
            for (int i = 0; i < MAX_SLOTS; i++) active += g_slots[i].used;
        }
        if (!active) { wait_or_stop(2000); continue; }

        int got = 0;
        for (int i = 0; i < MAX_SLOTS && !g_stop; i++)
            if (g_slots[i].used) got |= slot_poll(&g_slots[i], now_ms());
        if (now - last_web >= WEB_POLL_MS) {
            last_web = now;
            web_poll(0);
        }
        if (now - last_stop_check >= 1000) {
            last_stop_check = now;
            if (access(STOP_PATH, F_OK) == 0) g_stop = 1;
        }
        if (!got) usleep(1000);
    }

    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_slots[i].used) slot_close(&g_slots[i], "stop requested");
    gc_log("stopped");
    unlink(STOP_PATH);
    notify(VERSION " stopped");
    close(lock);
    return 0;
}
