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

void object_property_set_qobject(Object *obj, QObject *value,
                                 const char *name, Error **errp)
{
    Visitor *v;

    v = qobject_input_visitor_new(value);
    object_property_set(obj, v, name, errp);
    visit_free(v);
}

//取Object中名称为name的属性值
QObject *object_property_get_qobject(Object *obj, const char *name,
                                     Error **errp)
{
    QObject *ret = NULL;
    Error *local_err = NULL;
    Visitor *v;

    /*构造output visitor,并指明结果值存在ret指针中*/
    v = qobject_output_visitor_new(&ret);
    //获取相应属性值
    object_property_get(obj, v, name, &local_err);
    if (!local_err) {
        //未发生错误，执行complete
        visit_complete(v, &ret);
    }
    error_propagate(errp, local_err);
    visit_free(v);
    //返回内容
    return ret;
}
