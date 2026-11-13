/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include "mapper.h"

/* Mapping page on http://<console>:WEB_PORT/. Single-threaded: the caller
 * gives it time with web_poll(); a request is answered in one go. */
#define WEB_PORT 8096

int web_start(int port, const char *presets_dir);   /* 0 ok */
void web_poll(int timeout_ms);                       /* wait for and serve one request */

/* Provided by main.c. Connected controllers as JSON ("analog":0 = triggers
 * are on/off only, as on the Switch protocol). A controller is addressed by
 * its slot (1-based) and its id, so a request never lands on another
 * controller that took the slot meanwhile; get/set return 0 when it matches. */
size_t gc_status_json(char *buf, size_t size);
int gc_mapping_get(int slot, const char *id, struct gc_profile *out);
int gc_mapping_set(int slot, const char *id, const struct gc_profile *p);
