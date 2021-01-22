/*
 * Virtio Network Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_VIRTIO_NET_H
#define QEMU_VIRTIO_NET_H

#include "qemu/units.h"
#include "standard-headers/linux/virtio_net.h"
#include "hw/virtio/virtio.h"
#include "net/announce.h"
#include "qemu/option_int.h"
#include "qom/object.h"

#define TYPE_VIRTIO_NET "virtio-net-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIONet, VIRTIO_NET)

#define TX_TIMER_INTERVAL 150000 /* 150 us */

/* Limit the number of packets that can be sent via a single flush
 * of the TX queue.  This gives us a guaranteed exit condition and
 * ensures fairness in the io path.  256 conveniently matches the
 * length of the TX queue and shows a good balance of performance
 * and latency. */
#define TX_BURST 256

typedef struct virtio_net_conf
{
    uint32_t txtimer;
    int32_t txburst;
    char *tx;
    uint16_t rx_queue_size;/*rx队列长度*/
    uint16_t tx_queue_size;/*tx队列长度*/
    uint16_t mtu;//配置的mtu
    int32_t speed;//网卡速率
    char *duplex_str;//双工模式（字符串类型）
    uint8_t duplex;//双工模式
    char *primary_id_str;
} virtio_net_conf;

/* Coalesced packets type & status */
typedef enum {
    RSC_COALESCE,           /* Data been coalesced */
    RSC_FINAL,              /* Will terminate current connection */
    RSC_NO_MATCH,           /* No matched in the buffer pool */
    RSC_BYPASS,             /* Packet to be bypass, not tcp, tcp ctrl, etc */
    RSC_CANDIDATE                /* Data want to be coalesced */
} CoalesceStatus;

typedef struct VirtioNetRscStat {
    uint32_t received;
    uint32_t coalesced;
    uint32_t over_size;
    uint32_t cache;
    uint32_t empty_cache;
    uint32_t no_match_cache;
    uint32_t win_update;
    uint32_t no_match;
    uint32_t tcp_syn;
    uint32_t tcp_ctrl_drain;
    uint32_t dup_ack;
    uint32_t dup_ack1;
    uint32_t dup_ack2;
    uint32_t pure_ack;
    uint32_t ack_out_of_win;
    uint32_t data_out_of_win;
    uint32_t data_out_of_order;
    uint32_t data_after_pure_ack;
    uint32_t bypass_not_tcp;
    uint32_t tcp_option;
    uint32_t tcp_all_opt;
    uint32_t ip_frag;
    uint32_t ip_ecn;
    uint32_t ip_hacked;
    uint32_t ip_option;
    uint32_t purge_failed;
    uint32_t drain_failed;
    uint32_t final_failed;
    int64_t  timer;
} VirtioNetRscStat;

/* Rsc unit general info used to checking if can coalescing */
typedef struct VirtioNetRscUnit {
    void *ip;   /* ip header */
    uint16_t *ip_plen;      /* data len pointer in ip header field */
    struct tcp_header *tcp; /* tcp header */
    uint16_t tcp_hdrlen;    /* tcp header len */
    uint16_t payload;       /* pure payload without virtio/eth/ip/tcp */
} VirtioNetRscUnit;

/* Coalesced segment */
typedef struct VirtioNetRscSeg {
    QTAILQ_ENTRY(VirtioNetRscSeg) next;
    void *buf;
    size_t size;
    uint16_t packets;
    uint16_t dup_ack;
    bool is_coalesced;      /* need recal ipv4 header checksum, mark here */
    VirtioNetRscUnit unit;
    NetClientState *nc;
} VirtioNetRscSeg;


/* Chain is divided by protocol(ipv4/v6) and NetClientInfo */
typedef struct VirtioNetRscChain {
    QTAILQ_ENTRY(VirtioNetRscChain) next;
    VirtIONet *n;                            /* VirtIONet */
    uint16_t proto;
    uint8_t  gso_type;
    uint16_t max_payload;
    QEMUTimer *drain_timer;
    QTAILQ_HEAD(, VirtioNetRscSeg) buffers;
    VirtioNetRscStat stat;
} VirtioNetRscChain;

