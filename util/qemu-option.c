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

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qerror.h"
#include "qemu/option_int.h"
#include "qemu/cutils.h"
#include "qemu/id.h"
#include "qemu/help_option.h"

/*
 * Extracts the name of an option from the parameter string (@p points at the
 * first byte of the option name)
 *
 * The option name is @len characters long and is copied into @option. The
 * caller is responsible for free'ing @option when no longer required.
 *
 * The return value is the position of the delimiter/zero byte after the option
 * name in @p.
 */
//将buf中delim符号前的内容copy到p中，buf的最大长度为buf_size
static const char *get_opt_name(const char *p, char **option, size_t len)
{
    *option = g_strndup(p, len);
    return p + len;
}

/*
 * Extracts the value of an option from the parameter string p (p points at the
 * first byte of the option value)
 *
 * This function is comparable to get_opt_name with the difference that the
 * delimiter is fixed to be comma which starts a new option. To specify an
 * option value that contains commas, double each comma.
 */
const char *get_opt_value(const char *p, char **value)
{
    size_t capacity = 0, length;
    const char *offset;

    *value = NULL;
    while (1) {
        offset = qemu_strchrnul(p, ',');
        length = offset - p;
        if (*offset != '\0' && *(offset + 1) == ',') {
            length++;
        }
        *value = g_renew(char, *value, capacity + length + 1);
        strncpy(*value + capacity, p, length);
        (*value)[capacity + length] = '\0';
        capacity += length;
        if (*offset == '\0' ||
            *(offset + 1) != ',') {
            break;
        }

        p += (offset - p) + 2;
    }

    return offset;
}

static bool parse_option_number(const char *name, const char *value,
                                uint64_t *ret, Error **errp)
{
    uint64_t number;
    int err;

    //统一按无符号64位整数转
    err = qemu_strtou64(value, NULL, 0, &number);
    if (err == -ERANGE) {
        error_setg(errp, "Value '%s' is too large for parameter '%s'",
                   value, name);
        return false;
    }
    if (err) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, name, "a number");
        return false;
    }
    *ret = number;
    return true;
}

//通过名称查找desc
static const QemuOptDesc *find_desc_by_name(const QemuOptDesc *desc,
                                            const char *name)
{
    int i;

    for (i = 0; desc[i].name != NULL; i++) {
        if (strcmp(desc[i].name, name) == 0) {
            return &desc[i];
        }
    }

    return NULL;
}

static const char *find_default_by_name(QemuOpts *opts, const char *name)
{
    const QemuOptDesc *desc = find_desc_by_name(opts->list->desc, name);

    return desc ? desc->def_value_str : NULL;
}

//解析size类型数据，支持 k, M, G, T, P or E 等单位
bool parse_option_size(const char *name/*参数名称*/, const char *value/*参数字面值*/,
                       uint64_t *ret/*出参，解析结果*/, Error **errp)
{
    uint64_t size;
    int err;

    err = qemu_strtosz(value, NULL, &size);
    if (err == -ERANGE) {
        error_setg(errp, "Value '%s' is out of range for parameter '%s'",
                   value, name);
        return false;
    }
    if (err) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, name,
                   "a non-negative number below 2^64");
        error_append_hint(errp, "Optional suffix k, M, G, T, P or E means"
                          " kilo-, mega-, giga-, tera-, peta-\n"
                          "and exabytes, respectively.\n");
        return false;
    }
    *ret = size;
    return true;
}

static const char *opt_type_to_string(enum QemuOptType type)
{
    switch (type) {
    case QEMU_OPT_STRING:
        return "str";
    case QEMU_OPT_BOOL:
        return "bool (on/off)";
    case QEMU_OPT_NUMBER:
        return "num";
    case QEMU_OPT_SIZE:
        return "size";
    }

    g_assert_not_reached();
}

/**
 * Print the list of options available in the given list.  If
 * @print_caption is true, a caption (including the list name, if it
 * exists) is printed.  The options itself will be indented, so
 * @print_caption should only be set to false if the caller prints its
 * own custom caption (so that the indentation makes sense).
 */
