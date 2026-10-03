/*
 * libqq-stable-audio.so —— 让 QQ 看到的「默认音频设备」在蓝牙 profile 切换时不变。
 *
 * 背景（实测）：蓝牙耳机连着的时候点「屏幕共享」，BlueZ 会把耳机从 A2DP 切到 HFP
 * （共享要开麦克风），PipeWire/WirePlumber 重建 sink/source 节点，服务端的默认设备
 * 跟着变。QQ 的 PulseAudioWrapper 在「默认设备变了」的分支里写自己的加密日志
 * （mars xlog），在 realloc 上 SIGTRAP，整个 QQ 进程被杀：
 *
 *   线程 audio-capture-p / audio-render-pu，信号 5/TRAP
 *   AudioCaptureBase::SysThreadProc
 *    → AudioCapturePulse::OnDefaultDeviceChanged
 *    → PulseAudioWrapper::OnDefaultDeviceChanged → UpdateDefaultDeviceInfo(bool)
 *    → CTRAELog::Output → mars::xlog::LogCrypt::CryptAsyncLog
 *    → AutoBuffer::AllocWrite → __libc_realloc → tencent::fatalHandler
 *
 * 修不了 QQ 的代码（崩在它自己的进程里），但可以让它永远看不到「默认设备变了」：
 *
 *   1. pa_context_set_subscribe_callback()：server / sink / source / card
 *      四类事件丢掉，QQ 自己的流事件（sink input / source output）照旧传下去。
 *   2. pa_context_get_server_info()：把 default_sink_name / default_source_name
 *      冻结成进程里第一次看到的名字。QQ 是**轮询**这个接口来发现默认设备变化的
 *      （日志里的 UpdatePulseAudioServerInfo / PaServerInfoCallbackHandler /
 *      UpdateDefaultDeviceInfo），只挡订阅事件不够。每次真的变了都记一行日志。
 *   3. pa_stream_connect_playback()/connect_record()：bluez_output.* / bluez_input.*
 *      的名字换成 NULL（= 交给服务端选，即当前默认输出/输入）。蓝牙 profile 切换时
 *      节点名会变（bluez_output.<mac>.1-88 / .1-74 …），QQ 缓存的名字会失效；
 *      交给服务端选，流始终落在当前默认设备（还是那只耳机）上。
 *      （bluez_output.*.monitor 不在改写范围内，那是上游「共享电脑声音」的录音源。）
 *
 * 只影响本进程（LD_PRELOAD），不动系统音频配置，也不碰上游源码。
 *
 * 副作用：QQ 运行期间，它音频设置界面的设备列表不再刷新（重启 QQ 即恢复）；
 * 在 QQ 里显式选中的蓝牙设备会被忽略，改为跟随系统默认设备。
 * 关掉：QQ_WAYLAND_FIX_STABLE_AUDIO=0（本库仍在进程里，但全部直通）。
 *
 * 日志：走 stderr，启动器把它重定向到 $QQ_WAYLAND_FIX_LOG
 * （默认 $XDG_RUNTIME_DIR/linuxqq-wayland-fix.log），每进程最多 32 行。
 * 启动时会无条件写一行 "loaded"，用来确认本库真的进了 QQ 的进程。
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
    return !strcasecmp(v, "0") || !strcasecmp(v, "no") ||
           !strcasecmp(v, "off") || !strcasecmp(v, "false") ||
           !strcasecmp(v, "disable") || !strcasecmp(v, "disabled");
}

/* 默认开。只有显式设成 0/no/off/... 才直通。
 * 注意别写成 is_off(getenv(...)) 再取反：getenv 返回 NULL 时那样会变成「默认关」，
 * 于是本库悄悄地什么都不做（2026-10-04 踩过）。 */
static int enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *v = getenv("QQ_WAYLAND_FIX_STABLE_AUDIO");

        cached = (v && *v) ? !is_off(v) : 1;
    }
    return cached;
}

/* ---------- 日志 ---------- */

#define LOG_MAX 32
static int log_used;

