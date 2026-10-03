/*
 * qq-borderfix：修掉 QQ 在 Wayland 下的两个窗口问题。
 *
 * ── 问题 1：共享时的「屏幕共享」全屏边框窗口 ──
 * Wayland 下发起屏幕共享后，QQ 会显示一个标题为「屏幕共享」的全屏边框窗口。
 * 它是 QQ 原生代码（标题串在 wrapper.node 里）通过 Chromium/Ozone 建的
 * Wayland toplevel：
 *   - Electron 的 BrowserWindow / BaseWindow API 看不到它；
 *   - QQ 的 Chromium 静态链接了自带 Wayland client（qq 二进制既没有对系统
 *     libwayland 的符号引用，也没有 NEEDED），符号级 LD_PRELOAD 拦不到。
 * 所以本库退到 socket 层：拦 libc 的 sendmsg，解析出站的 Wayland 线协议，
 * 把这条 surface 的 wl_surface.attach(buffer) 原地改写成 attach(NULL)，
 * 让它永远不映射；其它窗口一律不碰。
 * 注意（KWin 的 explicit sync）：KWin 6.x 上 Chromium 会走
 * wp_linux_drm_syncobj_v1，同一个 commit 里还给 surface 设 acquire/release
 * point；只摘 buffer 会让 KWin 报「explicit sync is used, but no buffer is
 * attached」协议错误并断开连接（QQ 随之崩溃）。这两条消息也会被原地改写成
 * 同长度的 attach(NULL)（见 IF_SYNCOBJ_SURFACE）。
 *
 * ── 问题 2：截图覆盖层在平铺合成器里被当普通窗口 ──
 * 截图窗口是 Electron 的 Wayland toplevel（app_id "QQ"、标题为空、尺寸为
 * 屏幕大小）。这里在协议层替它补一条 xdg_toplevel.set_title("QQ 截图")
 * 和一条 xdg_toplevel.set_fullscreen，前者方便合成器窗口规则匹配，后者
 * 让合成器把它精确铺满当前输出。
 *
 * ── 问题 3：QQ 的提示条（如「正在使用…作为您的麦克风」）──
 * 这类提示是 app_id "QQ"、标题「提示」的宽扁小窗口（实测 1200x64）。
 * 这里在协议层把它的绘制缓冲摘掉，已经映射出来的再补一条 attach(NULL)。
 * 尺寸限制（高 ≤128、宽 ≥400）避免误伤同名的正常对话框。
 *
 * ── 实现要点 ──
 * - connect 时识别 Wayland socket 并记下 fd；一个进程可能有多条 Wayland
 *   连接（不同组件的连接对象 id 空间独立），所有状态按连接隔离；
 * - 按创建关系重建对象链：
 *     wl_display.get_registry → wl_registry.bind（载荷里带接口名字符串）
 *     → wl_compositor.create_surface → xdg_wm_base.get_xdg_surface
 *     → xdg_surface.get_toplevel → set_title / set_app_id / set_min_size /
 *       set_max_size / xdg_surface.set_window_geometry
 *   → 找到每个 xdg_toplevel 对应的 wl_surface；
 * - 边框判定：标题 ==「屏幕共享」，min < 600 且 max > 10000（不设限）；
 *   同标题但 min == max 的固定小窗（共享预览、87x40 工具条）排除；
 * - 覆盖层判定：app_id == "QQ"，标题为空，window_geometry ≥ 600x400；
 * - explicit sync：解析 wp_linux_drm_syncobj_manager_v1.get_surface 建立
 *   syncobj surface ↔ wl_surface 映射；被隐藏 surface 的 set_acquire_point /
 *   set_release_point 原地改写成 attach(NULL)（同为 20 字节）；
 * - 隐藏是原地改写 attach 的 buffer id；全屏是在消息边界上补发一条完整消息。
 *
 * ── 维护注意（踩过的坑）──
 * - 注入消息必须严格在「解析确认处于消息边界、且上一整块发送成功」时进行
 *   （见 flush_injection）。曾经在不确定边界时补发 attach(NULL)+commit，
 *   sendmsg 部分发送 / 携带 fd 分两次发时插进了半个消息中间，合成器报
 *   `invalid object` 并杀死 QQ；
 * - sendmsg 可能只发出一部分，被切断的消息要在下次跳过，保持消息边界对齐
 *   （见 wl_skip）；
 * - libwayland 环形缓冲的 iovec 可能是 2 段，需要拼起来解析。
 *
 * ── 环境变量 ──
 *   QQ_BORDER_FIX_DISABLE=1  关掉本库（边框隐藏 + 截图覆盖层全屏 + 提示条隐藏）
 *   QQ_BORDER_FIX_DEBUG=1    输出跟踪细节
 *
 * ── 如何整体删除本功能 ──
 * 1. 删除本文件 src/qq-borderfix.c；
 * 2. Makefile：删 BF_LIB 定义、$(BF_LIB) 构建规则、all / install / clean 里
 *    的 $(BF_LIB)；
 * 3. linuxqq-wayland-fix.in：删 BORDERFIX_LIB 定义、preload 循环里的
 *    "$BORDERFIX_LIB"、--doctor 的对应一行、帮助头部
 *    QQ_BORDER_FIX_DISABLE 注释；
 * 4. 删 README / docs 里相关段落。
 * 以上删掉后其余修复不受影响，本库也不依赖仓库内其它代码。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG(...) do { fprintf(stderr, "[qq-borderfix] " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define MAX_WL_OBJECTS 128
#define MAX_WL_FDS 8

enum wl_iface {
    IF_NONE = 0,
    IF_REGISTRY,
    IF_COMPOSITOR,
    IF_SURFACE,
    IF_XDG_WM_BASE,
    IF_XDG_SURFACE,
    IF_XDG_TOPLEVEL,
    IF_SYNCOBJ_MANAGER, /* wp_linux_drm_syncobj_manager_v1 */
    IF_SYNCOBJ_SURFACE, /* wp_linux_drm_syncobj_surface_v1 */
};