//显示支持的选项
void qemu_opts_print_help(QemuOptsList *list, bool print_caption)
{
    QemuOptDesc *desc;
    int i;
    GPtrArray *array = g_ptr_array_new();

    assert(list);
    desc = list->desc;
    while (desc && desc->name) {
        GString *str = g_string_new(NULL);
        g_string_append_printf(str, "  %s=<%s>", desc->name,
                               opt_type_to_string(desc->type));
        if (desc->help) {
            if (str->len < 24) {
                g_string_append_printf(str, "%*s", 24 - (int)str->len, "");
            }
            g_string_append_printf(str, " - %s", desc->help);
        }
        g_ptr_array_add(array, g_string_free(str, false));
        desc++;
    }

    g_ptr_array_sort(array, (GCompareFunc)qemu_pstrcmp0);
    if (print_caption && array->len > 0) {
        if (list->name) {
            printf("%s options:\n", list->name);
        } else {
            printf("Options:\n");
        }
    } else if (array->len == 0) {
        if (list->name) {
            printf("There are no options for %s.\n", list->name);
        } else {
            printf("No options available.\n");
        }
    }
    for (i = 0; i < array->len; i++) {
        printf("%s\n", (char *)array->pdata[i]);
    }
    g_ptr_array_set_free_func(array, g_free);
    g_ptr_array_free(array, true);

}
/* ------------------------------------------------------------------ */

//自一组选项中取出名称为name的选项
QemuOpt *qemu_opt_find(QemuOpts *opts, const char *name)
{
    QemuOpt *opt;

    QTAILQ_FOREACH_REVERSE(opt, &opts->head, next) {
        if (strcmp(opt->name, name) != 0)
            continue;
        return opt;
    }
    return NULL;
}

//释放opt
static void qemu_opt_del(QemuOpt *opt)
{
    QTAILQ_REMOVE(&opt->opts->head, opt, next);
    g_free(opt->name);
    g_free(opt->str);
    g_free(opt);
}

/* qemu_opt_set allows many settings for the same option.
 * This function deletes all settings for an option.
 */
//自opts中删除名称为name的所有选项
static void qemu_opt_del_all(QemuOpts *opts, const char *name)
{
    QemuOpt *opt, *next_opt;

    QTAILQ_FOREACH_SAFE(opt, &opts->head, next, next_opt) {
        if (!strcmp(opt->name, name)) {
            qemu_opt_del(opt);
        }
    }
}

//获取opts选项中名称为name的字符串给值
const char *qemu_opt_get(QemuOpts *opts, const char *name)
{
    QemuOpt *opt;

    if (opts == NULL) {
        return NULL;
    }

    opt = qemu_opt_find(opts, name);
    if (!opt) {
    	//如果此选项没有指定，则使用默认值
        return find_default_by_name(opts, name);
    }

    return opt->str;
}

void qemu_opt_iter_init(QemuOptsIter *iter, QemuOpts *opts, const char *name)
{
    iter->opts = opts;
    iter->opt = QTAILQ_FIRST(&opts->head);
    iter->name = name;
}

const char *qemu_opt_iter_next(QemuOptsIter *iter)
{
    QemuOpt *ret = iter->opt;
    if (iter->name) {
        while (ret && !g_str_equal(iter->name, ret->name)) {
            ret = QTAILQ_NEXT(ret, next);
        }
    }
    iter->opt = ret ? QTAILQ_NEXT(ret, next) : NULL;
    return ret ? ret->str : NULL;
}

/* Get a known option (or its default) and remove it from the list
 * all in one action. Return a malloced string of the option value.
 * Result must be freed by caller with g_free().
 */
char *qemu_opt_get_del(QemuOpts *opts, const char *name)
{
    QemuOpt *opt;
    char *str;

    if (opts == NULL) {
        return NULL;
    }

    opt = qemu_opt_find(opts, name);
    if (!opt) {
        return g_strdup(find_default_by_name(opts, name));
    }
    str = opt->str;
    opt->str = NULL;
    qemu_opt_del_all(opts, name);
    return str;
}

bool qemu_opt_has_help_opt(QemuOpts *opts)
{
    QemuOpt *opt;

    QTAILQ_FOREACH_REVERSE(opt, &opts->head, next) {
        //是否为help option ,如 -o ?或者-o help
        if (is_help_option(opt->name)) {
            return true;
        }
    }
    return false;
}

//自选项中提取name的给值，如果没有给选项值，使用选项默认值，如果选项默认值不存在，使用defval
//del控制着是否删除所有名称为name的选项
static bool qemu_opt_get_bool_helper(QemuOpts *opts, const char *name,
                                     bool defval, bool del)
{
    QemuOpt *opt;
    const char *def_val;
    bool ret = defval;

    if (opts == NULL) {
        return ret;
    }

    opt = qemu_opt_find(opts, name);
    if (opt == NULL) {
    	//没有找到名称为name的opt,即选项没有给出，尝试选项默认值
        def_val = find_default_by_name(opts, name);
        if (def_val) {
            qapi_bool_parse(name, def_val, &ret, &error_abort);
        }
        return ret;
    }
    assert(opt->desc && opt->desc->type == QEMU_OPT_BOOL);
    ret = opt->value.boolean;
    if (del) {
	//如果需要删除此选项给值，则自opts中删除名称为name的选项
        qemu_opt_del_all(opts, name);
    }
    return ret;
}

