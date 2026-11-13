/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include "mapper.h"

/* Mapping page on http://<console>:WEB_PORT/. Single-threaded: the caller
 * gives it time with web_poll(); a request is answered in one go. */
#define WEB_PORT 8096

int web_start(int port);              /* 0 ok, else retried in web_poll */
void web_poll(int timeout_ms);        /* wait for and serve one request */

/* Provided by main.c. A controller is addressed by its slot (1-based) and its
 * id, so a request never lands on another controller that took the slot
 * meanwhile. Return 0 on success unless noted. */
size_t gc_status_json(char *buf, size_t size);   /* controllers and profiles */
int gc_profile_get(const char *name, struct gc_profile *out);
int gc_profile_apply(const char *name, const struct gc_profile *p);
/* Saves p under the next free name and gives it to the controller; returns
 * the name, or NULL when the controller is gone or every name is taken. */
const char *gc_profile_new(int slot, const char *id, const struct gc_profile *p);
int gc_profile_delete(const char *name);
int gc_profile_rename(const char *from, const char *to);
/* profile NULL / nintendo -1 = leave as is. */
int gc_controller_set(int slot, const char *id, const char *profile, int nintendo);

/* Called by web_poll once a port that was busy at web_start is taken. */
void gc_web_ready(void);
