/*
 * QEMU Object Model
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "qemu/cutils.h"
#include "qemu/memalign.h"
#include "qapi/visitor.h"
#include "qapi/string-input-visitor.h"
#include "qapi/string-output-visitor.h"
#include "qapi/qobject-input-visitor.h"
#include "qapi/forward-visitor.h"
#include "qapi/qapi-builtin-visit.h"
#include "qapi/qmp/qjson.h"
#include "trace.h"

/* TODO: replace QObject with a simpler visitor to avoid a dependency
 * of the QOM core on QObject?  */
#include "qom/qom-qobject.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"
#include "qemu/error-report.h"

#define MAX_INTERFACES 32

typedef struct InterfaceImpl InterfaceImpl;
typedef struct TypeImpl TypeImpl;

struct InterfaceImpl
{
    const char *typename;//接口类型名称
};

struct TypeImpl
{
    const char *name;//类型名称

    //此类型ObjectClass内存大小
    size_t class_size;

    //类型对象大小，如果其为0，则其大小为父类型instance_size
    size_t instance_size;
    size_t instance_align;

    //此函数在所有父类class_base_init之后调用，以便容许设置默认的虚函数指针，当然也容许子类override
    //父类的虚方法。
    void (*class_init)(ObjectClass *klass, void *data);
    //在本类型构造函数完成后调用，依注释是为了消除memcopy对类型的影响
    void (*class_base_init)(ObjectClass *klass, void *data);
    //用于回调instance_init,instance_post_init,instance_finalize回调的参数
    //可用于构建动态class
    void *class_data;

    //实例的构造函数,对象初始化时会调用本函数，调用本函数时，父对象将已经被初始化，
    //故本类型仅仅初始化自有成员即可
    void (*instance_init)(Object *obj);
    //实例初始化完成后此函数将被调用，在所有@instance_init调用之后
    void (*instance_post_init)(Object *obj);
    //此函数在对象销毁期间被调用。在所有父类@instance_finalized调用之前调用，一个
    //对象仅仅需要稍毁此类型中的成员
    void (*instance_finalize)(Object *obj);

    //如果此字段为True,则此class不能被实附化。
    bool abstract;

    //对应的父类型名称
    const char *parent;
    //对应的父类型名称TypeImpl
    TypeImpl *parent_type;

    //每一个Type均有一个对应的class,每一个type实例前，其对应的class必须
    //已被初始化，函数type_initialize具体完成这一过程
    //class内存是平坦的，最前面与parent的ObjectClass一致
    ObjectClass *class;

    //接口数量
    int num_interfaces;
    //接口类型名称数组
    InterfaceImpl interfaces[MAX_INTERFACES];
};

/*interface类型*/
static Type type_interface;

//存储type的全局hash表,通过type名称可获得对应的TypeImpl
static GHashTable *type_table_get(void)
{
    static GHashTable *type_table;

    //如果type_tabe没有创建，则创建它
    if (type_table == NULL) {
        type_table = g_hash_table_new(g_str_hash, g_str_equal);
    }

    return type_table;
}

static bool enumerating_types;

//向类型hash表中添加指定type(取type名称做key,放入hash表中）
static void type_table_add(TypeImpl *ti)
{
    assert(!enumerating_types);
    g_hash_table_insert(type_table_get(), (void *)ti->name, ti);
}

//给定名称查询type
static TypeImpl *type_table_lookup(const char *name)
{
    return g_hash_table_lookup(type_table_get(), name);
}

//TypeInfo用于指明type的继承，大小等信息，每一个TypeInfo均会被
//转换为一个TypeImpl保存在type表中
//依据info构造一份TypeImpl数据
static TypeImpl *type_new(const TypeInfo *info)
{
    TypeImpl *ti = g_malloc0(sizeof(*ti));
    int i;

    //必须指明类型名称
    g_assert(info->name != NULL);

    //类型已存在，则注册失败，代码有误，abort
    if (type_table_lookup(info->name) != NULL) {
        fprintf(stderr, "Registering `%s' which already exists\n", info->name);
        abort();
    }

    //由TypeInfo构造TypeImpl（除接口信息外，其它均相同）
    ti->name = g_strdup(info->name);
    ti->parent = g_strdup(info->parent);

    ti->class_size = info->class_size;
    ti->instance_size = info->instance_size;
    ti->instance_align = info->instance_align;

    ti->class_init = info->class_init;
    ti->class_base_init = info->class_base_init;
    ti->class_data = info->class_data;

    ti->instance_init = info->instance_init;
    ti->instance_post_init = info->instance_post_init;
    ti->instance_finalize = info->instance_finalize;

    ti->abstract = info->abstract;

    //遍历type所有接口信息，设置接口类型名称
    for (i = 0; info->interfaces && info->interfaces[i].type; i++) {
        ti->interfaces[i].typename = g_strdup(info->interfaces[i].type);
    }
    ti->num_interfaces = i;

    return ti;
}

static bool type_name_is_valid(const char *name)
{
    const int slen = strlen(name);
    int plen;

    g_assert(slen > 1);

    /*
     * Ideally, the name should start with a letter - however, we've got
     * too many names starting with a digit already, so allow digits here,
     * too (except '0' which is not used yet)
     */
    if (!g_ascii_isalnum(name[0]) || name[0] == '0') {
        return false;
    }

    plen = strspn(name, "abcdefghijklmnopqrstuvwxyz"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                        "0123456789-_.");

    return plen == slen;
}

static TypeImpl *type_register_internal(const TypeInfo *info)
{
    TypeImpl *ti;

    if (!type_name_is_valid(info->name)) {
        fprintf(stderr, "Registering '%s' with illegal type name\n", info->name);
        abort();
    }

    ti = type_new(info);

    //加入hash表中
    type_table_add(ti);
    return ti;
}

//type注册函数
TypeImpl *type_register(const TypeInfo *info)
{
	//类型必须要有parent
    assert(info->parent);
    return type_register_internal(info);
}

//实现类型注册(等同于type_register,此函数应删除掉)
TypeImpl *type_register_static(const TypeInfo *info)
{
    return type_register(info);
}

//实现类型注册（容许一次性注册多个）
void type_register_static_array(const TypeInfo *infos, int nr_infos)
{
    int i;

    for (i = 0; i < nr_infos; i++) {
        type_register_static(&infos[i]);
    }
}

static TypeImpl *type_get_by_name_noload(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    return type_table_lookup(name);
}

static TypeImpl *type_get_or_load_by_name(const char *name, Error **errp)
{
    TypeImpl *type = type_get_by_name_noload(name);

#ifdef CONFIG_MODULES
    if (!type) {
        int rv = module_load_qom(name, errp);
        if (rv > 0) {
            type = type_get_by_name_noload(name);
        } else {
            error_prepend(errp, "could not load a module for type '%s'", name);
            return NULL;
        }
    }
#endif
    if (!type) {
        error_setg(errp, "unknown type '%s'", name);
    }

    return type;
}

//取指定TypeImpl的父TypeImpl
static TypeImpl *type_get_parent(TypeImpl *type)
{
	//如果parent_type未赋值，则查询并赋值，否则直接返回
    if (!type->parent_type && type->parent) {
        //设置父TypeImpl
        type->parent_type = type_get_by_name_noload(type->parent);
        if (!type->parent_type) {
            fprintf(stderr, "Type '%s' is missing its parent '%s'\n",
                    type->name, type->parent);
            abort();
        }
    }

    return type->parent_type;
}

//此类型是否有父类型
static bool type_has_parent(TypeImpl *type)
{
    return (type->parent != NULL);
}

//获取类型ti的ObjectClass大小
//如果自身class_size不为0，则使用，否则有父类型，使用父类型class_size
//无父类型则使用sizeof(ObjectClass)
static size_t type_class_get_size(TypeImpl *ti)
{
    if (ti->class_size) {
        //如果自身class_size不为零，则使用
        return ti->class_size;
    }

    //否则尝试使用父节点的class_size
    if (type_has_parent(ti)) {
        return type_class_get_size(type_get_parent(ti));
    }

    //如果没有父节点，则使用objectclass
    return sizeof(ObjectClass);
}

//获取类型ti的instance_size
//如果instance_size不为0,返回instance_size
//否则使用父类型的instance_size
static size_t type_object_get_size(TypeImpl *ti)
{
	//如果instance_size不为0,返回instance_size
    if (ti->instance_size) {
        return ti->instance_size;
    }

    //否则使用父类型的instance_size
    if (type_has_parent(ti)) {
        return type_object_get_size(type_get_parent(ti));
    }

    //如果没有父节点，则使用0
    return 0;
}

static size_t type_object_get_align(TypeImpl *ti)
{
    if (ti->instance_align) {
        return ti->instance_align;
    }

    if (type_has_parent(ti)) {
        return type_object_get_align(type_get_parent(ti));
    }

    return 0;
}

//检查给定的TypeImpl,是否继承自target_type类型或者就是target_type
static bool type_is_ancestor(TypeImpl *type, TypeImpl *target_type)
{
    assert(target_type);

    /* Check if target_type is a direct ancestor of type */
    while (type) {
    	//首次进入时，如果两者类型一致返回True，后续进入时，
    	//type继承自target_type类型
        if (type == target_type) {
            return true;
        }

        //取type的parent TypeImpl
        type = type_get_parent(type);
    }

    return false;
}

static void type_initialize(TypeImpl *ti);

//类型的接口初始化
static void type_initialize_interface(TypeImpl *ti, TypeImpl *interface_type,
                                      TypeImpl *parent_type)
{
    InterfaceClass *new_iface;
    TypeInfo info = { };
    TypeImpl *iface_impl;/*接口类型*/

    //将接口看成是名称为$ti->name::$interfacename的类型
    //接口是抽象类
    info.parent = parent_type->name;
    info.name = g_strdup_printf("%s::%s", ti->name, interface_type->name);
    info.abstract = true;

    //依据info创建接口类型的type_impl
    iface_impl = type_new(&info);
    iface_impl->parent_type = parent_type;
    //初始化此类型
    type_initialize(iface_impl);
    //销毁info
    g_free((char *)info.name);

    //实现函数继续
    new_iface = (InterfaceClass *)iface_impl->class;
    new_iface->concrete_class = ti->class;//类型归本类
    new_iface->interface_type = interface_type;//类型用上面的

    /*将iface_impl->class加入到interfaces结尾*/
    ti->class->interfaces = g_slist_append(ti->class->interfaces, new_iface);
}

