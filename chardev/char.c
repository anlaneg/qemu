/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qemu/qemu-print.h"
#include "chardev/char.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-char.h"
#include "qapi/qmp/qerror.h"
#include "sysemu/replay.h"
#include "qemu/help_option.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/id.h"
#include "qemu/coroutine.h"

#include "chardev-internal.h"

/***********************************************************/
/* character device */

//获取chardevs对象，如果不存在，则创建空的chardevs
//在此下，我们存储所有chardevs设备
Object *get_chardevs_root(void)
{
    return container_get(object_get_root(), "/chardevs");
}

static void chr_be_event(Chardev *s, QEMUChrEvent event)
{
    CharBackend *be = s->be;

    if (!be || !be->chr_event) {
        return;
    }

    be->chr_event(be->opaque, event);
}

//处理chr的事件
void qemu_chr_be_event(Chardev *s, QEMUChrEvent event)
{
    /* Keep track if the char device is open */
    switch (event) {
        case CHR_EVENT_OPENED:
            s->be_open = 1;
            break;
        case CHR_EVENT_CLOSED:
            s->be_open = 0;
            break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        /* Ignore */
        break;
    }

    CHARDEV_GET_CLASS(s)->chr_be_event(s, event);
}

/* Not reporting errors from writing to logfile, as logs are
 * defined to be "best effort" only */
//字符设备写向日志文件中写内容
static void qemu_chr_write_log(Chardev *s, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    ssize_t ret;

    if (s->logfd < 0) {
        return;
    }

    while (done < len) {
    retry:
        ret = write(s->logfd, buf + done, len - done);
        if (ret == -1 && errno == EAGAIN) {
        	//需要重试的，sleep 100us后重试
            g_usleep(100);
            goto retry;
        }

        if (ret <= 0) {
            return;
        }
        done += ret;
    }
}

//调用chr_write写入buf中的内容
static int qemu_chr_write_buffer(Chardev *s,
                                 const uint8_t *buf, int len,
                                 int *offset, bool write_all)
{
    ChardevClass *cc = CHARDEV_GET_CLASS(s);
    int res = 0;
    *offset = 0;

    qemu_mutex_lock(&s->chr_write_lock);
    while (*offset < len) {
    retry:
        res = cc->chr_write(s, buf + *offset, len - *offset);
        if (res < 0 && errno == EAGAIN && write_all) {
            if (qemu_in_coroutine()) {
                qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, 100000);
            } else {
                g_usleep(100);
            }
            goto retry;//稍候重试
        }

        if (res <= 0) {
            break;//写失败跳出
        }

        *offset += res;
        if (!write_all) {
            break;//不要求全写，直接跳出
        }
    }
    if (*offset > 0) {
        /*
         * If some data was written by backend, we should
         * only log what was actually written. This method
         * may be invoked again to write the remaining
         * method, thus we'll log the remainder at that time.
         */
    	//记录通过chr_write成功写入的内容
        qemu_chr_write_log(s, buf, *offset);
    } else if (res < 0) {
        /*
         * If a fatal error was reported by the backend,
         * assume this method won't be invoked again with
         * this buffer, so log it all right away.
         */
        qemu_chr_write_log(s, buf, len);
    }
    qemu_mutex_unlock(&s->chr_write_lock);

    return res;//返回写了多少字节
}

//向字符设备中写入len长度的buf,write_all为True时要求全部写入，False时不要求
int qemu_chr_write(Chardev *s, const uint8_t *buf, int len, bool write_all)
{
    int offset = 0;
    int res;

    if (qemu_chr_replay(s) && replay_mode == REPLAY_MODE_PLAY) {
        replay_char_write_event_load(&res, &offset);
        assert(offset <= len);
        qemu_chr_write_buffer(s, buf, offset, &offset, true);
        return res;
    }

    res = qemu_chr_write_buffer(s, buf, len, &offset, write_all);

    if (qemu_chr_replay(s) && replay_mode == REPLAY_MODE_RECORD) {
        replay_char_write_event_save(res, offset);
    }

    if (res < 0) {
        return res;
    }
    return offset;
}

int qemu_chr_be_can_write(Chardev *s)
{
    CharBackend *be = s->be;

    if (!be || !be->chr_can_read) {
        return 0;
    }

    return be->chr_can_read(be->opaque);
}

