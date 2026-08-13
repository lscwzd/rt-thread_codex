/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2006-03-16     Bernard      第一个版本
 * 2006-09-07     Bernard      将 kservice API 移至 rtthread.h
 * 2007-06-27     Bernard      修复rt_list_remove错误
 * 2012-03-22     Bernard      将 kservice.h 重命名为 rtservice.h
 * 2017-11-15     JasonJia     将 rt_slist_foreach 修改为 rt_slist_for_each_entry。
 *                             Make code cleanup.
 * 2024-01-03     Shell        添加 rt_slist_pop()
 */

#ifndef __RT_SERVICE_H__
#define __RT_SERVICE_H__

/**
 * @file rtservice.h
 * @brief RT-Thread 全局使用的仅头文件侵入式链表服务。
 *
 * 链表节点嵌入其所属对象，而不是单独分配包装对象。这样无需分配内存，并可通过嵌入多个具名
 * 节点让一个对象加入多个相互独立的链表。转换宏会根据节点地址和成员偏移恢复所属对象地址。
 *
 * 提供两种链表表示：
 *
 * - rt_list_t 是带哨兵头的循环双向链表。已知目标节点时，插入和移除均为常数时间。
 * - rt_slist_t 是带哨兵头、以 NULL 结尾的单向链表。它占用更少存储，但尾部插入和按前驱
 *   移除均为线性时间操作。
 *
 * 这些辅助函数均不加锁、不验证所有权，也不检测节点是否被插入多个链表。调用者必须串行化
 * 并发访问，并保持一个节点通过同一嵌入成员至多属于一个链表的约束。
 */

#include <rtdef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup group_kernel_service
 */

/**@{*/

/**
 * @brief 根据指向成员的指针恢复外层对象。
 *
 * 该表达式使用 `&((type *)0)->member` 计算 @p member 在 @p type 中的字节偏移，从
 * @p ptr 减去该偏移，再将结果转换为 `type *`。这是所有侵入式链表入口宏的基础。
 *
 * @param ptr 指向存活对象中嵌入成员的指针。
 * @param type 完整的外层结构类型。
 * @param member @p type 中的成员名称，不是字符串。
 * @return 指向外层 @p type 对象的指针。
 *
 * @warning 宏无法验证 @p ptr 是否确实指向所命名的成员。传入 RT_NULL（偏移为零时除外）、
 * 错误成员或其他类型的指针都会得到无效结果。该宏只执行指针运算，不管理对象生命周期。
 */
#define rt_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))


/**
 * @brief 循环双向链表头/节点的静态初始化器。
 *
 * 两个链接都指回 @p object，表示空的哨兵链表，也表示 rt_list_remove() 使用的脱链状态。
 * 参数必须是待初始化对象的左值名称。
 */
#define RT_LIST_OBJECT_INIT(object) { &(object), &(object) }

/**
 * @brief 将循环双向链表初始化为空。
 *
 * @param l 要将 next 和 prev 链接重置为自身的表头或脱链节点。
 * @note 重新初始化仍在链表中的节点，会使旧相邻节点指向现在自链接的节点并破坏旧链表；应先将其
 *       从旧链表移除。
 */
rt_inline void rt_list_init(rt_list_t *l)
{
    l->next = l->prev = l;
}

/**
 * @brief 将 @p n 立即插入 @p l 之后。
 *
 * @param l 已存在的链表节点；传入哨兵会在表头插入。
 * @param n 要插入的脱链节点。
 *
 * 会更新四个链接，使正向和反向遍历均有效。该操作为 O(1)，且不检查 @p n 是否已经链接。
 */
rt_inline void rt_list_insert_after(rt_list_t *l, rt_list_t *n)
{
    l->next->prev = n;
    n->next = l->next;

    l->next = n;
    n->prev = l;
}

/**
 * @brief 将 @p n 立即插入 @p l 之前。
 *
 * @param l 已存在的链表节点；传入哨兵会在表尾追加。
 * @param n 要插入的脱链节点。
 *
 * 该操作为 O(1)，且不检查 @p n 是否已经链接。
 */
