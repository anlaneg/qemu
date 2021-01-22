/*
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
#include "net/queue.h"
#include "qemu/queue.h"
#include "net/net.h"

/* The delivery handler may only return zero if it will call
 * qemu_net_queue_flush() when it determines that it is once again able
 * to deliver packets. It must also call qemu_net_queue_purge() in its
 * cleanup path.
 *
 * If a sent callback is provided to send(), the caller must handle a
 * zero return from the delivery handler by not sending any more packets
 * until we have invoked the callback. Only in that case will we queue
 * the packet.
 *
 * If a sent callback isn't provided, we just drop the packet to avoid
 * unbounded queueing.
 */

struct NetPacket {
    QTAILQ_ENTRY(NetPacket) entry;
    //报文所属nc
    NetClientState *sender;
    unsigned flags;
    //报文大小
    int size;
    //报文的发送回调
    NetPacketSent *sent_cb;
    //报文内容
    uint8_t data[];
};

//队列
struct NetQueue {
    //网络队列私有数据
    void *opaque;
    //队列最大长度
    uint32_t nq_maxlen;
    //队列中缓存的报文数
    uint32_t nq_count;
    NetQueueDeliverFunc *deliver;

    //记录队列中缓存的报文
    QTAILQ_HEAD(, NetPacket) packets;

    //标明队列正在投递报文
    unsigned delivering : 1;
};

//新建一个网络队列
NetQueue *qemu_new_net_queue(NetQueueDeliverFunc *deliver/*队列投递回调*/, void *opaque)
{
    NetQueue *queue;

    queue = g_new0(NetQueue, 1);

    queue->opaque = opaque;
    queue->nq_maxlen = 10000;
    queue->nq_count = 0;
    queue->deliver = deliver;

    QTAILQ_INIT(&queue->packets);

    queue->delivering = 0;

    return queue;
}

//移除网络队列
void qemu_del_net_queue(NetQueue *queue)
{
    NetPacket *packet, *next;

    //先释放网络队列中缓存的报文
    QTAILQ_FOREACH_SAFE(packet, &queue->packets, entry, next) {
        QTAILQ_REMOVE(&queue->packets, packet, entry);
        g_free(packet);
    }

    //再释放队列
    g_free(queue);
}

//向队列中加入一个报文（队列如果为满且没有send_cb则丢包）
static void qemu_net_queue_append(NetQueue *queue,
                                  NetClientState *sender/*报文从属的网卡*/,
                                  unsigned flags,
                                  const uint8_t *buf/*报文内容*/,
                                  size_t size,
                                  NetPacketSent *sent_cb)
{
    NetPacket *packet;

    /*队列满且没有发送回调，则丢弃*/
    if (queue->nq_count >= queue->nq_maxlen && !sent_cb) {
        return; /* drop if queue full and no callback */
    }

    //申请一个net packet
    packet = g_malloc(sizeof(NetPacket) + size);
    packet->sender = sender;
    packet->flags = flags;
    packet->size = size;
    packet->sent_cb = sent_cb;
    memcpy(packet->data, buf, size);//将报文内容copy进buffer

    //将net packet串入队列
    queue->nq_count++;
    QTAILQ_INSERT_TAIL(&queue->packets, packet, entry);
}

//通过iov方式向队列中加入一个net packet（容许net packet有多个iov）
void qemu_net_queue_append_iov(NetQueue *queue,
                               NetClientState *sender,
                               unsigned flags,
                               const struct iovec *iov,
                               int iovcnt/*iov数组长度*/,
                               NetPacketSent *sent_cb)
{
    NetPacket *packet;
    size_t max_len = 0;
    int i;

    //队列为满且没有cb，丢包
    if (queue->nq_count >= queue->nq_maxlen && !sent_cb) {
        return; /* drop if queue full and no callback */
    }

    //计算报文总长度
    for (i = 0; i < iovcnt; i++) {
        max_len += iov[i].iov_len;
    }

    //申请并填充net packet
    packet = g_malloc(sizeof(NetPacket) + max_len);
    packet->sender = sender;
    packet->sent_cb = sent_cb;
    packet->flags = flags;
    packet->size = 0;

    for (i = 0; i < iovcnt; i++) {
        size_t len = iov[i].iov_len;

        memcpy(packet->data + packet->size, iov[i].iov_base, len);
        packet->size += len;
    }

    //将其加入队列
    queue->nq_count++;
    QTAILQ_INSERT_TAIL(&queue->packets, packet, entry);
}