//属性值销毁函数
static void object_property_free(gpointer data)
{
    ObjectProperty *prop = data;

    if (prop->defval) {
        qobject_unref(prop->defval);
        prop->defval = NULL;
    }
    g_free(prop->name);
    g_free(prop->type);
    g_free(prop->description);
    g_free(prop);
}

//type Objectclass初始化（可简单理解为对象的初始化，通过memcpy父类初始化好的内存来实现）
//根据自身的class_size,申请class空间，然后先调基类的class_init,将基类的class空间copy到自身，再调用自身的class_init。
//按Type类型的注释说明，为了消除memcopy的影响，引入class_base_init在自身class_init之前来解决。
static void type_initialize(TypeImpl *ti)
{
    TypeImpl *parent;

    //如果type对应的objectclass已初始化，则直接返回
    if (ti->class) {
        return;
    }

    //获取ObjectClass数据大小，实例（对象）大小
    ti->class_size = type_class_get_size(ti);
    ti->instance_size = type_object_get_size(ti);
    ti->instance_align = type_object_get_align(ti);
    /* Any type with zero instance_size is implicitly abstract.
     * This means interface types are all abstract.
     */
    if (ti->instance_size == 0) {
        //将实例大小为0，修改为抽象类
        ti->abstract = true;
    }

    if (type_is_ancestor(ti, type_interface)) {
        //ti继承自type_interface时，ti必须为抽象类
        assert(ti->instance_size == 0);
        assert(ti->abstract);
        assert(!ti->instance_init);
        assert(!ti->instance_post_init);
        assert(!ti->instance_finalize);
        assert(!ti->num_interfaces);
    }

    //生成ObjectClass需要的内存
    ti->class = g_malloc0(ti->class_size);

    parent = type_get_parent(ti);
    if (parent) {
    	//递归初始化父类型
        type_initialize(parent);
        GSList *e;
        int i;

        //将父类型实例化好的数据copy到自身class上来
        g_assert(parent->class_size <= ti->class_size);
        g_assert(parent->instance_size <= ti->instance_size);
        //父类的class占用的内存是从ti->class的首地址开始的，长度为parent->class_size
        //将父类型创建好的class copy到自身
        memcpy(ti->class, parent->class, parent->class_size);
        ti->class->interfaces = NULL;

        //如果父类有接口，遍历所有父类接口
        for (e = parent->class->interfaces; e; e = e->next) {
            /*取interface对应的class*/
            InterfaceClass *iface = e->data;
            //强转interfaceClass为基类(ObjectClass)
            ObjectClass *klass = OBJECT_CLASS(iface);

            type_initialize_interface(ti, iface->interface_type, klass->type);
        }

        //初始化本类的接口
        for (i = 0; i < ti->num_interfaces; i++) {
            //查询接口对应的类型
            TypeImpl *t = type_get_by_name_noload(ti->interfaces[i].typename);
            if (!t) {
                error_report("missing interface '%s' for object '%s'",
                             ti->interfaces[i].typename, parent->name);
                abort();
            }
            for (e = ti->class->interfaces; e; e = e->next) {
                TypeImpl *target_type = OBJECT_CLASS(e->data)->type;

                if (type_is_ancestor(target_type, t)) {
                    //已在继承链上存在，不处理
                    break;
                }
            }

            if (e) {
                continue;
            }

            //需要增加的新接口，以前没有
            type_initialize_interface(ti, t, t);
        }
    }

    //构造ObjectClass的属性表
    ti->class->properties = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                                  object_property_free);

    ti->class->type = ti;//覆盖顶层基类的type

    //自下而上，调用class_base_init
    //用于消除memcopy对父类的影响（重复的多次调用）
    while (parent) {
        if (parent->class_base_init) {
            parent->class_base_init(ti->class, ti->class_data);
        }
        parent = type_get_parent(parent);
    }

    //优先调用父类的class_init,然后再调用本类的class_init
    if (ti->class_init) {
        ti->class_init(ti->class, ti->class_data);
    }
}

//采有ti类型初始化对象obj
static void object_init_with_type(Object *obj, TypeImpl *ti)
{
	//如果ti有父类，则由父类先初始化此obj
    if (type_has_parent(ti)) {
        object_init_with_type(obj, type_get_parent(ti));
    }

    //通过此类型的构造函数，初始化obj
    if (ti->instance_init) {
        ti->instance_init(obj);
    }
}

//采用ti类型针对obj调用post_init
static void object_post_init_with_type(Object *obj, TypeImpl *ti)
{
	//调用post_init函数
    if (ti->instance_post_init) {
        ti->instance_post_init(obj);
    }

    //如果此类型有父类型，则调用父类型的post_init函数
    if (type_has_parent(ti)) {
        object_post_init_with_type(obj, type_get_parent(ti));
    }
}

bool object_apply_global_props(Object *obj, const GPtrArray *props,
                               Error **errp)
{
    int i;

    if (!props) {
        return true;
    }

    for (i = 0; i < props->len; i++) {
        GlobalProperty *p = g_ptr_array_index(props, i);
        Error *err = NULL;

        if (object_dynamic_cast(obj, p->driver) == NULL) {
            continue;
        }
        if (p->optional && !object_property_find(obj, p->property)) {
            continue;
        }
        p->used = true;
        if (!object_property_parse(obj, p->property, p->value, &err)) {
            error_prepend(&err, "can't apply global %s.%s=%s: ",
                          p->driver, p->property, p->value);
            /*
             * If errp != NULL, propagate error and return.
             * If errp == NULL, report a warning, but keep going
             * with the remaining globals.
             */
            if (errp) {
                error_propagate(errp, err);
                return false;
            } else {
                warn_report_err(err);
            }
        }
    }

    return true;
}

/*
 * Global property defaults
 * Slot 0: accelerator's global property defaults
 * Slot 1: machine's global property defaults
 * Slot 2: global properties from legacy command line option
 * Each is a GPtrArray of of GlobalProperty.
 * Applied in order, later entries override earlier ones.
 */
static GPtrArray *object_compat_props[3];

/*
 * Retrieve @GPtrArray for global property defined with options
 * other than "-global".  These are generally used for syntactic
 * sugar and legacy command line options.
 */
void object_register_sugar_prop(const char *driver, const char *prop,
                                const char *value, bool optional)
{
    GlobalProperty *g;
    if (!object_compat_props[2]) {
        object_compat_props[2] = g_ptr_array_new();
    }
    g = g_new0(GlobalProperty, 1);
    g->driver = g_strdup(driver);
    g->property = g_strdup(prop);
    g->value = g_strdup(value);
    g->optional = optional;
    g_ptr_array_add(object_compat_props[2], g);
}

/*
 * Set machine's global property defaults to @compat_props.
 * May be called at most once.
 */
void object_set_machine_compat_props(GPtrArray *compat_props)
{
    assert(!object_compat_props[1]);
    object_compat_props[1] = compat_props;
}

/*
 * Set accelerator's global property defaults to @compat_props.
 * May be called at most once.
 */
void object_set_accelerator_compat_props(GPtrArray *compat_props)
{
    assert(!object_compat_props[0]);
    object_compat_props[0] = compat_props;
}

void object_apply_compat_props(Object *obj)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(object_compat_props); i++) {
        object_apply_global_props(obj, object_compat_props[i],
                                  i == 2 ? &error_fatal : &error_abort);
    }
}

//初始化ObjectClass的所有属性
static void object_class_property_init_all(Object *obj)
{
    ObjectPropertyIterator iter;
    ObjectProperty *prop;

    object_class_property_iter_init(&iter, object_get_class(obj));
    //遍历ObjectClass对应的所有prop及其父类的prop,针对每一个prop执行init函数
    while ((prop = object_property_iter_next(&iter))) {
        if (prop->init) {
            prop->init(obj, prop);
        }
    }
}

//object初始化
//@data 要初始化的对象
//@size 要初始化的对象内存长度
//@type 要初始化的对象属于那种类型
static void object_initialize_with_type(Object *obj/*待初始化的obj*/, size_t size, TypeImpl *type)
{
    //防止type的ObjectClass未初始化
    type_initialize(type);

    //可实例化的object都是object的子类，故大小必大于Object
    g_assert(type->instance_size >= sizeof(Object));
    g_assert(type->abstract == false);//可实例化的对象不可能是抽象类
    g_assert(size >= type->instance_size);//size一定是大于等于类型的实例size的，否则内存可能越界

    memset(obj, 0, type->instance_size);
    obj->class = type->class;//指明对象所属的class
    object_ref(obj);//增加对象的引用计数
    //初始化ObjectClass的所有属性
    object_class_property_init_all(obj);

    //初始化对象的属性表（key没有销毁函数）
    obj->properties = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            NULL, object_property_free);
    object_init_with_type(obj, type);//调用构造函数列表初始化对象
    object_post_init_with_type(obj, type);//自底向上调用instance_post_init回调
    //obj初始化完成
}

//已知类型名称的对象初始化
void object_initialize(void *data, size_t size, const char *typename)
{
    //通过类型名称，找到其对应的TypeImpl
    TypeImpl *type = type_get_or_load_by_name(typename, &error_fatal);

    object_initialize_with_type(data, size, type);
}

bool object_initialize_child_with_props(Object *parentobj,
                                        const char *propname,
                                        void *childobj, size_t size,
                                        const char *type,
                                        Error **errp, ...)
{
    va_list vargs;
    bool ok;

    va_start(vargs, errp);
    ok = object_initialize_child_with_propsv(parentobj, propname,
                                             childobj, size, type, errp,
                                             vargs);
    va_end(vargs);
    return ok;
}

