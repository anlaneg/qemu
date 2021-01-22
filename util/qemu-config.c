#include "qemu/osdep.h"
#include "block/qdict.h" /* for qdict_extract_subqdict() */
#include "qapi/error.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/config-file.h"

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
        if (desc[i].help) {
            info->has_help = true;
            info->help = g_strdup(desc[i].help);
        }
        if (desc[i].def_value_str) {
            info->has_q_default = true;
            info->q_default = g_strdup(desc[i].def_value_str);
        }

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

/* restore machine options that are now machine's properties */
static QemuOptsList machine_opts = {
    .merge_lists = true,
    .head = QTAILQ_HEAD_INITIALIZER(machine_opts.head),
    .desc = {
        {
            .name = "type",
            .type = QEMU_OPT_STRING,
            .help = "emulated machine"
        },{
            .name = "accel",
            .type = QEMU_OPT_STRING,
            .help = "accelerator list",
        },{
            .name = "kernel_irqchip",
            .type = QEMU_OPT_BOOL,
            .help = "use KVM in-kernel irqchip",
        },{
            .name = "kvm_shadow_mem",
            .type = QEMU_OPT_SIZE,
            .help = "KVM shadow MMU size",
        },{
            .name = "kernel",
            .type = QEMU_OPT_STRING,
            .help = "Linux kernel image file",
        },{
            .name = "initrd",
            .type = QEMU_OPT_STRING,
            .help = "Linux initial ramdisk file",
        },{
            .name = "append",
            .type = QEMU_OPT_STRING,
            .help = "Linux kernel command line",
        },{
            .name = "dtb",
            .type = QEMU_OPT_STRING,
            .help = "Linux kernel device tree file",
        },{
            .name = "dumpdtb",
            .type = QEMU_OPT_STRING,
            .help = "Dump current dtb to a file and quit",
        },{
            .name = "phandle_start",
            .type = QEMU_OPT_NUMBER,
            .help = "The first phandle ID we may generate dynamically",
        },{
            .name = "dt_compatible",
            .type = QEMU_OPT_STRING,
            .help = "Overrides the \"compatible\" property of the dt root node",
        },{
            .name = "dump-guest-core",
            .type = QEMU_OPT_BOOL,
            .help = "Include guest memory in  a core dump",
        },{
            .name = "mem-merge",
            .type = QEMU_OPT_BOOL,
            .help = "enable/disable memory merge support",
        },{
            .name = "usb",
            .type = QEMU_OPT_BOOL,
            .help = "Set on/off to enable/disable usb",
        },{
            .name = "firmware",
            .type = QEMU_OPT_STRING,
            .help = "firmware image",
        },{
            .name = "iommu",
            .type = QEMU_OPT_BOOL,
            .help = "Set on/off to enable/disable Intel IOMMU (VT-d)",
        },{
            .name = "suppress-vmdesc",
            .type = QEMU_OPT_BOOL,
            .help = "Set on to disable self-describing migration",
        },{
            .name = "aes-key-wrap",
            .type = QEMU_OPT_BOOL,
            .help = "enable/disable AES key wrapping using the CPACF wrapping key",
        },{
            .name = "dea-key-wrap",
            .type = QEMU_OPT_BOOL,
            .help = "enable/disable DEA key wrapping using the CPACF wrapping key",
        },{
            .name = "loadparm",
            .type = QEMU_OPT_STRING,
            .help = "Up to 8 chars in set of [A-Za-z0-9. ](lower case chars"
                    " converted to upper case) to pass to machine"
                    " loader, boot manager, and guest kernel",
        },
        { /* End of list */ }
    }
};