//将选项name的给值按bool确定，默认值为defval
bool qemu_opt_get_bool(QemuOpts *opts, const char *name, bool defval)
{
    return qemu_opt_get_bool_helper(opts, name, defval, false);
}

bool qemu_opt_get_bool_del(QemuOpts *opts, const char *name, bool defval)
{
    return qemu_opt_get_bool_helper(opts, name, defval, true);
}

//自选项中提取number（与其它qemu_opt_get_*_helper系列意义相同）
static uint64_t qemu_opt_get_number_helper(QemuOpts *opts, const char *name,
                                           uint64_t defval, bool del)
{
    QemuOpt *opt;
    const char *def_val;
    uint64_t ret = defval;

    if (opts == NULL) {
        return ret;
    }

    opt = qemu_opt_find(opts, name);
    if (opt == NULL) {
        def_val = find_default_by_name(opts, name);
        if (def_val) {
            parse_option_number(name, def_val, &ret, &error_abort);
        }
        return ret;
    }
    assert(opt->desc && opt->desc->type == QEMU_OPT_NUMBER);
    ret = opt->value.uint;
    if (del) {
        qemu_opt_del_all(opts, name);
    }
    return ret;
}

uint64_t qemu_opt_get_number(QemuOpts *opts, const char *name, uint64_t defval)
{
    return qemu_opt_get_number_helper(opts, name, defval, false);
}

uint64_t qemu_opt_get_number_del(QemuOpts *opts, const char *name,
                                 uint64_t defval)
{
    return qemu_opt_get_number_helper(opts, name, defval, true);
}

static uint64_t qemu_opt_get_size_helper(QemuOpts *opts, const char *name,
                                         uint64_t defval, bool del)
{
    QemuOpt *opt;
    const char *def_val;
    uint64_t ret = defval;

    //如果opts为NULL,使用defval
    if (opts == NULL) {
        return ret;
    }

    //查name对应的opt,如果没有找到，则查描述信息，并解析默认值
    opt = qemu_opt_find(opts, name);
    if (opt == NULL) {
        def_val = find_default_by_name(opts, name);
        if (def_val) {
            parse_option_size(name, def_val, &ret, &error_abort);
        }
        return ret;
    }

    //返回opt配置的值
    assert(opt->desc && opt->desc->type == QEMU_OPT_SIZE);
    ret = opt->value.uint;
    if (del) {
        qemu_opt_del_all(opts, name);
    }
    return ret;
}

uint64_t qemu_opt_get_size(QemuOpts *opts, const char *name, uint64_t defval)
{
    return qemu_opt_get_size_helper(opts, name, defval, false);
}

//取出获项中配置的name对应的值，如果opts为空，使用defval
uint64_t qemu_opt_get_size_del(QemuOpts *opts, const char *name,
                               uint64_t defval)
{
    return qemu_opt_get_size_helper(opts, name, defval, true);
}

//解析选项值
static bool qemu_opt_parse(QemuOpt *opt, Error **errp)
{
    if (opt->desc == NULL)
        return true;

    switch (opt->desc->type) {
    case QEMU_OPT_STRING://字符串类型什么也不需要做
        /* nothing */
        return true;
    case QEMU_OPT_BOOL://转为boolean类型
        return qapi_bool_parse(opt->name, opt->str, &opt->value.boolean, errp);
    case QEMU_OPT_NUMBER://转为number类型
        return parse_option_number(opt->name, opt->str, &opt->value.uint,
                                   errp);
    case QEMU_OPT_SIZE://解析为size方式，例如（K,M,G,T...)
        return parse_option_size(opt->name, opt->str, &opt->value.uint,
                                 errp);
    default:
        abort();
    }
}

//选项是否接受任意值（如果QemuOptsList的desc的首个name为NULL，则选项值接受任意值）
static bool opts_accepts_any(const QemuOptsList *list)
{
    return list->desc[0].name == NULL;
}

int qemu_opt_unset(QemuOpts *opts, const char *name)
{
    QemuOpt *opt = qemu_opt_find(opts, name);

    assert(opts_accepts_any(opts->list));

    if (opt == NULL) {
        return -1;
    } else {
        qemu_opt_del(opt);
        return 0;
    }
}