bool object_initialize_child_with_propsv(Object *parentobj,
                                         const char *propname,
                                         void *childobj, size_t size,
                                         const char *type,
                                         Error **errp, va_list vargs)
{
    bool ok = false;
    Object *obj;
    UserCreatable *uc;

    object_initialize(childobj, size, type);
    obj = OBJECT(childobj);

    if (!object_set_propv(obj, errp, vargs)) {
        goto out;
    }

    object_property_add_child(parentobj, propname, obj);

    uc = (UserCreatable *)object_dynamic_cast(obj, TYPE_USER_CREATABLE);
    if (uc) {
        if (!user_creatable_complete(uc, errp)) {
            object_unparent(obj);
            goto out;
        }
    }

    ok = true;

out:
    /*
     * We want @obj's reference to be 1 on success, 0 on failure.
     * On success, it's 2: one taken by object_initialize(), and one
     * by object_property_add_child().
     * On failure in object_initialize() or earlier, it's 1.
     * On failure afterwards, it's also 1: object_unparent() releases
     * the reference taken by object_property_add_child().
     */
    object_unref(obj);
    return ok;
}

void object_initialize_child_internal(Object *parent,
                                      const char *propname,
                                      void *child, size_t size,
                                      const char *type)
{
    object_initialize_child_with_props(parent, propname, child, size, type,
                                       &error_abort, NULL);
}

static inline bool object_property_is_child(ObjectProperty *prop)
{
	//检查prop->type是否以"child<"开头
    return strstart(prop->type, "child<", NULL);
}

//删除对象所有属性
static void object_property_del_all(Object *obj)
{
    g_autoptr(GHashTable) done = g_hash_table_new(NULL, NULL);
    ObjectProperty *prop;
    ObjectPropertyIterator iter;
    bool released;

    do {
        released = false;
        object_property_iter_init(&iter, obj);
        while ((prop = object_property_iter_next(&iter)) != NULL) {
            if (g_hash_table_add(done, prop)) {
            	//释放属性值
                if (prop->release) {
                    prop->release(obj, prop->name, prop->opaque);
                    released = true;
                    break;
                }
            }
        }
    //如果本次有释放，则续续，跳出时再无属性
    } while (released);

    g_hash_table_unref(obj->properties);
}

static void object_property_del_child(Object *obj, Object *child)
{
    ObjectProperty *prop;
    GHashTableIter iter;
    gpointer key, value;

    //遍历属性表
    g_hash_table_iter_init(&iter, obj->properties);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        prop = value;
        if (object_property_is_child(prop) && prop->opaque == child) {
            if (prop->release) {
                prop->release(obj, prop->name, prop->opaque);
                prop->release = NULL;
            }
            break;
        }
    }
    g_hash_table_iter_init(&iter, obj->properties);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        prop = value;
        if (object_property_is_child(prop) && prop->opaque == child) {
            g_hash_table_iter_remove(&iter);
            break;
        }
    }
}

void object_unparent(Object *obj)
{
    if (obj->parent) {
        object_property_del_child(obj->parent, obj);
    }
}

//调用对象的析构函数
static void object_deinit(Object *obj, TypeImpl *type)
{
	//析构函数与构造函数的顺序是相反的，且递归调用。
    if (type->instance_finalize) {
        type->instance_finalize(obj);
    }

    if (type_has_parent(type)) {
        object_deinit(obj, type_get_parent(type));
    }
}

//实现对象销毁
static void object_finalize(void *data)
{
    Object *obj = data;
    TypeImpl *ti = obj->class->type;

    //1.移除对象所有属性
    object_property_del_all(obj);
    //2.调用对象的析构函数
    object_deinit(obj, ti);

    //3.释放对象
    g_assert(obj->ref == 0);
    g_assert(obj->parent == NULL);
    if (obj->free) {
        obj->free(obj);
    }
}

/* Find the minimum alignment guaranteed by the system malloc. */
#if __STDC_VERSION__ >= 201112L
typedef max_align_t qemu_max_align_t;
#else
typedef union {
    long l;
    void *p;
    double d;
    long double ld;
} qemu_max_align_t;
#endif

//针对类型type ,new一个对象
static Object *object_new_with_type(Type type)
{
    Object *obj;
    size_t size, align;
    void (*obj_free)(void *);

    g_assert(type != NULL);
    //初始化type对应的ObjectClass
    type_initialize(type);

    size = type->instance_size;
    align = type->instance_align;

    /*
     * Do not use qemu_memalign unless required.  Depending on the
     * implementation, extra alignment implies extra overhead.
     */
    if (likely(align <= __alignof__(qemu_max_align_t))) {
    	//申请此类型obj所需要的合适内存
        obj = g_malloc(size);
        obj_free = g_free;
    } else {
        obj = qemu_memalign(align, size);
        obj_free = qemu_vfree;
    }

    //利用此类型初始化对象
    object_initialize_with_type(obj, size, type);
    obj->free = obj_free;

    return obj;
}

//给定class，构造其对应对象
Object *object_new_with_class(ObjectClass *klass)
{
    return object_new_with_type(klass->type);
}

//给定类型名称，构造对象
Object *object_new(const char *typename)
{
    //1。获得此type对应的TypeImpl
    TypeImpl *ti = type_get_or_load_by_name(typename, &error_fatal);

    //2.通过TypeImpl实例化object
    return object_new_with_type(ti);
}


//含属性的对象new（不定参）
Object *object_new_with_props(const char *typename,
                              Object *parent,
                              const char *id,
                              Error **errp,
                              ...)
{
    va_list vargs;
    Object *obj;

    va_start(vargs, errp);
    obj = object_new_with_propv(typename, parent, id, errp, vargs);
    va_end(vargs);

    return obj;
}

//含属性的对象new（va_list参数）
Object *object_new_with_propv(const char *typename,
                              Object *parent,
                              const char *id,
                              Error **errp,
                              va_list vargs)
{
    Object *obj;
    ObjectClass *klass;
    UserCreatable *uc;

    //通过typename找到此类型的ObjectClass数据
    klass = object_class_by_name(typename);
    if (!klass) {
        error_setg(errp, "invalid object type: %s", typename);
        return NULL;
    }

    //对抽象类返回NULL
    if (object_class_is_abstract(klass)) {
        error_setg(errp, "object type '%s' is abstract", typename);
        return NULL;
    }
    //构造此类型的对象
    obj = object_new_with_type(klass->type);

    //为对象设置属性
    if (!object_set_propv(obj, errp, vargs)) {
        goto error;
    }

    if (id != NULL) {
        object_property_add_child(parent, id, obj);
    }

    uc = (UserCreatable *)object_dynamic_cast(obj, TYPE_USER_CREATABLE);
    if (uc) {
        if (!user_creatable_complete(uc, errp)) {
            if (id != NULL) {
                object_unparent(obj);
            }
            goto error;
        }
    }

    object_unref(obj);
    return obj;

 error:
    object_unref(obj);
    return NULL;
}


bool object_set_props(Object *obj,
                     Error **errp,
                     ...)
{
    va_list vargs;
    bool ret;

    va_start(vargs, errp);
    ret = object_set_propv(obj, errp, vargs);
    va_end(vargs);

    return ret;
}

//通过vargs设置属性值（vargs的格式为 <char* prop_name,char*prop_value>
bool object_set_propv(Object *obj,
                     Error **errp,
                     va_list vargs)
{
    const char *propname;

    //取属性名称
    propname = va_arg(vargs, char *);
    while (propname != NULL) {
    	//当前存在属性名，再取属性值
        const char *value = va_arg(vargs, char *);

        g_assert(value != NULL);
        //设置obj中属性名称为propname的属性，其值为value（字符串形式）
        if (!object_property_parse(obj, propname, value, errp)) {
            return false;
        }
        //下一个属性名称
        propname = va_arg(vargs, char *);
    }

    return true;
}

//通过此函数可以将obj转换为类型typename
//如果obj可转换为typename类型，则返回obj,否则返回NULL
Object *object_dynamic_cast(Object *obj, const char *typename)
{
    if (obj && object_class_dynamic_cast(object_get_class(obj), typename)) {
        return obj;
    }

    return NULL;
}

//如果可转换，则返回obj
Object *object_dynamic_cast_assert(Object *obj, const char *typename,
                                   const char *file, int line, const char *func)
{
    trace_object_dynamic_cast_assert(obj ? obj->class->type->name : "(null)",
                                     typename, file, line, func);

#ifdef CONFIG_QOM_CAST_DEBUG
    int i;
    Object *inst;

    for (i = 0; obj && i < OBJECT_CLASS_CAST_CACHE; i++) {
        if (qatomic_read(&obj->class->object_cast_cache[i]) == typename) {
            goto out;
        }
    }

    inst = object_dynamic_cast(obj, typename);

    if (!inst && obj) {
    	//不可强转，报错，挂掉
        fprintf(stderr, "%s:%d:%s: Object %p is not an instance of type %s\n",
                file, line, func, obj, typename);
        abort();
    }

    assert(obj == inst);

    if (obj && obj == inst) {
        for (i = 1; i < OBJECT_CLASS_CAST_CACHE; i++) {
            qatomic_set(&obj->class->object_cast_cache[i - 1],
                       qatomic_read(&obj->class->object_cast_cache[i]));
        }
        qatomic_set(&obj->class->object_cast_cache[i - 1], typename);
    }

out:
#endif
    return obj;
}

