/* SutureOS module: intrusive doubly-linked list (phobos_ prefix)
 * Ported from: Phobos (C:\Users\fanqi\Desktop\自研操作系统\Phobos\include\container\list.h)
 * Original license: GPL-3.0
 * Changes: LIST_* macros kept as-is (upstream convention); list_entry/list_first_entry
 *          helpers added; container_of via uintptr_t to avoid -Wcast-align.
 */
#ifndef STITCH_PHOBOS_LIST_H
#define STITCH_PHOBOS_LIST_H

#include <stitch/types.h>

struct phobos_list_head {
    struct phobos_list_head *next, *prev;
};

#define PHOBOS_LIST_HEAD_INIT(name) { &(name), &(name) }
#define PHOBOS_LIST_HEAD(name) struct phobos_list_head name = PHOBOS_LIST_HEAD_INIT(name)

static inline void phobos_list_init(struct phobos_list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void phobos_list_add(struct phobos_list_head *new_node,
                                    struct phobos_list_head *prev,
                                    struct phobos_list_head *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

static inline void phobos_list_add_tail(struct phobos_list_head *new_node,
                                         struct phobos_list_head *head) {
    phobos_list_add(new_node, head->prev, head);
}

static inline void phobos_list_del(struct phobos_list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry;
    entry->prev = entry;
}

static inline bool phobos_list_empty(const struct phobos_list_head *head) {
    return head->next == head;
}

#define phobos_list_entry(ptr, type, member) \
    ((type *)((uintptr_t)(ptr) - (uintptr_t)&((type *)0)->member))

#define phobos_list_first_entry(ptr, type, member) \
    phobos_list_entry((ptr)->next, type, member)

#define phobos_list_next_entry(pos, member) \
    phobos_list_entry((pos)->member.next, typeof(*(pos)), member)

#define phobos_list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define phobos_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

#endif /* STITCH_PHOBOS_LIST_H */
