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
#include "standard-headers/misc/pvpanic.h"

static void handle_event(int event)
{
    static bool logged;

    //只支持二种event,如果不认识，则告警
    if (event & ~PVPANIC_EVENTS && !logged) {
        qemu_log_mask(LOG_GUEST_ERROR, "pvpanic: unknown event %#x.\n", event);
        logged = true;
    }

    if (event & PVPANIC_PANICKED) {
        //使guest系统panic
        qemu_system_guest_panicked(NULL);
        return;
    }

    if (event & PVPANIC_CRASH_LOADED) {
        //执行guest系统crashloaed
        qemu_system_guest_crashloaded(NULL);
        return;
    }

    if (event & PVPANIC_SHUTDOWN) {
        qemu_system_guest_pvshutdown();
        return;
    }
}

/* return supported events on read */
static uint64_t pvpanic_read(void *opaque, hwaddr addr, unsigned size)
{
    //对此inport读取，返回常量
    PVPanicState *pvp = opaque;
    return pvp->events;
}

static void pvpanic_write(void *opaque, hwaddr addr/*要写的地址*/, uint64_t val/*要写的值*/,
                                 unsigned size/*要写的大小*/)
{
    //对此inport执行写
    handle_event(val);
}

static const MemoryRegionOps pvpanic_ops = {
    .read = pvpanic_read,
    .write = pvpanic_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

//初始化PVPanicState
void pvpanic_setup_io(PVPanicState *s, DeviceState *dev, unsigned size)
{
    memory_region_init_io(&s->mr, OBJECT(dev), &pvpanic_ops, s, "pvpanic", size);
}
