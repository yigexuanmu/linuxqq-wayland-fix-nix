/*
 * libqq-stable-audio.so —— 让 QQ 看到的「默认音频设备」在蓝牙 profile 切换时不变。
 *
 * 背景（实测）：蓝牙耳机连着的时候点「屏幕共享」，BlueZ 会把耳机从 A2DP 切到 HFP
 * （共享要开麦克风），PipeWire/WirePlumber 重建 sink/source 节点，服务端的默认设备
 * 跟着变。QQ 的 PulseAudioWrapper 在「默认设备变了」的回调里写自己的加密日志
 * （mars xlog），在 realloc 上 SIGTRAP，整个 QQ 进程被杀：
 *
 *   线程 audio-capture-p / audio-render-pu，信号 5/TRAP
 *   AudioCaptureBase::SysThreadProc
 *    → AudioCapturePulse::OnDefaultDeviceChanged
 *    → PulseAudioWrapper::OnDefaultDeviceChanged → UpdateDefaultDeviceInfo(bool)
 *    → CTRAELog::Output → mars::xlog::LogCrypt::CryptAsyncLog
 *    → AutoBuffer::AllocWrite → __libc_realloc → tencent::fatalHandler
 *
 * 于是「共享还没出来，QQ 先没了」。修不了 QQ 的代码（崩在它自己的进程里），
 * 但可以让它永远收不到「默认设备变了」这件事：
 *
 *   1. 拦截 pa_context_set_subscribe_callback()：server / sink / source / card
 *      四类事件丢掉，QQ 自己的流事件（sink input / source output）照旧传下去。
 *      默认设备概念于是恒定，那个回调不会被调用。
 *   2. 拦截 pa_stream_connect_playback()：把 bluez_output.* 设备名换成 NULL
 *      （= 交给服务端选，即当前默认输出）。蓝牙 profile 切换时 sink 节点名会变
 *      （bluez_output.<mac>.1-88 / .1-74 …），QQ 缓存的名字会失效；交给服务端选，
 *      流始终落在当前默认输出（还是那只耳机）上。
 *
 * 只影响本进程（LD_PRELOAD），不动系统音频配置，也不碰上游源码。
 *
 * 副作用：QQ 运行期间，它音频设置界面的设备列表不再刷新（重启 QQ 即恢复）；
 * 在 QQ 里显式选中的蓝牙输出会被忽略，改为跟随系统默认输出。
 * 关掉：QQ_WAYLAND_FIX_STABLE_AUDIO=0（本库仍在进程里，但全部直通）。
 *
 * 日志：走 stderr，启动器把它重定向到 $QQ_WAYLAND_FIX_LOG
 * （默认 $XDG_RUNTIME_DIR/linuxqq-wayland-fix.log），每进程最多 24 行。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <pulse/pulseaudio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ---------- 开关 ---------- */

static int is_off(const char *v)
{
    return !v || !*v || !strcasecmp(v, "0") || !strcasecmp(v, "no") ||
           !strcasecmp(v, "off") || !strcasecmp(v, "false") ||
           !strcasecmp(v, "disable") || !strcasecmp(v, "disabled");
}

static int enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *v = getenv("QQ_WAYLAND_FIX_STABLE_AUDIO");
        cached = (v == NULL) ? 1 : !is_off(v);
    }
    return cached;
}

/* ---------- 日志 ---------- */

#define LOG_MAX 24

