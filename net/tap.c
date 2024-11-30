/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2009 Red Hat, Inc.
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
#include "tap_int.h"


#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <net/if.h>

#include "net/eth.h"
#include "net/net.h"
#include "clients.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"

#include "net/tap.h"

#include "net/vhost_net.h"

//用于描述tap设备
typedef struct TAPState {
    NetClientState nc;
    //tap口设备对应的fd
    int fd;
    //用于定义tap设备被移除时，需要调用的脚本及参数
    char down_script[1024];
    char down_script_arg[128];
    //提供自tap设备读取报文时的buffer
    uint8_t buf[NET_BUFSIZE];
    //是否需要进行read poll检测
    bool read_poll;
    //是否需要进行write poll检测
    bool write_poll;
    /*是否使用vnet_hdr*/
    bool using_vnet_hdr;
    /*是否支持ufo*/
    bool has_ufo;
    bool has_uso;
    /*此tap设备是否被使能*/
    bool enabled;
    VHostNetState *vhost_net;
    //指明vnet header长度
    unsigned host_vnet_hdr_len;
    Notifier exit;
} TAPState;

static void launch_script(const char *setup_script, const char *ifname,
                          int fd, Error **errp);

static void tap_send(void *opaque);
static void tap_writable(void *opaque);

//将fd注册到aio框架中
static void tap_update_fd_handler(TAPState *s)
{
    qemu_set_fd_handler(s->fd,
                        /*设备使能，且s->read_poll为true，则注册读函数*/
                        s->read_poll && s->enabled ? tap_send : NULL,
                        s->write_poll && s->enabled ? tap_writable : NULL,
                        s);
}

static void tap_read_poll(TAPState *s, bool enable)
{
    s->read_poll = enable;
    tap_update_fd_handler(s);
}

static void tap_write_poll(TAPState *s, bool enable)
{
    s->write_poll = enable;
    tap_update_fd_handler(s);
}

static void tap_writable(void *opaque)
{
    TAPState *s = opaque;

    tap_write_poll(s, false);

    //完成tap收到的incoming报文的投递
    qemu_flush_queued_packets(&s->nc);
}

//向tap设备发送iov格式的buffer
static ssize_t tap_write_packet(TAPState *s, const struct iovec *iov, int iovcnt)
{
    ssize_t len;

    //向tap口发送报文
    len = RETRY_ON_EINTR(writev(s->fd, iov, iovcnt));

    //发送出错，且出错原因为需要again时，返回0
    if (len == -1 && errno == EAGAIN) {
        tap_write_poll(s, true);
        return 0;
    }

    /*返回发送的长度*/
    return len;
}

static ssize_t tap_receive_iov(NetClientState *nc, const struct iovec *iov,
                               int iovcnt)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    const struct iovec *iovp = iov;
    g_autofree struct iovec *iov_copy = NULL;
    struct virtio_net_hdr hdr = { };

    //构造空的hdr_mrg内容，并自tap口发出
    if (s->host_vnet_hdr_len && !s->using_vnet_hdr) {
        iov_copy = g_new(struct iovec, iovcnt + 1);
        iov_copy[0].iov_base = &hdr;
        iov_copy[0].iov_len =  s->host_vnet_hdr_len;
        memcpy(&iov_copy[1], iov, iovcnt * sizeof(*iov));
        iovp = iov_copy;
        iovcnt++;
    }

    return tap_write_packet(s, iovp, iovcnt);
}

static ssize_t tap_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    struct iovec iov = {
        .iov_base = (void *)buf,
        .iov_len = size
    };

    return tap_receive_iov(nc, &iov, 1);
}

#ifndef __sun__
ssize_t tap_read_packet(int tapfd, uint8_t *buf, int maxlen)
{
    return read(tapfd, buf, maxlen);
}
#endif

static void tap_send_completed(NetClientState *nc, ssize_t len)
{
    //之前发送失败，将read_pool改为false,现在异步处理完成，改正为true
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    tap_read_poll(s, true);
}

