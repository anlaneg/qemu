/*
 * QEMU simulated pvpanic device.
 *
 * Copyright Fujitsu, Corp. 2013
 *
 * Authors:
 *     Wen Congyang <wency@cn.fujitsu.com>
 *     Hu Tao <hutao@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/runstate.h"

#include "hw/nvram/fw_cfg.h"
#include "hw/qdev-properties.h"
#include "hw/misc/pvpanic.h"
#include "qom/object.h"

/* The bit of supported pv event, TODO: include uapi header and remove this */
#define PVPANIC_F_PANICKED      0
#define PVPANIC_F_CRASHLOADED   1

/* The pv event value */
#define PVPANIC_PANICKED        (1 << PVPANIC_F_PANICKED)
#define PVPANIC_CRASHLOADED     (1 << PVPANIC_F_CRASHLOADED)

typedef struct PVPanicState PVPanicState;
DECLARE_INSTANCE_CHECKER(PVPanicState, ISA_PVPANIC_DEVICE,
                         TYPE_PVPANIC)

static void handle_event(int event)
{
    static bool logged;

    //只支持二种event,如果不认识，则告警
    if (event & ~(PVPANIC_PANICKED | PVPANIC_CRASHLOADED) && !logged) {
        qemu_log_mask(LOG_GUEST_ERROR, "pvpanic: unknown event %#x.\n", event);
        logged = true;
    }

    if (event & PVPANIC_PANICKED) {
        //使guest系统panic
        qemu_system_guest_panicked(NULL);
        return;
    }

    if (event & PVPANIC_CRASHLOADED) {
        //执行guest系统crashloaed
        qemu_system_guest_crashloaded(NULL);
        return;
    }
}

#include "hw/isa/isa.h"

//pvpanic设备
struct PVPanicState {
    ISADevice parent_obj;

    MemoryRegion io;//pvapnic对应的内存区域
    uint16_t ioport;
    uint8_t events;
};

/* return supported events on read */
static uint64_t pvpanic_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    //对此inport读取，返回常量
    PVPanicState *pvp = opaque;
    return pvp->events;
}

static void pvpanic_ioport_write(void *opaque, hwaddr addr/*要写的地址*/, uint64_t val/*要写的值*/,
                                 unsigned size/*要写的大小*/)
{
    //对此inport执行写
    handle_event(val);
}

static const MemoryRegionOps pvpanic_ops = {
    .read = pvpanic_ioport_read,
    .write = pvpanic_ioport_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

//初始化PVPanicState
static void pvpanic_isa_initfn(Object *obj)
{
    PVPanicState *s = ISA_PVPANIC_DEVICE(obj);

    memory_region_init_io(&s->io, OBJECT(s), &pvpanic_ops, s, "pvpanic", 1);
}

static void pvpanic_isa_realizefn(DeviceState *dev, Error **errp)
{
    ISADevice *d = ISA_DEVICE(dev);
    PVPanicState *s = ISA_PVPANIC_DEVICE(dev);
    FWCfgState *fw_cfg = fw_cfg_find();
    uint16_t *pvpanic_port;

    if (!fw_cfg) {
        return;
    }

    pvpanic_port = g_malloc(sizeof(*pvpanic_port));
    *pvpanic_port = cpu_to_le16(s->ioport);
    fw_cfg_add_file(fw_cfg, "etc/pvpanic-port", pvpanic_port,
                    sizeof(*pvpanic_port));

    //注册此ioport
    isa_register_ioport(d, &s->io, s->ioport);
}

//定义ioport属性
static Property pvpanic_isa_properties[] = {
    DEFINE_PROP_UINT16(PVPANIC_IOPORT_PROP/*属性名称*/, PVPanicState, ioport, 0x505),
    DEFINE_PROP_UINT8("events", PVPanicState, events, PVPANIC_PANICKED | PVPANIC_CRASHLOADED),
    DEFINE_PROP_END_OF_LIST(),
};

//pvpanic设备class初始化
static void pvpanic_isa_class_init(ObjectClass *klass, void *data)
{
    //取DeviceClass,将realize的
    DeviceClass *dc = DEVICE_CLASS(klass);

    //将realize回调更为pvapnic
    dc->realize = pvpanic_isa_realizefn;
    //为class增加属性
    device_class_set_props(dc, pvpanic_isa_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static TypeInfo pvpanic_isa_info = {
    .name          = TYPE_PVPANIC,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(PVPanicState),
    .instance_init = pvpanic_isa_initfn,
    .class_init    = pvpanic_isa_class_init,
};

static void pvpanic_register_types(void)
{
    //注册pvpanic类型
    type_register_static(&pvpanic_isa_info);
}

type_init(pvpanic_register_types)