void qemu_chr_be_write_impl(Chardev *s, uint8_t *buf, int len)
{
    CharBackend *be = s->be;

    if (be && be->chr_read) {
        be->chr_read(be->opaque, buf, len);
    }
}

void qemu_chr_be_write(Chardev *s, uint8_t *buf, int len)
{
    if (qemu_chr_replay(s)) {
        if (replay_mode == REPLAY_MODE_PLAY) {
            return;
        }
        replay_chr_be_write(s, buf, len);
    } else {
        qemu_chr_be_write_impl(s, buf, len);
    }
}

void qemu_chr_be_update_read_handlers(Chardev *s,
                                      GMainContext *context)
{
    ChardevClass *cc = CHARDEV_GET_CLASS(s);

    assert(qemu_chr_has_feature(s, QEMU_CHAR_FEATURE_GCONTEXT)
           || !context);
    s->gcontext = context;
    if (cc->chr_update_read_handler) {
        cc->chr_update_read_handler(s);
    }
}

int qemu_chr_add_client(Chardev *s, int fd)
{
    return CHARDEV_GET_CLASS(s)->chr_add_client ?
        CHARDEV_GET_CLASS(s)->chr_add_client(s, fd) : -1;
}

//字符设备打开
static void qemu_char_open(Chardev *chr, ChardevBackend *backend,
                           bool *be_opened, Error **errp)
{
    ChardevClass *cc = CHARDEV_GET_CLASS(chr);
    /* Any ChardevCommon member would work */
    ChardevCommon *common = backend ? backend->u.null.data : NULL;

    //打开log文件
    if (common && common->has_logfile) {
        int flags = O_WRONLY | O_CREAT;
        if (common->has_logappend &&
            common->logappend) {
            flags |= O_APPEND;
        } else {
            flags |= O_TRUNC;
        }
        chr->logfd = qemu_open_old(common->logfile, flags, 0666);
        if (chr->logfd < 0) {
            error_setg_errno(errp, errno,
                             "Unable to open logfile %s",
                             common->logfile);
            return;
        }
    }

    //打开字符设备
    if (cc->open) {
        cc->open(chr, backend, be_opened, errp);
    }
}

static void char_init(Object *obj)
{
    Chardev *chr = CHARDEV(obj);

    chr->logfd = -1;
    qemu_mutex_init(&chr->chr_write_lock);

    /*
     * Assume if chr_update_read_handler is implemented it will
     * take the updated gcontext into account.
     */
    if (CHARDEV_GET_CLASS(chr)->chr_update_read_handler) {
        qemu_chr_set_feature(chr, QEMU_CHAR_FEATURE_GCONTEXT);
    }

}

static int null_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    return len;
}

static void char_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_write = null_chr_write;//挂载null write
    cc->chr_be_event = chr_be_event;
}

static void char_finalize(Object *obj)
{
    Chardev *chr = CHARDEV(obj);

    if (chr->be) {
        chr->be->chr = NULL;
    }
    g_free(chr->filename);
    g_free(chr->label);
    if (chr->logfd != -1) {
        close(chr->logfd);
    }
    qemu_mutex_destroy(&chr->chr_write_lock);
}

//字符类型信息
static const TypeInfo char_type_info = {
    .name = TYPE_CHARDEV,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(Chardev),
    .instance_init = char_init,
    .instance_finalize = char_finalize,
    .abstract = true,
    .class_size = sizeof(ChardevClass),
    .class_init = char_class_init,
};

static bool qemu_chr_is_busy(Chardev *s)
{
    if (CHARDEV_IS_MUX(s)) {
        MuxChardev *d = MUX_CHARDEV(s);
        return d->mux_cnt >= 0;
    } else {
        return s->be != NULL;
    }
}


//调用字符设备的chr_wait_connected回调
int qemu_chr_wait_connected(Chardev *chr, Error **errp)
{
    ChardevClass *cc = CHARDEV_GET_CLASS(chr);

    if (cc->chr_wait_connected) {
        return cc->chr_wait_connected(chr, errp);
    }

    return 0;
}

