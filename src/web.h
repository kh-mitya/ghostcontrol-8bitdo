/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>

/* Mapping page on http://<console>:WEB_PORT/. Single-threaded: the caller
 * gives it time with web_poll(); a request is answered in one go. */
#define WEB_PORT 8096

int web_start(int port, const char *mapping_path);   /* 0 ok */
void web_poll(int timeout_ms);                       /* wait for and serve one request */

/* Provided by main.c: connected controllers as JSON. */
size_t gc_status_json(char *buf, size_t size);
