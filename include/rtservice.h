/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-16     Bernard      the first version
 * 2006-09-07     Bernard      move the kservice APIs to rtthread.h
 * 2007-06-27     Bernard      fix the rt_list_remove bug
 * 2012-03-22     Bernard      rename kservice.h to rtservice.h
 * 2017-11-15     JasonJia     Modify rt_slist_foreach to rt_slist_for_each_entry.
 *                             Make code cleanup.
 * 2024-01-03     Shell        add rt_slist_pop()
 */

#ifndef __RT_SERVICE_H__
#define __RT_SERVICE_H__

/**
 * @file rtservice.h
 * @brief Header-only intrusive list services used throughout RT-Thread.
 *
 * The list node is embedded inside its owner object instead of allocated as a
 * separate wrapper. This avoids allocation and lets one object participate in
 * multiple independent lists by embedding multiple named nodes. The conversion
 * macros recover the owner address from a node address and member offset.
 *
 * Two representations are provided:
 *
 * - rt_list_t is a circular, doubly linked list with a sentinel head. It offers
 *   constant-time insertion and removal when the target node is known.
 * - rt_slist_t is a NULL-terminated, singly linked list with a sentinel head.
 *   It uses less storage but tail insertion and predecessor-based removal are
 *   linear-time operations.
 *
 * None of these helpers performs locking, validates ownership, or detects a
 * node inserted in multiple lists. Callers must serialize concurrent access
 * and maintain the invariant that a node belongs to at most one list through a
 * given embedded member.
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
 * @brief Recover an enclosing object from a pointer to one of its members.
 *
 * The expression computes the byte offset of @p member within @p type using
 * `&((type *)0)->member`, subtracts that offset from @p ptr, and converts the
 * result to `type *`. This is the basis of all intrusive-list entry macros.
 *
 * @param ptr Pointer to the embedded member in a live object.
 * @param type Complete enclosing structure type.
 * @param member Name of the member within @p type, not a string.
 * @return Pointer to the enclosing @p type object.
 *
 * @warning The macro cannot verify that @p ptr actually points to the named
 * member. Passing RT_NULL (except when the offset is zero), the wrong member,
 * or a pointer from another type produces an invalid result. The macro performs
 * pointer arithmetic only; it does not manage the object's lifetime.
 */
#define rt_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))


/**
 * @brief Static initializer for a circular doubly linked list head/node.
 *
 * Both links point back to @p object, which represents an empty sentinel list
 * and also the detached state used by rt_list_remove(). The argument must be
 * the lvalue name of the object being initialized.
 */
#define RT_LIST_OBJECT_INIT(object) { &(object), &(object) }

/**
 * @brief Initialize a circular doubly linked list as empty.
 *
 * @param l Head or detached node whose next and prev links are reset to itself.
 * @note Reinitializing a node that is still linked leaves its old neighbors
 *       pointing at the now self-linked node and corrupts the old list; remove
 *       it from that list first.
 */
rt_inline void rt_list_init(rt_list_t *l)
{
    l->next = l->prev = l;
}

/**
 * @brief Insert @p n immediately after @p l.
 *
 * @param l Existing list node; passing the sentinel inserts at the front.
 * @param n Detached node to insert.
 *
 * Four links are updated so both forward and reverse traversal remain valid.
 * The operation is O(1) and does not check whether @p n is already linked.
 */
rt_inline void rt_list_insert_after(rt_list_t *l, rt_list_t *n)
{
    l->next->prev = n;
    n->next = l->next;

    l->next = n;
    n->prev = l;
}

/**
 * @brief Insert @p n immediately before @p l.
 *
 * @param l Existing list node; passing the sentinel appends at the tail.
 * @param n Detached node to insert.
 *
 * The operation is O(1) and does not check whether @p n is already linked.
 */
rt_inline void rt_list_insert_before(rt_list_t *l, rt_list_t *n)
{
    l->prev->next = n;
    n->prev = l->prev;

    l->prev = n;
    n->next = l;
}

/**
 * @brief Unlink @p n from its circular doubly linked list.
 * @param n Linked node to remove.
 *
 * After repairing both neighbors, the node is self-linked. This detached state
 * permits repeated removal without damaging another list, but such use may
 * still indicate a lifecycle error. Removing the sentinel head is invalid from
 * the owning container's perspective even though the pointer updates compile.
 */
rt_inline void rt_list_remove(rt_list_t *n)
{
    n->next->prev = n->prev;
    n->prev->next = n->next;

    n->next = n->prev = n;
}

/**
 * @brief Test whether a circular list contains no nodes after @p l.
 * @return Nonzero when @p l points to itself through next; zero otherwise.
 * @note Normally @p l is the sentinel head. A detached ordinary node also
 *       appears empty because removal makes it self-linked.
 */
rt_inline int rt_list_isempty(const rt_list_t *l)
{
    return l->next == l;
}

