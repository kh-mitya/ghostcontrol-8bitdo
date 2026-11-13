/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* Bind a virtual pad to a user by calling sceMbusBindDeviceWithUserId inside
 * SceShellUI, so the console does not ask "who is using this controller".
 * Returns 0 on success; on failure the console simply shows that dialog. */
int shellui_bind(uint64_t device_id, int32_t user_id);
