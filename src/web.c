/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "web.h"
#include "mapper.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#define INCASSET(name, file)                                                \
    __asm__(".section .rodata\n.global " #name "\n.global " #name "_size\n"  \
            ".align 16\n" #name ":\n.incbin \"" file "\"\n" #name "_end:\n"  \
            ".align 8\n" #name "_size:\n.quad " #name "_end - " #name "\n"   \
            ".previous\n");                                                 \
    extern const char name[];                                               \
    extern const size_t name##_size;

INCASSET(web_index_html, "assets/index.html");

static int g_listen = -1;
static const char *g_mapping_path;

int web_start(int port, const char *mapping_path) {
    g_mapping_path = mapping_path;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                               .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0 || listen(s, 4) != 0) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    g_listen = s;
    return 0;
}

static void send_all(int c, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(c, p, n, 0);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void respond(int c, int code, const char *type, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request",
                     type, len);
    send_all(c, head, (size_t)n);
    send_all(c, body, len);
}

static void serve(int c) {
    static char req[12288];
    static char out[8192];
    size_t n = 0, need = 0;
    char *body = NULL;
    struct timeval tv = { 0, 500000 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    while (n < sizeof(req) - 1) {
        ssize_t r = recv(c, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) return;
        n += (size_t)r; req[n] = '\0';
        if (!body && (body = strstr(req, "\r\n\r\n")) != NULL) {
            body += 4;
            char *cl = strstr(req, "Content-Length:");
            if (!cl) cl = strstr(req, "content-length:");
            need = (size_t)(body - req) + (cl && cl < body ? strtoul(cl + 15, NULL, 10) : 0);
            if (need >= sizeof(req)) { respond(c, 400, "text/plain", "too large", 9); return; }
        }
        if (body && n >= need) break;
    }
    if (!body) return;
    req[need] = '\0';
    int post = strncmp(req, "POST ", 5) == 0;
    char *path = req + (post ? 5 : 4);
    if (!post && strncmp(req, "GET ", 4) != 0) { respond(c, 400, "text/plain", "method", 6); return; }
    path[strcspn(path, " ?\r\n")] = '\0';

    if (!post && strcmp(path, "/") == 0) {
        respond(c, 200, "text/html; charset=utf-8", web_index_html, web_index_html_size);
    } else if (!post && strcmp(path, "/status") == 0) {
        size_t len = gc_status_json(out, sizeof(out));
        respond(c, 200, "application/json", out, len);
    } else if (!post && strcmp(path, "/mapping") == 0) {
        size_t len = mapper_format(g_profiles, out, sizeof(out));
        respond(c, 200, "text/plain; charset=utf-8", out, len);
    } else if (!post && strcmp(path, "/mapping/default") == 0) {
        struct gc_profile def[GC_PROFILE_COUNT];
        mapper_defaults(def);
        size_t len = mapper_format(def, out, sizeof(out));
        respond(c, 200, "text/plain; charset=utf-8", out, len);
    } else if (post && strcmp(path, "/mapping") == 0) {
        struct gc_profile p[GC_PROFILE_COUNT];
        int bad = mapper_parse(body, p);
        if (bad) {
            int len = snprintf(out, sizeof(out), "error on line %d", bad);
            respond(c, 400, "text/plain", out, (size_t)len);
            return;
        }
        memcpy(g_profiles, p, sizeof(p));
        if (mapper_save(g_mapping_path) != 0) respond(c, 200, "text/plain", "applied, not saved", 18);
        else respond(c, 200, "text/plain", "saved", 5);
    } else {
        respond(c, 404, "text/plain", "not found", 9);
    }
}

void web_poll(int timeout_ms) {
    if (g_listen < 0) {
        if (timeout_ms > 0) usleep((useconds_t)timeout_ms * 1000);
        return;
    }
    struct pollfd pf = { g_listen, POLLIN, 0 };
    if (poll(&pf, 1, timeout_ms) <= 0) return;
    int c = accept(g_listen, NULL, NULL);
    if (c < 0) return;
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);
    serve(c);
    close(c);
}