static void msg(const char *fmt, ...)
{
    static int n;
    va_list ap;

    /* 每个进程最多打 LOG_MAX 行，别把启动器日志刷爆。 */
    if (__sync_fetch_and_add(&n, 1) >= LOG_MAX)
        return;

    fprintf(stderr, "[qq-stable-audio pid=%ld] ", (long)getpid());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---------- 取真函数 ---------- */

/*
 * 上游的 libqq-wl-portal.so 拦截了 dlsym（蹦床），所以这里跟它一样：先用 dlvsym
 * 拿到真正的 dlsym，再用它查符号。这样调用的返回地址落在本库里，RTLD_NEXT
 * 会从本库之后开始找，命中的才是 libpulse 的实现（而不是我们自己）。
 */
static void *real_sym(const char *name)
{
    static void *(*real_dlsym)(void *, const char *);

    if (!real_dlsym) {
        real_dlsym = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym)
            real_dlsym = (void *(*)(void *, const char *))
                dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
    return real_dlsym ? real_dlsym(RTLD_NEXT, name) : NULL;
}

/* ---------- 1. 订阅回调：丢掉设备类事件 ---------- */

struct sub_cb {
    pa_context *c;
    pa_context_subscribe_cb_t cb;
    void *userdata;
};

#define SUB_SLOTS 8
static struct sub_cb subs[SUB_SLOTS];
static pthread_mutex_t subs_mu = PTHREAD_MUTEX_INITIALIZER;

/* 会牵动 QQ「默认设备变了」逻辑的事件类别。 */
static int is_device_event(pa_subscription_event_type_t t)
{
    unsigned f = (unsigned)t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

    return f == PA_SUBSCRIPTION_EVENT_SERVER || f == PA_SUBSCRIPTION_EVENT_SINK ||
           f == PA_SUBSCRIPTION_EVENT_SOURCE || f == PA_SUBSCRIPTION_EVENT_CARD;
}

static void filter_cb(pa_context *c, pa_subscription_event_type_t t,
                      uint32_t idx, void *userdata)
{
    struct sub_cb *s = userdata;

    if (is_device_event(t)) {
        msg("drop device event: facility=0x%x type=0x%x idx=%u",
            (unsigned)t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK,
            (unsigned)t & PA_SUBSCRIPTION_EVENT_TYPE_MASK, idx);
        return;
    }
    if (s->cb)
        s->cb(c, t, idx, s->userdata);
}

void pa_context_set_subscribe_callback(pa_context *c,
                                       pa_context_subscribe_cb_t cb, void *userdata)
{
    static void (*real)(pa_context *, pa_context_subscribe_cb_t, void *);
    struct sub_cb *slot = NULL;
    int i;

    if (!real)
        real = real_sym("pa_context_set_subscribe_callback");
    if (!real)
        return;

    if (!enabled()) {
        real(c, cb, userdata);
        return;
    }

    pthread_mutex_lock(&subs_mu);

    /* 撤销回调：清掉槽位，原样传下去。 */
    if (!cb) {
        for (i = 0; i < SUB_SLOTS; i++)
            if (subs[i].c == c)
                memset(&subs[i], 0, sizeof(subs[i]));
        pthread_mutex_unlock(&subs_mu);
        real(c, NULL, userdata);
        return;
    }

    for (i = 0; i < SUB_SLOTS; i++)
        if (subs[i].c == c) {
            slot = &subs[i];
            break;
        }
    if (!slot)
        for (i = 0; i < SUB_SLOTS; i++)
            if (subs[i].c == NULL) {
                slot = &subs[i];
                break;
            }

    if (slot) {
        slot->c = c;
        slot->cb = cb;
        slot->userdata = userdata;
        pthread_mutex_unlock(&subs_mu);
        msg("subscribe callback installed: device events (server/sink/source/card) will be dropped");
        real(c, filter_cb, slot);
    } else {
        /* 槽位用光，不干预。 */
        pthread_mutex_unlock(&subs_mu);
        real(c, cb, userdata);
    }
}

/* ---------- 2. 播放流别绑死在会消失的蓝牙节点名上 ---------- */

static int is_bluetooth_sink_name(const char *dev)
{
    return dev && strncmp(dev, "bluez_output.", 13) == 0;
}

int pa_stream_connect_playback(pa_stream *s, const char *dev,
                               const pa_buffer_attr *attr, pa_stream_flags_t flags,
                               const pa_cvolume *volume, pa_stream *sync_stream)
{
    static int (*real)(pa_stream *, const char *, const pa_buffer_attr *,
                       pa_stream_flags_t, const pa_cvolume *, pa_stream *);

    if (!real)
        real = real_sym("pa_stream_connect_playback");
    if (!real)
        return -1;

    if (enabled() && is_bluetooth_sink_name(dev)) {
        msg("playback stream: %s -> server default (bluetooth node name changes with the profile)",
            dev);
        dev = NULL;
    }
    return real(s, dev, attr, flags, volume, sync_stream);
}