struct wl_object_state {
    uint32_t id;
    int conn; /* 属于哪条 Wayland 连接：不同连接的对象 id 空间互相独立 */
    uint8_t iface;
    uint8_t border;
    uint8_t hide;        /* 该 wl_surface 的 attach 要改成 NULL */
    uint8_t hidden;      /* 该 toplevel 已经处理过隐藏（避免重复） */
    uint8_t has_title;   /* 标题是「屏幕共享」（共享边框判定用） */
    uint8_t title_set;   /* 调用过 set_title（截图覆盖层/通知判定用） */
    uint8_t app_id_set;
    uint8_t is_overlay;  /* 已确认是截图覆盖层并请求过全屏 */
    uint32_t surface;    /* xdg_surface / xdg_toplevel 对应的 wl_surface */
    uint32_t xdg_surface;/* toplevel 对应的 xdg_surface 对象 id */
    int32_t min_w, min_h, max_w, max_h;
    int32_t geom_w, geom_h; /* xdg_surface.set_window_geometry */
    char app_id[64];
    char title[64];
};

static struct wl_object_state wl_objects[MAX_WL_OBJECTS];
static int wl_fds[MAX_WL_FDS];
static int wl_fd_count;
static size_t wl_skip[MAX_WL_FDS]; /* 部分发送后，需要跳过的消息余量 */
static uint8_t inject_buf[MAX_WL_FDS][64]; /* 待注入的完整消息（只在消息边界发） */
static size_t inject_len[MAX_WL_FDS];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t io_once = PTHREAD_ONCE_INIT;
static int (*real_connect)(int, const struct sockaddr *, socklen_t);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);

static int disabled, debug;

__attribute__((constructor))
static void qq_borderfix_init(void)
{
    const char *v = getenv("QQ_BORDER_FIX_DISABLE");
    if (v && *v && strcmp(v, "0") != 0)
        disabled = 1;
    v = getenv("QQ_BORDER_FIX_DEBUG");
    debug = v && *v && strcmp(v, "0") != 0;
    if (debug)
        LOG("watching wayland traffic (pid=%ld)%s", (long)getpid(),
            disabled ? " (disabled)" : "");
}

static void resolve_io(void)
{
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
}

/* ---------------- 小工具 ---------------- */

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* ---------------- 连接表 ---------------- */

