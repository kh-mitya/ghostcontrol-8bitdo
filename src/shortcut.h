/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Home-screen shortcut to the mapping page, the way Payload Manager and other
 * payloads add theirs: a title with only param.json (deeplinkUri) and an
 * icon, registered with AppInstUtil on every start. Files are written only
 * when missing or changed. Returns 1 when they were written, 0 when they were
 * already in place, -2 when they could not be written, or the system error. */
#define SHORTCUT_TITLE_ID "GCBD00001"
int shortcut_install(void);