//单个报文非iov方式投递
static ssize_t qemu_net_queue_deliver(NetQueue *queue,
                                      NetClientState *sender,
                                      unsigned flags,
                                      const uint8_t *data,
                                      size_t size)
{
    ssize_t ret = -1;
    struct iovec iov = {
        .iov_base = (void *)data,
        .iov_len = size
    };

    queue->delivering = 1;
    //将data封装成iov格式进行报文投递（返回0表示没有投递出去）
    ret = queue->deliver(sender, flags, &iov, 1, queue->opaque);
    queue->delivering = 0;

    return ret;
}

//单个报文iov方式投递
static ssize_t qemu_net_queue_deliver_iov(NetQueue *queue,
                                          NetClientState *sender,
                                          unsigned flags,
                                          const struct iovec *iov,
                                          int iovcnt)
{
    ssize_t ret = -1;

    queue->delivering = 1;
    ret = queue->deliver(sender, flags, iov, iovcnt, queue->opaque);
    queue->delivering = 0;

    return ret;
}

ssize_t qemu_net_queue_send(NetQueue *queue,
                            NetClientState *sender,
                            unsigned flags,
                            const uint8_t *data,
                            size_t size,
                            NetPacketSent *sent_cb)
{
    ssize_t ret;

    if (queue->delivering || !qemu_can_send_packet(sender)) {
        //队列正在投递报文或者当前不能发送报文，则报文被缓存进queue
        qemu_net_queue_append(queue, sender, flags, data, size, sent_cb);
        return 0;
    }

    ret = qemu_net_queue_deliver(queue, sender, flags, data, size);
    if (ret == 0) {
    	//缓存报文
        qemu_net_queue_append(queue, sender, flags, data, size, sent_cb);
        return 0;
    }

    //尝试着刷缓冲
    qemu_net_queue_flush(queue);

    return ret;
}

ssize_t qemu_net_queue_send_iov(NetQueue *queue,
                                NetClientState *sender,
                                unsigned flags,
                                const struct iovec *iov,
                                int iovcnt,
                                NetPacketSent *sent_cb)
{
    ssize_t ret;

    if (queue->delivering || !qemu_can_send_packet(sender)) {
        //队列正在投递报文或者当前不能发送报文，则报文被缓存进queue
        qemu_net_queue_append_iov(queue, sender, flags, iov, iovcnt, sent_cb);
        return 0;
    }

    ret = qemu_net_queue_deliver_iov(queue, sender, flags, iov, iovcnt);
    if (ret == 0) {
        qemu_net_queue_append_iov(queue, sender, flags, iov, iovcnt, sent_cb);
        return 0;
    }

    qemu_net_queue_flush(queue);

    return ret;
}

void qemu_net_queue_purge(NetQueue *queue, NetClientState *from)
{
    NetPacket *packet, *next;

    QTAILQ_FOREACH_SAFE(packet, &queue->packets, entry, next) {
        if (packet->sender == from) {
            QTAILQ_REMOVE(&queue->packets, packet, entry);
            queue->nq_count--;
            if (packet->sent_cb) {
                packet->sent_cb(packet->sender, 0);
            }
            g_free(packet);
        }
    }
}

//将队列中的报文投递给对端
bool qemu_net_queue_flush(NetQueue *queue)
{
    if (queue->delivering)
        return false;

    while (!QTAILQ_EMPTY(&queue->packets)) {
        NetPacket *packet;
        int ret;

        packet = QTAILQ_FIRST(&queue->packets);
        QTAILQ_REMOVE(&queue->packets, packet, entry);
        queue->nq_count--;

        ret = qemu_net_queue_deliver(queue,
                                     packet->sender,
                                     packet->flags,
                                     packet->data,
                                     packet->size);
        //没有转发出去，重新入队到队头并返回
        if (ret == 0) {
            queue->nq_count++;
            QTAILQ_INSERT_HEAD(&queue->packets, packet, entry);
            return false;
        }

        //发成成功，如果报文有回调，则调用回调
        if (packet->sent_cb) {
            packet->sent_cb(packet->sender, ret);
        }

        g_free(packet);
    }
    return true;
}