//返回NULL表示不能强转，返回！NULL表示可强转
ObjectClass *object_class_dynamic_cast(ObjectClass *class,
                                       const char *typename)
{
    ObjectClass *ret = NULL;
    TypeImpl *target_type;
    TypeImpl *type;

    if (!class) {
        return NULL;
    }

    /* A simple fast path that can trigger a lot for leaf classes.  */
    //如果type->name == typename,则检查的是最终的类类型（leaf class)
    type = class->type;
    if (type->name == typename) {
        return class;
    }

    //非最终类类型（1。检查是否有这种类型，如无，失败）
    target_type = type_get_by_name_noload(typename);
    if (!target_type) {
        /* target class type unknown, so fail the cast */
        return NULL;
    }

    if (type->class->interfaces &&
            type_is_ancestor(target_type, type_interface)) {
        int found = 0;
        GSList *i;

        for (i = class->interfaces; i; i = i->next) {
            ObjectClass *target_class = i->data;

            if (type_is_ancestor(target_class->type, target_type)) {
                ret = target_class;
                found++;
            }
         }

        /* The match was ambiguous, don't allow a cast */
        if (found > 1) {
            ret = NULL;
        }
    } else if (type_is_ancestor(type, target_type)) {
    	    //继承链上的中间节点
        ret = class;
    }

    return ret;
}

//确认class的类型是否可强转为typename
ObjectClass *object_class_dynamic_cast_assert(ObjectClass *class,
                                              const char *typename,
                                              const char *file, int line,
                                              const char *func)
{
    ObjectClass *ret;

    trace_object_class_dynamic_cast_assert(class ? class->type->name : "(null)",
                                           typename, file, line, func);

#ifdef CONFIG_QOM_CAST_DEBUG
    int i;

    for (i = 0; class && i < OBJECT_CLASS_CAST_CACHE; i++) {
        if (qatomic_read(&class->class_cast_cache[i]) == typename) {
            ret = class;
            goto out;
        }
    }
#else
    if (!class || !class->interfaces) {
        return class;
    }
#endif

    ret = object_class_dynamic_cast(class, typename);
    if (!ret && class) {
        fprintf(stderr, "%s:%d:%s: Object %p is not an instance of type %s\n",
                file, line, func, class, typename);
        abort();
    }

#ifdef CONFIG_QOM_CAST_DEBUG
    if (class && ret == class) {
        for (i = 1; i < OBJECT_CLASS_CAST_CACHE; i++) {
            qatomic_set(&class->class_cast_cache[i - 1],
                       qatomic_read(&class->class_cast_cache[i]));
        }
        qatomic_set(&class->class_cast_cache[i - 1], typename);
    }
out:
#endif
    return ret;
}

//获得Object对应的类型名称
const char *object_get_typename(const Object *obj)
{
    return obj->class->type->name;
}

//取对象对应的ObjectClass
ObjectClass *object_get_class(Object *obj)
{
    return obj->class;
}

//检查此ObjectClass是否为抽象类
bool object_class_is_abstract(ObjectClass *klass)
{
    return klass->type->abstract;
}

//通过ObjectClass获得类型名称
const char *object_class_get_name(ObjectClass *klass)
{
    return klass->type->name;
}

//通过typename找ObjectClass
ObjectClass *object_class_by_name(const char *typename)
{
    //通过type找到TypeImpl
    TypeImpl *type = type_get_by_name_noload(typename);

    if (!type) {
        return NULL;
    }

    type_initialize(type);//初始化此类型的class

    return type->class;
}

ObjectClass *module_object_class_by_name(const char *typename)
{
    TypeImpl *type = type_get_or_load_by_name(typename, NULL);

    if (!type) {
        return NULL;
    }

    type_initialize(type);

    return type->class;
}

//取class的父节点
ObjectClass *object_class_get_parent(ObjectClass *class)
{
    TypeImpl *type = type_get_parent(class->type);

    if (!type) {
        return NULL;
    }

    type_initialize(type);

    return type->class;
}

typedef struct OCFData
{
    void (*fn)(ObjectClass *klass, void *opaque);//遍历函数
    const char *implements_type;//限制的实现基类（遍历时，如果类型不为其的子类，则不遍历）
    bool include_abstract;//是否遍历抽象结构
    void *opaque;//用户为回调注册的参数
} OCFData;

//此hash表针对全局的type hash表进行key,value的比对
static void object_class_foreach_tramp(gpointer key, gpointer value,
                                       gpointer opaque)
{
    OCFData *data = opaque;
    TypeImpl *type = value;/*全局type hash表存的value为TypeImpl类型*/
    ObjectClass *k;

    type_initialize(type);
    k = type->class;

    //指明不遍历抽象类型，本type为抽象类，则直接返回
    if (!data->include_abstract && type->abstract) {
        return;
    }

    //指明了基类，但类型不能强转为基类，则直接返回
    if (data->implements_type && 
        !object_class_dynamic_cast(k, data->implements_type)) {
        return;
    }

    //遍历
    data->fn(k, data->opaque);
}

//通过函数遍历type hash表
void object_class_foreach(void (*fn)(ObjectClass *klass, void *opaque)/*遍历函数*/,
                          const char *implements_type/*是否遍历子类*/, bool include_abstract/*是否遍历抽象类*/,
                          void *opaque/*遍历参数*/)
{
    OCFData data = { fn, implements_type, include_abstract, opaque };

    enumerating_types = true;
    //按函数 object_class_foreach_tramp遍历全局的type hash表
    g_hash_table_foreach(type_table_get(), object_class_foreach_tramp, &data);
    enumerating_types = false;
}

static int do_object_child_foreach(Object *obj,
                                   int (*fn)(Object *child, void *opaque),
                                   void *opaque, bool recurse)
{
    GHashTableIter iter;
    ObjectProperty *prop;
    int ret = 0;

    g_hash_table_iter_init(&iter, obj->properties);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&prop)) {
        if (object_property_is_child(prop)) {
            Object *child = prop->opaque;

            ret = fn(child, opaque);
            if (ret != 0) {
                break;
            }
            if (recurse) {
                ret = do_object_child_foreach(child, fn, opaque, true);
                if (ret != 0) {
                    break;
                }
            }
        }
    }
    return ret;
}

int object_child_foreach(Object *obj, int (*fn)(Object *child, void *opaque),
                         void *opaque)
{
    return do_object_child_foreach(obj, fn, opaque, false);
}

int object_child_foreach_recursive(Object *obj,
                                   int (*fn)(Object *child, void *opaque),
                                   void *opaque)
{
    return do_object_child_foreach(obj, fn, opaque, true);
}

/*将klass加入到list中*/
static void object_class_get_list_tramp(ObjectClass *klass, void *opaque)
{
    GSList **list = opaque;

    *list = g_slist_prepend(*list, klass);
}

//查找系统中所有 implements_type的基类及子类,并形成list返回
GSList *object_class_get_list(const char *implements_type/*要查询的基类名称*/,
                              bool include_abstract/*是否包含抽象类*/)
{
    GSList *list = NULL;

    object_class_foreach(object_class_get_list_tramp,
                         implements_type, include_abstract, &list);
    return list;
}

static gint object_class_cmp(gconstpointer a, gconstpointer b)
{
    return strcasecmp(object_class_get_name((ObjectClass *)a),
                      object_class_get_name((ObjectClass *)b));
}

GSList *object_class_get_list_sorted(const char *implements_type,
                                     bool include_abstract)
{
    return g_slist_sort(object_class_get_list(implements_type, include_abstract),
                        object_class_cmp);
}

//增加对象的引用计数
Object *object_ref(void *objptr)
{
    Object *obj = OBJECT(objptr);
    uint32_t ref;

    if (!obj) {
        return NULL;
    }
    ref = qatomic_fetch_inc(&obj->ref);
    /* Assert waaay before the integer overflows */
    g_assert(ref < INT_MAX);
    return obj;
}

void object_unref(void *objptr)
{
    Object *obj = OBJECT(objptr);
    if (!obj) {
        return;
    }
    g_assert(obj->ref > 0);

    /* parent always holds a reference to its children */
    if (qatomic_fetch_dec(&obj->ref) == 1) {
        object_finalize(obj);
    }
}

//向obj中添加属性
ObjectProperty *
object_property_try_add(Object *obj/*要添加属性的名称*/, const char *name/*属性名称*/, const char *type/*属性类型*/,
                        ObjectPropertyAccessor *get,
                        ObjectPropertyAccessor *set,
                        ObjectPropertyRelease *release,
                        void *opaque, Error **errp)
{
    ObjectProperty *prop;
    /*属性名称长度*/
    size_t name_len = strlen(name);

    //属性名称以'[*]'结尾的情况
    if (name_len >= 3 && !memcmp(name + name_len - 3, "[*]", 4)) {
        /*属性名称以[*]结尾的*/
        int i;
        ObjectProperty *ret = NULL;
        char *name_no_array = g_strdup(name);

        name_no_array[name_len - 3] = '\0';
        for (i = 0; i < INT16_MAX; ++i) {
            char *full_name = g_strdup_printf("%s[%d]", name_no_array, i);

            /*如果full_name的已存在，则增加i并尝试下一个，直到full_name不存在并填加成功*/
            ret = object_property_try_add(obj, full_name, type, get, set,
                                          release, opaque, NULL);
            g_free(full_name);
            if (ret) {
                break;
            }
        }
        g_free(name_no_array);
        assert(ret);
        return ret;
    }

    //检查属性是否已在obj中存在，如存在，则返回NULL
    if (object_property_find(obj, name) != NULL) {
        error_setg(errp, "attempt to add duplicate property '%s' to object (type '%s')",
                   name, object_get_typename(obj));
        return NULL;
    }

    /*构造prop,并将其加入到obj的properties表里*/
    prop = g_malloc0(sizeof(*prop));

    prop->name = g_strdup(name);
    prop->type = g_strdup(type);

    prop->get = get;
    prop->set = set;
    prop->release = release;
    prop->opaque = opaque;

    /*obj属性添加*/
    g_hash_table_insert(obj->properties, prop->name, prop);
    return prop;
}

//向ObjectClass中添加属性
ObjectProperty *
object_property_add(Object *obj, const char *name, const char *type,
                    ObjectPropertyAccessor *get,
                    ObjectPropertyAccessor *set,
                    ObjectPropertyRelease *release,
                    void *opaque)
{
    return object_property_try_add(obj, name, type, get, set, release,
                                   opaque, &error_abort);
}