static int tracked_fd(int fd)
{
    for (int i = 0; i < wl_fd_count; i++)
        if (wl_fds[i] == fd)
            return i;
    return -1;
}

/* 记录新连接；同一 fd 被新连接复用时清掉旧连接留下的状态。 */
static void track_conn(int conn, int fd)
{
    wl_fds[conn] = fd;
    wl_skip[conn] = 0;
    for (size_t i = 0; i < MAX_WL_OBJECTS; i++)
        if (wl_objects[i].conn == conn)
            memset(&wl_objects[i], 0, sizeof wl_objects[i]);
}

/* ---------------- 对象表（按连接隔离） ---------------- */

static struct wl_object_state *object_state(int conn, uint32_t id, int create)
{
    struct wl_object_state *free_slot = NULL;
    for (size_t i = 0; i < MAX_WL_OBJECTS; i++) {
        if (wl_objects[i].iface != IF_NONE && wl_objects[i].conn == conn &&
            wl_objects[i].id == id)
            return &wl_objects[i];
        if (create && !free_slot && wl_objects[i].iface == IF_NONE)
            free_slot = &wl_objects[i];
    }
    if (create && free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->id = id;
        free_slot->conn = conn;
        return free_slot;
    }
    return NULL;
}

static void object_forget(int conn, uint32_t id)
{
    struct wl_object_state *o = object_state(conn, id, 0);
    if (o)
        memset(o, 0, sizeof *o);
}

static int iface_from_name(const uint8_t *name, size_t len)
{
    if (len == sizeof("wl_registry") && !memcmp(name, "wl_registry", len))
        return IF_REGISTRY;
    if (len == sizeof("wl_compositor") && !memcmp(name, "wl_compositor", len))
        return IF_COMPOSITOR;
    if (len == sizeof("xdg_wm_base") && !memcmp(name, "xdg_wm_base", len))
        return IF_XDG_WM_BASE;
    if (len == sizeof("wp_linux_drm_syncobj_manager_v1") &&
        !memcmp(name, "wp_linux_drm_syncobj_manager_v1", len))
        return IF_SYNCOBJ_MANAGER;
    return IF_NONE;
}

/* ---------------- 判定与改写 ---------------- */

static void border_check(struct wl_object_state *o)
{
    if (debug)
        LOG("candidate conn=%d id=%u title=%u surface=%u min=%dx%d max=%dx%d",
            o->conn, o->id, o->has_title, o->surface,
            o->min_w, o->min_h, o->max_w, o->max_h);
    if (o->border || !o->has_title || !o->surface)
        return;
    if (!(o->min_w < 600 && o->min_h < 600 && o->max_w > 10000 && o->max_h > 10000))
        return;

    o->border = 1;
    struct wl_object_state *surf = object_state(o->conn, o->surface, 0);
    if (surf)
        surf->border = 1;
    LOG("hiding 屏幕共享 border window (min %dx%d max %dx%d)",
        o->min_w, o->min_h, o->max_w, o->max_h);
}

/* ---------------- 截图覆盖层全屏 ----------------
 *
 * 截图窗口是 Electron 的 Wayland toplevel：app_id "QQ"、标题为空、尺寸为
 * 屏幕大小。平铺合成器会把它当普通窗口摆出来。这里在协议层替它补一条
 * xdg_toplevel.set_fullscreen，让合成器把它精确铺满当前输出。
 *
 * 注入只在「解析确认处于消息边界、且上一整块发送成功」时进行（见
 * flush_injection）：曾经在不确定边界时补发消息，插进了半个消息中间，
 * 合成器报 `invalid object` 把 QQ 杀了。
 */
static void queue_message(int conn, const uint8_t *msg, size_t len)
{
    if (conn < 0 || inject_len[conn] + len > sizeof inject_buf[conn])
        return;
    memcpy(inject_buf[conn] + inject_len[conn], msg, len);
    inject_len[conn] += len;
}

static void queue_fullscreen(int conn, uint32_t toplevel)
{
    uint8_t msg[12];
    put_u32(msg, toplevel);
    put_u32(msg + 4, (uint32_t)((12u << 16) | 11u)); /* xdg_toplevel.set_fullscreen */
    put_u32(msg + 8, 0);                             /* output = NULL */
    queue_message(conn, msg, sizeof msg);
}

