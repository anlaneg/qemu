#include "qemu/osdep.h"
#include "block/qdict.h" /* for qdict_extract_subqdict() */
#include "qapi/error.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "hw/boards.h"

//存放qemu选项
//选项采用name区分（称为group-name）例如"chardev","netdev"等
static QemuOptsList *vm_config_groups[48];

//存放qemu drive选项
static QemuOptsList *drive_config_groups[5];


//在给出的lists数组中查找名称为group的lists,找不到返回NULL，并返回错误信息
static QemuOptsList *find_list(QemuOptsList **lists, const char *group,
                               Error **errp)
{
    int i;

    qemu_load_module_for_opts(group);
    for (i = 0; lists[i] != NULL; i++) {
    	//遍历并比对lists名称
        if (strcmp(lists[i]->name, group) == 0)
            break;
    }
    if (lists[i] == NULL) {
        error_setg(errp, "There is no option group '%s'", group);
    }
    return lists[i];
}

//查找指定group的optsList（找不到时返回NULL)
QemuOptsList *qemu_find_opts(const char *group)
{
    QemuOptsList *ret;
    Error *local_err = NULL;

    ret = find_list(vm_config_groups, group, &local_err);
    if (local_err) {
        error_report_err(local_err);
    }

    return ret;
}

//如果名称为group的QemuOptsList中不存在含有id的opts，则返回它，否则创建一个并返回
QemuOpts *qemu_find_opts_singleton(const char *group)
{
    QemuOptsList *list;
    QemuOpts *opts;

    //先找group对应的list
    list = qemu_find_opts(group);
    assert(list);

    //查找此group中是否有无id的opts，如果无，则创建一个无id的opts
    opts = qemu_opts_find(list, NULL);
    if (!opts) {
    	//不存在，创建一个无id的opts
        opts = qemu_opts_create(list, NULL, 0, &error_abort);
    }
    return opts;
}

//通过desc转换成parameterinfolist
static CommandLineParameterInfoList *query_option_descs(const QemuOptDesc *desc)
{
    CommandLineParameterInfoList *param_list = NULL;
    CommandLineParameterInfo *info;
    int i;

    for (i = 0; desc[i].name != NULL; i++) {
        info = g_malloc0(sizeof(*info));
        //填充名称
        info->name = g_strdup(desc[i].name);

        //填充类型
        switch (desc[i].type) {
        case QEMU_OPT_STRING:
            info->type = COMMAND_LINE_PARAMETER_TYPE_STRING;
            break;
        case QEMU_OPT_BOOL:
            info->type = COMMAND_LINE_PARAMETER_TYPE_BOOLEAN;
            break;
        case QEMU_OPT_NUMBER:
            info->type = COMMAND_LINE_PARAMETER_TYPE_NUMBER;
            break;
        case QEMU_OPT_SIZE:
            info->type = COMMAND_LINE_PARAMETER_TYPE_SIZE;
            break;
        }

        //指出有help信息
        info->help = g_strdup(desc[i].help);
        info->q_default = g_strdup(desc[i].def_value_str);

        //将entry放置在param_list的前面
        QAPI_LIST_PREPEND(param_list, info);
    }

    return param_list;
}

/* remove repeated entry from the info list */
static void cleanup_infolist(CommandLineParameterInfoList *head)
{
    CommandLineParameterInfoList *pre_entry, *cur, *del_entry;

    cur = head;
    while (cur->next) {
        pre_entry = head;
        while (pre_entry != cur->next) {
            if (!strcmp(pre_entry->value->name, cur->next->value->name)) {
                del_entry = cur->next;
                cur->next = cur->next->next;
                del_entry->next = NULL;
                qapi_free_CommandLineParameterInfoList(del_entry);
                break;
            }
            pre_entry = pre_entry->next;
        }
        cur = cur->next;
    }
}