static void msg(const char *fmt, ...)
{
    va_list ap;

    if (__sync_fetch_and_add(&log_used, 1) >= LOG_MAX)
        return;
    fprintf(stderr, "[qq-stable-audio pid=%ld] ", (long)getpid());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

__attribute__((constructor)) static void shim_init(void)
{
    if (!enabled()) {
        msg("loaded, but disabled by QQ_WAYLAND_FIX_STABLE_AUDIO");
        return;
    }
    msg("loaded: subscribe / server-info / playback / record interposed");
}

/* ---------- 取真函数 ---------- */

/*
 * 上游的 libqq-wl-portal.so 拦截了 dlsym（蹦床），所以这里跟它一样：先用 dlvsym
 * 拿到真正的 dlsym，再用它查符号。这样调用的返回地址落在本库里，RTLD_NEXT
 * 会从本库之后开始找，命中的才是 libpulse 的实现（而不是我们自己）。
 *
 * RTLD_NEXT 找不到时，必须再退回 dlopen("libpulse.so.0", RTLD_NOLOAD)：
 * QQ 的 libAVSDKPlugin.so / broadcast-core.so 是 dlopen 进来的，libpulse 作为它们
 * 的依赖处在**局部作用域**，全局作用域里根本没有它，RTLD_NEXT 会得到 NULL。
 * 漏掉这一步的后果不是「没生效」而是「悄悄把调用废掉」——2026-10-04 实测：
 * pa_stream_connect_playback 直接返回 -1，QQ 的音频流开不起来，共享 2 秒就断。
 * 上游 src/qq-wl-portal.c 的 real_pa() 有同样的兜底。
 */
static void *real_sym(const char *name)
{
    static void *(*real_dlsym)(void *, const char *);
    void *f;

    if (!real_dlsym) {
        real_dlsym = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym)
            real_dlsym = (void *(*)(void *, const char *))
                dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
    if (!real_dlsym)
        return NULL;

    f = real_dlsym(RTLD_NEXT, name);
    if (!f) {
        /* libpulse 躲在别人 dlopen 出来的局部作用域里。 */
        void *h = dlopen("libpulse.so.0", RTLD_NOW | RTLD_NOLOAD);

        if (h) {
            f = real_dlsym(h, name);
            dlclose(h);
        }
        if (f)
            msg("resolved %s via dlopen(RTLD_NOLOAD) fallback", name);
        else
            msg("cannot resolve %s, call passed through untouched", name);
    }
    return f;
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
    if (!real) {
        msg("subscribe callback NOT installed (no real symbol)");
        return;
    }

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

/* ---------- 2. 默认设备名冻结（QQ 轮询这个接口） ---------- */

#define DEVNAME_MAX 256
static char frozen_sink[DEVNAME_MAX];
static char frozen_src[DEVNAME_MAX];
static pthread_mutex_t frozen_mu = PTHREAD_MUTEX_INITIALIZER;

struct srv_trampoline {
    pa_server_info_cb_t cb;
    void *userdata;
};

static void srv_filter(pa_context *c, const pa_server_info *i, void *data)
{
    struct srv_trampoline *t = data;

    if (i) {
        pa_server_info copy = *i;

        pthread_mutex_lock(&frozen_mu);
        if (i->default_sink_name && *i->default_sink_name) {
            if (!frozen_sink[0]) {
                snprintf(frozen_sink, sizeof(frozen_sink), "%s", i->default_sink_name);
                msg("pin default sink: %s", frozen_sink);
            } else if (strcmp(frozen_sink, i->default_sink_name) != 0) {
                msg("hide default sink change: %s -> %s (QQ keeps seeing %s)",
                    frozen_sink, i->default_sink_name, frozen_sink);
            }
        }
        if (i->default_source_name && *i->default_source_name) {
            if (!frozen_src[0]) {
                snprintf(frozen_src, sizeof(frozen_src), "%s", i->default_source_name);
                msg("pin default source: %s", frozen_src);
            } else if (strcmp(frozen_src, i->default_source_name) != 0) {
                msg("hide default source change: %s -> %s (QQ keeps seeing %s)",
                    frozen_src, i->default_source_name, frozen_src);
            }
        }
        if (frozen_sink[0])
            copy.default_sink_name = frozen_sink;
        if (frozen_src[0])
            copy.default_source_name = frozen_src;
        pthread_mutex_unlock(&frozen_mu);

        t->cb(c, &copy, t->userdata);
    } else {
        t->cb(c, NULL, t->userdata);
    }
    free(t);
}

pa_operation *pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb,
                                         void *userdata)
{
    static pa_operation *(*real)(pa_context *, pa_server_info_cb_t, void *);
    struct srv_trampoline *t;

    if (!real)
        real = real_sym("pa_context_get_server_info");
    if (!real || !enabled())
        return real ? real(c, cb, userdata) : NULL;

    t = malloc(sizeof(*t));
    if (!t)
        return real(c, cb, userdata);
    t->cb = cb;
    t->userdata = userdata;
    return real(c, srv_filter, t);
}

/* ---------- 3. 播放/录音流别绑死在会消失的蓝牙节点名上 ---------- */

static int has_prefix(const char *s, const char *p)
{
    return s && strncmp(s, p, strlen(p)) == 0;
}

int pa_stream_connect_playback(pa_stream *s, const char *dev,
                               const pa_buffer_attr *attr, pa_stream_flags_t flags,
                               const pa_cvolume *volume, pa_stream *sync_stream)
{
    static int (*real)(pa_stream *, const char *, const pa_buffer_attr *,
                       pa_stream_flags_t, const pa_cvolume *, pa_stream *);

    if (!real)
        real = real_sym("pa_stream_connect_playback");
    if (!real) {
        msg("playback stream passed through untouched (no real symbol)");
        return -1;
    }

    if (enabled() && has_prefix(dev, "bluez_output.")) {
        msg("playback stream: %s -> server default (bluetooth node name changes with the profile)",
            dev);
        dev = NULL;
    }
    return real(s, dev, attr, flags, volume, sync_stream);
}

int pa_stream_connect_record(pa_stream *s, const char *dev,
                             const pa_buffer_attr *attr, pa_stream_flags_t flags)
{
    static int (*real)(pa_stream *, const char *, const pa_buffer_attr *,
                       pa_stream_flags_t);

    if (!real)
        real = real_sym("pa_stream_connect_record");
    if (!real) {
        msg("record stream passed through untouched (no real symbol)");
        return -1;
    }

    /*
     * 只改 bluez_input.* / bluez_source.*（麦克风）。bluez_output.*.monitor 是上游
     * 用来做「共享电脑声音」的录音源，不能碰。
     */
    if (enabled() && (has_prefix(dev, "bluez_input.") || has_prefix(dev, "bluez_source."))) {
        msg("record stream: %s -> server default (bluetooth node name changes with the profile)",
            dev);
        dev = NULL;
    }
    return real(s, dev, attr, flags);
}