CommandLineOptionInfoList *qmp_query_command_line_options(bool has_option,
                                                          const char *option,
                                                          Error **errp)
{
    CommandLineOptionInfoList *conf_list = NULL;
    CommandLineOptionInfo *info;
    int i;

    for (i = 0; vm_config_groups[i] != NULL; i++) {
        if (!has_option || !strcmp(option, vm_config_groups[i]->name)) {
            info = g_malloc0(sizeof(*info));
            info->option = g_strdup(vm_config_groups[i]->name);
            if (!strcmp("drive", vm_config_groups[i]->name)) {
            	//为driver时，取其所有参数
                info->parameters = get_drive_infolist();
            } else if (!strcmp("machine", vm_config_groups[i]->name)) {
                info->parameters = query_option_descs(machine_opts.desc);
            } else {
                info->parameters =
                    query_option_descs(vm_config_groups[i]->desc);
            }
            QAPI_LIST_PREPEND(conf_list, info);
        }
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

struct ConfigWriteData {
    QemuOptsList *list;
    FILE *fp;
};

//输出每个子选项名称及取值
static int config_write_opt(void *opaque, const char *name, const char *value,
                            Error **errp)
{
    struct ConfigWriteData *data = opaque;

    fprintf(data->fp, "  %s = \"%s\"\n", name, value);
    return 0;
}

static int config_write_opts(void *opaque, QemuOpts *opts, Error **errp)
{
    struct ConfigWriteData *data = opaque;
    //取id选项
    const char *id = qemu_opts_id(opts);

    if (id) {
    	//如果有id选项，向文件中写入(id)
        fprintf(data->fp, "[%s \"%s\"]\n", data->list->name, id);
    } else {
    	//无id情况
        fprintf(data->fp, "[%s]\n", data->list->name);
    }
    //遍历所有选项并输出
    qemu_opt_foreach(opts, config_write_opt, data, NULL);
    fprintf(data->fp, "\n");
    return 0;
}

//向文件中写入选项
void qemu_config_write(FILE *fp)
{
    struct ConfigWriteData data = { .fp = fp };
    QemuOptsList **lists = vm_config_groups;
    int i;

    //加入注释行
    fprintf(fp, "# qemu config file\n\n");
    for (i = 0; lists[i] != NULL; i++) {
        data.list = lists[i];
        //遍历每个配置选项组中的选项，针对其调用config_write_opts
        qemu_opts_foreach(data.list, config_write_opts, &data, NULL);
    }
}

/* Returns number of config groups on success, -errno on error */
int qemu_config_parse(FILE *fp, QemuOptsList **lists, const char *fname)
{
    char line[1024], group[64], id[64], arg[64], value[1024];
    Location loc;
    QemuOptsList *list = NULL;
    Error *local_err = NULL;
    QemuOpts *opts = NULL;
    int res = -EINVAL, lno = 0;
    int count = 0;

    loc_push_none(&loc);
    while (fgets(line, sizeof(line), fp) != NULL) {
        loc_set_file(fname, ++lno);
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
        if (sscanf(line, "[%63s \"%63[^\"]\"]", group, id) == 2) {
            /* group with id */
            list = find_list(lists, group, &local_err);
            if (local_err) {
            		//查找grouplist,如果找不到报错
                error_report_err(local_err);
                goto out;
            }
            //注册id选项并赋值
            opts = qemu_opts_create(list, id, 1, NULL);
            count++;
            continue;
        }

        //提取其它group名称（无id的group情况）
        if (sscanf(line, "[%63[^]]]", group) == 1) {
            /* group without id */
            list = find_list(lists, group, &local_err);
            if (local_err) {
                error_report_err(local_err);
                goto out;
            }
            opts = qemu_opts_create(list, NULL, 0, &error_abort);
            count++;
            continue;
        }

        //提取配置的key及value
        value[0] = '\0';
        if (sscanf(line, " %63s = \"%1023[^\"]\"", arg, value) == 2 ||
            sscanf(line, " %63s = \"\"", arg) == 1) {
            /* arg = value */
            if (opts == NULL) {
                error_report("no group defined");
                goto out;
            }
            //设置选项值
            if (!qemu_opt_set(opts, arg, value, &local_err)) {
                error_report_err(local_err);
                goto out;
            }
            continue;
        }
        error_report("parse error");
        goto out;
    }
    if (ferror(fp)) {
        error_report("error reading file");
        goto out;
    }
    res = count;
out:
    loc_pop(&loc);
    return res;
}

//自文件中读取选项
int qemu_read_config_file(const char *filename)
{
    FILE *f = fopen(filename, "r");
    int ret;

    if (f == NULL) {
        return -errno;
    }

    //解析文件
    ret = qemu_config_parse(f, vm_config_groups, filename);
    fclose(f);
    return ret;
}

static void config_parse_qdict_section(QDict *options, QemuOptsList *opts,
                                       Error **errp)
{
    QemuOpts *subopts;
    QDict *subqdict;
    QList *list = NULL;
    size_t orig_size, enum_size;
    char *prefix;

    prefix = g_strdup_printf("%s.", opts->name);
    qdict_extract_subqdict(options, &subqdict, prefix);
    g_free(prefix);
    orig_size = qdict_size(subqdict);
    if (!orig_size) {
        goto out;
    }

    subopts = qemu_opts_create(opts, NULL, 0, errp);
    if (!subopts) {
        goto out;
    }

    if (!qemu_opts_absorb_qdict(subopts, subqdict, errp)) {
        goto out;
    }

    enum_size = qdict_size(subqdict);
    if (enum_size < orig_size && enum_size) {
        error_setg(errp, "Unknown option '%s' for [%s]",
                   qdict_first(subqdict)->key, opts->name);
        goto out;
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
            goto out;
        }

        QLIST_FOREACH_ENTRY(list, list_entry) {
            QDict *section = qobject_to(QDict, qlist_entry_obj(list_entry));
            char *opt_name;

            if (!section) {
                error_setg(errp, "[%s] section (index %u) does not consist of "
                           "keys", opts->name, i);
                goto out;
            }

            opt_name = g_strdup_printf("%s.%u", opts->name, i++);
            subopts = qemu_opts_create(opts, opt_name, 1, errp);
            g_free(opt_name);
            if (!subopts) {
                goto out;
            }

            if (!qemu_opts_absorb_qdict(subopts, section, errp)) {
                qemu_opts_del(subopts);
                goto out;
            }

            if (qdict_size(section)) {
                error_setg(errp, "[%s] section doesn't support the option '%s'",
                           opts->name, qdict_first(section)->key);
                qemu_opts_del(subopts);
                goto out;
            }
        }
    }

out:
    qobject_unref(subqdict);
    qobject_unref(list);
}

void qemu_config_parse_qdict(QDict *options, QemuOptsList **lists,
                             Error **errp)
{
    int i;
    Error *local_err = NULL;

    for (i = 0; lists[i]; i++) {
        config_parse_qdict_section(options, lists[i], &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }
}