rt_inline void rt_list_insert_before(rt_list_t *l, rt_list_t *n)
{
    l->prev->next = n;
    n->prev = l->prev;

    l->prev = n;
    n->next = l;
}

/**
 * @brief 将 @p n 从其循环双向链表中脱链。
 * @param n 要移除的已链接节点。
 *
 * 修复两个相邻节点后，该节点会自链接。此脱链状态允许重复移除而不破坏其他链表，但这种用法
 * 仍可能表示生命周期错误。尽管指针更新可以编译，从所属容器角度移除哨兵表头仍是无效操作。
 */
rt_inline void rt_list_remove(rt_list_t *n)
{
    n->next->prev = n->prev;
    n->prev->next = n->next;

    n->next = n->prev = n;
}

/**
 * @brief 测试循环链表是否不包含@p l之后的节点。
 * 当 @p l 通过 next 指向自身时，@return 非零；否则为零。
 * @note 通常@p l 是哨兵头。分离的普通节点也显示为空，因为删除使其成为自链接。
 */
rt_inline int rt_list_isempty(const rt_list_t *l)
{
    return l->next == l;
}

/**
 * @brief 计数哨兵头@p l之后的节点，直到遍历返回到它。
 * @return 数据节点数量，不包括@p l。
 * @note 这是 O(n)。并发突变或损坏的链接可能会使结果不一致或阻止终止。
 */
rt_inline unsigned int rt_list_len(const rt_list_t *l)
{
    unsigned int len = 0;
    const rt_list_t *p = l;
    while (p->next != l)
    {
        p = p->next;
        len ++;
    }

    return len;
}

/**
 * @brief 将嵌入的双向链接节点转换为其所有者对象。
 * @param node 指向所有者的@p member 节点的指针。
 * @param type 所有者结构类型。
 * @param member 嵌入 rt_list_t 成员名称。
 * @return 指向封闭所有者对象的指针。
 * @warning @p node 必须对应于实际 @p type 对象中的 @p member。
 */
#define rt_list_entry(node, type, member) \
    rt_container_of(node, type, member)

/**
 * @brief 迭代循环双向链表中的原始节点。
 * @param pos rt_list_t 指针用作循环光标并修改每一遍。
 * @param head 哨兵头；由生成的循环重复评估。
 *
 * 迭代从head->next开始，在访问@p head之前结束。请勿移除
 * @p pos 在循环体中，因为增量在循环体之后读取 pos->next；当当前节点可能被删除时，使用 rt_list_for_each_safe()。
 */
#define rt_list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * @brief 迭代原始节点，同时允许删除当前节点。
 * @param pos 当前 rt_list_t 光标。
 * @param n 临时 rt_list_t 游标持有主体之前的下一个节点。
 * @param head 哨兵列表头。
 *
 * 调用者必须为 @p pos 和 @p n 提供不同的左值。在循环内删除或重新定位 @p n 仍然会使保存的遍历无效。
 */
#define rt_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
        pos = n, n = pos->next)

/**
 * @brief 迭代循环双向链表中的所有者对象。
 * @param pos 所有者类型指针，用作循环光标并修改。
 * @param head 哨兵列表头。
 * @param member rt_list_t 成员，通过该成员链接所有者。
 *
 * rt_typeof(*pos) 推断所有者类型，rt_list_entry() 执行侵入节点转换。用于循环终止的哨兵派生游标绝不能作为真实对象取消引用。当当前条目被删除时，此表单不安全。
 */
#define rt_list_for_each_entry(pos, head, member) \
    for (pos = rt_list_entry((head)->next, rt_typeof(*pos), member); \
         &pos->member != (head); \
         pos = rt_list_entry(pos->member.next, rt_typeof(*pos), member))

/**
 * @brief 迭代所有者对象，同时允许删除当前对象。
 * @param pos 当前所有者对象光标。
 * @param n 保存下一个条目的临时所有者对象光标。
 * @param head 哨兵列表头。
 * @param member 此列表使用的嵌入式 rt_list_t 成员。
 *
 * @p pos 和 @p n 必须是兼容指针类型的不同左值。下一个条目是在循环体之前捕获的，因此取消链接 @p pos 是安全的；删除或重新链接已保存的 @p n 需要单独小心。
 */
