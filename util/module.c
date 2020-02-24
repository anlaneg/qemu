/*
 * QEMU Module Infrastructure
 *
 * Copyright IBM, Corp. 2009
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#ifdef CONFIG_MODULES
#include <gmodule.h>
#endif
#include "qemu/queue.h"
#include "qemu/module.h"

typedef struct ModuleEntry
{
    void (*init)(void);//初始化函数
    QTAILQ_ENTRY(ModuleEntry) node;//挂接点
    module_init_type type;//模块类型
} ModuleEntry;

typedef QTAILQ_HEAD(, ModuleEntry) ModuleTypeList;

//按模块类型组织成链表
static ModuleTypeList init_type_list[MODULE_INIT_MAX];

static ModuleTypeList dso_init_list;//与init_type_list相同，实现dso的初始化

//初始化
static void init_lists(void)
{
    static int inited;
    int i;

    if (inited) {
    	//仅初始化一次
        return;
    }

    //初始化为空链表
    for (i = 0; i < MODULE_INIT_MAX; i++) {
        QTAILQ_INIT(&init_type_list[i]);
    }

    QTAILQ_INIT(&dso_init_list);

    inited = 1;
}

//通过type查找初始化函数列表
static ModuleTypeList *find_type(module_init_type type)
{
    init_lists();

    return &init_type_list[type];
}

//注册模块回调（将fn,注册到type列表中）
void register_module_init(void (*fn/*要注册的回调*/)(void), module_init_type type/*模块类型*/)
{
    ModuleEntry *e;
    ModuleTypeList *l;

    e = g_malloc0(sizeof(*e));
    e->init = fn;
    e->type = type;

    //找type对应的list并加入链上
    l = find_type(type);

    QTAILQ_INSERT_TAIL(l, e, node);
}

//dso模块初始化注册函数
void register_dso_module_init(void (*fn)(void), module_init_type type)
{
    ModuleEntry *e;

    init_lists();

    e = g_malloc0(sizeof(*e));
    e->init = fn;
    e->type = type;

    QTAILQ_INSERT_TAIL(&dso_init_list, e, node);
}

//调用指定type的所有init函数（这些init函数由module_init宏完成注册）
void module_call_init(module_init_type type)
{
    ModuleTypeList *l;
    ModuleEntry *e;

    //查找指定type，获得list,针对每个list成员执行init函数
    l = find_type(type);

    QTAILQ_FOREACH(e, l, node) {
        e->init();
    }
}

#ifdef CONFIG_MODULES
static int module_load_file(const char *fname)
{
    GModule *g_module;
    void (*sym)(void);
    const char *dsosuf = HOST_DSOSUF;
    int len = strlen(fname);
    int suf_len = strlen(dsosuf);
    ModuleEntry *e, *next;
    int ret;

    if (len <= suf_len || strcmp(&fname[len - suf_len], dsosuf)) {
        /* wrong suffix */
        ret = -EINVAL;
        goto out;
    }

    //检查fname是否可访问
    if (access(fname, F_OK)) {
        ret = -ENOENT;
        goto out;
    }

    assert(QTAILQ_EMPTY(&dso_init_list));

    g_module = g_module_open(fname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (!g_module) {
        fprintf(stderr, "Failed to open module: %s\n",
                g_module_error());
        ret = -EINVAL;
        goto out;
    }
    if (!g_module_symbol(g_module, DSO_STAMP_FUN_STR, (gpointer *)&sym)) {
        fprintf(stderr, "Failed to initialize module: %s\n",
                fname);
        /* Print some info if this is a QEMU module (but from different build),
         * this will make debugging user problems easier. */
        if (g_module_symbol(g_module, "qemu_module_dummy", (gpointer *)&sym)) {
            fprintf(stderr,
                    "Note: only modules from the same build can be loaded.\n");
        }
        g_module_close(g_module);
        ret = -EINVAL;
    } else {
        QTAILQ_FOREACH(e, &dso_init_list, node) {
            e->init();
            register_module_init(e->init, e->type);
        }
        ret = 0;
    }

    QTAILQ_FOREACH_SAFE(e, &dso_init_list, node, next) {
        QTAILQ_REMOVE(&dso_init_list, e, node);
        g_free(e);
    }
out:
    return ret;
}
#endif

bool module_load_one(const char *prefix, const char *lib_name)
{
    bool success = false;

#ifdef CONFIG_MODULES
    char *fname = NULL;
    char *exec_dir;
    const char *search_dir;
    char *dirs[4];
    char *module_name;
    int i = 0, n_dirs = 0;
    int ret;
    static GHashTable *loaded_modules;

    if (!g_module_supported()) {
        fprintf(stderr, "Module is not supported by system.\n");
        return false;
    }

    if (!loaded_modules) {
        loaded_modules = g_hash_table_new(g_str_hash, g_str_equal);
    }

    module_name = g_strdup_printf("%s%s", prefix, lib_name);

    if (!g_hash_table_add(loaded_modules, module_name)) {
        g_free(module_name);
        return true;
    }

    exec_dir = qemu_get_exec_dir();
    search_dir = getenv("QEMU_MODULE_DIR");
    if (search_dir != NULL) {
        dirs[n_dirs++] = g_strdup_printf("%s", search_dir);
    }
    dirs[n_dirs++] = g_strdup_printf("%s", CONFIG_QEMU_MODDIR);
    dirs[n_dirs++] = g_strdup_printf("%s/..", exec_dir ? : "");
    dirs[n_dirs++] = g_strdup_printf("%s", exec_dir ? : "");
    assert(n_dirs <= ARRAY_SIZE(dirs));

    g_free(exec_dir);
    exec_dir = NULL;

    for (i = 0; i < n_dirs; i++) {
        fname = g_strdup_printf("%s/%s%s",
                dirs[i], module_name, HOST_DSOSUF);
        ret = module_load_file(fname);
        g_free(fname);
        fname = NULL;
        /* Try loading until loaded a module file */
        if (!ret) {
            success = true;
            break;
        }
    }

    if (!success) {
        g_hash_table_remove(loaded_modules, module_name);
        g_free(module_name);
    }

    for (i = 0; i < n_dirs; i++) {
        g_free(dirs[i]);
    }

#endif
    return success;
}
