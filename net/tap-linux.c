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
#include "tap-linux.h"
#include "net/tap.h"

#include <net/if.h>
#include <sys/ioctl.h>

#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"

#define PATH_NET_TUN "/dev/net/tun"

int tap_open(char *ifname/*接口名称*/, int ifname_size, int *vnet_hdr/*出参，是否支持vnet_hdr功能*/,
             int vnet_hdr_required/*是否强制要vnet_hdr功能*/, int mq_required/*是否要求多队列支持*/, Error **errp)
{
    struct ifreq ifr;
    int fd, ret;
    int len = sizeof(struct virtio_net_hdr);
    unsigned int features;

    //打开tun设备
    fd = RETRY_ON_EINTR(open(PATH_NET_TUN, O_RDWR));
    if (fd < 0) {
        error_setg_errno(errp, errno, "could not open %s", PATH_NET_TUN);
        return -1;
    }


    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    //取当前tun设备支持的features列表
    if (ioctl(fd, TUNGETFEATURES, &features) == -1) {
        warn_report("TUNGETFEATURES failed: %s", strerror(errno));
        features = 0;
    }

    //如果支持单队列，就使用单队列功能
    if (features & IFF_ONE_QUEUE) {
        ifr.ifr_flags |= IFF_ONE_QUEUE;
    }

    if (*vnet_hdr) {
        //如果有vnet_hdr功能，则打开此功能
        if (features & IFF_VNET_HDR) {
            *vnet_hdr = 1;
            ifr.ifr_flags |= IFF_VNET_HDR;
        } else {
            *vnet_hdr = 0;
        }

        //如果需要vnet_hdr功能，但kernel不支持此功能，则报错
        if (vnet_hdr_required && !*vnet_hdr) {
            error_setg(errp, "vnet_hdr=1 requested, but no kernel "
                       "support for IFF_VNET_HDR available");
            close(fd);
            return -1;
        }
        /*
         * Make sure vnet header size has the default value: for a persistent
         * tap it might have been modified e.g. by another instance of qemu.
         * Ignore errors since old kernels do not support this ioctl: in this
         * case the header size implicitly has the correct value.
         */
        //设置vnet header长度为sizeof(virtio_net_hdr)
        ioctl(fd, TUNSETVNETHDRSZ, &len);
    }

    //要求多队列支持，但kernel features不支持多队列，报错
    if (mq_required) {
        if (!(features & IFF_MULTI_QUEUE)) {
            error_setg(errp, "multiqueue required, but no kernel "
                       "support for IFF_MULTI_QUEUE available");
            close(fd);
            return -1;
        } else {
            //指明开启多队列
            ifr.ifr_flags |= IFF_MULTI_QUEUE;
        }
    }

    //设置接口名称
    if (ifname[0] != '\0')
        pstrcpy(ifr.ifr_name, IFNAMSIZ, ifname);
    else
        pstrcpy(ifr.ifr_name, IFNAMSIZ, "tap%d");//要求动态分配

    //执行ioctl设置tun口
    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (ret != 0) {
        //配置tun口失败，返回
        if (ifname[0] != '\0') {
            error_setg_errno(errp, errno, "could not configure %s (%s)",
                             PATH_NET_TUN, ifr.ifr_name);
        } else {
            error_setg_errno(errp, errno, "could not configure %s",
                             PATH_NET_TUN);
        }
        close(fd);
        return -1;
    }

    //更新接口名称
    pstrcpy(ifname, ifname_size, ifr.ifr_name);
    //接口置为非阻塞
    g_unix_set_fd_nonblocking(fd, true, NULL);
    return fd;
}

/* sndbuf implements a kind of flow control for tap.
 * Unfortunately when it's enabled, and packets are sent
 * to other guests on the same host, the receiver
 * can lock up the transmitter indefinitely.
 *
 * To avoid packet loss, sndbuf should be set to a value lower than the tx
 * queue capacity of any destination network interface.
 * Ethernet NICs generally have txqueuelen=1000, so 1Mb is
 * a good value, given a 1500 byte MTU.
 */
#define TAP_DEFAULT_SNDBUF 0

//设置tap口发送buffer缓冲大小
void tap_set_sndbuf(int fd, const NetdevTapOptions *tap, Error **errp)
{
    int sndbuf;

    sndbuf = !tap->has_sndbuf       ? TAP_DEFAULT_SNDBUF :
             tap->sndbuf > INT_MAX  ? INT_MAX :
             tap->sndbuf;

    if (!sndbuf) {
        sndbuf = INT_MAX;
    }

    if (ioctl(fd, TUNSETSNDBUF, &sndbuf) == -1 && tap->has_sndbuf) {
        error_setg_errno(errp, errno, "TUNSETSNDBUF ioctl failed");
    }
}

//检查tap口是否支持VNET_HDR
int tap_probe_vnet_hdr(int fd, Error **errp)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));

    if (ioctl(fd, TUNGETIFF, &ifr) != 0) {
        /* TUNGETIFF is available since kernel v2.6.27 */
        error_setg_errno(errp, errno,
                         "Unable to query TUNGETIFF on FD %d", fd);
        return -1;
    }

    return ifr.ifr_flags & IFF_VNET_HDR;
}

