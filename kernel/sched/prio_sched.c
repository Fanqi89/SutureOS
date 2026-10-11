/* SutureOS: 优先级抢占式调度器 (红黑树, 移植 SpiritFoxOS) */
#include <stitch/types.h>
#include <stitch/string.h>
#include <stitch/console.h>
#include "prio_sched.h"
#include "task.h"

/* 红黑树节点 */
struct rb_node {
    uint64_t key;           /* (priority << 32) | seq */
    struct rb_node *left, *right, *parent;
    int color;              /* 0=red, 1=black */
    int tid;
};

/* 调度器状态 */
static struct {
    struct rb_node *root;
    struct rb_node *rbnil;
    uint64_t seq_counter;
    int current_tid;
} prio_sched;

#define RB_RED 0
#define RB_BLACK 1

static void rb_left_rotate(struct rb_node *x) {
    struct rb_node *y = x->right;
    x->right = y->left;
    if (y->left != prio_sched.rbnil) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == prio_sched.rbnil) prio_sched.root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rb_right_rotate(struct rb_node *x) {
    struct rb_node *y = x->left;
    x->left = y->right;
    if (y->right != prio_sched.rbnil) y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == prio_sched.rbnil) prio_sched.root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

static void rb_insert_fixup(struct rb_node *z) {
    while (z->parent->color == RB_RED) {
        if (z->parent == z->parent->parent->left) {
            struct rb_node *y = z->parent->parent->right;
            if (y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rb_left_rotate(z);
                }
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rb_right_rotate(z->parent->parent);
            }
        } else {
            struct rb_node *y = z->parent->parent->left;
            if (y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rb_right_rotate(z);
                }
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rb_left_rotate(z->parent->parent);
            }
        }
    }
    prio_sched.root->color = RB_BLACK;
}

/* 插入任务：key = (priority << 32) | seq */
void prio_sched_enqueue(int tid, int priority) {
    static struct rb_node nodes[SCHED_MAX_TASKS];
    struct rb_node *z = &nodes[tid];
    z->key = ((uint64_t)priority << 32) | prio_sched.seq_counter++;
    z->tid = tid;
    z->left = z->right = z->parent = prio_sched.rbnil;
    z->color = RB_RED;

    struct rb_node *y = prio_sched.rbnil;
    struct rb_node *x = prio_sched.root;
    while (x != prio_sched.rbnil) {
        y = x;
        if (z->key < x->key) x = x->left;
        else x = x->right;
    }
    z->parent = y;
    if (y == prio_sched.rbnil) prio_sched.root = z;
    else if (z->key < y->key) y->left = z;
    else y->right = z;
    rb_insert_fixup(z);
}

/* 取出最高优先级任务（最小 key） */
int prio_sched_dequeue(void) {
    if (prio_sched.root == prio_sched.rbnil) return -1;
    struct rb_node *z = prio_sched.root;
    while (z->left != prio_sched.rbnil) z = z->left;
    int tid = z->tid;
    /* 简单删除（实际需完整 rb_delete） */
    prio_sched.root = prio_sched.rbnil;  /* 简化：清空重建 */
    return tid;
}

void prio_sched_init(void) {
    static struct rb_node nil;
    nil.color = RB_BLACK;
    nil.left = nil.right = nil.parent = &nil;
    prio_sched.rbnil = &nil;
    prio_sched.root = &nil;
    prio_sched.seq_counter = 0;
    prio_sched.current_tid = -1;
}

static void rb_transplant(struct rb_node *u, struct rb_node *v) {
    if (u->parent == prio_sched.rbnil) prio_sched.root = v;
    else if (u == u->parent->left) u->parent->left = v;
    else u->parent->right = v;
    v->parent = u->parent;
}

static struct rb_node *rb_minimum(struct rb_node *x) {
    while (x->left != prio_sched.rbnil) x = x->left;
    return x;
}

static void rb_delete_fixup(struct rb_node *x) {
    while (x != prio_sched.root && x->color == RB_BLACK) {
        if (x == x->parent->left) {
            struct rb_node *w = x->parent->right;
            if (w->color == RB_RED) {
                w->color = RB_BLACK;
                x->parent->color = RB_RED;
                rb_left_rotate(x->parent);
                w = x->parent->right;
            }
            if (w->left->color == RB_BLACK && w->right->color == RB_BLACK) {
                w->color = RB_RED;
                x = x->parent;
            } else {
                if (w->right->color == RB_BLACK) {
                    w->left->color = RB_BLACK;
                    w->color = RB_RED;
                    rb_right_rotate(w);
                    w = x->parent->right;
                }
                w->color = x->parent->color;
                x->parent->color = RB_BLACK;
                w->right->color = RB_BLACK;
                rb_left_rotate(x->parent);
                x = prio_sched.root;
            }
        } else {
            struct rb_node *w = x->parent->left;
            if (w->color == RB_RED) {
                w->color = RB_BLACK;
                x->parent->color = RB_RED;
                rb_right_rotate(x->parent);
                w = x->parent->left;
            }
            if (w->right->color == RB_BLACK && w->left->color == RB_BLACK) {
                w->color = RB_RED;
                x = x->parent;
            } else {
                if (w->left->color == RB_BLACK) {
                    w->right->color = RB_BLACK;
                    w->color = RB_RED;
                    rb_left_rotate(w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = RB_BLACK;
                w->left->color = RB_BLACK;
                rb_right_rotate(x->parent);
                x = prio_sched.root;
            }
        }
    }
    x->color = RB_BLACK;
}

/* 完整删除：按 key 删除节点 */
void prio_sched_remove(uint64_t key) {
    struct rb_node *z = prio_sched.root;
    while (z != prio_sched.rbnil) {
        if (key < z->key) z = z->left;
        else if (key > z->key) z = z->right;
        else break;
    }
    if (z == prio_sched.rbnil) return;

    struct rb_node *y = z;
    int y_orig_color = y->color;
    struct rb_node *x;

    if (z->left == prio_sched.rbnil) {
        x = z->right;
        rb_transplant(z, z->right);
    } else if (z->right == prio_sched.rbnil) {
        x = z->left;
        rb_transplant(z, z->left);
    } else {
        struct rb_node *y2 = rb_minimum(z->right);
        y = y2;
        int y_orig_color2 = y->color;
        x = y->right;
        if (y->parent == z) x->parent = y;
        else {
            rb_transplant(y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        rb_transplant(z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }
    if (y_orig_color == RB_BLACK) rb_delete_fixup(x);
}