ObjectProperty *
object_class_property_add(ObjectClass *klass,
                          const char *name/*属性名称*/,
                          const char *type/*属性类型*/,
                          ObjectPropertyAccessor *get/*属性get函数*/,
                          ObjectPropertyAccessor *set/*属性set函数*/,
                          ObjectPropertyRelease *release/*属性释放函数*/,
                          void *opaque/*访问函数不透明参数*/)
{
    ObjectProperty *prop;

    //1.检查objectclass中是否存在此属性
    assert(!object_class_property_find(klass, name));

    //2。申请内存，填写属性名称及类型
    prop = g_malloc0(sizeof(*prop));

    prop->name = g_strdup(name);
    prop->type = g_strdup(type);

    //3.设置属性get,set，及释放函数
    prop->get = get;
    prop->set = set;
    prop->release = release;
    prop->opaque = opaque;

    //4.插入到属性表
    g_hash_table_insert(klass->properties, prop->name, prop);

    return prop;
}

//在对象中查找属性（含类静态变量）
ObjectProperty *object_property_find(Object *obj, const char *name)
{
    ObjectProperty *prop;
    ObjectClass *klass = object_get_class(obj);

    //在klass中查找名称为name的属性（类变量）
    prop = object_class_property_find(klass, name);
    if (prop) {
        return prop;
    }

    //对象属性查找
    return g_hash_table_lookup(obj->properties, name);
}

ObjectProperty *object_property_find_err(Object *obj, const char *name,
                                         Error **errp)
{
    ObjectProperty *prop = object_property_find(obj, name);
    if (!prop) {
        error_setg(errp, "Property '%s.%s' not found",
                   object_get_typename(obj), name);
    }
    return prop;
}

void object_property_iter_init(ObjectPropertyIterator *iter,
                               Object *obj)
{
    g_hash_table_iter_init(&iter->iter, obj->properties);
    iter->nextclass = object_get_class(obj);
}

//返回迭代器对应的value
ObjectProperty *object_property_iter_next(ObjectPropertyIterator *iter)
{
    gpointer key, val;
    while (!g_hash_table_iter_next(&iter->iter, &key, &val)) {
        /*没有找到新的元素，迭代父节点*/
        if (!iter->nextclass) {
            /*如果nextClass为NULL，则返回NULL*/
            return NULL;
        }
        //再用父节点初始化迭代器
        g_hash_table_iter_init(&iter->iter, iter->nextclass->properties);
        iter->nextclass = object_class_get_parent(iter->nextclass);
    }
    return val;
}

//初始化objectClass属性迭代器
void object_class_property_iter_init(ObjectPropertyIterator *iter,
                                     ObjectClass *klass)
{
    g_hash_table_iter_init(&iter->iter, klass->properties);
    iter->nextclass = object_class_get_parent(klass);
}

//在klass中查找属性名称name,出错信息存入errp （类属性查找）
ObjectProperty *object_class_property_find(ObjectClass *klass, const char *name/*属性名称*/)
{
    ObjectClass *parent_klass;

    //先递归从父节点中查找指定属性，如果找到就返回
    parent_klass = object_class_get_parent(klass);
    if (parent_klass) {
        ObjectProperty *prop =
            object_class_property_find(parent_klass, name);
        if (prop) {
            return prop;
        }
    }

    //父节点中未找到指定属性，查找自身属性表
    return g_hash_table_lookup(klass->properties, name);
}

ObjectProperty *object_class_property_find_err(ObjectClass *klass,
                                               const char *name,
                                               Error **errp)
{
    ObjectProperty *prop = object_class_property_find(klass, name);
    if (!prop) {
    	//查找失败时，报错，并返回NULL
        error_setg(errp, "Property '.%s' not found", name);
    }
    return prop;
}

//对象属性删除（仅可以删除自身属性表中的属性）
void object_property_del(Object *obj, const char *name)
{
    ObjectProperty *prop = g_hash_table_lookup(obj->properties, name);

    //释放，并移除
    if (prop->release) {
        prop->release(obj, name, prop->opaque);
    }
    g_hash_table_remove(obj->properties, name);
}

//通过get取属性值
bool object_property_get(Object *obj, const char *name, Visitor *v,
                         Error **errp)
{
    Error *err = NULL;
    ObjectProperty *prop = object_property_find_err(obj, name, errp);

    //查找object中名称为name的属性
    if (prop == NULL) {
        return false;
    }

    if (!prop->get) {
        /*如果属性无get回调，则报错*/
        error_setg(errp, "Property '%s.%s' is not readable",
                   object_get_typename(obj), name);
        return false;
    }
    //通过get回调获属性值
    prop->get(obj, v, name, prop->opaque, &err);
    error_propagate(errp, err);
    return !err;
}

//Object属性设置
bool object_property_set(Object *obj, const char *name, Visitor *v,
                         Error **errp)
{
    ERRP_GUARD();
    //如果名称为name的属性不存在，则返回
    ObjectProperty *prop = object_property_find_err(obj, name, errp);

    if (prop == NULL) {
        return false;
    }

    //如果名称为name的属性存在，但没有set函数，则报错
    if (!prop->set) {
        error_setg(errp, "Property '%s.%s' is not writable",
                   object_get_typename(obj), name);
        return false;
    }
    //调用属性的set函数，设置此属性
    prop->set(obj, v, name, prop->opaque, errp);
    return !*errp;
}

bool object_property_set_str(Object *obj, const char *name,
                             const char *value, Error **errp)
{
    QString *qstr = qstring_from_str(value);
    bool ok = object_property_set_qobject(obj, name, QOBJECT(qstr), errp);

    qobject_unref(qstr);
    return ok;
}

char *object_property_get_str(Object *obj, const char *name,
                              Error **errp)
{
    QObject *ret = object_property_get_qobject(obj, name, errp);
    QString *qstring;
    char *retval;

    if (!ret) {
        return NULL;
    }
    qstring = qobject_to(QString, ret);
    if (!qstring) {
        error_setg(errp, "Invalid parameter type for '%s', expected: string",
                   name);
        retval = NULL;
    } else {
        retval = g_strdup(qstring_get_str(qstring));
    }

    qobject_unref(ret);
    return retval;
}

bool object_property_set_link(Object *obj, const char *name,
                              Object *value, Error **errp)
{
    g_autofree char *path = NULL;

    if (value) {
        path = object_get_canonical_path(value);
    }
    return object_property_set_str(obj, name, path ?: "", errp);
}

Object *object_property_get_link(Object *obj, const char *name,
                                 Error **errp)
{
    char *str = object_property_get_str(obj, name, errp);
    Object *target = NULL;

    if (str && *str) {
        target = object_resolve_path(str, NULL);
        if (!target) {
            error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                      "Device '%s' not found", str);
        }
    }

    g_free(str);
    return target;
}

bool object_property_set_bool(Object *obj, const char *name/*属性名*/,
                              bool value/*bool属性值*/, Error **errp)
{
    /*构造bool值*/
    QBool *qbool = qbool_from_bool(value);
    bool ok = object_property_set_qobject(obj, name, QOBJECT(qbool), errp);

    qobject_unref(qbool);
    return ok;
}

bool object_property_get_bool(Object *obj, const char *name,
                              Error **errp)
{
    QObject *ret = object_property_get_qobject(obj, name, errp);
    QBool *qbool;
    bool retval;

    if (!ret) {
        return false;
    }
    qbool = qobject_to(QBool, ret);
    if (!qbool) {
        error_setg(errp, "Invalid parameter type for '%s', expected: boolean",
                   name);
        retval = false;
    } else {
        retval = qbool_get_bool(qbool);
    }

    qobject_unref(ret);
    return retval;
}

bool object_property_set_int(Object *obj, const char *name,
                             int64_t value, Error **errp)
{
    QNum *qnum = qnum_from_int(value);
    bool ok = object_property_set_qobject(obj, name, QOBJECT(qnum), errp);

    qobject_unref(qnum);
    return ok;
}

int64_t object_property_get_int(Object *obj, const char *name,
                                Error **errp)
{
    QObject *ret = object_property_get_qobject(obj, name, errp);
    QNum *qnum;
    int64_t retval;

    if (!ret) {
        return -1;
    }

    qnum = qobject_to(QNum, ret);
    if (!qnum || !qnum_get_try_int(qnum, &retval)) {
        error_setg(errp, "Invalid parameter type for '%s', expected: int",
                   name);
        retval = -1;
    }

    qobject_unref(ret);
    return retval;
}

static void object_property_init_defval(Object *obj, ObjectProperty *prop)
{
    Visitor *v = qobject_input_visitor_new(prop->defval);

    assert(prop->set != NULL);
    prop->set(obj, v, prop->name, prop->opaque, &error_abort);

    visit_free(v);
}

static void object_property_set_default(ObjectProperty *prop, QObject *defval)
{
    assert(!prop->defval);
    assert(!prop->init);

    prop->defval = defval;
    prop->init = object_property_init_defval;
}

void object_property_set_default_bool(ObjectProperty *prop, bool value)
{
    object_property_set_default(prop, QOBJECT(qbool_from_bool(value)));
}

void object_property_set_default_str(ObjectProperty *prop, const char *value)
{
    object_property_set_default(prop, QOBJECT(qstring_from_str(value)));
}

void object_property_set_default_list(ObjectProperty *prop)
{
    object_property_set_default(prop, QOBJECT(qlist_new()));
}

void object_property_set_default_int(ObjectProperty *prop, int64_t value)
{
    object_property_set_default(prop, QOBJECT(qnum_from_int(value)));
}

void object_property_set_default_uint(ObjectProperty *prop, uint64_t value)
{
    object_property_set_default(prop, QOBJECT(qnum_from_uint(value)));
}

bool object_property_set_uint(Object *obj, const char *name,
                              uint64_t value, Error **errp)
{
    QNum *qnum = qnum_from_uint(value);
    bool ok = object_property_set_qobject(obj, name, QOBJECT(qnum), errp);

    qobject_unref(qnum);
    return ok;
}