/* Maximum packet size we can receive from tap device: header + 64k */
#define VIRTIO_NET_MAX_BUFSIZE (sizeof(struct virtio_net_hdr) + (64 * KiB))

#define VIRTIO_NET_RSS_MAX_KEY_SIZE     40
#define VIRTIO_NET_RSS_MAX_TABLE_LEN    128

typedef struct VirtioNetRssData {
    bool    enabled;
    bool    redirect;
    bool    populate_hash;
    uint32_t hash_types;
    uint8_t key[VIRTIO_NET_RSS_MAX_KEY_SIZE];
    uint16_t indirections_len;
    uint16_t *indirections_table;
    uint16_t default_queue;
} VirtioNetRssData;

typedef struct VirtIONetQueue {
    VirtQueue *rx_vq;//rx队列
    VirtQueue *tx_vq;//tx队列
    QEMUTimer *tx_timer;
    QEMUBH *tx_bh;
    uint32_t tx_waiting;
    struct {
        VirtQueueElement *elem;
    } async_tx;
    struct VirtIONet *n;//队列所属的网络设备
} VirtIONetQueue;

//virtio网络设备
struct VirtIONet {
    VirtIODevice parent_obj;
    uint8_t mac[ETH_ALEN];//设备mac地址
    uint16_t status;
    VirtIONetQueue *vqs;/*网络设备队列数组，长度为max_queues*/
    VirtQueue *ctrl_vq;
    NICState *nic;
    /* RSC Chains - temporary storage of coalesced data,
       all these data are lost in case of migration */
    QTAILQ_HEAD(, VirtioNetRscChain) rsc_chains;
    uint32_t tx_timeout;
    int32_t tx_burst;/*网卡最大一次burst的数目*/
    uint32_t has_vnet_hdr;
    size_t host_hdr_len;/*host头部长度，buf跳过此长度为报文mac头*/
    size_t guest_hdr_len;
    uint64_t host_features;
    uint32_t rsc_timeout;
    uint8_t rsc4_enabled;
    uint8_t rsc6_enabled;
    uint8_t has_ufo;
    uint32_t mergeable_rx_bufs;
    uint8_t promisc;//混杂模式是否开启
    uint8_t allmulti;//是否收取所有组播（mac层）
    uint8_t alluni;//是否收取所有的单播地址
    uint8_t nomulti;//不收取组播地址（mac层）
    uint8_t nouni;//不收取单播地址（mac层）
    uint8_t nobcast;//不接收广播地址（mac层)
    uint8_t vhost_started;
    struct {
        uint32_t in_use;//macs表大小
        uint32_t first_multi;//首个mac地址索引（组播单播）
        uint8_t multi_overflow;
        uint8_t uni_overflow;
        uint8_t *macs;//记录容许的mac地址
    } mac_table;
    //vlan一共有12bits,5个高位bits为一个单位，则vlans数组长度为vlan>>5
    //每个数组元素，按位记录每个vlan值，共可以记录0x20个vlan(即32个）
    //记录接口可接收的vlan id
    uint32_t *vlans;
    virtio_net_conf net_conf;//网络配置信息
    NICConf nic_conf;//网卡配置信息
    DeviceState *qdev;
    int multiqueue;//是否多队列
    uint16_t max_queues;//总队列数（rx+tx+ctl)
    uint16_t curr_queues;
    size_t config_size;
    char *netclient_name;
    char *netclient_type;
    uint64_t curr_guest_offloads;
    /* used on saved state restore phase to preserve the curr_guest_offloads */
    uint64_t saved_guest_offloads;
    AnnounceTimer announce_timer;
    bool needs_vnet_hdr_swap;
    bool mtu_bypass_backend;
    /* primary failover device is hidden*/
    bool failover_primary_hidden;
    bool failover;/*是否开启failover功能*/
    DeviceListener primary_listener;
    Notifier migration_state;
    VirtioNetRssData rss_data;
    struct NetRxPkt *rx_pkt;
};

void virtio_net_set_netclient_name(VirtIONet *n, const char *name,
                                   const char *type);

#endif