//构造QemuOpt将其串在opts链上，prepend用于控制是否串在最前面
static QemuOpt *opt_create(QemuOpts *opts, const char *name/*参数名*/, char *value/*参数值*/,
                           bool prepend)
{
    QemuOpt *opt = g_malloc0(sizeof(*opt));

    opt->name = g_strdup(name);//选项名称
    opt->str = value;
    opt->opts = opts;
    if (prepend) {
        QTAILQ_INSERT_HEAD(&opts->head, opt, next);
    } else {
        QTAILQ_INSERT_TAIL(&opts->head, opt, next);
    }

    return opt;
}

static bool opt_validate(QemuOpt *opt, bool *help_wanted,
                         Error **errp)
{
    const QemuOptDesc *desc;
    const QemuOptsList *list = opt->opts->list;

    desc = find_desc_by_name(list->desc, opt->name);
    if (!desc && !opts_accepts_any(list)) {
        error_setg(errp, QERR_INVALID_PARAMETER, opt->name);
        if (help_wanted && is_help_option(opt->name)) {
            *help_wanted = true;
        }
        return false;
    }

    opt->desc = desc;
    //解析选项取值
    if (!qemu_opt_parse(opt, errp)) {
        return false;
    }

    return true;
}

//qemu选项设置,name为选项名，value为选项值
bool qemu_opt_set(QemuOpts *opts, const char *name, const char *value,
                  Error **errp)
{
    QemuOpt *opt = opt_create(opts, name, g_strdup(value), false);

    if (!opt_validate(opt, NULL, errp)) {
        qemu_opt_del(opt);
        return false;
    }
    return true;
}

bool qemu_opt_set_bool(QemuOpts *opts, const char *name, bool val,
                       Error **errp)
{
    QemuOpt *opt;
    const QemuOptDesc *desc;
    const QemuOptsList *list = opts->list;

    desc = find_desc_by_name(list->desc, name);
    if (!desc && !opts_accepts_any(list)) {
        error_setg(errp, QERR_INVALID_PARAMETER, name);
        return false;
    }

    opt = g_malloc0(sizeof(*opt));
    opt->name = g_strdup(name);
    opt->opts = opts;
    opt->desc = desc;
    opt->value.boolean = !!val;
    opt->str = g_strdup(val ? "on" : "off");
    QTAILQ_INSERT_TAIL(&opts->head, opt, next);
    return true;
}

bool qemu_opt_set_number(QemuOpts *opts, const char *name, int64_t val,
                         Error **errp)
{
    QemuOpt *opt;
    const QemuOptDesc *desc;
    const QemuOptsList *list = opts->list;

    desc = find_desc_by_name(list->desc, name);
    if (!desc && !opts_accepts_any(list)) {
    	//没有找到，且opts首个name不为空，报错
        error_setg(errp, QERR_INVALID_PARAMETER, name);
        return false;
    }

    //创建opt
    opt = g_malloc0(sizeof(*opt));
    //构造相应的opt
    opt->name = g_strdup(name);
    opt->opts = opts;
    opt->desc = desc;
    opt->value.uint = val;
    opt->str = g_strdup_printf("%" PRId64, val);
    QTAILQ_INSERT_TAIL(&opts->head, opt, next);
    return true;
}

/**
 * For each member of @opts, call @func(@opaque, name, value, @errp).
 * @func() may store an Error through @errp, but must return non-zero then.
 * When @func() returns non-zero, break the loop and return that value.
 * Return zero when the loop completes.
 */
int qemu_opt_foreach(QemuOpts *opts, qemu_opt_loopfunc func, void *opaque,
                     Error **errp)
{
    QemuOpt *opt;
    int rc;

    QTAILQ_FOREACH(opt, &opts->head, next) {
        rc = func(opaque, opt->name, opt->str, errp);
        if (rc) {
            return rc;
        }
        assert(!errp || !*errp);
    }
    return 0;
}

//检查选项列表中是否已存在此id(NULL==NULL)
QemuOpts *qemu_opts_find(QemuOptsList *list, const char *id)
{
    QemuOpts *opts;

    QTAILQ_FOREACH(opts, &list->head, next) {
        if (!opts->id && !id) {
            return opts;
        }
        if (opts->id && id && !strcmp(opts->id, id)) {
            return opts;
        }
    }
    return NULL;
}