/* merge the description items of two parameter infolists */
static void connect_infolist(CommandLineParameterInfoList *head,
                             CommandLineParameterInfoList *new)
{
    CommandLineParameterInfoList *cur;

    cur = head;
    while (cur->next) {
        cur = cur->next;
    }
    cur->next = new;
}

/* access all the local QemuOptsLists for drive option */
static CommandLineParameterInfoList *get_drive_infolist(void)
{
    CommandLineParameterInfoList *head = NULL, *cur;
    int i;

    for (i = 0; drive_config_groups[i] != NULL; i++) {
        if (!head) {
        	//首个时
            head = query_option_descs(drive_config_groups[i]->desc);
        } else {
        	//非首个时，引入cur局部变量，将其指向的链表与head指向的链表合并
            cur = query_option_descs(drive_config_groups[i]->desc);
            connect_infolist(head, cur);
        }
    }
    cleanup_infolist(head);

    return head;
}

static CommandLineParameterInfo *objprop_to_cmdline_prop(ObjectProperty *prop)
{
    CommandLineParameterInfo *info;

    info = g_malloc0(sizeof(*info));
    info->name = g_strdup(prop->name);

    if (g_str_equal(prop->type, "bool") || g_str_equal(prop->type, "OnOffAuto")) {
        info->type = COMMAND_LINE_PARAMETER_TYPE_BOOLEAN;
    } else if (g_str_equal(prop->type, "int")) {
        info->type = COMMAND_LINE_PARAMETER_TYPE_NUMBER;
    } else if (g_str_equal(prop->type, "size")) {
        info->type = COMMAND_LINE_PARAMETER_TYPE_SIZE;
    } else {
        info->type = COMMAND_LINE_PARAMETER_TYPE_STRING;
    }

    if (prop->description) {
        info->help = g_strdup(prop->description);
    }

    return info;
}

static CommandLineParameterInfoList *query_all_machine_properties(void)
{
    CommandLineParameterInfoList *params = NULL, *clpiter;
    CommandLineParameterInfo *info;
    GSList *machines, *curr_mach;
    ObjectPropertyIterator op_iter;
    ObjectProperty *prop;
    bool is_new;

    machines = object_class_get_list(TYPE_MACHINE, false);
    assert(machines);

    /* Loop over all machine classes */
    for (curr_mach = machines; curr_mach; curr_mach = curr_mach->next) {
        object_class_property_iter_init(&op_iter, curr_mach->data);
        /* ... and over the properties of each machine: */
        while ((prop = object_property_iter_next(&op_iter))) {
            if (!prop->set) {
                continue;
            }
            /*
             * Check whether the property has already been put into the list
             * (via another machine class)
             */
            is_new = true;
            for (clpiter = params; clpiter != NULL; clpiter = clpiter->next) {
                if (g_str_equal(clpiter->value->name, prop->name)) {
                    is_new = false;
                    break;
                }
            }
            /* If it hasn't been added before, add it now to the list */
            if (is_new) {
                info = objprop_to_cmdline_prop(prop);
                QAPI_LIST_PREPEND(params, info);
            }
        }
    }

    g_slist_free(machines);

    /* Add entry for the "type" parameter */
    info = g_malloc0(sizeof(*info));
    info->name = g_strdup("type");
    info->type = COMMAND_LINE_PARAMETER_TYPE_STRING;
    info->help = g_strdup("machine type");
    QAPI_LIST_PREPEND(params, info);

    return params;
}

