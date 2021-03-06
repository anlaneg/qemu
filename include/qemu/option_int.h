/*
 * Commandline option parsing functions
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2009 Kevin Wolf <kwolf@redhat.com>
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

#ifndef QEMU_OPTION_INT_H
#define QEMU_OPTION_INT_H

#include "qemu/option.h"
#include "qemu/error-report.h"

struct QemuOpt {
    char *name;//选项名称
    char *str;//选项的值（字符串形式）

    const QemuOptDesc *desc;//为此选项定义的描述信息
    union {
        bool boolean;
        uint64_t uint;
    } value;//选项取值

    QemuOpts     *opts;//从属于哪个opts
    QTAILQ_ENTRY(QemuOpt) next;//用于串连opt
};

struct QemuOpts {
    char *id;//选项id（外部可引用的名称，例如-netdev type=vhost-user,id=mynet1,chardev=char1,vhostforce中的id)
    QemuOptsList *list;//选项从属于哪个QemuOptsList（或者说挂在哪个链上）
    Location loc;
    QTAILQ_HEAD(, QemuOpt) head;//同一id的挂在这（QemuOptHead类型）
    QTAILQ_ENTRY(QemuOpts) next;//下一个QemuOpts(id不同）
};

#endif