//取Object中name属性值（此值为无符号整数）
uint64_t object_property_get_uint(Object *obj, const char *name,
                                  Error **errp)
{
    //取object中的name属性值
    QObject *ret = object_property_get_qobject(obj, name, errp);
    QNum *qnum;
    uint64_t retval;

    if (!ret) {
        return 0;
    }
    //将ret转为QNum类型
    qnum = qobject_to(QNum, ret);

    //取QNum中保存的无符号整数值
    if (!qnum || !qnum_get_try_uint(qnum, &retval)) {
        //获取属性值失败，报错
        error_setg(errp, "Invalid parameter type for '%s', expected: uint",
                   name);
        retval = 0;
    }

    qobject_unref(ret);
    //返回此值
    return retval;
}

typedef struct EnumProperty {
    const QEnumLookup *lookup;
    int (*get)(Object *, Error **);
    void (*set)(Object *, int, Error **);
} EnumProperty;

int object_property_get_enum(Object *obj, const char *name,
                             const char *typename, Error **errp)
{
    char *str;
    int ret;
    ObjectProperty *prop = object_property_find_err(obj, name, errp);
    EnumProperty *enumprop;

    if (prop == NULL) {
        return -1;
    }

    if (!g_str_equal(prop->type, typename)) {
        error_setg(errp, "Property %s on %s is not '%s' enum type",
                   name, object_class_get_name(
                       object_get_class(obj)), typename);
        return -1;
    }

    enumprop = prop->opaque;

    str = object_property_get_str(obj, name, errp);
    if (!str) {
        return -1;
    }

    ret = qapi_enum_parse(enumprop->lookup, str, -1, errp);
    g_free(str);

    return ret;
}

//对象属性值解析，并调用属性的set设置其value
bool object_property_parse(Object *obj, const char *name,
                           const char *string, Error **errp)
{
    //通过string构造Visitor，以便可以解析string
    Visitor *v = string_input_visitor_new(string);
    bool ok = object_property_set(obj, name, v, errp);

    visit_free(v);
    return ok;
}

char *object_property_print(Object *obj, const char *name, bool human,
                            Error **errp)
{
    Visitor *v;
    char *string = NULL;

    v = string_output_visitor_new(human, &string);
    if (!object_property_get(obj, name, v, errp)) {
        goto out;
    }

    visit_complete(v, &string);

out:
    visit_free(v);
    return string;
}

const char *object_property_get_type(Object *obj, const char *name, Error **errp)
{
    ObjectProperty *prop = object_property_find_err(obj, name, errp);
    if (prop == NULL) {
        return NULL;
    }

    return prop->type;
}

//获得root
Object *object_get_root(void)
{
    static Object *root;

    if (!root) {
        //如果root不存在，则创建一个container类型的对象
        root = object_new("container");
    }

    return root;
}

Object *object_get_objects_root(void)
{
    return container_get(object_get_root(), "/objects");
}

/*构造一个container类型对象*/
Object *object_get_internal_root(void)
{
    static Object *internal_root;

    if (!internal_root) {
        internal_root = object_new("container");
    }

    return internal_root;
}

//返回obj的名称为name的child属性
static void object_get_child_property(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    Object *child = opaque;
    char *path;

    path = object_get_canonical_path(child);
    visit_type_str(v, name, &path, errp);
    g_free(path);
}

static Object *object_resolve_child_property(Object *parent, void *opaque,
                                             const char *part)
{
    return opaque;
}

static void object_finalize_child_property(Object *obj, const char *name,
                                           void *opaque)
{
    Object *child = opaque;

    if (child->class->unparent) {
        (child->class->unparent)(child);
    }
    child->parent = NULL;
    object_unref(child);
}

ObjectProperty *
object_property_try_add_child(Object *obj, const char *name,
                              Object *child, Error **errp)
{
    g_autofree char *type = NULL;
    ObjectProperty *op;

    assert(!child->parent);

    type = g_strdup_printf("child<%s>", object_get_typename(child));

    //向obj中添加属性name，并指明类型为child<%s>
    op = object_property_try_add(obj, name, type, object_get_child_property,
                                 NULL, object_finalize_child_property,
                                 child, errp);
    if (!op) {
        return NULL;
    }
    op->resolve = object_resolve_child_property;
    object_ref(child);
    child->parent = obj;
    return op;
}

ObjectProperty *
object_property_add_child(Object *obj, const char *name,
                          Object *child)
{
    return object_property_try_add_child(obj, name, child, &error_abort);
}

void object_property_allow_set_link(const Object *obj, const char *name,
                                    Object *val, Error **errp)
{
    /* Allow the link to be set, always */
}

typedef struct {
    union {
        Object **targetp;
        Object *target; /* if OBJ_PROP_LINK_DIRECT, when holding the pointer  */
        ptrdiff_t offset; /* if OBJ_PROP_LINK_CLASS */
    };
    void (*check)(const Object *, const char *, Object *, Error **);
    ObjectPropertyLinkFlags flags;
} LinkProperty;

static Object **
object_link_get_targetp(Object *obj, LinkProperty *lprop)
{
    if (lprop->flags & OBJ_PROP_LINK_DIRECT) {
        return &lprop->target;
    } else if (lprop->flags & OBJ_PROP_LINK_CLASS) {
        return (void *)obj + lprop->offset;
    } else {
        return lprop->targetp;
    }
}

static void object_get_link_property(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    LinkProperty *lprop = opaque;
    Object **targetp = object_link_get_targetp(obj, lprop);
    char *path;

    if (*targetp) {
        path = object_get_canonical_path(*targetp);
        visit_type_str(v, name, &path, errp);
        g_free(path);
    } else {
        path = (char *)"";
        visit_type_str(v, name, &path, errp);
    }
}

/*
 * object_resolve_link:
 *
 * Lookup an object and ensure its type matches the link property type.  This
 * is similar to object_resolve_path() except type verification against the
 * link property is performed.
 *
 * Returns: The matched object or NULL on path lookup failures.
 */
static Object *object_resolve_link(Object *obj, const char *name,
                                   const char *path, Error **errp)
{
    const char *type;
    char *target_type;
    bool ambiguous = false;
    Object *target;

    /* Go from link<FOO> to FOO.  */
    type = object_property_get_type(obj, name, NULL);
    target_type = g_strndup(&type[5], strlen(type) - 6);
    target = object_resolve_path_type(path, target_type, &ambiguous);

    if (ambiguous) {
        error_setg(errp, "Path '%s' does not uniquely identify an object",
                   path);
    } else if (!target) {
        target = object_resolve_path(path, &ambiguous);
        if (target || ambiguous) {
            error_setg(errp, "Invalid parameter type for '%s', expected: %s",
                             name, target_type);
        } else {
            error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                      "Device '%s' not found", path);
        }
        target = NULL;
    }
    g_free(target_type);

    return target;
}

static void object_set_link_property(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    Error *local_err = NULL;
    LinkProperty *prop = opaque;
    Object **targetp = object_link_get_targetp(obj, prop);
    Object *old_target = *targetp;
    Object *new_target;
    char *path = NULL;

    if (!visit_type_str(v, name, &path, errp)) {
        return;
    }

    if (*path) {
        new_target = object_resolve_link(obj, name, path, errp);
        if (!new_target) {
            g_free(path);
            return;
        }
    } else {
        new_target = NULL;
    }

    g_free(path);

    prop->check(obj, name, new_target, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    *targetp = new_target;
    if (prop->flags & OBJ_PROP_LINK_STRONG) {
        object_ref(new_target);
        object_unref(old_target);
    }
}

static Object *object_resolve_link_property(Object *parent, void *opaque,
                                            const char *part)
{
    LinkProperty *lprop = opaque;

    return *object_link_get_targetp(parent, lprop);
}

static void object_release_link_property(Object *obj, const char *name,
                                         void *opaque)
{
    LinkProperty *prop = opaque;
    Object **targetp = object_link_get_targetp(obj, prop);

    if ((prop->flags & OBJ_PROP_LINK_STRONG) && *targetp) {
        object_unref(*targetp);
    }
    if (!(prop->flags & OBJ_PROP_LINK_CLASS)) {
        g_free(prop);
    }
}

static ObjectProperty *
object_add_link_prop(Object *obj, const char *name,
                     const char *type, void *ptr,
                     void (*check)(const Object *, const char *,
                                   Object *, Error **),
                     ObjectPropertyLinkFlags flags)
{
    LinkProperty *prop = g_malloc(sizeof(*prop));
    g_autofree char *full_type = NULL;
    ObjectProperty *op;

    if (flags & OBJ_PROP_LINK_DIRECT) {
        prop->target = ptr;
    } else {
        prop->targetp = ptr;
    }
    prop->check = check;
    prop->flags = flags;

    full_type = g_strdup_printf("link<%s>", type);

    op = object_property_add(obj, name, full_type,
                             object_get_link_property,
                             check ? object_set_link_property : NULL,
                             object_release_link_property,
                             prop);
    op->resolve = object_resolve_link_property;
    return op;
}

ObjectProperty *
object_property_add_link(Object *obj, const char *name,
                         const char *type, Object **targetp,
                         void (*check)(const Object *, const char *,
                                       Object *, Error **),
                         ObjectPropertyLinkFlags flags)
{
    return object_add_link_prop(obj, name, type, targetp, check, flags);
}

//向ObjectClass中添加LinkProperty类型属性
ObjectProperty *
object_class_property_add_link(ObjectClass *oc,
    const char *name/*属性名称*/,
    const char *type, ptrdiff_t offset,
    void (*check)(const Object *obj, const char *name,
                  Object *val, Error **errp),
    ObjectPropertyLinkFlags flags)
{
    LinkProperty *prop = g_new0(LinkProperty, 1);
    char *full_type;
    ObjectProperty *op;

    prop->offset = offset;
    prop->check = check;
    prop->flags = flags | OBJ_PROP_LINK_CLASS;

    full_type = g_strdup_printf("link<%s>", type);

    op = object_class_property_add(oc, name, full_type,
                                   object_get_link_property,
                                   check ? object_set_link_property : NULL,
                                   object_release_link_property,
                                   prop);

    op->resolve = object_resolve_link_property;

    g_free(full_type);
    return op;
}

ObjectProperty *
object_property_add_const_link(Object *obj, const char *name,
                               Object *target)
{
    return object_add_link_prop(obj, name,
                                object_get_typename(target), target,
                                NULL, OBJ_PROP_LINK_DIRECT);
}

const char *object_get_canonical_path_component(const Object *obj)
{
    ObjectProperty *prop = NULL;
    GHashTableIter iter;

    if (obj->parent == NULL) {
        return NULL;
    }

    //遍历父节点的properties
    g_hash_table_iter_init(&iter, obj->parent->properties);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&prop)) {
        if (!object_property_is_child(prop)) {
            /*跳过非child的property*/
            continue;
        }

        //如果prop与obj相等，则返回此属性名称
        if (prop->opaque == obj) {
            return prop->name;
        }
    }

    /* obj had a parent but was not a child, should never happen */
    g_assert_not_reached();
}