QemuOpts *qemu_chr_parse_compat(const char *label, const char *filename,
                                bool permit_mux_mon)
{
    char host[65], port[33], width[8], height[8];
    int pos;
    const char *p;
    QemuOpts *opts;
    Error *local_err = NULL;

    //取chardev上所有选项
    opts = qemu_opts_create(qemu_find_opts("chardev"), label, 1, &local_err);
    if (local_err) {
        error_report_err(local_err);
        return NULL;
    }

    if (strstart(filename, "mon:", &p)) {
        if (!permit_mux_mon) {
            error_report("mon: isn't supported in this context");
            return NULL;
        }
        filename = p;
        qemu_opt_set(opts, "mux", "on", &error_abort);
        if (strcmp(filename, "stdio") == 0) {
            /* Monitor is muxed to stdio: do not exit on Ctrl+C by default
             * but pass it to the guest.  Handle this only for compat syntax,
             * for -chardev syntax we have special option for this.
             * This is what -nographic did, redirecting+muxing serial+monitor
             * to stdio causing Ctrl+C to be passed to guest. */
            qemu_opt_set(opts, "signal", "off", &error_abort);
        }
    }

    if (strcmp(filename, "null")    == 0 ||
        strcmp(filename, "pty")     == 0 ||
        strcmp(filename, "msmouse") == 0 ||
        strcmp(filename, "wctablet") == 0 ||
        strcmp(filename, "braille") == 0 ||
        strcmp(filename, "testdev") == 0 ||
        strcmp(filename, "stdio")   == 0) {
        qemu_opt_set(opts, "backend", filename, &error_abort);
        return opts;
    }
    if (strstart(filename, "vc", &p)) {
        qemu_opt_set(opts, "backend", "vc", &error_abort);
        if (*p == ':') {
            if (sscanf(p+1, "%7[0-9]x%7[0-9]", width, height) == 2) {
                /* pixels */
                qemu_opt_set(opts, "width", width, &error_abort);
                qemu_opt_set(opts, "height", height, &error_abort);
            } else if (sscanf(p+1, "%7[0-9]Cx%7[0-9]C", width, height) == 2) {
                /* chars */
                qemu_opt_set(opts, "cols", width, &error_abort);
                qemu_opt_set(opts, "rows", height, &error_abort);
            } else {
                goto fail;
            }
        }
        return opts;
    }
    if (strcmp(filename, "con:") == 0) {
        qemu_opt_set(opts, "backend", "console", &error_abort);
        return opts;
    }
    if (strstart(filename, "COM", NULL)) {
        qemu_opt_set(opts, "backend", "serial", &error_abort);
        qemu_opt_set(opts, "path", filename, &error_abort);
        return opts;
    }
    if (strstart(filename, "file:", &p)) {
        qemu_opt_set(opts, "backend", "file", &error_abort);
        qemu_opt_set(opts, "path", p, &error_abort);
        return opts;
    }
    if (strstart(filename, "pipe:", &p)) {
        qemu_opt_set(opts, "backend", "pipe", &error_abort);
        qemu_opt_set(opts, "path", p, &error_abort);
        return opts;
    }
    if (strstart(filename, "tcp:", &p) ||
        strstart(filename, "telnet:", &p) ||
        strstart(filename, "tn3270:", &p) ||
        strstart(filename, "websocket:", &p)) {
        if (sscanf(p, "%64[^:]:%32[^,]%n", host, port, &pos) < 2) {
            host[0] = 0;
            if (sscanf(p, ":%32[^,]%n", port, &pos) < 1)
                goto fail;
        }
        qemu_opt_set(opts, "backend", "socket", &error_abort);
        qemu_opt_set(opts, "host", host, &error_abort);
        qemu_opt_set(opts, "port", port, &error_abort);
        if (p[pos] == ',') {
            if (!qemu_opts_do_parse(opts, p + pos + 1, NULL, &local_err)) {
                error_report_err(local_err);
                goto fail;
            }
        }
        if (strstart(filename, "telnet:", &p)) {
            qemu_opt_set(opts, "telnet", "on", &error_abort);
        } else if (strstart(filename, "tn3270:", &p)) {
            qemu_opt_set(opts, "tn3270", "on", &error_abort);
        } else if (strstart(filename, "websocket:", &p)) {
            qemu_opt_set(opts, "websocket", "on", &error_abort);
        }
        return opts;
    }
    if (strstart(filename, "udp:", &p)) {
        qemu_opt_set(opts, "backend", "udp", &error_abort);
        if (sscanf(p, "%64[^:]:%32[^@,]%n", host, port, &pos) < 2) {
            host[0] = 0;
            if (sscanf(p, ":%32[^@,]%n", port, &pos) < 1) {
                goto fail;
            }
        }
        qemu_opt_set(opts, "host", host, &error_abort);
        qemu_opt_set(opts, "port", port, &error_abort);
        if (p[pos] == '@') {
            p += pos + 1;
            if (sscanf(p, "%64[^:]:%32[^,]%n", host, port, &pos) < 2) {
                host[0] = 0;
                if (sscanf(p, ":%32[^,]%n", port, &pos) < 1) {
                    goto fail;
                }
            }
            qemu_opt_set(opts, "localaddr", host, &error_abort);
            qemu_opt_set(opts, "localport", port, &error_abort);
        }
        return opts;
    }
    if (strstart(filename, "unix:", &p)) {
        qemu_opt_set(opts, "backend", "socket", &error_abort);
        if (!qemu_opts_do_parse(opts, p, "path", &local_err)) {
            error_report_err(local_err);
            goto fail;
        }
        return opts;
    }
    if (strstart(filename, "/dev/parport", NULL) ||
        strstart(filename, "/dev/ppi", NULL)) {
        qemu_opt_set(opts, "backend", "parallel", &error_abort);
        qemu_opt_set(opts, "path", filename, &error_abort);
        return opts;
    }
    if (strstart(filename, "/dev/", NULL)) {
        qemu_opt_set(opts, "backend", "serial", &error_abort);
        qemu_opt_set(opts, "path", filename, &error_abort);
        return opts;
    }

    error_report("'%s' is not a valid char driver", filename);

fail:
    qemu_opts_del(opts);
    return NULL;
}

