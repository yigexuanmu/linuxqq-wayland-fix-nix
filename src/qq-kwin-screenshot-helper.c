/*
 * qq-kwin-screenshot-helper：替 QQ 调 KWin 的 org.kde.KWin.ScreenShot2。
 *
 * KWin 会检查调用方可执行文件（/proc/<pid>/exe）对应的 .desktop 里有没有
 *   X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2;
 * QQ 自带的 qq.desktop 没有这个声明，直接调会被拒绝。所以由这个 helper
 * 来调：安装的 qq-kwin-screenshot-helper.desktop 里有声明，KWin 按 exe
 * 路径匹配到它，权限检查即可通过（机制与 Spectacle 相同）。
 *
 * 用法：qq-kwin-screenshot-helper > 输出
 *   输出 = struct kwin_shot_header + stride*height 字节原始像素。
 *   像素格式由 KWin 返回的 QImage::Format 决定（见头里的 format 字段）。
 *   失败时退出码非 0，原因打到 stderr。
 *
 * 调用的是 CaptureWorkspace（整个工作区，native-resolution=false 即逻辑
 * 尺寸），父进程再按显示器裁剪；与 portal 不同的是这里拿的是原始像素，
 * 不用解码 PNG、不落盘。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERVICE "org.kde.KWin.ScreenShot2"
#define OBJECT  "/org/kde/KWin/ScreenShot2"
#define IFACE   "org.kde.KWin.ScreenShot2"
#define METHOD  "CaptureWorkspace"

struct kwin_shot_header {
    char magic[4]; /* "QQKS" */
    uint32_t width, height, stride, format; /* QImage::Format */
    double scale;
};

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int main(void)
{
    alarm(30); /* 安全网：任何一步卡住都不要挂在那里 */

    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        fprintf(stderr, "[qq-kde-shot] session bus: %s\n", err->message);
        return 1;
    }

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        perror("[qq-kde-shot] pipe2");
        return 1;
    }

    GUnixFDList *fdlist = g_unix_fd_list_new();
    int idx = g_unix_fd_list_append(fdlist, fds[1], &err);
    close(fds[1]); /* KWin 收到后会 dup，自己的写端可以关了 */
    if (idx < 0) {
        fprintf(stderr, "[qq-kde-shot] fd list: %s\n", err->message);
        return 1;
    }

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "native-resolution", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "include-cursor", g_variant_new_boolean(FALSE));

    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(bus, SERVICE, OBJECT, IFACE, METHOD,
                                                                    g_variant_new("(a{sv}h)", &opts, idx),
                                                                    G_VARIANT_TYPE("(a{sv})"),
                                                                    G_DBUS_CALL_FLAGS_NONE, 10000,
                                                                    fdlist, NULL, NULL, &err);
    g_object_unref(fdlist);
    if (!reply) {
        fprintf(stderr, "[qq-kde-shot] %s failed: %s\n", METHOD, err->message);
        return 1;
    }

    GVariant *dict = NULL;
    g_variant_get(reply, "(@a{sv})", &dict);
    char *type = NULL;
    uint32_t width = 0, height = 0, stride = 0, format = 0;
    double scale = 1.0;
    int ok = g_variant_lookup(dict, "type", "s", &type) &&
             g_variant_lookup(dict, "width", "u", &width) &&
             g_variant_lookup(dict, "height", "u", &height) &&
             g_variant_lookup(dict, "stride", "u", &stride) &&
             g_variant_lookup(dict, "format", "u", &format);
    g_variant_lookup(dict, "scale", "d", &scale);
    g_variant_unref(dict);
    g_variant_unref(reply);

    if (!ok || !type || strcmp(type, "raw") || !width || !height || !stride) {
        fprintf(stderr, "[qq-kde-shot] unexpected reply (type=%s %ux%u stride=%u format=%u)\n",
                type ? type : "?", width, height, stride, format);
        g_free(type);
        return 1;
    }
    g_free(type);

    struct kwin_shot_header hdr;
    memcpy(hdr.magic, "QQKS", 4);
    hdr.width = width;
    hdr.height = height;
    hdr.stride = stride;
    hdr.format = format;
    hdr.scale = scale;
    if (write_all(STDOUT_FILENO, &hdr, sizeof hdr) != 0) {
        fprintf(stderr, "[qq-kde-shot] write header: %s\n", strerror(errno));
        return 1;
    }

    size_t total = (size_t)stride * height, done = 0;
    char buf[65536];
    while (done < total) {
        size_t want = total - done;
        if (want > sizeof buf)
            want = sizeof buf;
        ssize_t n = read(fds[0], buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("[qq-kde-shot] read image");
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "[qq-kde-shot] short image (%zu/%zu)\n", done, total);
            return 1;
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
            fprintf(stderr, "[qq-kde-shot] write image: %s\n", strerror(errno));
            return 1;
        }
        done += (size_t)n;
    }
    close(fds[0]);
    return 0;
}
