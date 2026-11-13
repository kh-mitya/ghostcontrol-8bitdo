/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Home-screen shortcut to the mapping page, the way Payload Manager and other
 * payloads add theirs: a title with only param.json (deeplinkUri) and an
 * icon, registered with AppInstUtil. Registered only when its files are
 * missing or changed. Returns 0 when it is in place, 1 when it was
 * (re)installed, -2 when the files could not be written, or the system error. */
#define SHORTCUT_TITLE_ID "GCBD00001"
int shortcut_install(void);