char *object_get_canonical_path(const Object *obj)
{
    Object *root = object_get_root();
    char *newpath, *path = NULL;

    if (obj == root) {
        //如果obj就是root,则返回‘/’
        return g_strdup("/");
    }

    do {
        const char *component = object_get_canonical_path_component(obj);

        if (!component) {
            /* A canonical path must be complete, so discard what was
             * collected so far.
             */
            g_free(path);
            return NULL;
        }

        //反向的path到'/'
        newpath = g_strdup_printf("/%s%s", component, path ? path : "");
        g_free(path);
        path = newpath;
        obj = obj->parent;
    } while (obj != root);

    return path;
}

//通过part查找属性，如果属性有resolve回调，则调用回调完成OBJ返回
Object *object_resolve_path_component(Object *parent, const char *part)
{
    ObjectProperty *prop = object_property_find(parent, part);
    if (prop == NULL) {
        return NULL;
    }

    if (prop->resolve) {
        return prop->resolve(parent, prop->opaque, part);
    } else {
        return NULL;
    }
}

static Object *object_resolve_abs_path(Object *parent,
                                          char **parts,
                                          const char *typename)
{
    Object *child;

    if (*parts == NULL) {
        return object_dynamic_cast(parent, typename);
    }

    if (strcmp(*parts, "") == 0) {
        return object_resolve_abs_path(parent, parts + 1, typename);
    }

    child = object_resolve_path_component(parent, *parts);
    if (!child) {
        return NULL;
    }

    return object_resolve_abs_path(child, parts + 1, typename);
}

static Object *object_resolve_partial_path(Object *parent,
                                           char **parts,
                                           const char *typename,
                                           bool *ambiguous)
{
    Object *obj;
    GHashTableIter iter;
    ObjectProperty *prop;

    obj = object_resolve_abs_path(parent, parts, typename);

    g_hash_table_iter_init(&iter, parent->properties);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&prop)) {
        Object *found;

        if (!object_property_is_child(prop)) {
            continue;
        }

        found = object_resolve_partial_path(prop->opaque, parts,
                                            typename, ambiguous);
        if (found) {
            if (obj) {
                *ambiguous = true;
                return NULL;
            }
            obj = found;
        }

        if (*ambiguous) {
            return NULL;
        }
    }

    return obj;
}

Object *object_resolve_path_type(const char *path, const char *typename,
                                 bool *ambiguous)
{
    Object *obj;
    char **parts;

    parts = g_strsplit(path, "/", 0);
    assert(parts);

    if (parts[0] == NULL || strcmp(parts[0], "") != 0) {
        bool ambig = false;
        obj = object_resolve_partial_path(object_get_root(), parts,
                                          typename, &ambig);
        if (ambiguous) {
            *ambiguous = ambig;
        }
    } else {
        obj = object_resolve_abs_path(object_get_root(), parts + 1, typename);
        if (ambiguous) {
            *ambiguous = false;
        }
    }

    g_strfreev(parts);

    return obj;
}

Object *object_resolve_path(const char *path, bool *ambiguous)
{
    return object_resolve_path_type(path, TYPE_OBJECT, ambiguous);
}

Object *object_resolve_path_at(Object *parent, const char *path)
{
    g_auto(GStrv) parts = g_strsplit(path, "/", 0);

    if (*path == '/') {
        return object_resolve_abs_path(object_get_root(), parts + 1,
                                       TYPE_OBJECT);
    }
    return object_resolve_abs_path(parent, parts, TYPE_OBJECT);
}

Object *object_resolve_type_unambiguous(const char *typename, Error **errp)
{
    bool ambig = false;
    Object *o = object_resolve_path_type("", typename, &ambig);

    if (ambig) {
        error_setg(errp, "More than one object of type %s", typename);
        return NULL;
    }
    if (!o) {
        error_setg(errp, "No object found of type %s", typename);
        return NULL;
    }
    return o;
}

//字符串属性
typedef struct StringProperty
{
    char *(*get)(Object *, Error **);//object某一属性的get函数
    void (*set)(Object *, const char *, Error **);//object某一属性的set函数
} StringProperty;

//获取属性的字符串取值
static void property_get_str(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    StringProperty *prop = opaque;
    char *value;
    Error *err = NULL;

    //调用property的get回调，获得$name属性的值value
    value = prop->get(obj, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    visit_type_str(v, name, &value, errp);
    g_free(value);
}

//设置字符串类型属性
static void property_set_str(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    StringProperty *prop = opaque;
    char *value;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }

    prop->set(obj, value, errp);
    g_free(value);
}

static void property_release_data(Object *obj, const char *name,
                                  void *opaque)
{
    g_free(opaque);
}

//添加字符串类型属性
ObjectProperty *
object_property_add_str(Object *obj, const char *name,
                        char *(*get)(Object *, Error **),
                        void (*set)(Object *, const char *, Error **))
{
    StringProperty *prop = g_malloc0(sizeof(*prop));

    prop->get = get;
    prop->set = set;

    //为object添加属性
    return object_property_add(obj, name, "string",
                               get ? property_get_str : NULL,
                               set ? property_set_str : NULL,
                               property_release_data,
                               prop);
}

/*向klass中添加名称为$name的字符串类型属性，get,set用于获取此属性值*/
ObjectProperty *
object_class_property_add_str(ObjectClass *klass, const char *name,
                                   char *(*get)(Object *, Error **),
                                   void (*set)(Object *, const char *,
                                               Error **))
{
    StringProperty *prop = g_malloc0(sizeof(*prop));

    prop->get = get;
    prop->set = set;

    //添加string类型名称为$name的property
    return object_class_property_add(klass, name, "string",
                                     get ? property_get_str : NULL,
                                     set ? property_set_str : NULL,
                                     NULL,
                                     prop/*将prop做为参数*/);
}

/*boolean类型属性函数集*/
typedef struct BoolProperty
{
	/*取boolean属性值*/
    bool (*get)(Object *, Error **);
    /*设置boolean属性值*/
    void (*set)(Object *, bool, Error **);
} BoolProperty;

//通过opaque属性函数集，获取对象obj的bool属性值
static void property_get_bool(Object *obj, Visitor *v, const char *name,
                              void *opaque/*boolean类型函数集*/, Error **errp)
{
    //转为bool property
    BoolProperty *prop = opaque;
    bool value;
    Error *err = NULL;

    /*通过prop函数集获取到obj对应属性的bool值*/
    value = prop->get(obj, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    visit_type_bool(v, name, &value, errp);
}

static void property_set_bool(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    BoolProperty *prop = opaque;
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }

    //设置object的属性
    prop->set(obj, value, errp);
}

//添加name名称的bool类型属性
ObjectProperty *
object_property_add_bool(Object *obj/*要添加的对象*/, const char *name/*boolean属性值名称*/,
                         bool (*get)(Object *, Error **)/*boolean属性get函数*/,
                         void (*set)(Object *, bool, Error **)/*boolean属性set函数*/)
{
    /*构造boolean属性*/
    BoolProperty *prop = g_malloc0(sizeof(*prop));

    /*设置属性函数*/
    prop->get = get;
    prop->set = set;

    return object_property_add(obj, name, "bool",
                               /*如果提供了get回调，则使用property_get_bool进行代理此回调*/
                               get ? property_get_bool : NULL,
                               /*如果未提供set回调，则使用property_set_bool进行代理此回调*/
                               set ? property_set_bool : NULL,
                               property_release_data,
                               prop/*属性函数集*/);
}

ObjectProperty *
//向ObjectClass中添加bool类型的属性，属性名为name
object_class_property_add_bool(ObjectClass *klass, const char *name/*属性名称*/,
                                    bool (*get/*属性获取*/)(Object *, Error **),
                                    void (*set/*属性设置*/)(Object *, bool, Error **))
{
    BoolProperty *prop = g_malloc0(sizeof(*prop));

    prop->get = get;
    prop->set = set;

    //添加名称为name的bool类型属性
    return object_class_property_add(klass, name, "bool",
                                     get ? property_get_bool : NULL,
                                     set ? property_set_bool : NULL,
                                     NULL,
                                     prop/*传入BoolProperty*/);
}