/* 替覆盖层设一个稳定标题，方便合成器窗口规则匹配（Electron 自己不设标题）。 */
static void queue_title(int conn, uint32_t toplevel, const char *title)
{
    size_t slen = strlen(title) + 1; /* 线协议里的字符串长度含结尾 NUL */
    size_t padded = (slen + 3) & ~(size_t)3;
    size_t size = 8 + 4 + padded;
    if (inject_len[conn] + size > sizeof inject_buf[conn])
        return;
    uint8_t *p = inject_buf[conn] + inject_len[conn];
    put_u32(p, toplevel);
    put_u32(p + 4, (uint32_t)((size << 16) | 2u)); /* xdg_toplevel.set_title */
    put_u32(p + 8, (uint32_t)slen);
    memset(p + 12, 0, padded);
    memcpy(p + 12, title, slen);
    inject_len[conn] += size;
}

/* 补一条 attach(NULL)+commit，把已经映射出来的窗口撤下去。 */
static void queue_unmap(int conn, uint32_t surface)
{
    uint8_t msg[32];
    put_u32(msg, surface);
    put_u32(msg + 4, (uint32_t)((20u << 16) | 1u)); /* wl_surface.attach */
    put_u32(msg + 8, 0);
    put_u32(msg + 12, 0);
    put_u32(msg + 16, 0);
    put_u32(msg + 20, surface);
    put_u32(msg + 24, (uint32_t)((12u << 16) | 6u)); /* wl_surface.commit */
    queue_message(conn, msg, sizeof msg);
}

/*
 * QQ 的麦克风设备提示条：app_id "QQ"、标题「提示」、宽扁小条
 * （实测 1200x64）。捡到就摘掉它的绘制缓冲；已经映射出来的补一条 unmap。
 * 尺寸限制是为了不误伤同名的正常对话框。
 */
static void notification_check(struct wl_object_state *o)
{
    if (disabled || o->hidden || o->border || o->is_overlay)
        return;
    if (!o->app_id_set || strcmp(o->app_id, "QQ") != 0)
        return;
    if (!o->title_set || strcmp(o->title, "提示") != 0)
        return;
    if (!(o->geom_w >= 400 && o->geom_h > 0 && o->geom_h <= 128))
        return;

    o->hidden = 1;
    struct wl_object_state *surf = object_state(o->conn, o->surface, 0);
    if (surf)
        surf->hide = 1;
    queue_unmap(o->conn, o->surface);
    LOG("hiding QQ notification window (提示, %dx%d)", o->geom_w, o->geom_h);
}

static void overlay_check(struct wl_object_state *o)
{
    if (disabled || o->is_overlay || o->border)
        return;
    if (!o->app_id_set || strcmp(o->app_id, "QQ") != 0)
        return;
    if (o->title_set && o->title[0] != '\0')
        return;
    if (!(o->geom_w >= 600 && o->geom_h >= 400))
        return;

    o->is_overlay = 1;
    queue_title(o->conn, o->id, "QQ 截图");
    queue_fullscreen(o->conn, o->id);
    LOG("requesting title + fullscreen for the screenshot overlay (window %u, %dx%d)",
        o->id, o->geom_w, o->geom_h);
}

/* 把注入消息写到 socket；调用方必须已确认 app 的流在消息边界上。 */
static void flush_injection(int conn)
{
    if (conn < 0 || !inject_len[conn])
        return;
    int fd = wl_fds[conn];
    while (inject_len[conn]) {
        ssize_t w = send(fd, inject_buf[conn], inject_len[conn], MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = { fd, POLLOUT, 0 };
                if (poll(&p, 1, 1000) > 0)
                    continue;
            }
            LOG("cannot send injected wayland message: %s", strerror(errno));
            inject_len[conn] = 0;
            return;
        }
        if ((size_t)w < inject_len[conn])
            memmove(inject_buf[conn], inject_buf[conn] + w, inject_len[conn] - (size_t)w);
        inject_len[conn] -= (size_t)w;
    }
}

/* ---------------- Wayland 线协议 ---------------- */