//设置checksum offload
int tap_probe_has_ufo(int fd)
{
    unsigned offload;

    offload = TUN_F_CSUM | TUN_F_UFO;

    if (ioctl(fd, TUNSETOFFLOAD, offload) < 0)
        return 0;

    return 1;
}

int tap_probe_has_uso(int fd)
{
    unsigned offload;

    offload = TUN_F_CSUM | TUN_F_USO4 | TUN_F_USO6;

    if (ioctl(fd, TUNSETOFFLOAD, offload) < 0) {
        return 0;
    }
    return 1;
}

//设置vnet hdr长度
void tap_fd_set_vnet_hdr_len(int fd, int len)
{
    if (ioctl(fd, TUNSETVNETHDRSZ, &len) == -1) {
        fprintf(stderr, "TUNSETVNETHDRSZ ioctl() failed: %s. Exiting.\n",
                strerror(errno));
        abort();
    }
}

int tap_fd_set_vnet_le(int fd, int is_le)
{
    int arg = is_le ? 1 : 0;

    if (!ioctl(fd, TUNSETVNETLE, &arg)) {
        return 0;
    }

    /* Check if our kernel supports TUNSETVNETLE */
    if (errno == EINVAL) {
        return -errno;
    }

    error_report("TUNSETVNETLE ioctl() failed: %s.", strerror(errno));
    abort();
}

int tap_fd_set_vnet_be(int fd, int is_be)
{
    int arg = is_be ? 1 : 0;

    if (!ioctl(fd, TUNSETVNETBE, &arg)) {
        return 0;
    }

    /* Check if our kernel supports TUNSETVNETBE */
    if (errno == EINVAL) {
        return -errno;
    }

    error_report("TUNSETVNETBE ioctl() failed: %s.", strerror(errno));
    abort();
}

void tap_fd_set_offload(int fd, int csum, int tso4,
                        int tso6, int ecn, int ufo, int uso4, int uso6)
{
    unsigned int offload = 0;

    /* Check if our kernel supports TUNSETOFFLOAD */
    if (ioctl(fd, TUNSETOFFLOAD, 0) != 0 && errno == EINVAL) {
        //kernel不支持TUNSETOFFLOAD，退出
        return;
    }

    if (csum) {
        offload |= TUN_F_CSUM;//要求offload checksum
        if (tso4)
            offload |= TUN_F_TSO4;//要求offload tso4
        if (tso6)
            offload |= TUN_F_TSO6;//要求offload tso6
        if ((tso4 || tso6) && ecn)
            offload |= TUN_F_TSO_ECN;
        if (ufo)
            offload |= TUN_F_UFO;
        if (uso4) {
            offload |= TUN_F_USO4;
        }
        if (uso6) {
            offload |= TUN_F_USO6;
        }
    }

    if (ioctl(fd, TUNSETOFFLOAD, offload) != 0) {
        //如果失败，取消掉UFO的offload再试一次
        offload &= ~(TUN_F_USO4 | TUN_F_USO6);
        if (ioctl(fd, TUNSETOFFLOAD, offload) != 0) {
            offload &= ~TUN_F_UFO;
            if (ioctl(fd, TUNSETOFFLOAD, offload) != 0) {
                fprintf(stderr, "TUNSETOFFLOAD ioctl() failed: %s\n",
                    strerror(errno));
            }
        }
    }
}

/* Enable a specific queue of tap. */
int tap_fd_enable(int fd)
{
    struct ifreq ifr;
    int ret;

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_flags = IFF_ATTACH_QUEUE;
    ret = ioctl(fd, TUNSETQUEUE, (void *) &ifr);

    if (ret != 0) {
        error_report("could not enable queue");
    }

    return ret;
}

/* Disable a specific queue of tap/ */
int tap_fd_disable(int fd)
{
    struct ifreq ifr;
    int ret;

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_flags = IFF_DETACH_QUEUE;
    ret = ioctl(fd, TUNSETQUEUE, (void *) &ifr);

    if (ret != 0) {
        error_report("could not disable queue");
    }

    return ret;
}

//获取tap设备名称
int tap_fd_get_ifname(int fd, char *ifname)
{
    struct ifreq ifr;

    if (ioctl(fd, TUNGETIFF, &ifr) != 0) {
        error_report("TUNGETIFF ioctl() failed: %s",
                     strerror(errno));
        return -1;
    }

    pstrcpy(ifname, sizeof(ifr.ifr_name), ifr.ifr_name);
    return 0;
}

int tap_fd_set_steering_ebpf(int fd, int prog_fd)
{
    if (ioctl(fd, TUNSETSTEERINGEBPF, (void *) &prog_fd) != 0) {
        error_report("Issue while setting TUNSETSTEERINGEBPF:"
                    " %s with fd: %d, prog_fd: %d",
                    strerror(errno), fd, prog_fd);

       return -1;
    }

    return 0;
}
