/*
 * Device Container
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "qemu/module.h"

//定义container类型
//继承链:container->object
static const TypeInfo container_info = {
    .name          = "container",
    .parent        = TYPE_OBJECT,
};

//注册container类型
static void container_register_types(void)
{
    type_register_static(&container_info);
}

//按path查找object(每个path的文件名对应一个obj)，如果对象不存在，则创建container类型的对象
Object *container_get(Object *root, const char *path)
{
    Object *obj, *child;
    char **parts;
    int i;

    //按‘/'拆分,parts[0]==''
    parts = g_strsplit(path, "/", 0);
    assert(parts != NULL && parts[0] != NULL && !parts[0][0]);
    obj = root;

    //跳过“0“号
    for (i = 1; parts[i] != NULL; i++, obj = child) {
    	//按路径名称查找子OBJ
        child = object_resolve_path_component(obj, parts[i]);
        if (!child) {
            //parts[i]不存在，添加新的child('container')，并添加parts[i]属性
            child = object_new("container");
            object_property_add_child(obj, parts[i], child);
            object_unref(child);
        }
    }

    g_strfreev(parts);

    return obj;
}


type_init(container_register_types)