static void property_get_enum(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    EnumProperty *prop = opaque;
    int value;
    Error *err = NULL;

    value = prop->get(obj, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    visit_type_enum(v, name, &value, prop->lookup, errp);
}

static void property_set_enum(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    EnumProperty *prop = opaque;
    int value;

    if (!visit_type_enum(v, name, &value, prop->lookup, errp)) {
        return;
    }
    prop->set(obj, value, errp);
}

ObjectProperty *
object_property_add_enum(Object *obj, const char *name,
                         const char *typename,
                         const QEnumLookup *lookup,
                         int (*get)(Object *, Error **),
                         void (*set)(Object *, int, Error **))
{
    EnumProperty *prop = g_malloc(sizeof(*prop));

    prop->lookup = lookup;
    prop->get = get;
    prop->set = set;

    return object_property_add(obj, name, typename,
                               get ? property_get_enum : NULL,
                               set ? property_set_enum : NULL,
                               property_release_data,
                               prop);
}

ObjectProperty *
object_class_property_add_enum(ObjectClass *klass, const char *name,
                                    const char *typename,
                                    const QEnumLookup *lookup,
                                    int (*get)(Object *, Error **),
                                    void (*set)(Object *, int, Error **))
{
    EnumProperty *prop = g_malloc(sizeof(*prop));

    prop->lookup = lookup;
    prop->get = get;
    prop->set = set;

    return object_class_property_add(klass, name, typename,
                                     get ? property_get_enum : NULL,
                                     set ? property_set_enum : NULL,
                                     NULL,
                                     prop);
}

typedef struct TMProperty {
    void (*get)(Object *, struct tm *, Error **);
} TMProperty;

static void property_get_tm(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    TMProperty *prop = opaque;
    Error *err = NULL;
    struct tm value;

    prop->get(obj, &value, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    if (!visit_start_struct(v, name, NULL, 0, errp)) {
        return;
    }
    if (!visit_type_int32(v, "tm_year", &value.tm_year, errp)) {
        goto out_end;
    }
    if (!visit_type_int32(v, "tm_mon", &value.tm_mon, errp)) {
        goto out_end;
    }
    if (!visit_type_int32(v, "tm_mday", &value.tm_mday, errp)) {
        goto out_end;
    }
    if (!visit_type_int32(v, "tm_hour", &value.tm_hour, errp)) {
        goto out_end;
    }
    if (!visit_type_int32(v, "tm_min", &value.tm_min, errp)) {
        goto out_end;
    }
    if (!visit_type_int32(v, "tm_sec", &value.tm_sec, errp)) {
        goto out_end;
    }
    visit_check_struct(v, errp);
out_end:
    visit_end_struct(v, NULL);
}

ObjectProperty *
object_property_add_tm(Object *obj, const char *name,
                       void (*get)(Object *, struct tm *, Error **))
{
    TMProperty *prop = g_malloc0(sizeof(*prop));

    prop->get = get;

    return object_property_add(obj, name, "struct tm",
                               get ? property_get_tm : NULL, NULL,
                               property_release_data,
                               prop);
}

ObjectProperty *
object_class_property_add_tm(ObjectClass *klass, const char *name,
                             void (*get)(Object *, struct tm *, Error **))
{
    TMProperty *prop = g_malloc0(sizeof(*prop));

    prop->get = get;

    return object_class_property_add(klass, name, "struct tm",
                                     get ? property_get_tm : NULL,
                                     NULL, NULL, prop);
}

//复制一份obj对应的类型名称
static char *object_get_type(Object *obj, Error **errp)
{
    return g_strdup(object_get_typename(obj));
}

static void property_get_uint8_ptr(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint8_t value = *(uint8_t *)opaque;
    visit_type_uint8(v, name, &value, errp);
}

static void property_set_uint8_ptr(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint8_t *field = opaque;
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }

    *field = value;
}

static void property_get_uint16_ptr(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint16_t value = *(uint16_t *)opaque;
    visit_type_uint16(v, name, &value, errp);
}

static void property_set_uint16_ptr(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint16_t *field = opaque;
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }

    *field = value;
}

static void property_get_uint32_ptr(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint32_t value = *(uint32_t *)opaque;
    visit_type_uint32(v, name, &value, errp);
}

static void property_set_uint32_ptr(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint32_t *field = opaque;
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    *field = value;
}

static void property_get_uint64_ptr(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint64_t value = *(uint64_t *)opaque;
    visit_type_uint64(v, name, &value, errp);
}

static void property_set_uint64_ptr(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint64_t *field = opaque;
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }

    *field = value;
}

ObjectProperty *
object_property_add_uint8_ptr(Object *obj, const char *name,
                              const uint8_t *v,
                              ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint8_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint8_ptr;
    }

    return object_property_add(obj, name, "uint8",
                               getter, setter, NULL, (void *)v);
}

ObjectProperty *
object_class_property_add_uint8_ptr(ObjectClass *klass, const char *name,
                                    const uint8_t *v,
                                    ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint8_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint8_ptr;
    }

    return object_class_property_add(klass, name, "uint8",
                                     getter, setter, NULL, (void *)v);
}

ObjectProperty *
object_property_add_uint16_ptr(Object *obj, const char *name,
                               const uint16_t *v,
                               ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint16_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint16_ptr;
    }

    return object_property_add(obj, name, "uint16",
                               getter, setter, NULL, (void *)v);
}

ObjectProperty *
object_class_property_add_uint16_ptr(ObjectClass *klass, const char *name,
                                     const uint16_t *v,
                                     ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint16_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint16_ptr;
    }

    return object_class_property_add(klass, name, "uint16",
                                     getter, setter, NULL, (void *)v);
}

ObjectProperty *
object_property_add_uint32_ptr(Object *obj, const char *name,
                               const uint32_t *v,
                               ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint32_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint32_ptr;
    }

    return object_property_add(obj, name, "uint32",
                               getter, setter, NULL, (void *)v);
}

ObjectProperty *
object_class_property_add_uint32_ptr(ObjectClass *klass, const char *name,
                                     const uint32_t *v,
                                     ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint32_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint32_ptr;
    }

    return object_class_property_add(klass, name, "uint32",
                                     getter, setter, NULL, (void *)v);
}

ObjectProperty *
object_property_add_uint64_ptr(Object *obj, const char *name,
                               const uint64_t *v,
                               ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint64_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint64_ptr;
    }

    return object_property_add(obj, name, "uint64",
                               getter, setter, NULL, (void *)v);
}

ObjectProperty *
object_class_property_add_uint64_ptr(ObjectClass *klass, const char *name,
                                     const uint64_t *v,
                                     ObjectPropertyFlags flags)
{
    ObjectPropertyAccessor *getter = NULL;
    ObjectPropertyAccessor *setter = NULL;

    if ((flags & OBJ_PROP_FLAG_READ) == OBJ_PROP_FLAG_READ) {
        getter = property_get_uint64_ptr;
    }

    if ((flags & OBJ_PROP_FLAG_WRITE) == OBJ_PROP_FLAG_WRITE) {
        setter = property_set_uint64_ptr;
    }

    return object_class_property_add(klass, name, "uint64",
                                     getter, setter, NULL, (void *)v);
}

typedef struct {
    Object *target_obj;
    char *target_name;
} AliasProperty;

static void property_get_alias(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    AliasProperty *prop = opaque;
    Visitor *alias_v = visitor_forward_field(v, prop->target_name, name);

    object_property_get(prop->target_obj, prop->target_name, alias_v, errp);
    visit_free(alias_v);
}

static void property_set_alias(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    AliasProperty *prop = opaque;
    Visitor *alias_v = visitor_forward_field(v, prop->target_name, name);

    object_property_set(prop->target_obj, prop->target_name, alias_v, errp);
    visit_free(alias_v);
}

static Object *property_resolve_alias(Object *obj, void *opaque,
                                      const char *part)
{
    AliasProperty *prop = opaque;

    return object_resolve_path_component(prop->target_obj, prop->target_name);
}

static void property_release_alias(Object *obj, const char *name, void *opaque)
{
    AliasProperty *prop = opaque;

    g_free(prop->target_name);
    g_free(prop);
}

ObjectProperty *
object_property_add_alias(Object *obj, const char *name,
                          Object *target_obj, const char *target_name)
{
    AliasProperty *prop;
    ObjectProperty *op;
    ObjectProperty *target_prop;
    g_autofree char *prop_type = NULL;

    target_prop = object_property_find_err(target_obj, target_name,
                                           &error_abort);

    if (object_property_is_child(target_prop)) {
        prop_type = g_strdup_printf("link%s",
                                    target_prop->type + strlen("child"));
    } else {
        prop_type = g_strdup(target_prop->type);
    }

    prop = g_malloc(sizeof(*prop));
    prop->target_obj = target_obj;
    prop->target_name = g_strdup(target_name);

    op = object_property_add(obj, name, prop_type,
                             property_get_alias,
                             property_set_alias,
                             property_release_alias,
                             prop);
    op->resolve = property_resolve_alias;
    if (target_prop->defval) {
        op->defval = qobject_ref(target_prop->defval);
    }

    object_property_set_description(obj, op->name,
                                    target_prop->description);
    return op;
}

//为object的属性设置描述信息
void object_property_set_description(Object *obj, const char *name,
                                     const char *description)
{
    ObjectProperty *op;

    //obj必须具有name属性
    op = object_property_find_err(obj, name, &error_abort);
    //更新属性描述符
    g_free(op->description);
    op->description = g_strdup(description);
}

//为class的属性设置描述信息
void object_class_property_set_description(ObjectClass *klass,
                                           const char *name,
                                           const char *description)
{
    ObjectProperty *op;

    //检查class中是否有此property
    op = g_hash_table_lookup(klass->properties, name);
    //存在此属性，则设置描述信息
    g_free(op->description);
    op->description = g_strdup(description);
}

//添加字符串类型type,用于返回klass类型名称
static void object_class_init(ObjectClass *klass, void *data)
{
    object_class_property_add_str(klass, "type", object_get_type,
                                  NULL);
}

//实现基础类注册
static void register_types(void)
{
    //interface类型信息（interface的class为InterfaceClass)
    static const TypeInfo interface_info = {
        .name = TYPE_INTERFACE,
        .class_size = sizeof(InterfaceClass),
        .abstract = true,
    };

    //object类型信息
    static const TypeInfo object_info = {
        .name = TYPE_OBJECT,
        .instance_size = sizeof(Object),
        .class_init = object_class_init,
        .abstract = true,
    };

    //注册interface类型
    type_interface = type_register_internal(&interface_info);

    //注册object类型
    type_register_internal(&object_info);
}

//注册类型初始化函数
type_init(register_types)