#define rt_list_for_each_entry_safe(pos, n, head, member) \
    for (pos = rt_list_entry((head)->next, rt_typeof(*pos), member), \
         n = rt_list_entry(pos->member.next, rt_typeof(*pos), member); \
         &pos->member != (head); \
         pos = n, n = rt_list_entry(n->member.next, rt_typeof(*n), member))

/**
 * @brief 返回哨兵@p ptr之后第一个节点的所有者。
 * @param ptr 哨兵列表头。
 * @param type 所有者结构类型。
 * @param member 嵌入 rt_list_t 成员名称。
 *
 * @warning 该列表必须非空。在空列表上，ptr->next 是哨兵而不是 @p type 的嵌入成员，因此结果无效。
 */
#define rt_list_first_entry(ptr, type, member) \
    rt_list_entry((ptr)->next, type, member)

/** 空的 NULL 终止的单链表头的静态初始化程序。 */
#define RT_SLIST_OBJECT_INIT(object) { RT_NULL }

/**
 * @brief 将 NULL 终止的单链表初始化为空。
 *
 * @param l 下一个链接变为 RT_NULL 的哨兵头或分离节点。
 * @note 重新初始化链接节点不会修复其前任节点。
 */
rt_inline void rt_slist_init(rt_slist_t *l)
{
    l->next = RT_NULL;
}

/**
 * @brief 将分离节点 @p n 附加到以 @p l 为首的列表的尾部。
 *
 * 从@p l开始遍历，一直往下遍历，直到找到当前尾部，所以这个操作就是O(n)。该函数强制 n->next 到 RT_NULL，因此附加一个节点，而不是预先存在的链。它不检测重复链接。
 */
rt_inline void rt_slist_append(rt_slist_t *l, rt_slist_t *n)
{
    struct rt_slist_node *node;

    node = l;
    while (node->next) node = node->next;

    /* 将节点追加到尾部 */
    node->next = n;
    n->next = RT_NULL;
}

/**
 * @brief 紧接着节点 @p l 插入 @p n。
 *
 * 通过哨兵头在 O(1) 中执行前插入。传递数据节点会在该节点之后插入。 @p n 不得已属于列表。
 */
rt_inline void rt_slist_insert(rt_slist_t *l, rt_slist_t *n)
{
    n->next = l->next;
    l->next = n;
}

/**
 * @brief 统计单链哨兵@p l之后的数据节点。
 * @return RT_NULL之前的节点数量，不包括哨兵。
 * @note O(n);需要调用者提供针对突变的同步。
 */
rt_inline unsigned int rt_slist_len(const rt_slist_t *l)
{
    unsigned int len = 0;
    const rt_slist_t *list = l->next;
    while (list != RT_NULL)
    {
        list = list->next;
        len ++;
    }

    return len;
}

/**
 * @brief 删除并返回哨兵@p l之后的第一个数据节点。
 * @return 分离第一个节点，当列表为空时为 RT_NULL。
 *
 * 返回节点的下一个指针被清除，使其分离状态明确，并防止调用者意外地将其视为链。
 */
rt_inline rt_slist_t *rt_slist_pop(rt_slist_t *l)
{
    struct rt_slist_node *node = l;

    /* 删除节点 */
    node = node->next;
    if (node != (rt_slist_t *)0)
    {
        ((struct rt_slist_node *)l)->next = node->next;
        node->next = RT_NULL;
    }

    return node;
}

/**
 * @brief 从以 @p l 为首的列表中删除第一次出现的节点 @p n。
 * @return @p l 在所有情况下，使调用者能够保留头部表达式。
 *
 * 因为单链节点没有前驱指针，所以函数从哨兵开始扫描，为O(n)。如果找到 @p n，则取消链接并清除其下一个指针。如果未找到，则 list 和 @p n 均保持不变。
 */
rt_inline rt_slist_t *rt_slist_remove(rt_slist_t *l, rt_slist_t *n)
{
    /* 删除链表头 */
    struct rt_slist_node *node = l;
    while (node->next && node->next != n) node = node->next;

    /* 删除节点 */
    if (node->next != (rt_slist_t *)0)
    {
        node->next = node->next->next;
        n->next = RT_NULL;
    }

    return l;
}