/**
 * @brief Count nodes following sentinel head @p l until traversal returns to it.
 * @return Number of data nodes, excluding @p l.
 * @note This is O(n). Concurrent mutation or corrupted links can make the
 *       result inconsistent or prevent termination.
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
 * @brief Convert an embedded doubly linked node to its owner object.
 * @param node Pointer to the owner's @p member node.
 * @param type Owner structure type.
 * @param member Embedded rt_list_t member name.
 * @return Pointer to the enclosing owner object.
 * @warning @p node must correspond to @p member in an actual @p type object.
 */
#define rt_list_entry(node, type, member) \
    rt_container_of(node, type, member)

/**
 * @brief Iterate over raw nodes in a circular doubly linked list.
 * @param pos rt_list_t pointer used as the loop cursor and modified each pass.
 * @param head Sentinel head; evaluated repeatedly by the generated loop.
 *
 * Iteration begins at head->next and ends before visiting @p head. Do not remove
 * @p pos in the loop body because the increment reads pos->next after the body;
 * use rt_list_for_each_safe() when the current node may be removed.
 */
#define rt_list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * @brief Iterate raw nodes while allowing removal of the current node.
 * @param pos Current rt_list_t cursor.
 * @param n Temporary rt_list_t cursor holding the next node before the body.
 * @param head Sentinel list head.
 *
 * The caller must supply distinct lvalues for @p pos and @p n. Removing or
 * relocating @p n inside the loop can still invalidate the saved traversal.
 */
#define rt_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
        pos = n, n = pos->next)

/**
 * @brief Iterate over owner objects in a circular doubly linked list.
 * @param pos Pointer of the owner type, used and modified as loop cursor.
 * @param head Sentinel list head.
 * @param member rt_list_t member through which the owner is linked.
 *
 * rt_typeof(*pos) infers the owner type, and rt_list_entry() performs the
 * intrusive-node conversion. The sentinel-derived cursor used for loop
 * termination must never be dereferenced as a real object. This form is not
 * safe when the current entry is removed.
 */
#define rt_list_for_each_entry(pos, head, member) \
    for (pos = rt_list_entry((head)->next, rt_typeof(*pos), member); \
         &pos->member != (head); \
         pos = rt_list_entry(pos->member.next, rt_typeof(*pos), member))

/**
 * @brief Iterate owner objects while allowing removal of the current object.
 * @param pos Current owner-object cursor.
 * @param n Temporary owner-object cursor holding the next entry.
 * @param head Sentinel list head.
 * @param member Embedded rt_list_t member used by this list.
 *
 * @p pos and @p n must be distinct lvalues of compatible pointer type. The next
 * entry is captured before the loop body, so unlinking @p pos is safe; deleting
 * or relinking the saved @p n requires separate care.
 */
#define rt_list_for_each_entry_safe(pos, n, head, member) \
    for (pos = rt_list_entry((head)->next, rt_typeof(*pos), member), \
         n = rt_list_entry(pos->member.next, rt_typeof(*pos), member); \
         &pos->member != (head); \
         pos = n, n = rt_list_entry(n->member.next, rt_typeof(*n), member))

/**
 * @brief Return the owner of the first node after sentinel @p ptr.
 * @param ptr Sentinel list head.
 * @param type Owner structure type.
 * @param member Embedded rt_list_t member name.
 *
 * @warning The list must be nonempty. On an empty list, ptr->next is the
 * sentinel rather than an embedded member of @p type, so the result is invalid.
 */
#define rt_list_first_entry(ptr, type, member) \
    rt_list_entry((ptr)->next, type, member)

/** Static initializer for an empty NULL-terminated singly linked list head. */
#define RT_SLIST_OBJECT_INIT(object) { RT_NULL }

/**
 * @brief Initialize a NULL-terminated singly linked list as empty.
 *
 * @param l Sentinel head or detached node whose next link becomes RT_NULL.
 * @note Reinitializing a linked node does not repair its former predecessor.
 */
rt_inline void rt_slist_init(rt_slist_t *l)
{
    l->next = RT_NULL;
}

/**
 * @brief Append detached node @p n to the tail of list headed by @p l.
 *
 * Traversal starts at @p l and follows next until the current tail is found, so
 * this operation is O(n). The function forces n->next to RT_NULL and therefore
 * appends one node, not a preexisting chain. It does not detect duplicate links.
 */
rt_inline void rt_slist_append(rt_slist_t *l, rt_slist_t *n)
{
    struct rt_slist_node *node;

    node = l;
    while (node->next) node = node->next;

    /* append the node to the tail */
    node->next = n;
    n->next = RT_NULL;
}

/**
 * @brief Insert @p n immediately after node @p l.
 *
 * Passing the sentinel head performs front insertion in O(1). Passing a data
 * node inserts after that node. @p n must not already belong to a list.
 */
rt_inline void rt_slist_insert(rt_slist_t *l, rt_slist_t *n)
{
    n->next = l->next;
    l->next = n;
}

/**
 * @brief Count data nodes after singly linked sentinel @p l.
 * @return Number of nodes before RT_NULL, excluding the sentinel.
 * @note O(n); requires caller-provided synchronization against mutation.
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
 * @brief Remove and return the first data node after sentinel @p l.
 * @return Detached first node, or RT_NULL when the list is empty.
 *
 * The returned node's next pointer is cleared, making its detached state
 * explicit and preventing callers from accidentally treating it as a chain.
 */
