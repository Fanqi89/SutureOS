/* SutureOS module: red-black tree (cp_ prefix)
 * Ported from: CoolPotOS (C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\src\lib\container\rbtree.c)
 * Original license: GPL-3.0
 * Changes: rb_* names kept (upstream convention); parent pointers removed
 *          (use stack-based traversal); iterative insert/delete; no malloc
 *          (caller provides nodes); key type uint64_t.
 */
#ifndef STITCH_RBTREE_H
#define STITCH_RBTREE_H

#include <stitch/types.h>

typedef enum { CP_RB_BLACK = 0, CP_RB_RED = 1 } cp_rb_color_t;

typedef struct cp_rb_node {
    struct cp_rb_node *left;
    struct cp_rb_node *right;
    uint64_t           key;
    cp_rb_color_t      color;
    void              *data;
} cp_rb_node_t;

typedef struct cp_rb_tree {
    cp_rb_node_t *root;
    uint64_t      count;
} cp_rb_tree_t;

void        cp_rb_init(cp_rb_tree_t *tree);
void        cp_rb_insert(cp_rb_tree_t *tree, uint64_t key, void *data);
void       *cp_rb_find(cp_rb_tree_t *tree, uint64_t key);
void        cp_rb_delete(cp_rb_tree_t *tree, uint64_t key);
void       *cp_rb_erase(cp_rb_tree_t *tree, uint64_t key);
uint64_t    cp_rb_count(cp_rb_tree_t *tree);
cp_rb_node_t *cp_rb_first(cp_rb_tree_t *tree);
cp_rb_node_t *cp_rb_last(cp_rb_tree_t *tree);
cp_rb_node_t *cp_rb_next(cp_rb_node_t *node);
cp_rb_node_t *cp_rb_prev(cp_rb_node_t *node);

void st_cp_rbtree_test(void);

#endif /* STITCH_RBTREE_H */