//自tap设备中异步读取报文并发送给对端
static void tap_send(void *opaque)
{
    TAPState *s = opaque;
    int size;
    int packets = 0;

    while (true) {
        uint8_t *buf = s->buf;
        uint8_t min_pkt[ETH_ZLEN];
        size_t min_pktsz = sizeof(min_pkt);

        //自tap口中读取报文,存入s->buf中
        size = tap_read_packet(s->fd, s->buf, sizeof(s->buf));
        if (size <= 0) {
            break;
        }

        //跳过vnet_hdr
        if (s->host_vnet_hdr_len && !s->using_vnet_hdr) {
            buf  += s->host_vnet_hdr_len;
            size -= s->host_vnet_hdr_len;
        }

        if (net_peer_needs_padding(&s->nc)) {
            if (eth_pad_short_frame(min_pkt, &min_pktsz, buf, size)) {
                buf = min_pkt;
                size = min_pktsz;
            }
        }

        //异步完成向s->nc对端报文投递（需要过filter)
        size = qemu_send_packet_async(&s->nc, buf, size, tap_send_completed);
        if (size == 0) {
            //采用异步发送方式，这里关闭read_poll,等待tap_send_completed完成后再调用
            tap_read_poll(s, false);
            break;
        } else if (size < 0) {
            break;
        }

        /*
         * When the host keeps receiving more packets while tap_send() is
         * running we can hog the BQL.  Limit the number of
         * packets that are processed per tap_send() callback to prevent
         * stalling the guest.
         */
        packets++;
        if (packets >= 50) {
            break;
        }
    }
}

static bool tap_has_ufo(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    return s->has_ufo;
}

static bool tap_has_uso(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    return s->has_uso;
}

static bool tap_has_vnet_hdr(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    return !!s->host_vnet_hdr_len;
}

static bool tap_has_vnet_hdr_len(NetClientState *nc, int len)
{
    return tap_has_vnet_hdr(nc);
}

static void tap_set_vnet_hdr_len(NetClientState *nc, int len)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    tap_fd_set_vnet_hdr_len(s->fd, len);
    s->host_vnet_hdr_len = len;
    s->using_vnet_hdr = true;
}

static int tap_set_vnet_le(NetClientState *nc, bool is_le)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    return tap_fd_set_vnet_le(s->fd, is_le);
}

static int tap_set_vnet_be(NetClientState *nc, bool is_be)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    return tap_fd_set_vnet_be(s->fd, is_be);
}

//使tap打开相应的offload功能
static void tap_set_offload(NetClientState *nc, int csum, int tso4,
                     int tso6, int ecn, int ufo, int uso4, int uso6)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    if (s->fd < 0) {
        return;
    }

    tap_fd_set_offload(s->fd, csum, tso4, tso6, ecn, ufo, uso4, uso6);
}

//如果配置了down_script,则触发down_script脚本调用
static void tap_exit_notify(Notifier *notifier, void *data)
{
    TAPState *s = container_of(notifier, TAPState, exit);
    Error *err = NULL;

    if (s->down_script[0]) {
        launch_script(s->down_script, s->down_script_arg, s->fd, &err);
        if (err) {
            error_report_err(err);
        }
    }
}

//执行TAP设备的移除时的清理工作
static void tap_cleanup(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);

    if (s->vhost_net) {
        vhost_net_cleanup(s->vhost_net);
        g_free(s->vhost_net);
        s->vhost_net = NULL;
    }

    qemu_purge_queued_packets(nc);

    tap_exit_notify(&s->exit, NULL);
    qemu_remove_exit_notifier(&s->exit);

    //停止tap口的读写
    tap_read_poll(s, false);
    tap_write_poll(s, false);
    //关闭fd
    close(s->fd);
    s->fd = -1;
}

static void tap_poll(NetClientState *nc, bool enable)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    tap_read_poll(s, enable);
    tap_write_poll(s, enable);
}

static bool tap_set_steering_ebpf(NetClientState *nc, int prog_fd)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);

    return tap_fd_set_steering_ebpf(s->fd, prog_fd) == 0;
}

/*取tap口对应的fd*/
int tap_get_fd(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);
    return s->fd;
}

/* fd support */

static NetClientInfo net_tap_info = {
    .type = NET_CLIENT_DRIVER_TAP,
    .size = sizeof(TAPState),
    //将收到的报文自tap发出
    .receive = tap_receive,
    //iov格式的buffer自tap发出
    .receive_iov = tap_receive_iov,
    .poll = tap_poll,
    //指明tap类设备在nc移除时，需要执行清理工作
    .cleanup = tap_cleanup,
    .has_ufo = tap_has_ufo,
    .has_uso = tap_has_uso,
    .has_vnet_hdr = tap_has_vnet_hdr,
    .has_vnet_hdr_len = tap_has_vnet_hdr_len,
    .set_offload = tap_set_offload,
    .set_vnet_hdr_len = tap_set_vnet_hdr_len,
    .set_vnet_le = tap_set_vnet_le,
    .set_vnet_be = tap_set_vnet_be,
    .set_steering_ebpf = tap_set_steering_ebpf,
};