void qemu_chr_parse_common(QemuOpts *opts, ChardevCommon *backend)
{
    const char *logfile = qemu_opt_get(opts, "logfile");

    backend->has_logfile = logfile != NULL;
    backend->logfile = g_strdup(logfile);

    backend->has_logappend = true;
    backend->logappend = qemu_opt_get_bool(opts, "logappend", false);
}

//加载driver对应的ChardevClass
static const ChardevClass *char_get_class(const char *driver, Error **errp)
{
    ObjectClass *oc;
    const ChardevClass *cc;
    //查找名称为chardev-%s的chardev类driver
    char *typename = g_strdup_printf("chardev-%s", driver);

    //获取typename的类元数据
    oc = module_object_class_by_name(typename);
    g_free(typename);

    //oc必须是chardev类型
    if (!object_class_dynamic_cast(oc, TYPE_CHARDEV)) {
        error_setg(errp, "'%s' is not a valid char driver name", driver);
        return NULL;
    }

    //oc不能是抽象类
    if (object_class_is_abstract(oc)) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "driver",
                   "an abstract device type");
        return NULL;
    }

    cc = CHARDEV_CLASS(oc);
    if (cc->internal) {
        error_setg(errp, "'%s' is not a valid char driver name", driver);
        return NULL;
    }

    return cc;
}

static const struct ChardevAlias {
    const char *typename;
    const char *alias;
} chardev_alias_table[] = {
#ifdef HAVE_CHARDEV_PARPORT
    { "parallel", "parport" },
#endif
#ifdef HAVE_CHARDEV_SERIAL
    { "serial", "tty" },
#endif
};

typedef struct ChadevClassFE {
    void (*fn)(const char *name, void *opaque);
    void *opaque;
} ChadevClassFE;

static void
chardev_class_foreach(ObjectClass *klass, void *opaque)
{
    ChadevClassFE *fe = opaque;

    assert(g_str_has_prefix(object_class_get_name(klass), "chardev-"));
    if (CHARDEV_CLASS(klass)->internal) {
        return;
    }

    fe->fn(object_class_get_name(klass) + 8, fe->opaque);
}

static void
chardev_name_foreach(void (*fn)(const char *name, void *opaque), void *opaque)
{
    ChadevClassFE fe = { .fn = fn, .opaque = opaque };
    int i;

    object_class_foreach(chardev_class_foreach, TYPE_CHARDEV, false, &fe);

    for (i = 0; i < (int)ARRAY_SIZE(chardev_alias_table); i++) {
        fn(chardev_alias_table[i].alias, opaque);
    }
}

