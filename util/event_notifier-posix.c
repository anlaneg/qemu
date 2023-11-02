/*
 * event notifier support
 *
 * Copyright Red Hat, Inc. 2010
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/event_notifier.h"
#include "qemu/main-loop.h"

#ifdef CONFIG_EVENTFD
#include <sys/eventfd.h>
#endif

#ifdef CONFIG_EVENTFD
/*
 * Initialize @e with existing file descriptor @fd.
 * @fd must be a genuine eventfd object, emulation with pipe won't do.
 */
void event_notifier_init_fd(EventNotifier *e, int fd)
{
    e->rfd = fd;
    e->wfd = fd;
    e->initialized = true;
}
#endif

//e会创建一对pipe fd,一个负责读，一个负责写,如果active为真，则将触发e的通知
int event_notifier_init(EventNotifier *e, int active)
{
    int fds[2];
    int ret;

#ifdef CONFIG_EVENTFD
    //创建eventfd
    ret = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
    ret = -1;
    errno = ENOSYS;
#endif
    if (ret >= 0) {
        /*如果支持eventfd,则使用eventfd创建的fd*/
        e->rfd = e->wfd = ret;
    } else {
        if (errno != ENOSYS) {
            return -errno;
        }
        //不支持eventfd,创建pipe
        if (!g_unix_open_pipe(fds, FD_CLOEXEC, NULL)) {
            return -errno;
        }
        //指定为非阻塞
        if (!g_unix_set_fd_nonblocking(fds[0], true, NULL)) {
            ret = -errno;
            goto fail;
        }
        if (!g_unix_set_fd_nonblocking(fds[1], true, NULL)) {
            ret = -errno;
            goto fail;
        }
        e->rfd = fds[0];
        e->wfd = fds[1];
    }
    e->initialized = true;
    //如果活跃，则向e发送通知
    if (active) {
        event_notifier_set(e);
    }
    return 0;

fail:
    close(fds[0]);
    close(fds[1]);
    return ret;
}

void event_notifier_cleanup(EventNotifier *e)
{
    if (!e->initialized) {
        return;
    }

    if (e->rfd != e->wfd) {
        close(e->rfd);
    }

    e->rfd = -1;
    close(e->wfd);
    e->wfd = -1;
    e->initialized = false;
}

//notifier的读fd
int event_notifier_get_fd(const EventNotifier *e)
{
    return e->rfd;
}

int event_notifier_get_wfd(const EventNotifier *e)
{
    return e->wfd;
}

//向e的wfd中写数据，完成通知
int event_notifier_set(EventNotifier *e)
{
    static const uint64_t value = 1;
    ssize_t ret;

    if (!e->initialized) {
        return -1;
    }

    do {
        //向wfd中写数据
        ret = write(e->wfd, &value, sizeof(value));
    } while (ret < 0 && errno == EINTR);

    /* EAGAIN is fine, a read must be pending.  */
    if (ret < 0 && errno != EAGAIN) {
        return -errno;
    }
    return 0;
}

int event_notifier_test_and_clear(EventNotifier *e)
{
    int value;
    ssize_t len;
    char buffer[512];

    if (!e->initialized) {
        return 0;
    }

    /* Drain the notify pipe.  For eventfd, only 8 bytes will be read.  */
    value = 0;
    do {
        /*检查e->rfd是否可读取,如果不可读取，则阻塞*/
        len = read(e->rfd, buffer, sizeof(buffer));
        value |= (len > 0);
    } while ((len == -1 && errno == EINTR) || len == sizeof(buffer));

    return value;
}