//创建一个选项队列头
QemuOpts *qemu_opts_create(QemuOptsList *list, const char *id,
                           int fail_if_exists/*如果id已存在是否报错*/, Error **errp)
{
    QemuOpts *opts = NULL;

    if (id) {
    	//id格式有误时报错
        if (!id_wellformed(id)) {
            error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "id",
                       "an identifier");
            error_append_hint(errp, "Identifiers consist of letters, digits, "
                              "'-', '.', '_', starting with a letter.\n");
            return NULL;
        }

        //检查是否已存在
        opts = qemu_opts_find(list, id);
        if (opts != NULL) {
        	//检查是否有必要报错，有则报错
            if (fail_if_exists && !list->merge_lists) {
                error_setg(errp, "Duplicate ID '%s' for %s", id, list->name);
                return NULL;
            } else {
            	//使用已有的
                return opts;
            }
        }
    } else if (list->merge_lists) {
    	//list支持merge,用NULL做id查一遍
        opts = qemu_opts_find(list, NULL);
        if (opts) {
            return opts;
        }
    }

    //确实不存在此opts,生成opts
    opts = g_malloc0(sizeof(*opts));
    opts->id = g_strdup(id);//填充id
    opts->list = list;//指明自已属于那个list
    loc_save(&opts->loc);//记录位置信息
    QTAILQ_INIT(&opts->head);
    QTAILQ_INSERT_TAIL(&list->head, opts, next);//将opts加入list链中
    return opts;
}

//将optslist清空
void qemu_opts_reset(QemuOptsList *list)
{
    QemuOpts *opts, *next_opts;

    QTAILQ_FOREACH_SAFE(opts, &list->head, next, next_opts) {
    	//清空opts及其下的opt
        qemu_opts_del(opts);
    }
}

void qemu_opts_loc_restore(QemuOpts *opts)
{
    loc_restore(&opts->loc);
}

//向给定的list中添加id为*id的一个选项，选项名称为name,选项值为vlaue
bool qemu_opts_set(QemuOptsList *list, const char *name, const char *value, Error **errp)
{
    QemuOpts *opts;

    assert(list->merge_lists);
    opts = qemu_opts_create(list, NULL, 0, &error_abort);
    return qemu_opt_set(opts, name, value, errp);
}

const char *qemu_opts_id(QemuOpts *opts)
{
    return opts->id;
}

/* The id string will be g_free()d by qemu_opts_del */
void qemu_opts_set_id(QemuOpts *opts, char *id)
{
    opts->id = id;
}

//删除opts
void qemu_opts_del(QemuOpts *opts)
{
    QemuOpt *opt;

    if (opts == NULL) {
        return;
    }

    for (;;) {
        opt = QTAILQ_FIRST(&opts->head);
        if (opt == NULL)
            break;
        qemu_opt_del(opt);
    }
    QTAILQ_REMOVE(&opts->list->head, opts, next);
    g_free(opts->id);
    g_free(opts);
}

/* print value, escaping any commas in value */
static void escaped_print(const char *value)
{
    const char *ptr;

    for (ptr = value; *ptr; ++ptr) {
        if (*ptr == ',') {
            putchar(',');
        }
        putchar(*ptr);
    }
}

void qemu_opts_print(QemuOpts *opts, const char *separator)
{
    QemuOpt *opt;
    QemuOptDesc *desc = opts->list->desc;
    const char *sep = "";

    if (opts->id) {
        printf("id=%s", opts->id); /* passed id_wellformed -> no commas */
        sep = separator;
    }

    if (desc[0].name == NULL) {
        QTAILQ_FOREACH(opt, &opts->head, next) {
            printf("%s%s=", sep, opt->name);
            escaped_print(opt->str);
            sep = separator;
        }
        return;
    }
    for (; desc && desc->name; desc++) {
        const char *value;
        opt = qemu_opt_find(opts, desc->name);

        value = opt ? opt->str : desc->def_value_str;
        if (!value) {
            continue;
        }
        if (desc->type == QEMU_OPT_STRING) {
            printf("%s%s=", sep, desc->name);
            escaped_print(value);
        } else if ((desc->type == QEMU_OPT_SIZE ||
                    desc->type == QEMU_OPT_NUMBER) && opt) {
            printf("%s%s=%" PRId64, sep, desc->name, opt->value.uint);
        } else {
            printf("%s%s=%s", sep, desc->name, value);
        }
        sep = separator;
    }
}

static const char *get_opt_name_value(const char *params,
                                      const char *firstname,
                                      char **name, char **value)
{
    const char *p;
    size_t len;

    len = strcspn(params, "=,");
    if (params[len] != '=') {
        /* found "foo,more" */
        if (firstname) {
            /* implicitly named first option */
            *name = g_strdup(firstname);
            p = get_opt_value(params, value);
        } else {
            /* option without value, must be a flag */
            p = get_opt_name(params, name, len);
            if (strncmp(*name, "no", 2) == 0) {
                memmove(*name, *name + 2, strlen(*name + 2) + 1);
                *value = g_strdup("off");
            } else {
                *value = g_strdup("on");
            }
        }
    } else {
        /* found "foo=bar,more" */
        p = get_opt_name(params, name, len);
        assert(*p == '=');
        p++;
        p = get_opt_value(p, value);
    }

    assert(!*p || *p == ',');
    if (*p == ',') {
        p++;
    }
    return p;
}