rt_inline rt_slist_t *rt_slist_pop(rt_slist_t *l)
{
    struct rt_slist_node *node = l;

    /* remove node */
    node = node->next;
    if (node != (rt_slist_t *)0)
    {
        ((struct rt_slist_node *)l)->next = node->next;
        node->next = RT_NULL;
    }

    return node;
}

/**
 * @brief Remove the first occurrence of node @p n from list headed by @p l.
 * @return @p l in all cases, enabling callers to retain the head expression.
 *
 * Because a singly linked node has no predecessor pointer, the function scans
 * from the sentinel and is O(n). If @p n is found, it is unlinked and its next
 * pointer is cleared. If not found, both list and @p n are left unchanged.
 */
rt_inline rt_slist_t *rt_slist_remove(rt_slist_t *l, rt_slist_t *n)
{
    /* remove slist head */
    struct rt_slist_node *node = l;
    while (node->next && node->next != n) node = node->next;

    /* remove node */
    if (node->next != (rt_slist_t *)0)
    {
        node->next = node->next->next;
        n->next = RT_NULL;
    }

    return l;
}

/**
 * @brief Return the first data node without removing it.
 * @return l->next, which is RT_NULL for an empty list.
 */
rt_inline rt_slist_t *rt_slist_first(rt_slist_t *l)
{
    return l->next;
}

/**
 * @brief Find the final node reachable from @p l.
 * @return Last data node, or @p l itself when the list after it is empty.
 * @note O(n). When @p l is the sentinel, callers must distinguish the empty
 *       result before converting it to an owner object.
 */
rt_inline rt_slist_t *rt_slist_tail(rt_slist_t *l)
{
    while (l->next) l = l->next;

    return l;
}

/**
 * @brief Return the successor of singly linked node @p n.
 * @return Next node or RT_NULL at the end of the list.
 */
rt_inline rt_slist_t *rt_slist_next(rt_slist_t *n)
{
    return n->next;
}

/**
 * @brief Test whether no data node follows sentinel @p l.
 * @return Nonzero if l->next is RT_NULL, otherwise zero.
 */
rt_inline int rt_slist_isempty(rt_slist_t *l)
{
    return l->next == RT_NULL;
}

/**
 * @brief Convert an embedded singly linked node to its owner object.
 * @param node Pointer to the owner's @p member node.
 * @param type Owner structure type.
 * @param member Embedded rt_slist_t member name.
 * @warning @p node must be non-NULL and correspond to this exact member/type.
 */
#define rt_slist_entry(node, type, member) \
    rt_container_of(node, type, member)

/**
 * @brief Iterate raw data nodes in a NULL-terminated singly linked list.
 * @param pos rt_slist_t pointer used and modified as the loop cursor.
 * @param head Sentinel head; iteration starts at head->next.
 *
 * This form is not safe for removal of @p pos because the increment expression
 * needs pos->next after the body. Capture the successor explicitly before
 * unlinking when mutation is required.
 */
#define rt_slist_for_each(pos, head) \
    for (pos = (head)->next; pos != RT_NULL; pos = pos->next)

/**
 * @brief Iterate owner objects in a NULL-terminated singly linked list.
 * @param pos Pointer of owner type used and modified as loop cursor.
 * @param head Sentinel singly linked list head.
 * @param member Embedded rt_slist_t member used by this list.
 *
 * The conditional conversions avoid applying rt_container_of() to RT_NULL.
 * This form is not removal-safe; save the next node before unlinking @p pos.
 */
#define rt_slist_for_each_entry(pos, head, member) \
    for (pos = ((head)->next == (RT_NULL) ? (RT_NULL) : rt_slist_entry((head)->next, rt_typeof(*pos), member)); \
         pos != (RT_NULL) && &pos->member != (RT_NULL); \
         pos = (pos->member.next == (RT_NULL) ? (RT_NULL) : rt_slist_entry(pos->member.next, rt_typeof(*pos), member)))

/**
 * @brief Return the owner of the first data node after sentinel @p ptr.
 * @param ptr Sentinel singly linked list head.
 * @param type Owner structure type.
 * @param member Embedded rt_slist_t member name.
 *
 * @warning The list must be nonempty; rt_container_of(RT_NULL, ...) does not
 * produce a valid owner pointer.
 */
#define rt_slist_first_entry(ptr, type, member) \
    rt_slist_entry((ptr)->next, type, member)

/**
 * @brief Return the owner of the final data node after sentinel @p ptr.
 * @param ptr Sentinel singly linked list head.
 * @param type Owner structure type.
 * @param member Embedded rt_slist_t member name.
 *
 * @warning The list must be nonempty. rt_slist_tail() returns @p ptr for an
 * empty list, and that sentinel is not an embedded member of @p type.
 */
#define rt_slist_tail_entry(ptr, type, member) \
    rt_slist_entry(rt_slist_tail(ptr), type, member)

/**@}*/

#ifdef __cplusplus
}
#endif

#endif