static void handle_wire_request(int conn, uint8_t *msg, uint32_t id, uint32_t opcode,
                                uint8_t *args, size_t len)
{
    if (id == 1) { /* wl_display：get_registry */
        if (opcode == 1 && len >= 4) {
            struct wl_object_state *o = object_state(conn, get_u32(args), 1);
            if (o)
                o->iface = IF_REGISTRY;
        }
        return;
    }

    struct wl_object_state *o = object_state(conn, id, 0);
    if (!o)
        return;

    switch (o->iface) {
    case IF_REGISTRY:
        if (opcode == 0 && len >= 16) { /* bind(name, interface, version, new_id) */
            size_t slen = get_u32(args + 4);
            size_t padded = (slen + 3) & ~(size_t)3;
            if (slen < 1 || slen > 128 || len < 16 + padded)
                return;
            uint32_t new_id = get_u32(args + 12 + padded);
            int iface = iface_from_name(args + 8, slen);
            struct wl_object_state *n = object_state(conn, new_id, 1);
            if (n)
                n->iface = (uint8_t)iface;
        }
        break;
    case IF_COMPOSITOR:
        if (opcode == 0 && len >= 4) { /* create_surface(new_id) */
            struct wl_object_state *n = object_state(conn, get_u32(args), 1);
            if (n)
                n->iface = IF_SURFACE;
        }
        break;
    case IF_XDG_WM_BASE:
        if (opcode == 2 && len >= 8) { /* get_xdg_surface(new_id, wl_surface) */
            struct wl_object_state *n = object_state(conn, get_u32(args), 1);
            if (n) {
                n->iface = IF_XDG_SURFACE;
                n->surface = get_u32(args + 4);
            }
        }
        break;
    case IF_XDG_SURFACE:
        if (opcode == 1 && len >= 4) { /* get_toplevel(new_id) */
            struct wl_object_state *n = object_state(conn, get_u32(args), 1);
            if (n) {
                n->iface = IF_XDG_TOPLEVEL;
                n->surface = o->surface;
                n->xdg_surface = id;
            }
        } else if (opcode == 3 && len >= 16) { /* set_window_geometry(x, y, w, h) */
            int32_t w = (int32_t)get_u32(args + 8);
            int32_t h = (int32_t)get_u32(args + 12);
            for (size_t i = 0; i < MAX_WL_OBJECTS; i++) {
                struct wl_object_state *t = &wl_objects[i];
                if (t->iface == IF_XDG_TOPLEVEL && t->conn == conn && t->xdg_surface == id) {
                    t->geom_w = w;
                    t->geom_h = h;
                    overlay_check(t);
                    notification_check(t);
                }
            }
        } else if (opcode == 0) { /* destroy */
            object_forget(conn, id);
        }
        break;
    case IF_XDG_TOPLEVEL:
        if (opcode == 2 && len >= 4) { /* set_title(string) */
            size_t slen = get_u32(args);
            size_t padded = (slen + 3) & ~(size_t)3;
            if (slen >= 1 && len >= 4 + padded) {
                o->title_set = 1;
                size_t n = slen - 1;
                if (n >= sizeof o->title)
                    n = sizeof o->title - 1;
                memcpy(o->title, args + 4, n);
                o->title[n] = '\0';
                if (slen == sizeof("屏幕共享") &&
                    !memcmp(args + 4, "屏幕共享", sizeof("屏幕共享")))
                    o->has_title = 1;
            }
            border_check(o);
            overlay_check(o);
            notification_check(o);
        } else if (opcode == 3 && len >= 4) { /* set_app_id(string) */
            size_t slen = get_u32(args);
            size_t padded = (slen + 3) & ~(size_t)3;
            if (slen >= 1 && len >= 4 + padded) {
                o->app_id_set = 1;
                size_t n = slen - 1;
                if (n >= sizeof o->app_id)
                    n = sizeof o->app_id - 1;
                memcpy(o->app_id, args + 4, n);
                o->app_id[n] = '\0';
            }
            overlay_check(o);
            notification_check(o);
        } else if (opcode == 7 && len >= 8) { /* set_max_size */
            o->max_w = (int32_t)get_u32(args);
            o->max_h = (int32_t)get_u32(args + 4);
            border_check(o);
            overlay_check(o);
            notification_check(o);
        } else if (opcode == 8 && len >= 8) { /* set_min_size */
            o->min_w = (int32_t)get_u32(args);
            o->min_h = (int32_t)get_u32(args + 4);
            border_check(o);
            overlay_check(o);
            notification_check(o);
        } else if (opcode == 0) { /* destroy */
            object_forget(conn, id);
        }
        break;
    case IF_SYNCOBJ_MANAGER:
        if (opcode == 1 && len >= 8) { /* get_surface(new_id, wl_surface) */
            struct wl_object_state *n = object_state(conn, get_u32(args), 1);
            if (n) {
                n->iface = IF_SYNCOBJ_SURFACE;
                n->surface = get_u32(args + 4);
            }
        }
        break;
    case IF_SYNCOBJ_SURFACE:
        if ((opcode == 1 || opcode == 2) && len == 12) {
            /* set_acquire_point / set_release_point（12 字节载荷，共 20 字节）。
             * 对应 wl_surface 的 buffer 已被我们摘掉（共享边框）时，保留显式同步点
             * 会让 KWin 在 commit 时报「explicit sync is used, but no buffer is
             * attached」协议错误并断开连接，QQ 随之崩溃。把这条消息原地改写成
             * 同一 surface 的 attach(NULL,0,0)（同样 20 字节）：不产生同步状态，
             * 也不改变消息边界。 */
            struct wl_object_state *surf = object_state(conn, o->surface, 0);
            if (surf && (surf->border || surf->hide)) {
                put_u32(msg, o->surface);
                put_u32(msg + 4, (uint32_t)((20u << 16) | 1u)); /* wl_surface.attach */
                put_u32(args, 0);
                put_u32(args + 4, 0);
                put_u32(args + 8, 0);
                if (debug)
                    LOG("neutralized explicit-sync point on hidden surface %u", o->surface);
            }
        } else if (opcode == 0) { /* destroy */
            object_forget(conn, id);
        }
        break;
    case IF_SURFACE:
        if (opcode == 1 && (o->border || o->hide)) { /* attach(buffer, x, y) -> attach(NULL) */
            if (len >= 12 && get_u32(args) != 0)
                put_u32(args, 0);
        } else if (opcode == 0) { /* destroy */
            object_forget(conn, id);
        }
        break;
    }
}

