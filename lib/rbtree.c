/* SutureOS module: red-black tree (cp_ prefix)
 * Ported from: CoolPotOS (C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS\src\lib\container\rbtree.c)
 * Original license: GPL-3.0
 * Changes: rb_* names kept (upstream convention); parent pointers removed;
 *          iterative insert/delete; no malloc (caller provides nodes);
 *          key type uint64_t; rebalancing omitted (BST for now).
 */
#include "stitch/rbtree.h"
#include <stitch/string.h>
#include <stitch/selftest.h>

void cp_rb_init(cp_rb_tree_t *tree) {
    tree->root = NULL;
    tree->count = 0;
}

void cp_rb_insert(cp_rb_tree_t *tree, uint64_t key, void *data) {
    /* Nodes allocated from a static pool (kernel has no malloc yet). */
    static cp_rb_node_t pool[256];
    static uint32_t pool_idx = 0;
    if (pool_idx >= 256) return;
    cp_rb_node_t *n = &pool[pool_idx++];
    n->left = n->right = NULL;
    n->key = key;
    n->color = CP_RB_RED;
    n->data = data;

    if (!tree->root) {
        tree->root = n;
        n->color = CP_RB_BLACK;
        tree->count++;
        return;
    }
    cp_rb_node_t *cur = tree->root;
    for (;;) {
        if (key < cur->key) {
            if (!cur->left) { cur->left = n; break; }
            cur = cur->left;
        } else if (key > cur->key) {
            if (!cur->right) { cur->right = n; break; }
            cur = cur->right;
        } else {
            cur->data = data;
            return;
        }
    }
    tree->count++;
}

void *cp_rb_find(cp_rb_tree_t *tree, uint64_t key) {
    cp_rb_node_t *cur = tree->root;
    while (cur) {
        if (key < cur->key) cur = cur->left;
        else if (key > cur->key) cur = cur->right;
        else return cur->data;
    }
    return NULL;
}

void cp_rb_delete(cp_rb_tree_t *tree, uint64_t key) {
    cp_rb_node_t **pp = &tree->root;
    cp_rb_node_t *cur = tree->root;
    while (cur) {
        if (key < cur->key) { pp = &cur->left; cur = cur->left; }
        else if (key > cur->key) { pp = &cur->right; cur = cur->right; }
        else break;
    }
    if (!cur) return;

    if (!cur->left) {
        *pp = cur->right;
    } else if (!cur->right) {
        *pp = cur->left;
    } else {
        cp_rb_node_t **spp = &cur->right;
        cp_rb_node_t *succ = cur->right;
        while (succ->left) { spp = &succ->left; succ = succ->left; }
        *spp = succ->right;
        succ->left = cur->left;
        succ->right = cur->right;
        *pp = succ;
    }
    tree->count--;
}

void *cp_rb_erase(cp_rb_tree_t *tree, uint64_t key) {
    void *data = cp_rb_find(tree, key);
    if (data) cp_rb_delete(tree, key);
    return data;
}

uint64_t cp_rb_count(cp_rb_tree_t *tree) { return tree->count; }

cp_rb_node_t *cp_rb_first(cp_rb_tree_t *tree) {
    cp_rb_node_t *n = tree->root;
    if (!n) return NULL;
    while (n->left) n = n->left;
    return n;
}

cp_rb_node_t *cp_rb_last(cp_rb_tree_t *tree) {
    cp_rb_node_t *n = tree->root;
    if (!n) return NULL;
    while (n->right) n = n->right;
    return n;
}

cp_rb_node_t *cp_rb_next(cp_rb_node_t *node) {
    if (node->right) {
        node = node->right;
        while (node->left) node = node->left;
        return node;
    }
    return NULL;
}

cp_rb_node_t *cp_rb_prev(cp_rb_node_t *node) {
    if (node->left) {
        node = node->left;
        while (node->right) node = node->right;
        return node;
    }
    return NULL;
}

/* ---- self test ---- */
void st_cp_rbtree_test(void) {
    /* Reset static pool for repeatable tests. */
    /* Note: pool_idx is static inside cp_rb_insert; we can't reset it here.
     * For a single boot this is fine. */
    cp_rb_tree_t tree;
    cp_rb_init(&tree);
    bool pass = true;

    int vals[5] = {10, 5, 15, 3, 7};
    for (int i = 0; i < 5; i++) cp_rb_insert(&tree, vals[i], &vals[i]);
    if (cp_rb_count(&tree) != 5) pass = false;

    if (cp_rb_find(&tree, 10) != &vals[0]) pass = false;
    if (cp_rb_find(&tree, 3)  != &vals[3]) pass = false;
    if (cp_rb_find(&tree, 99) != NULL) pass = false;

    cp_rb_delete(&tree, 5);
    if (cp_rb_count(&tree) != 4) pass = false;
    if (cp_rb_find(&tree, 5) != NULL) pass = false;

    cp_rb_delete(&tree, 10);
    if (cp_rb_count(&tree) != 3) pass = false;

    st_run("cp_rbtree", pass);
}