//利用fd创建TAPstate
static TAPState *net_tap_fd_init(NetClientState *peer,
                                 const char *model,
                                 const char *name,
                                 int fd/*tap设备fd*/,
                                 int vnet_hdr/*是否包含vnet_hdr结构*/)
{
    NetClientState *nc;
    TAPState *s;

    //申请TAPState,并先初始化其成员nc
    nc = qemu_new_net_client(&net_tap_info/*指为tap类info*/, peer, model, name);

    s = DO_UPCAST(TAPState, nc, nc);

    /*设置tap对应的fd*/
    s->fd = fd;
    s->host_vnet_hdr_len = vnet_hdr ? sizeof(struct virtio_net_hdr) : 0;
    s->using_vnet_hdr = false;
    s->has_ufo = tap_probe_has_ufo(s->fd);
    s->has_uso = tap_probe_has_uso(s->fd);
    s->enabled = true;
    /*默认关闭offload功能*/
    tap_set_offload(&s->nc, 0, 0, 0, 0, 0, 0, 0);
    /*
     * Make sure host header length is set correctly in tap:
     * it might have been modified by another instance of qemu.
     */
    if (vnet_hdr) {
        //更改tap口的vnet_hdr
        tap_fd_set_vnet_hdr_len(s->fd, s->host_vnet_hdr_len);
    }
    //指明容许自tap口读取报文
    tap_read_poll(s, true);
    s->vhost_net = NULL;

    s->exit.notify = tap_exit_notify;
    qemu_add_exit_notifier(&s->exit);

    return s;
}

static void close_all_fds_after_fork(int excluded_fd)
{
    const int skip_fd[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO,
                           excluded_fd};
    unsigned int nskip = ARRAY_SIZE(skip_fd);

    /*
     * skip_fd must be an ordered array of distinct fds, exclude
     * excluded_fd if already included in the [STDIN_FILENO - STDERR_FILENO]
     * range
     */
    if (excluded_fd <= STDERR_FILENO) {
        nskip--;
    }

    qemu_close_all_open_fd(skip_fd, nskip);
}

static void launch_script(const char *setup_script, const char *ifname,
                          int fd, Error **errp)
{
    int pid, status;
    char *args[3];
    char **parg;

    /* try to launch network script */
    pid = fork();
    if (pid < 0) {
        error_setg_errno(errp, errno, "could not launch network script %s",
                         setup_script);
        return;
    }
    if (pid == 0) {
        close_all_fds_after_fork(fd);
        parg = args;
        *parg++ = (char *)setup_script;
        *parg++ = (char *)ifname;
        *parg = NULL;
        //执行脚本
        execv(setup_script, args);
        _exit(1);
    } else {
        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return;
        }
        error_setg(errp, "network script %s failed with status %d",
                   setup_script, status);
    }
}

static int recv_fd(int c)
{
    int fd;
    uint8_t msgbuf[CMSG_SPACE(sizeof(fd))];
    struct msghdr msg = {
        .msg_control = msgbuf,
        .msg_controllen = sizeof(msgbuf),
    };
    struct cmsghdr *cmsg;
    struct iovec iov;
    uint8_t req[1];
    ssize_t len;

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    msg.msg_controllen = cmsg->cmsg_len;

    iov.iov_base = req;
    iov.iov_len = sizeof(req);

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    len = recvmsg(c, &msg, 0);
    if (len > 0) {
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
        return fd;
    }

    return len;
}