CommandLineOptionInfoList *qmp_query_command_line_options(const char *option,
                                                          Error **errp)
{
    CommandLineOptionInfoList *conf_list = NULL;
    CommandLineOptionInfo *info;
    int i;

    for (i = 0; vm_config_groups[i] != NULL; i++) {
        if (!option || !strcmp(option, vm_config_groups[i]->name)) {
            info = g_malloc0(sizeof(*info));
            info->option = g_strdup(vm_config_groups[i]->name);
            if (!strcmp("drive", vm_config_groups[i]->name)) {
            	//为driver时，取其所有参数
                info->parameters = get_drive_infolist();
            } else {
                info->parameters =
                    query_option_descs(vm_config_groups[i]->desc);
            }
            QAPI_LIST_PREPEND(conf_list, info);
        }
    }

    if (!option || !strcmp(option, "machine")) {
        info = g_malloc0(sizeof(*info));
        info->option = g_strdup("machine");
        info->parameters = query_all_machine_properties();
        QAPI_LIST_PREPEND(conf_list, info);
    }

    if (conf_list == NULL) {
        error_setg(errp, "invalid option name: %s", option);
    }

    return conf_list;
}

//给定group名称查找group名称对应的optsList
QemuOptsList *qemu_find_opts_err(const char *group, Error **errp)
{
    return find_list(vm_config_groups, group, errp);
}

//qemu 驱动配置选项添加
void qemu_add_drive_opts(QemuOptsList *list)
{
    int entries, i;

    entries = ARRAY_SIZE(drive_config_groups);
    entries--; /* keep list NULL terminated */
    for (i = 0; i < entries; i++) {
    	//在entries中找出一个空闲的节点，将其放入
        if (drive_config_groups[i] == NULL) {
            drive_config_groups[i] = list;
            return;
        }
    }
    fprintf(stderr, "ran out of space in drive_config_groups");
    abort();
}

//qemu选项添加
void qemu_add_opts(QemuOptsList *list)
{
    int entries, i;

    entries = ARRAY_SIZE(vm_config_groups);
    entries--; /* keep list NULL terminated */
    for (i = 0; i < entries; i++) {
    	//找一个空闲的位置，初始化此位置的值为list
        if (vm_config_groups[i] == NULL) {
            vm_config_groups[i] = list;
            return;
        }
    }
    fprintf(stderr, "ran out of space in vm_config_groups");
    abort();//如果没有空闲的，则挂掉
}

/* Returns number of config groups on success, -errno on error */
static int qemu_config_foreach(FILE *fp, QEMUConfigCB *cb, void *opaque,
                               const char *fname, Error **errp)
{
    ERRP_GUARD();
    char line[1024], prev_group[64], group[64], arg[64], value[1024];
    Location loc;
    QDict *qdict = NULL;
    int res = -EINVAL, lno = 0;
    int count = 0;

    loc_push_none(&loc);
    while (fgets(line, sizeof(line), fp) != NULL) {
        ++lno;
        //跳过空行
        if (line[0] == '\n') {
            /* skip empty lines */
            continue;
        }
        //跳过注释行
        if (line[0] == '#') {
            /* comment */
            continue;
        }
        //提取group名称及id号
        if (line[0] == '[') {
            QDict *prev = qdict;
            if (sscanf(line, "[%63s \"%63[^\"]\"]", group, value) == 2) {
                qdict = qdict_new();
                qdict_put_str(qdict, "id", value);
                count++;
            } else if (sscanf(line, "[%63[^]]]", group) == 1) {
        	//提取其它group名称（无id的group情况）
                qdict = qdict_new();
                count++;
            }
            if (qdict != prev) {
                if (prev) {
                    cb(prev_group, prev, opaque, errp);
                    qobject_unref(prev);
                    if (*errp) {
                        goto out;
                    }
                }
                strcpy(prev_group, group);
                continue;
            }
        }
        loc_set_file(fname, lno);
        //提取配置的key及value
        value[0] = '\0';
        if (sscanf(line, " %63s = \"%1023[^\"]\"", arg, value) == 2 ||
            sscanf(line, " %63s = \"\"", arg) == 1) {
            /* arg = value */
            if (qdict == NULL) {
                error_setg(errp, "no group defined");
                goto out;
            }
            qdict_put_str(qdict, arg, value);
            continue;
        }
        error_setg(errp, "parse error");
        goto out;
    }
    if (ferror(fp)) {
        loc_pop(&loc);
        error_setg_errno(errp, errno, "Cannot read config file");
        goto out_no_loc;
    }
    res = count;
    if (qdict) {
        cb(group, qdict, opaque, errp);
    }
out:
    loc_pop(&loc);
out_no_loc:
    qobject_unref(qdict);
    return res;
}

