/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shortcut.h"
#include "web.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ps5/kernel.h>

#define INCASSET(name, file)                                                \
    __asm__(".section .rodata\n.global " #name "\n.global " #name "_size\n"  \
            ".align 16\n" #name ":\n.incbin \"" file "\"\n" #name "_end:\n"  \
            ".align 8\n" #name "_size:\n.quad " #name "_end - " #name "\n"   \
            ".previous\n");                                                 \
    extern const uint8_t name[];                                            \
    extern const size_t name##_size;

INCASSET(shortcut_icon_png, "assets/icon0.png");

#define STR2(x) #x
#define STR(x) STR2(x)
static const char param_json[] =
    "{\n"
    "    \"titleId\": \"" SHORTCUT_TITLE_ID "\",\n"
    "    \"applicationCategoryType\": 65536,\n"
    "    \"deeplinkUri\": \"http://127.0.0.1:" STR(WEB_PORT) "/\",\n"
    "    \"localizedParameters\": {\n"
    "        \"defaultLanguage\": \"en-US\",\n"
    "        \"en-US\": {\n"
    "            \"titleName\": \"GhostControl 8BitDo\"\n"
    "        }\n"
    "    }\n"
    "}\n";

#define APP_DIR "/user/app/" SHORTCUT_TITLE_ID
#define SYS_DIR APP_DIR "/sce_sys"

extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilTerminate(void);

static int same(const char *path, const void *data, size_t size) {
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size != size) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    static uint8_t buf[256 * 1024];
    int ok = size <= sizeof(buf) && read(fd, buf, size) == (ssize_t)size &&
             memcmp(buf, data, size) == 0;
    close(fd);
    return ok;
}

static int write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, data, size) == (ssize_t)size;
    close(fd);
    return ok ? 0 : -1;
}

int shortcut_install(void) {
    if (same(SYS_DIR "/param.json", param_json, sizeof(param_json) - 1) &&
        same(SYS_DIR "/icon0.png", shortcut_icon_png, shortcut_icon_png_size))
        return 0;
    int err = sceAppInstUtilInitialize();
    if (err) return -1;
    int r = -2;
    if ((mkdir(APP_DIR, 0755) == 0 || errno == EEXIST) &&
        (mkdir(SYS_DIR, 0755) == 0 || errno == EEXIST) &&
        write_file(SYS_DIR "/param.json", param_json, sizeof(param_json) - 1) == 0 &&
        write_file(SYS_DIR "/icon0.png", shortcut_icon_png, shortcut_icon_png_size) == 0) {
        int (*install_title_dir)(const char *, const char *, void *) = NULL;
        uint32_t handle;
        if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
            install_title_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
        r = install_title_dir && install_title_dir(SHORTCUT_TITLE_ID, "/user/app/", 0) == 0 ? 1 : -3;
    }
    sceAppInstUtilTerminate();
    return r;
}