static int net_bridge_run_helper(const char *helper, const char *bridge,
                                 Error **errp)
{
    sigset_t oldmask, mask;
    g_autofree char *default_helper = NULL;
    int pid, status;
    char *args[5];
    char **parg;
    int sv[2];

    //清空信号集
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    if (!helper) {
        helper = default_helper = get_relocated_path(DEFAULT_BRIDGE_HELPER);
    }

    //创建unix socket的pair
    if (socketpair(PF_UNIX, SOCK_STREAM, 0, sv) == -1) {
        error_setg_errno(errp, errno, "socketpair() failed");
        return -1;
    }

    /* try to launch bridge helper */
    pid = fork();
    if (pid < 0) {
        error_setg_errno(errp, errno, "Can't fork bridge helper");
        return -1;
    }
    if (pid == 0) {
        //子进程
        char *fd_buf = NULL;
        char *br_buf = NULL;
        char *helper_cmd = NULL;

        //构造fd参数
        close_all_fds_after_fork(sv[1]);
        fd_buf = g_strdup_printf("%s%d", "--fd=", sv[1]);

        if (strrchr(helper, ' ') || strrchr(helper, '\t')) {
            /* assume helper is a command */

            //构造桥名称参数
            if (strstr(helper, "--br=") == NULL) {
                br_buf = g_strdup_printf("%s%s", "--br=", bridge);
            }

            helper_cmd = g_strdup_printf("%s %s %s %s", helper,
                            "--use-vnet", fd_buf, br_buf ? br_buf : "");

            parg = args;
            *parg++ = (char *)"sh";
            *parg++ = (char *)"-c";
            *parg++ = helper_cmd;
            *parg++ = NULL;

            //执行helper命令
            execv("/bin/sh", args);
            g_free(helper_cmd);
        } else {
            /* assume helper is just the executable path name */

            br_buf = g_strdup_printf("%s%s", "--br=", bridge);

            parg = args;
            *parg++ = (char *)helper;
            *parg++ = (char *)"--use-vnet";
            *parg++ = fd_buf;
            *parg++ = br_buf;
            *parg++ = NULL;

            execv(helper, args);
        }
        g_free(fd_buf);
        g_free(br_buf);
        _exit(1);

    } else {
        //父进程自pair中读取
        int fd;
        int saved_errno;

        close(sv[1]);

        //获取helper打开的fd
        fd = RETRY_ON_EINTR(recv_fd(sv[0]));
        saved_errno = errno;

        close(sv[0]);

        //等待子进程退出
        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }
        //还原信号mask
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        if (fd < 0) {
            error_setg_errno(errp, saved_errno,
                             "failed to recv file descriptor");
            return -1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            error_setg(errp, "bridge helper failed");
            return -1;
        }
        //返回helper产生的fd
        return fd;
    }
}

int net_init_bridge(const Netdev *netdev, const char *name,
                    NetClientState *peer, Error **errp)
{
    const NetdevBridgeOptions *bridge;
    const char *helper, *br;
    TAPState *s;
    int fd, vnet_hdr;

    assert(netdev->type == NET_CLIENT_DRIVER_BRIDGE);
    bridge = &netdev->u.bridge;
    //桥的helper命令行
    helper = bridge->helper;
    br     = bridge->br ?: DEFAULT_BRIDGE_INTERFACE;

    //运行bridge的命令，创建tap口，并加入到桥$br中，返回tap对应的fd,准备加入事件
    fd = net_bridge_run_helper(helper, br, errp);
    if (fd == -1) {
        return -1;
    }

    if (!g_unix_set_fd_nonblocking(fd, true, NULL)) {
        error_setg_errno(errp, errno, "Failed to set FD nonblocking");
        return -1;
    }
    vnet_hdr = tap_probe_vnet_hdr(fd, errp);
    if (vnet_hdr < 0) {
        close(fd);
        return -1;
    }
    s = net_tap_fd_init(peer, "bridge", name, fd, vnet_hdr);

    qemu_set_info_str(&s->nc, "helper=%s,br=%s", helper, br);

    return 0;
}

//打开一个tap设备，并运行指定的脚本
static int net_tap_init(const NetdevTapOptions *tap, int *vnet_hdr,
                        const char *setup_script/*启动脚本*/, char *ifname/*接口名称*/,
                        size_t ifname_sz/*接口名称长度*/, int mq_required, Error **errp)
{
    Error *err = NULL;
    int fd, vnet_hdr_required;

    if (tap->has_vnet_hdr) {
        *vnet_hdr = tap->vnet_hdr;
        vnet_hdr_required = *vnet_hdr;
    } else {
        *vnet_hdr = 1;
        vnet_hdr_required = 0;
    }

    //打开tap设备
    fd = RETRY_ON_EINTR(tap_open(ifname, ifname_sz, vnet_hdr, vnet_hdr_required,
                      mq_required, errp));
    if (fd < 0) {
        return -1;
    }

    //如有必要，运行setup脚本
    if (setup_script &&
        setup_script[0] != '\0' &&
        strcmp(setup_script, "no") != 0) {
        //装载运行脚本
        launch_script(setup_script, ifname, fd, &err);
        if (err) {
            error_propagate(errp, err);
            close(fd);
            return -1;
        }
    }

    return fd;
}

