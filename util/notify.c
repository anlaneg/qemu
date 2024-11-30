/*
 * Notifier lists
 *
 * Copyright IBM, Corp. 2010
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
#include "qemu/notify.h"

//notifier list初始化
void notifier_list_init(NotifierList *list)
{
    QLIST_INIT(&list->notifiers);
}

//向list中添加新的notifier
void notifier_list_add(NotifierList *list, Notifier *notifier)
{
    QLIST_INSERT_HEAD(&list->notifiers, notifier, node);
}

//notifiy list中移除指定通知回调
void notifier_remove(Notifier *notifier)
{
    QLIST_REMOVE(notifier, node);
}

//调用list上所有的notify,触发所有回调
void notifier_list_notify(NotifierList *list, void *data)
{
    Notifier *notifier, *next;

    //遍历list上的通知回调，逐个回调
    QLIST_FOREACH_SAFE(notifier, &list->notifiers, node, next) {
        notifier->notify(notifier, data);
    }
}

//检查Notify list是否为空
bool notifier_list_empty(NotifierList *list)
{
    return QLIST_EMPTY(&list->notifiers);
}

//含返回值的通知链初始化，添加，移除
void notifier_with_return_list_init(NotifierWithReturnList *list)
{
    QLIST_INIT(&list->notifiers);
}

void notifier_with_return_list_add(NotifierWithReturnList *list,
                                   NotifierWithReturn *notifier)
{
    QLIST_INSERT_HEAD(&list->notifiers, notifier, node);
}

void notifier_with_return_remove(NotifierWithReturn *notifier)
{
    QLIST_REMOVE(notifier, node);
}

//触发list上的所有notifier
int notifier_with_return_list_notify(NotifierWithReturnList *list, void *data,
                                     Error **errp)
{
    NotifierWithReturn *notifier, *next;
    int ret = 0;

    QLIST_FOREACH_SAFE(notifier, &list->notifiers, node, next) {
        ret = notifier->notify(notifier, data, errp);
        if (ret != 0) {
            break;//如果任意一个返回非0,则停止并返回
        }
    }
    return ret;
}
