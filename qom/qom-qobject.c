/*
 * QEMU Object Model - QObject wrappers
 *
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * Author: Paolo Bonzini <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "qom/qom-qobject.h"
#include "qapi/visitor.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/qobject-output-visitor.h"

bool object_property_set_qobject(Object *obj,
                                 const char *name, QObject *value,
                                 Error **errp)
{
    Visitor *v;
    bool ok;

    v = qobject_input_visitor_new(value);
    ok = object_property_set(obj, name, v, errp);
    visit_free(v);
    return ok;
}

//取Object中名称为name的属性值
QObject *object_property_get_qobject(Object *obj, const char *name,
                                     Error **errp)
{
    QObject *ret = NULL;
    Visitor *v;

    /*构造output visitor,并指明结果值存在ret指针中*/
    v = qobject_output_visitor_new(&ret);
    //获取相应属性值
    if (object_property_get(obj, name, v, errp)) {
        //未发生错误，执行complete
        visit_complete(v, &ret);
    }
    visit_free(v);
    //返回内容
    return ret;
}