#define MAX_TAP_QUEUES 1024

static void net_init_tap_one(const NetdevTapOptions *tap, NetClientState *peer/*tap的对端nc*/,
                             const char *model, const char *name,
                             const char *ifname/*接口名称*/, const char *script/*tap设备up时需要执行的脚本*/,
                             const char *downscript/*设备移除时需执行的脚本*/, const char *vhostfdname/*vhost设备对应的fd名称*/,
                             int vnet_hdr, int fd, Error **errp)
{
    Error *err = NULL;
    TAPState *s = net_tap_fd_init(peer, model, name, fd, vnet_hdr);
    int vhostfd;

    //设置发送缓冲大小
    tap_set_sndbuf(s->fd, tap, &err);
    if (err) {
        error_propagate(errp, err);
        goto failed;
    }

    //填充info_str
    if (tap->fd || tap->fds) {
        qemu_set_info_str(&s->nc, "fd=%d", fd);
    } else if (tap->helper) {
        qemu_set_info_str(&s->nc, "helper=%s", tap->helper);
    } else {
        /*设备移除时需执行脚本，参数为ifname*/
        qemu_set_info_str(&s->nc, "ifname=%s,script=%s,downscript=%s", ifname,
                          script, downscript);

        if (strcmp(downscript, "no") != 0) {
            snprintf(s->down_script, sizeof(s->down_script), "%s", downscript);
            snprintf(s->down_script_arg, sizeof(s->down_script_arg),
                     "%s", ifname);
        }
    }

    if (tap->has_vhost ? tap->vhost :
        vhostfdname || (tap->has_vhostforce && tap->vhostforce)) {
        VhostNetOptions options;

        /*使用vhost-net做为后端*/
        options.backend_type = VHOST_BACKEND_TYPE_KERNEL;
        options.net_backend = &s->nc;
        if (tap->has_poll_us) {
            options.busyloop_timeout = tap->poll_us;
        } else {
            options.busyloop_timeout = 0;
        }

        if (vhostfdname) {
            /*通过vhostfd字符串，获得vhostfd*/
            vhostfd = monitor_fd_param(monitor_cur(), vhostfdname, &err);
            if (vhostfd == -1) {
                error_propagate(errp, err);
                goto failed;
            }
            if (!g_unix_set_fd_nonblocking(vhostfd, true, NULL)) {
                error_setg_errno(errp, errno, "%s: Can't use file descriptor %d",
                                 name, fd);
                goto failed;
            }
        } else {
            /*打开vhost-net字符设备，获得对应的fd*/
            vhostfd = open("/dev/vhost-net", O_RDWR);
            if (vhostfd < 0) {
                error_setg_errno(errp, errno,
                                 "tap: open vhost char device failed");
                goto failed;
            }
            if (!g_unix_set_fd_nonblocking(vhostfd, true, NULL)) {
                error_setg_errno(errp, errno, "Failed to set FD nonblocking");
                goto failed;
            }
        }
        options.opaque = (void *)(uintptr_t)vhostfd;
        options.nvqs = 2;

        /*初始化vhost-net*/
        s->vhost_net = vhost_net_init(&options);
        if (!s->vhost_net) {
            error_setg(errp,
                       "vhost-net requested but could not be initialized");
            goto failed;
        }
    } else if (vhostfdname) {
        error_setg(errp, "vhostfd(s)= is not valid without vhost");
        goto failed;
    }

    return;

failed:
    qemu_del_net_client(&s->nc);
}

static int get_fds(char *str, char *fds[], int max)
{
    char *ptr = str, *this;
    size_t len = strlen(str);
    int i = 0;

    //将ptr按':'划开，将每个元素存入fds中，并返回
    while (i < max && ptr < str + len) {
        this = strchr(ptr, ':');

        if (this == NULL) {
            fds[i] = g_strdup(ptr);
        } else {
            fds[i] = g_strndup(ptr, this - ptr);
        }

        i++;
        if (this == NULL) {
            break;
        } else {
            ptr = this + 1;
        }
    }

    return i;
}