/* 从缓冲区开头按消息头推进；size 不合法就整块停止。 */
static void rewrite_chunk(int conn, uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off + 8 <= len) {
        uint32_t id = get_u32(buf + off);
        uint32_t word = get_u32(buf + off + 4);
        size_t size = word >> 16;
        uint32_t opcode = word & 0xffff;
        if (size < 8 || (size & 3) || size > len - off)
            break;
        handle_wire_request(conn, buf + off, id, opcode, buf + off + 8, size - 8);
        off += size;
    }
}

/* 找到包含第 sent 个字节的那条消息的结束偏移（部分发送后的重同步用）。 */
static size_t message_end_at(const uint8_t *buf, size_t len, size_t start, size_t sent)
{
    size_t off = start;
    while (off + 8 <= len) {
        uint32_t word = get_u32(buf + off + 4);
        size_t size = word >> 16;
        if (size < 8 || (size & 3) || size > len - off)
            break;
        if (off + size > sent)
            return off + size;
        off += size;
    }
    return len;
}

/* ---------------- 拦截 ---------------- */

/* 地址是不是 Wayland socket：常规名字含 "wayland"，另外兼容自定义
 * WAYLAND_DISPLAY（可能是绝对路径或抽象名）。 */
static int wayland_addr_matches(const struct sockaddr_un *un, socklen_t len)
{
    const uint8_t *p = (const uint8_t *)un->sun_path;
    size_t base = offsetof(struct sockaddr_un, sun_path);
    size_t n = len > base ? (size_t)len - base : 0;

    for (size_t i = 0; i + 7 <= n; i++)
        if (!memcmp(p + i, "wayland", 7))
            return 1;

    const char *disp = getenv("WAYLAND_DISPLAY");
    if (!disp || !*disp)
        return 0;
    const char *name = strrchr(disp, '/');
    name = name ? name + 1 : disp;
    size_t dl = strlen(name);
    for (size_t i = 0; dl && i + dl <= n; i++)
        if (!memcmp(p + i, name, dl))
            return 1;
    return 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    pthread_once(&io_once, resolve_io);
    int rc = real_connect(fd, addr, len);

    if (rc == 0 && !disabled && addr && addr->sa_family == AF_UNIX &&
        wayland_addr_matches((const struct sockaddr_un *)addr, len)) {
        pthread_mutex_lock(&lock);
        int conn = tracked_fd(fd);
        if (conn < 0 && wl_fd_count < MAX_WL_FDS)
            conn = wl_fd_count++;
        if (conn >= 0)
            track_conn(conn, fd);
        pthread_mutex_unlock(&lock);
        if (debug)
            LOG("tracking wayland fd=%d", fd);
    }
    return rc;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    pthread_once(&io_once, resolve_io);

    int conn = (!disabled && msg) ? tracked_fd(fd) : -1;

    if (debug && conn >= 0) {
        static int calls;
        if (calls < 12) {
            calls++;
            LOG("sendmsg fd=%d iovlen=%zu len0=%zu len1=%zu",
                fd, msg->msg_iovlen,
                msg->msg_iov ? msg->msg_iov[0].iov_len : 0,
                (msg->msg_iov && msg->msg_iovlen > 1) ? msg->msg_iov[1].iov_len : 0);
        }
    }

    struct iovec *iov = msg ? msg->msg_iov : NULL;
    size_t total = 0;
    uint8_t *tmp = NULL;
    uint8_t *buf = NULL;
    size_t parse_start = 0;

    if (conn >= 0 && msg && msg->msg_iovlen >= 1 && iov && iov[0].iov_base) {
        total = iov[0].iov_len;
        if (msg->msg_iovlen >= 2 && iov[1].iov_base && iov[1].iov_len > 0) {
            /* 环形缓冲可能拆成两段，拼起来一起解析/改写 */
            tmp = malloc(total + iov[1].iov_len);
            if (tmp) {
                memcpy(tmp, iov[0].iov_base, iov[0].iov_len);
                memcpy(tmp + iov[0].iov_len, iov[1].iov_base, iov[1].iov_len);
                total += iov[1].iov_len;
                buf = tmp;
            }
        } else {
            buf = iov[0].iov_base;
        }
    }

    if (buf && total >= 8) {
        pthread_mutex_lock(&lock);
        if (wl_skip[conn] >= total) {
            wl_skip[conn] -= total;
        } else {
            if (wl_skip[conn] > 0) {
                parse_start = wl_skip[conn];
                wl_skip[conn] = 0;
            }
            rewrite_chunk(conn, buf + parse_start, total - parse_start);
        }
        pthread_mutex_unlock(&lock);
    }

    if (tmp) {
        memcpy(iov[0].iov_base, tmp, iov[0].iov_len);
        if (iov[1].iov_base)
            memcpy(iov[1].iov_base, tmp + iov[0].iov_len, total - iov[0].iov_len);
    }

    ssize_t rc = real_sendmsg(fd, msg, flags);

    /* 整块发送成功且落在消息边界上时，才把待注入消息写进去。 */
    if (conn >= 0 && buf && rc > 0 && (size_t)rc == total) {
        pthread_mutex_lock(&lock);
        if (wl_skip[conn] == 0)
            flush_injection(conn);
        pthread_mutex_unlock(&lock);
    }

    /* 部分发送把一条消息切成两半时，下次跳过它的余量，保持边界对齐。 */
    if (buf && rc > 0 && (size_t)rc < total) {
        size_t skip;
        if ((size_t)rc < parse_start)
            skip = parse_start - (size_t)rc;
        else
            skip = message_end_at(buf, total, parse_start, (size_t)rc) - (size_t)rc;
        pthread_mutex_lock(&lock);
        wl_skip[conn] = skip;
        pthread_mutex_unlock(&lock);
        if (debug)
            LOG("partial send (%zd/%zu), skip %zu next", rc, total, skip);
    }

    free(tmp);
    return rc;
}