static void
help_string_append(const char *name, void *opaque)
{
    GString *str = opaque;

    g_string_append_printf(str, "\n  %s", name);
}

//别名检查
static const char *chardev_alias_translate(const char *name)
{
    int i;
    //如果与别名表中元素匹配，则返回别名表中的类型
    for (i = 0; i < (int)ARRAY_SIZE(chardev_alias_table); i++) {
        if (g_strcmp0(chardev_alias_table[i].alias, name) == 0) {
            return chardev_alias_table[i].typename;
        }
    }
    return name;
}

//通过解析参数，完成chardev backend初始化
ChardevBackend *qemu_chr_parse_opts(QemuOpts *opts, Error **errp)
{
    Error *local_err = NULL;
    const ChardevClass *cc;
    ChardevBackend *backend = NULL;
    //支持多种取值，例如'socket'
    const char *name = chardev_alias_translate(qemu_opt_get(opts, "backend"));

    //后端参数未指定，报错
    if (name == NULL) {
        error_setg(errp, "chardev: \"%s\" missing backend",
                   qemu_opts_id(opts));
        return NULL;
    }

    //获取backend对应的元数据
    cc = char_get_class(name, errp);
    if (cc == NULL) {
        return NULL;
    }

    backend = g_new0(ChardevBackend, 1);
    backend->type = CHARDEV_BACKEND_KIND_NULL;

    //如果backend的元数据有parse，则调用parse解析命令行
    //并填充backend
    if (cc->parse) {
        cc->parse(opts, backend, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            qapi_free_ChardevBackend(backend);
            return NULL;
        }
    } else {
    	//如果无parse，则解析公共参数
        ChardevCommon *ccom = g_new0(ChardevCommon, 1);
        qemu_chr_parse_common(opts, ccom);
        backend->u.null.data = ccom; /* Any ChardevCommon member would work */
    }

    return backend;
}

//按opts创建chardev
Chardev *qemu_chr_new_from_opts(QemuOpts *opts, GMainContext *context,
                                Error **errp)
{
    const ChardevClass *cc;
    Chardev *chr = NULL;
    ChardevBackend *backend = NULL;

    //取backend配置类型(例如socket)
    const char *name = chardev_alias_translate(qemu_opt_get(opts, "backend"));
    const char *id = qemu_opts_id(opts);
    char *bid = NULL;

    //收到帮助请求，显示帮助信息
    if (name && is_help_option(name)) {
        GString *str = g_string_new("");

        chardev_name_foreach(help_string_append, str);

        qemu_printf("Available chardev backend types: %s\n", str->str);
        g_string_free(str, true);
        return NULL;
    }

    //如果没有id，则报错
    if (id == NULL) {
        error_setg(errp, "chardev: no id specified");
        return NULL;
    }

    //解析选项,利用选项，生成backend
    backend = qemu_chr_parse_opts(opts, errp);
    if (backend == NULL) {
        return NULL;
    }

    //加载对应驱动(例如当name为socket时，请求的typename则为TYPE_CHARDEV_SOCKET，
    //即char_socket_type_info结构，本函数将返回其对应的类元数据）
    cc = char_get_class(name, errp);
    if (cc == NULL) {
        goto out;
    }

    if (qemu_opt_get_bool(opts, "mux", 0)) {
        bid = g_strdup_printf("%s-base", id);
    }

    //新创建char设备
    chr = qemu_chardev_new(bid ? bid : id,
                           object_class_get_name(OBJECT_CLASS(cc)),
                           backend, context, errp);

    if (chr == NULL) {
        goto out;
    }

    if (bid) {
        Chardev *mux;
        qapi_free_ChardevBackend(backend);
        backend = g_new0(ChardevBackend, 1);
        backend->type = CHARDEV_BACKEND_KIND_MUX;
        backend->u.mux.data = g_new0(ChardevMux, 1);
        backend->u.mux.data->chardev = g_strdup(bid);
        mux = qemu_chardev_new(id, TYPE_CHARDEV_MUX, backend, context, errp);
        if (mux == NULL) {
            object_unparent(OBJECT(chr));
            chr = NULL;
            goto out;
        }
        chr = mux;
    }

out:
    qapi_free_ChardevBackend(backend);
    g_free(bid);
    return chr;
}