void qemu_config_do_parse(const char *group, QDict *qdict, void *opaque, Error **errp)
{
    QemuOptsList **lists = opaque;
    QemuOptsList *list;

    list = find_list(lists, group, errp);
    if (!list) {
        return;
    }

    qemu_opts_from_qdict(list, qdict, errp);
}

int qemu_config_parse(FILE *fp, QemuOptsList **lists, const char *fname, Error **errp)
{
    return qemu_config_foreach(fp, qemu_config_do_parse, lists, fname, errp);
}

//自文件中读取选项
int qemu_read_config_file(const char *filename, QEMUConfigCB *cb, Error **errp)
{
    FILE *f = fopen(filename, "r");
    int ret;

    if (f == NULL) {
        error_setg_file_open(errp, errno, filename);
        return -errno;
    }

    //解析文件
    ret = qemu_config_foreach(f, cb, vm_config_groups, filename, errp);
    fclose(f);
    return ret;
}

static bool config_parse_qdict_section(QDict *options, QemuOptsList *opts,
                                       Error **errp)
{
    QemuOpts *subopts;
    g_autoptr(QDict) subqdict = NULL;
    g_autoptr(QList) list = NULL;
    size_t orig_size, enum_size;
    char *prefix;

    prefix = g_strdup_printf("%s.", opts->name);
    qdict_extract_subqdict(options, &subqdict, prefix);
    g_free(prefix);
    orig_size = qdict_size(subqdict);
    if (!orig_size) {
        return true;
    }

    subopts = qemu_opts_create(opts, NULL, 0, errp);
    if (!subopts) {
        return false;
    }

    if (!qemu_opts_absorb_qdict(subopts, subqdict, errp)) {
        return false;
    }

    enum_size = qdict_size(subqdict);
    if (enum_size < orig_size && enum_size) {
        error_setg(errp, "Unknown option '%s' for [%s]",
                   qdict_first(subqdict)->key, opts->name);
        return false;
    }

    if (enum_size) {
        /* Multiple, enumerated sections */
        QListEntry *list_entry;
        unsigned i = 0;

        /* Not required anymore */
        qemu_opts_del(subopts);

        qdict_array_split(subqdict, &list);
        if (qdict_size(subqdict)) {
            error_setg(errp, "Unused option '%s' for [%s]",
                       qdict_first(subqdict)->key, opts->name);
            return false;
        }

        QLIST_FOREACH_ENTRY(list, list_entry) {
            QDict *section = qobject_to(QDict, qlist_entry_obj(list_entry));
            char *opt_name;

            if (!section) {
                error_setg(errp, "[%s] section (index %u) does not consist of "
                           "keys", opts->name, i);
                return false;
            }

            opt_name = g_strdup_printf("%s.%u", opts->name, i++);
            subopts = qemu_opts_create(opts, opt_name, 1, errp);
            g_free(opt_name);
            if (!subopts) {
                return false;
            }

            if (!qemu_opts_absorb_qdict(subopts, section, errp)) {
                qemu_opts_del(subopts);
                return false;
            }

            if (qdict_size(section)) {
                error_setg(errp, "[%s] section doesn't support the option '%s'",
                           opts->name, qdict_first(section)->key);
                qemu_opts_del(subopts);
                return false;
            }
        }
    }

    return true;
}

bool qemu_config_parse_qdict(QDict *options, QemuOptsList **lists,
                             Error **errp)
{
    int i;

    for (i = 0; lists[i]; i++) {
        if (!config_parse_qdict_section(options, lists[i], errp)) {
            return false;
        }
    }

    return true;
}