//解析params中指明的选项，将选项串连在opts上，prepend用于控制是否串在首位
static bool opts_do_parse(QemuOpts *opts, const char *params,
                          const char *firstname, bool prepend,
                          bool *help_wanted, Error **errp)
{
    char *option, *value;
    const char *p;
    QemuOpt *opt;

    for (p = params; *p;) {
        p = get_opt_name_value(p, firstname, &option, &value);
        firstname = NULL;

        if (!strcmp(option, "id")) {
            g_free(option);
            g_free(value);
            continue;
        }

        opt = opt_create(opts, option, value, prepend);
        g_free(option);
        if (!opt_validate(opt, help_wanted, errp)) {
            qemu_opt_del(opt);
            return false;
        }
    }

    return true;
}

static char *opts_parse_id(const char *params)
{
    const char *p;
    char *name, *value;

    for (p = params; *p;) {
        p = get_opt_name_value(p, NULL, &name, &value);
        if (!strcmp(name, "id")) {
            g_free(name);
            return value;
        }
        g_free(name);
        g_free(value);
    }

    return NULL;
}

//检查param中是否有help选项
bool has_help_option(const char *params)
{
    const char *p;
    char *name, *value;
    bool ret;

    for (p = params; *p;) {
        p = get_opt_name_value(p, NULL, &name, &value);
        //是否为help option ,如 -o ?或者-o help
        ret = is_help_option(name);
        g_free(name);
        g_free(value);
        if (ret) {
            return true;
        }
    }

    return false;
}

/**
 * Store options parsed from @params into @opts.
 * If @firstname is non-null, the first key=value in @params may omit
 * key=, and is treated as if key was @firstname.
 * On error, store an error object through @errp if non-null.
 */
//解析选项，并生成opts对象
bool qemu_opts_do_parse(QemuOpts *opts, const char *params,
                       const char *firstname, Error **errp)
{
    return opts_do_parse(opts, params, firstname, false, NULL, errp);
}

//由于list为一组opts,而params为一组配置字符串，利用list将params转换单个的QemuOpts，并返回
//例如“-chardev socket,id=char1,path=/usr/local/var/run/openvswitch/vhost-user-1”
static QemuOpts *opts_parse(QemuOptsList *list, const char *params,
                            bool permit_abbrev, bool defaults,
                            bool *help_wanted, Error **errp)
{
    const char *firstname;
    char *id = opts_parse_id(params);
    QemuOpts *opts;

    //如果支持隐含名称，则使用implied_opt_name值为隐含名称
    //例如-char的隐含名称为'backend',在此行将记为firstname
    assert(!permit_abbrev || list->implied_opt_name);
    firstname = permit_abbrev ? list->implied_opt_name : NULL;

    /*
     * This code doesn't work for defaults && !list->merge_lists: when
     * params has no id=, and list has an element with !opts->id, it
     * appends a new element instead of returning the existing opts.
     * However, we got no use for this case.  Guard against possible
     * (if unlikely) future misuse:
     */
    assert(!defaults || list->merge_lists);
    //利用id值，创建opts
    opts = qemu_opts_create(list, id, !defaults, errp);
    g_free(id);
    if (opts == NULL) {
        /*记录出错信息*/
        return NULL;
    }

    //解析此id对应的其它参数
    if (!opts_do_parse(opts, params, firstname, defaults, help_wanted,
                       errp)) {
        qemu_opts_del(opts);
        return NULL;
    }

    return opts;
}

/**
 * Create a QemuOpts in @list and with options parsed from @params.
 * If @permit_abbrev, the first key=value in @params may omit key=,
 * and is treated as if key was @list->implied_opt_name.
 * On error, store an error object through @errp if non-null.
 * Return the new QemuOpts on success, null pointer on error.
 */
QemuOpts *qemu_opts_parse(QemuOptsList *list, const char *params,
                          bool permit_abbrev, Error **errp)
{
    return opts_parse(list, params, permit_abbrev, false, NULL, errp);
}

/**
 * Create a QemuOpts in @list and with options parsed from @params.
 * If @permit_abbrev, the first key=value in @params may omit key=,
 * and is treated as if key was @list->implied_opt_name.
 * Report errors with error_report_err().  This is inappropriate in
 * QMP context.  Do not use this function there!
 * Return the new QemuOpts on success, null pointer on error.
 */