//创建tap类net client
int net_init_tap(const Netdev *netdev, const char *name/*net client唯一标识*/,
                 NetClientState *peer, Error **errp)
{
    const NetdevTapOptions *tap;
    int fd, vnet_hdr = 0, i = 0, queues;
    /* for the no-fd, no-helper case */
    const char *script;
    const char *downscript;
    Error *err = NULL;
    const char *vhostfdname;
    char ifname[128];
    int ret = 0;

    //此时由于创建的tap类net client,故netdev类型为tap
    assert(netdev->type == NET_CLIENT_DRIVER_TAP);
    tap = &netdev->u.tap;
    queues = tap->has_queues ? tap->queues : 1;
    vhostfdname = tap->vhostfd;
    script = tap->script;
    downscript = tap->downscript;

    /* QEMU hubs do not support multiqueue tap, in this case peer is set.
     * For -netdev, peer is always NULL. */
    if (peer && (tap->has_queues || tap->fds || tap->vhostfds)) {
        error_setg(errp, "Multiqueue tap cannot be used with hubs");
        return -1;
    }

    if (tap->fd) {
        /*如果指供了fd,则以下参数不得提供*/
        if (tap->ifname || tap->script || tap->downscript ||
            tap->has_vnet_hdr || tap->helper || tap->has_queues ||
            tap->fds || tap->vhostfds) {
            error_setg(errp, "ifname=, script=, downscript=, vnet_hdr=, "
                       "helper=, queues=, fds=, and vhostfds= "
                       "are invalid with fd=");
            return -1;
        }

        //获得tap->fd指定的tap设备fd描述符
        fd = monitor_fd_param(monitor_cur(), tap->fd, errp);
        if (fd == -1) {
            return -1;
        }

        if (!g_unix_set_fd_nonblocking(fd, true, NULL)) {
            error_setg_errno(errp, errno, "%s: Can't use file descriptor %d",
                             name, fd);
            close(fd);
            return -1;
        }

        vnet_hdr = tap_probe_vnet_hdr(fd, errp);
        if (vnet_hdr < 0) {
            close(fd);
            return -1;
        }

        //通过以上参数创始TapState
        net_init_tap_one(tap, peer, "tap", name, NULL,
                         script, downscript,
                         vhostfdname, vnet_hdr/*是否使能了vnet_hdr*/, fd/*tap设备fd*/, &err);
        if (err) {
            error_propagate(errp, err);
            close(fd);
            return -1;
        }
    } else if (tap->fds) {
        //如果提供了fds，多队列情况
        char **fds;
        char **vhost_fds;
        int nfds = 0, nvhosts = 0;

        //与以下参数相冲突
        if (tap->ifname || tap->script || tap->downscript ||
            tap->has_vnet_hdr || tap->helper || tap->has_queues ||
            tap->vhostfd) {
            error_setg(errp, "ifname=, script=, downscript=, vnet_hdr=, "
                       "helper=, queues=, and vhostfd= "
                       "are invalid with fds=");
            return -1;
        }

        fds = g_new0(char *, MAX_TAP_QUEUES);
        vhost_fds = g_new0(char *, MAX_TAP_QUEUES);

        //分隔tap->fds中配置了多少个fd,存入到fds中
        nfds = get_fds(tap->fds, fds, MAX_TAP_QUEUES);
        if (tap->vhostfds) {
            //如果还配置了vhostfds,则split并存入vhost_fds中
            nvhosts = get_fds(tap->vhostfds, vhost_fds, MAX_TAP_QUEUES);
            if (nfds != nvhosts) {
                error_setg(errp, "The number of fds passed does not match "
                           "the number of vhostfds passed");
                ret = -1;
                goto free_fail;
            }
        }

        //针对每个nfds,创建一个TAPState
        for (i = 0; i < nfds; i++) {
            fd = monitor_fd_param(monitor_cur(), fds[i], errp);
            if (fd == -1) {
                ret = -1;
                goto free_fail;
            }

            ret = g_unix_set_fd_nonblocking(fd, true, NULL);
            if (!ret) {
                error_setg_errno(errp, errno, "%s: Can't use file descriptor %d",
                                 name, fd);
                goto free_fail;
            }

            if (i == 0) {
                vnet_hdr = tap_probe_vnet_hdr(fd, errp);
                if (vnet_hdr < 0) {
                    ret = -1;
                    goto free_fail;
                }
            } else if (vnet_hdr != tap_probe_vnet_hdr(fd, NULL)) {
                error_setg(errp,
                           "vnet_hdr not consistent across given tap fds");
                ret = -1;
                goto free_fail;
            }

            //创建此TAPState
            net_init_tap_one(tap, peer, "tap", name, ifname,
                             script, downscript,
                             tap->vhostfds ? vhost_fds[i] : NULL/*传入此tap对应的vhostfd*/,
                             vnet_hdr, fd, &err);
            if (err) {
                error_propagate(errp, err);
                ret = -1;
                goto free_fail;
            }
        }

free_fail:
        for (i = 0; i < nvhosts; i++) {
            g_free(vhost_fds[i]);
        }
        for (i = 0; i < nfds; i++) {
            g_free(fds[i]);
        }
        g_free(fds);
        g_free(vhost_fds);
        return ret;
    } else if (tap->helper) {
        /*如果提供了helper,则以下参数与之冲突*/
        if (tap->ifname || tap->script || tap->downscript ||
            tap->has_vnet_hdr || tap->has_queues || tap->vhostfds) {
            error_setg(errp, "ifname=, script=, downscript=, vnet_hdr=, "
                       "queues=, and vhostfds= are invalid with helper=");
            return -1;
        }

        fd = net_bridge_run_helper(tap->helper,
                                   tap->br ?: DEFAULT_BRIDGE_INTERFACE,
                                   errp);
        if (fd == -1) {
            return -1;
        }

        if (!g_unix_set_fd_nonblocking(fd, true, NULL)) {
            error_setg_errno(errp, errno, "Failed to set FD nonblocking");
            return -1;
        }
        vnet_hdr = tap_probe_vnet_hdr(fd, errp);
        if (vnet_hdr < 0) {
            close(fd);
            return -1;
        }

        /*创建TAPState*/
        net_init_tap_one(tap, peer, "bridge", name, ifname,
                         script, downscript, vhostfdname,
                         vnet_hdr, fd, &err);
        if (err) {
            error_propagate(errp, err);
            close(fd);
            return -1;
        }
    } else {
        g_autofree char *default_script = NULL;
        g_autofree char *default_downscript = NULL;
        /*其它方式，与hash_vhostfds相冲突*/
        if (tap->vhostfds) {
            error_setg(errp, "vhostfds= is invalid if fds= wasn't specified");
            return -1;
        }

        //接口up/down脚本
        if (!script) {
            script = default_script = get_relocated_path(DEFAULT_NETWORK_SCRIPT);
        }
        if (!downscript) {
            downscript = default_downscript =
                                 get_relocated_path(DEFAULT_NETWORK_DOWN_SCRIPT);
        }

        if (tap->ifname) {
            pstrcpy(ifname, sizeof ifname, tap->ifname);
        } else {
            ifname[0] = '\0';
        }

        //针对每个queues，创建一个tap设备
        for (i = 0; i < queues; i++) {
            fd = net_tap_init(tap, &vnet_hdr, i >= 1 ? "no" : script,
                              ifname/*tap接口名称*/, sizeof ifname, queues > 1, errp);
            if (fd == -1) {
                return -1;
            }

            if (queues > 1 && i == 0 && !tap->ifname) {
                if (tap_fd_get_ifname(fd, ifname)) {
                    error_setg(errp, "Fail to get ifname");
                    close(fd);
                    return -1;
                }
            }

            /*创建TAPState*/
            net_init_tap_one(tap, peer, "tap", name, ifname,
                             i >= 1 ? "no" : script,
                             i >= 1 ? "no" : downscript,
                             vhostfdname, vnet_hdr, fd/*tap设备对应的fd*/, &err);
            if (err) {
                error_propagate(errp, err);
                close(fd);
                return -1;
            }
        }
    }

    return 0;
}

VHostNetState *tap_get_vhost_net(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    assert(nc->info->type == NET_CLIENT_DRIVER_TAP);
    return s->vhost_net;
}

int tap_enable(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    int ret;

    if (s->enabled) {
        return 0;
    } else {
        ret = tap_fd_enable(s->fd);
        if (ret == 0) {
            s->enabled = true;
            tap_update_fd_handler(s);
        }
        return ret;
    }
}

int tap_disable(NetClientState *nc)
{
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    int ret;

    if (s->enabled == 0) {
        return 0;
    } else {
        ret = tap_fd_disable(s->fd);
        if (ret == 0) {
            qemu_purge_queued_packets(nc);
            s->enabled = false;
            tap_update_fd_handler(s);
        }
        return ret;
    }
}