Chardev *qemu_chr_new_noreplay(const char *label, const char *filename,
                               bool permit_mux_mon, GMainContext *context)
{
    const char *p;
    Chardev *chr;
    QemuOpts *opts;
    Error *err = NULL;

    if (strstart(filename, "chardev:", &p)) {
        return qemu_chr_find(p);
    }

    opts = qemu_chr_parse_compat(label, filename, permit_mux_mon);
    if (!opts)
        return NULL;

    chr = qemu_chr_new_from_opts(opts, context, &err);
    if (!chr) {
        error_report_err(err);
        goto out;
    }

    if (qemu_opt_get_bool(opts, "mux", 0)) {
        assert(permit_mux_mon);
        monitor_init_hmp(chr, true, &err);
        if (err) {
            error_report_err(err);
            object_unparent(OBJECT(chr));
            chr = NULL;
            goto out;
        }
    }

out:
    qemu_opts_del(opts);
    return chr;
}

static Chardev *qemu_chr_new_permit_mux_mon(const char *label,
                                          const char *filename,
                                          bool permit_mux_mon,
                                          GMainContext *context)
{
    Chardev *chr;
    chr = qemu_chr_new_noreplay(label, filename, permit_mux_mon, context);
    if (chr) {
        if (replay_mode != REPLAY_MODE_NONE) {
            qemu_chr_set_feature(chr, QEMU_CHAR_FEATURE_REPLAY);
        }
        if (qemu_chr_replay(chr) && CHARDEV_GET_CLASS(chr)->chr_ioctl) {
            error_report("Replay: ioctl is not supported "
                         "for serial devices yet");
        }
        replay_register_char_driver(chr);
    }
    return chr;
}

Chardev *qemu_chr_new(const char *label, const char *filename,
                      GMainContext *context)
{
    return qemu_chr_new_permit_mux_mon(label, filename, false, context);
}

Chardev *qemu_chr_new_mux_mon(const char *label, const char *filename,
                              GMainContext *context)
{
    return qemu_chr_new_permit_mux_mon(label, filename, true, context);
}

static int qmp_query_chardev_foreach(Object *obj, void *data)
{
    Chardev *chr = CHARDEV(obj);
    ChardevInfoList **list = data;
    ChardevInfo *value = g_malloc0(sizeof(*value));

    value->label = g_strdup(chr->label);
    value->filename = g_strdup(chr->filename);
    value->frontend_open = chr->be && chr->be->fe_open;

    QAPI_LIST_PREPEND(*list, value);

    return 0;
}

ChardevInfoList *qmp_query_chardev(Error **errp)
{
    ChardevInfoList *chr_list = NULL;

    object_child_foreach(get_chardevs_root(),
                         qmp_query_chardev_foreach, &chr_list);

    return chr_list;
}

static void
qmp_prepend_backend(const char *name, void *opaque)
{
    ChardevBackendInfoList **list = opaque;
    ChardevBackendInfo *value = g_new0(ChardevBackendInfo, 1);

    value->name = g_strdup(name);
    QAPI_LIST_PREPEND(*list, value);
}

ChardevBackendInfoList *qmp_query_chardev_backends(Error **errp)
{
    ChardevBackendInfoList *backend_list = NULL;

    chardev_name_foreach(qmp_prepend_backend, &backend_list);

    return backend_list;
}

//查找是否存在名称为name的字符设备
Chardev *qemu_chr_find(const char *name)
{
	//在字符设备中查找名称为name的设备
    Object *obj = object_resolve_path_component(get_chardevs_root(), name);

    return obj ? CHARDEV(obj) : NULL;
}