//解析参数串，构造opts，并加入到list
QemuOpts *qemu_opts_parse_noisily(QemuOptsList *list, const char *params,
                                  bool permit_abbrev)
{
    Error *err = NULL;
    QemuOpts *opts;
    bool help_wanted = false;

    //解析字符串，生成opts
    opts = opts_parse(list, params, permit_abbrev, false, &help_wanted, &err);
    if (err) {
        if (help_wanted) {
            qemu_opts_print_help(list, true);
            error_free(err);
        } else {
            error_report_err(err);
        }
    }
    return opts;
}

void qemu_opts_set_defaults(QemuOptsList *list, const char *params,
                            int permit_abbrev)
{
    QemuOpts *opts;

    opts = opts_parse(list, params, permit_abbrev, true, NULL, NULL);
    assert(opts);
}

static bool qemu_opts_from_qdict_entry(QemuOpts *opts,
                                       const QDictEntry *entry,
                                       Error **errp)
{
    const char *key = qdict_entry_key(entry);
    QObject *obj = qdict_entry_value(entry);
    char buf[32];
    g_autofree char *tmp = NULL;
    const char *value;

    if (!strcmp(key, "id")) {
        return true;
    }

    switch (qobject_type(obj)) {
    case QTYPE_QSTRING:
        value = qstring_get_str(qobject_to(QString, obj));
        break;
    case QTYPE_QNUM:
        tmp = qnum_to_string(qobject_to(QNum, obj));
        value = tmp;
        break;
    case QTYPE_QBOOL:
        pstrcpy(buf, sizeof(buf),
                qbool_get_bool(qobject_to(QBool, obj)) ? "on" : "off");
        value = buf;
        break;
    default:
        return true;
    }

    return qemu_opt_set(opts, key, value, errp);
}

/*
 * Create QemuOpts from a QDict.
 * Use value of key "id" as ID if it exists and is a QString.  Only
 * QStrings, QNums and QBools are copied.  Entries with other types
 * are silently ignored.
 */
QemuOpts *qemu_opts_from_qdict(QemuOptsList *list, const QDict *qdict,
                               Error **errp)
{
    QemuOpts *opts;
    const QDictEntry *entry;

    opts = qemu_opts_create(list, qdict_get_try_str(qdict, "id"), 1, errp);
    if (!opts) {
        return NULL;
    }

    assert(opts != NULL);

    for (entry = qdict_first(qdict);
         entry;
         entry = qdict_next(qdict, entry)) {
        if (!qemu_opts_from_qdict_entry(opts, entry, errp)) {
            qemu_opts_del(opts);
            return NULL;
        }
    }

    return opts;
}

/*
 * Adds all QDict entries to the QemuOpts that can be added and removes them
 * from the QDict. When this function returns, the QDict contains only those
 * entries that couldn't be added to the QemuOpts.
 */
bool qemu_opts_absorb_qdict(QemuOpts *opts, QDict *qdict, Error **errp)
{
    const QDictEntry *entry, *next;

    entry = qdict_first(qdict);

    while (entry != NULL) {
        next = qdict_next(qdict, entry);

        if (find_desc_by_name(opts->list->desc, entry->key)) {
            if (!qemu_opts_from_qdict_entry(opts, entry, errp)) {
                return false;
            }
            qdict_del(qdict, entry->key);
        }

        entry = next;
    }

    return true;
}

/*
 * Convert from QemuOpts to QDict. The QDict values are of type QString.
 *
 * If @list is given, only add those options to the QDict that are contained in
 * the list. If @del is true, any options added to the QDict are removed from
 * the QemuOpts, otherwise they remain there.
 *
 * If two options in @opts have the same name, they are processed in order
 * so that the last one wins (consistent with the reverse iteration in
 * qemu_opt_find()), but all of them are deleted if @del is true.
 *
 * TODO We'll want to use types appropriate for opt->desc->type, but
 * this is enough for now.
 */