/**
 * @brief 返回第一个数据节点而不删除它。
 * @return l->next，对于空列表是 RT_NULL。
 */
rt_inline rt_slist_t *rt_slist_first(rt_slist_t *l)
{
    return l->next;
}

/**
 * @brief 查找从 @p l 可到达的最终节点。
 * @return 最后一个数据节点，或者当列表为空时 @p l 本身。
 * @note O(n)。当 @p l 为哨兵时，调用者必须先区分空结果，然后再将其转换为所有者对象。
 */
rt_inline rt_slist_t *rt_slist_tail(rt_slist_t *l)
{
    while (l->next) l = l->next;

    return l;
}

/**
 * @brief 返回单链接节点 @p n 的后继节点。
 * @return 列表末尾的下一个节点或 RT_NULL。
 */
rt_inline rt_slist_t *rt_slist_next(rt_slist_t *n)
{
    return n->next;
}

/**
 * @brief 测试是否没有数据节点跟随哨兵@p l。
 * @return 如果 l->next 是 RT_NULL，则非零，否则为零。
 */
rt_inline int rt_slist_isempty(rt_slist_t *l)
{
    return l->next == RT_NULL;
}

/**
 * @brief 将嵌入的单链接节点转换为其所有者对象。
 * @param node 指向所有者的@p member 节点的指针。
 * @param type 所有者结构类型。
 * @param member 嵌入 rt_slist_t 成员名称。
 * @warning @p node 必须是非 NULL 并且与此确切的成员/类型相对应。
 */
#define rt_slist_entry(node, type, member) \
    rt_container_of(node, type, member)

/**
 * @brief 迭代 NULL 终止的单链表中的原始数据节点。
 * @param pos rt_slist_t 指针使用并修改为循环光标。
 * @param head 哨兵头；迭代从 head->next 开始。
 *
 * 这种形式对于删除 @p pos 是不安全的，因为增量表达式需要在正文之后添加 pos->next。当需要突变时，在取消链接之前显式捕获后继者。
 */
#define rt_slist_for_each(pos, head) \
    for (pos = (head)->next; pos != RT_NULL; pos = pos->next)

/**
 * @brief 迭代 NULL 终止的单链表中的所有者对象。
 * @param pos 所有者类型指针用作循环光标并修改。
 * @param head Sentinel 单链表头。
 * @param member 此列表使用的嵌入式 rt_slist_t 成员。
 *
 * 条件转换避免将 rt_container_of() 应用于 RT_NULL。此表格不能安全移除；在取消链接 @p pos 之前保存下一个节点。
 */
#define rt_slist_for_each_entry(pos, head, member) \
    for (pos = ((head)->next == (RT_NULL) ? (RT_NULL) : rt_slist_entry((head)->next, rt_typeof(*pos), member)); \
         pos != (RT_NULL) && &pos->member != (RT_NULL); \
         pos = (pos->member.next == (RT_NULL) ? (RT_NULL) : rt_slist_entry(pos->member.next, rt_typeof(*pos), member)))

/**
 * @brief 返回哨兵@p ptr之后第一个数据节点的所有者。
 * @param ptr Sentinel 单链表头。
 * @param type 所有者结构类型。
 * @param member 嵌入 rt_slist_t 成员名称。
 *
 * @warning 列表必须非空； rt_container_of(RT_NULL, ...) 不会生成有效的所有者指针。
 */
#define rt_slist_first_entry(ptr, type, member) \
    rt_slist_entry((ptr)->next, type, member)

/**
 * @brief 返回哨兵@p ptr之后最终数据节点的所有者。
 * @param ptr Sentinel 单链表头。
 * @param type 所有者结构类型。
 * @param member 嵌入 rt_slist_t 成员名称。
 *
 * @warning 该列表必须非空。 rt_slist_tail() 对于空列表返回 @p ptr，并且该哨兵不是 @p type 的嵌入成员。
 */
#define rt_slist_tail_entry(ptr, type, member) \
    rt_slist_entry(rt_slist_tail(ptr), type, member)

/**@}*/

#ifdef __cplusplus
}
#endif

#endif