QemuOptsList qemu_chardev_opts = {
    .name = "chardev",
    .implied_opt_name = "backend",//首个参数，隐含为backend
    .head = QTAILQ_HEAD_INITIALIZER(qemu_chardev_opts.head),
    .desc = {//定义接受哪些参数
        {
            .name = "backend",
            .type = QEMU_OPT_STRING,
        },{
            .name = "path",
            .type = QEMU_OPT_STRING,
        },{
            .name = "host",
            .type = QEMU_OPT_STRING,
        },{
            .name = "port",
            .type = QEMU_OPT_STRING,
        },{
            .name = "fd",
            .type = QEMU_OPT_STRING,
        },{
            .name = "localaddr",
            .type = QEMU_OPT_STRING,
        },{
            .name = "localport",
            .type = QEMU_OPT_STRING,
        },{
            .name = "to",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "ipv4",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "ipv6",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "wait",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "server",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "delay",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "reconnect",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "telnet",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "tn3270",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "tls-creds",
            .type = QEMU_OPT_STRING,
        },{
            .name = "tls-authz",
            .type = QEMU_OPT_STRING,
        },{
            .name = "websocket",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "width",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "height",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "cols",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "rows",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "mux",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "signal",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "name",
            .type = QEMU_OPT_STRING,
        },{
            .name = "debug",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "size",
            .type = QEMU_OPT_SIZE,
        },{
            .name = "chardev",
            .type = QEMU_OPT_STRING,
        },{
            .name = "append",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "logfile",
            .type = QEMU_OPT_STRING,
        },{
            .name = "logappend",
            .type = QEMU_OPT_BOOL,
#ifdef CONFIG_LINUX
        },{
            .name = "tight",
            .type = QEMU_OPT_BOOL,
            .def_value_str = "on",
        },{
            .name = "abstract",
            .type = QEMU_OPT_BOOL,
#endif
        },
        { /* end of list */ }
    },
};

//检查字符设备是否存在指定功能
bool qemu_chr_has_feature(Chardev *chr,
                          ChardevFeature feature)
{
    return test_bit(feature, chr->features);
}

//设置字符设备支持指定功能
void qemu_chr_set_feature(Chardev *chr,
                           ChardevFeature feature)
{
    return set_bit(feature, chr->features);
}

//创建指定类型的chardev并打开
static Chardev *chardev_new(const char *id, const char *typename,
                            ChardevBackend *backend,
                            GMainContext *gcontext,
                            Error **errp)
{
    Object *obj;
    Chardev *chr = NULL;
    Error *local_err = NULL;
    bool be_opened = true;

    assert(g_str_has_prefix(typename, "chardev-"));

    //生成对象（例如TYPE_CHARDEV_SOCKET）
    obj = object_new(typename);
    chr = CHARDEV(obj);//强转为字符设备
    chr->label = g_strdup(id);//设置字符名称
    chr->gcontext = gcontext;

    //打开char设备
    qemu_char_open(chr, backend, &be_opened, &local_err);
    if (local_err) {
        goto end;
    }

    if (!chr->filename) {
    	//跳过'chardev-'
        chr->filename = g_strdup(typename + 8);
    }
    if (be_opened) {
    		//触发字符设备被打开事件
        qemu_chr_be_event(chr, CHR_EVENT_OPENED);
    }

    if (id) {
	//在chardevs中添加此chardev设备
        object_property_try_add_child(get_chardevs_root(), id, obj,
                                      &local_err);
        if (local_err) {
            goto end;
        }
        object_unref(obj);
    }

end:
    if (local_err) {
        error_propagate(errp, local_err);
        object_unref(obj);
        return NULL;
    }

    return chr;
}

Chardev *qemu_chardev_new(const char *id, const char *typename,
                          ChardevBackend *backend,
                          GMainContext *gcontext,
                          Error **errp)
{
    g_autofree char *genid = NULL;

    if (!id) {
        genid = id_generate(ID_CHR);
        id = genid;
    }

    return chardev_new(id, typename, backend, gcontext, errp);
}

ChardevReturn *qmp_chardev_add(const char *id, ChardevBackend *backend,
                               Error **errp)
{
    const ChardevClass *cc;
    ChardevReturn *ret;
    Chardev *chr;

    cc = char_get_class(ChardevBackendKind_str(backend->type), errp);
    if (!cc) {
        return NULL;
    }

    chr = chardev_new(id, object_class_get_name(OBJECT_CLASS(cc)),
                      backend, NULL, errp);
    if (!chr) {
        return NULL;
    }

    ret = g_new0(ChardevReturn, 1);
    if (CHARDEV_IS_PTY(chr)) {
        ret->pty = g_strdup(chr->filename + 4);
        ret->has_pty = true;
    }

    return ret;
}