QDict *qemu_opts_to_qdict_filtered(QemuOpts *opts, QDict *qdict,
                                   QemuOptsList *list, bool del)
{
    QemuOpt *opt, *next;

    if (!qdict) {
        qdict = qdict_new();
    }
    if (opts->id) {
        qdict_put_str(qdict, "id", opts->id);
    }
    QTAILQ_FOREACH_SAFE(opt, &opts->head, next, next) {
        if (list) {
            QemuOptDesc *desc;
            bool found = false;
            for (desc = list->desc; desc->name; desc++) {
                if (!strcmp(desc->name, opt->name)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        }
        qdict_put_str(qdict, opt->name, opt->str);
        if (del) {
            qemu_opt_del(opt);
        }
    }
    return qdict;
}

/* Copy all options in a QemuOpts to the given QDict. See
 * qemu_opts_to_qdict_filtered() for details. */
QDict *qemu_opts_to_qdict(QemuOpts *opts, QDict *qdict)
{
    return qemu_opts_to_qdict_filtered(opts, qdict, NULL, false);
}

/* Validate parsed opts against descriptions where no
 * descriptions were provided in the QemuOptsList.
 */
bool qemu_opts_validate(QemuOpts *opts, const QemuOptDesc *desc, Error **errp)
{
    QemuOpt *opt;

    assert(opts_accepts_any(opts->list));

    QTAILQ_FOREACH(opt, &opts->head, next) {
        opt->desc = find_desc_by_name(desc, opt->name);
        if (!opt->desc) {
            error_setg(errp, QERR_INVALID_PARAMETER, opt->name);
            return false;
        }

        if (!qemu_opt_parse(opt, errp)) {
            return false;
        }
    }

    return true;
}

/**
 * For each member of @list, call @func(@opaque, member, @errp).
 * Call it with the current location temporarily set to the member's.
 * @func() may store an Error through @errp, but must return non-zero then.
 * When @func() returns non-zero, break the loop and return that value.
 * Return zero when the loop completes.
 */
//采用函数func遍历给定optslist中每一个opts
int qemu_opts_foreach(QemuOptsList *list, qemu_opts_loopfunc func,
                      void *opaque, Error **errp)
{
    Location loc;
    QemuOpts *opts;
    int rc = 0;

    loc_push_none(&loc);
    QTAILQ_FOREACH(opts, &list->head, next) {
        loc_restore(&opts->loc);
        rc = func(opaque, opts, errp);//针对每一个opts，调用函数func
        if (rc) {
        	//如果调用失败，则不再继续
            break;
        }
        assert(!errp || !*errp);
    }
    loc_pop(&loc);
    return rc;
}

//描述信息数组list->desc大小
static size_t count_opts_list(QemuOptsList *list)
{
    QemuOptDesc *desc = NULL;
    size_t num_opts = 0;

    if (!list) {
        return 0;//list为空时，返回0
    }

    desc = list->desc;
    while (desc && desc->name) {
        num_opts++;
        desc++;
    }

    return num_opts;
}

void qemu_opts_free(QemuOptsList *list)
{
    g_free(list);
}

/* Realloc dst option list and append options from an option list (list)
 * to it. dst could be NULL or a malloced list.
 * The lifetime of dst must be shorter than the input list because the
 * QemuOptDesc->name, ->help, and ->def_value_str strings are shared.
 */
//将list的选项信息合并到dst中
QemuOptsList *qemu_opts_append(QemuOptsList *dst,
                               QemuOptsList *list)
{
    size_t num_opts, num_dst_opts;
    QemuOptDesc *desc;
    bool need_init = false;
    bool need_head_update;

    if (!list) {
    	//如果list为空，就直接返回dst
        return dst;
    }

    /* If dst is NULL, after realloc, some area of dst should be initialized
     * before adding options to it.
     */
    if (!dst) {
    	//如果dst为空，则need_init为true
        need_init = true;
        need_head_update = true;
    } else {
        /* Moreover, even if dst is not NULL, the realloc may move it to a
         * different address in which case we may get a stale tail pointer
         * in dst->head. */
        need_head_update = QTAILQ_EMPTY(&dst->head);
    }

    num_opts = count_opts_list(dst);//已有的数目
    num_dst_opts = num_opts;
    num_opts += count_opts_list(list);//合并后总数
    //保留dst,并扩大内存
    dst = g_realloc(dst, sizeof(QemuOptsList) +
                    (num_opts + 1) * sizeof(QemuOptDesc));
    if (need_init) {
    	//第一次存放，dst需要初始化
        dst->name = NULL;
        dst->implied_opt_name = NULL;
        dst->merge_lists = false;
    }

    //是否需要初始化head
    if (need_head_update) {
        QTAILQ_INIT(&dst->head);
    }
    dst->desc[num_dst_opts].name = NULL;//最后一块赋为空，准备放入

    /* append list->desc to dst->desc */
    if (list) {
    		//添加list->desc的值到dst中
        desc = list->desc;
        while (desc && desc->name) {
        	//如果现有的desc中没有，则加入
            if (find_desc_by_name(dst->desc, desc->name) == NULL) {
                dst->desc[num_dst_opts++] = *desc;
                dst->desc[num_dst_opts].name = NULL;
            }
            desc++;
        }
    }

    return dst;
}