ChardevReturn *qmp_chardev_change(const char *id, ChardevBackend *backend,
                                  Error **errp)
{
    CharBackend *be;
    const ChardevClass *cc;
    Chardev *chr, *chr_new;
    bool closed_sent = false;
    ChardevReturn *ret;

    chr = qemu_chr_find(id);
    if (!chr) {
        error_setg(errp, "Chardev '%s' does not exist", id);
        return NULL;
    }

    //不支持mux
    if (CHARDEV_IS_MUX(chr)) {
        error_setg(errp, "Mux device hotswap not supported yet");
        return NULL;
    }

    //不能为replay模式
    if (qemu_chr_replay(chr)) {
        error_setg(errp,
            "Chardev '%s' cannot be changed in record/replay mode", id);
        return NULL;
    }

    be = chr->be;
    if (!be) {
        /* easy case */
        object_unparent(OBJECT(chr));
        return qmp_chardev_add(id, backend, errp);
    }

    if (!be->chr_be_change) {
        error_setg(errp, "Chardev user does not support chardev hotswap");
        return NULL;
    }

    cc = char_get_class(ChardevBackendKind_str(backend->type), errp);
    if (!cc) {
        return NULL;
    }

    chr_new = chardev_new(NULL, object_class_get_name(OBJECT_CLASS(cc)),
                          backend, chr->gcontext, errp);
    if (!chr_new) {
        return NULL;
    }
    chr_new->label = g_strdup(id);

    if (chr->be_open && !chr_new->be_open) {
        qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
        closed_sent = true;
    }

    chr->be = NULL;
    qemu_chr_fe_init(be, chr_new, &error_abort);

    if (be->chr_be_change(be->opaque) < 0) {
        error_setg(errp, "Chardev '%s' change failed", chr_new->label);
        chr_new->be = NULL;
        qemu_chr_fe_init(be, chr, &error_abort);
        if (closed_sent) {
            qemu_chr_be_event(chr, CHR_EVENT_OPENED);
        }
        object_unref(OBJECT(chr_new));
        return NULL;
    }

    object_unparent(OBJECT(chr));
    object_property_add_child(get_chardevs_root(), chr_new->label,
                              OBJECT(chr_new));
    object_unref(OBJECT(chr_new));

    ret = g_new0(ChardevReturn, 1);
    if (CHARDEV_IS_PTY(chr_new)) {
        ret->pty = g_strdup(chr_new->filename + 4);
        ret->has_pty = true;
    }

    return ret;
}

void qmp_chardev_remove(const char *id, Error **errp)
{
    Chardev *chr;

    chr = qemu_chr_find(id);
    if (chr == NULL) {
        error_setg(errp, "Chardev '%s' not found", id);
        return;
    }
    if (qemu_chr_is_busy(chr)) {
        error_setg(errp, "Chardev '%s' is busy", id);
        return;
    }
    if (qemu_chr_replay(chr)) {
        error_setg(errp,
            "Chardev '%s' cannot be unplugged in record/replay mode", id);
        return;
    }
    object_unparent(OBJECT(chr));
}

void qmp_chardev_send_break(const char *id, Error **errp)
{
    Chardev *chr;

    chr = qemu_chr_find(id);
    if (chr == NULL) {
        error_setg(errp, "Chardev '%s' not found", id);
        return;
    }
    qemu_chr_be_event(chr, CHR_EVENT_BREAK);
}

/*
 * Add a timeout callback for the chardev (in milliseconds), return
 * the GSource object created. Please use this to add timeout hook for
 * chardev instead of g_timeout_add() and g_timeout_add_seconds(), to
 * make sure the gcontext that the task bound to is correct.
 */
GSource *qemu_chr_timeout_add_ms(Chardev *chr, guint ms,
                                 GSourceFunc func, void *private)
{
	//创建一个timeout类型的定时器（事件源）
    GSource *source = g_timeout_source_new(ms);

    assert(func);
    //设置定时器回调及其回调参数
    g_source_set_callback(source, func, private, NULL);
    g_source_attach(source, chr->gcontext);

    return source;
}

void qemu_chr_cleanup(void)
{
    object_unparent(get_chardevs_root());
}

static void register_types(void)
{
    type_register_static(&char_type_info);
}

type_init(register_types);
