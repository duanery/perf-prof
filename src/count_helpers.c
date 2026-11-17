#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/const.h>
#include <linux/refcount.h>
#include <linux/rblist.h>
#include <monitor.h>
#include <count_helpers.h>

struct count_dist {
    struct rblist dist;
    bool ins;
    bool id;
    bool key;
    int hist_size;
    u64 max;
};

static int count_dist_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct count_node *n = rb_entry(rbn, struct count_node, rbn);
    const struct count_node *e = entry;
    const struct count_dist *dist = e->dist;

    if (dist->ins) {
        if (n->ins > e->ins)
            return 1;
        else if (n->ins < e->ins)
            return -1;
    }
    if (dist->id) {
        if (n->id > e->id)
            return 1;
        else if (n->id < e->id)
            return -1;
    }
    if (dist->key) {
        if (n->key > e->key)
            return 1;
        else if (n->key < e->key)
            return -1;
    }
    return 0;
}

static struct rb_node *count_dist_node_new(struct rblist *rlist, const void *new_entry)
{
    struct count_dist *dist = container_of(rlist, struct count_dist, dist);
    const struct count_node *e = new_entry;
    struct count_node *n = malloc(sizeof(*n) + sizeof(u64) * dist->hist_size);
    if (n) {
        memset(n, 0, sizeof(*n) + sizeof(u64) * dist->hist_size);
        RB_CLEAR_NODE(&n->rbn);
        n->ins = e->ins;
        n->id = e->id;
        n->key = e->key;
        return &n->rbn;
    } else
        return NULL;
}

static void count_dist_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct count_node *n = rb_entry(rb_node, struct count_node, rbn);
    free(n);
}

struct count_dist *count_dist_new(bool ins, bool id, bool key, int hist_size)
{
    struct count_dist *dist;

    dist = malloc(sizeof(*dist));
    if (!dist)
        return NULL;

    dist->ins = ins;
    dist->id  = id;
    dist->key = key;
    dist->hist_size = hist_size;
    dist->max = 0;

    rblist__init(&dist->dist);
    dist->dist.node_cmp = count_dist_node_cmp;
    dist->dist.node_new = count_dist_node_new;
    dist->dist.node_delete = count_dist_node_delete;

    return dist;
}

void count_dist_free(struct count_dist *dist)
{
    if (dist) {
        rblist__exit(&dist->dist);
        free(dist);
    }
}

void count_dist_input(struct count_dist *dist, u64 ins, u64 id, u64 key, u64 count)
{
    struct count_node e = {
        .dist = dist,
        .ins = dist->ins ? ins : 0UL,
        .id  = dist->id  ? id  : 0UL,
        .key = dist->key ? key : 0UL,
    };
    struct rb_node *rbn = NULL;
    struct count_node *n = NULL;

    if (!dist)
        return ;

    rbn = rblist__findnew(&dist->dist, &e);
    if (rbn) {
        n = rb_entry(rbn, struct count_node, rbn);
        n->hist[n->hist_len] += count;
        if (n->max < n->hist[n->hist_len])
            n->max = n->hist[n->hist_len];
        if (dist->max < n->hist[n->hist_len])
            dist->max = n->hist[n->hist_len];
        n->sum += count;
        n->hist_len = (n->hist_len + 1) % dist->hist_size;
    }
}

void count_dist_insert(struct count_dist *dist, u64 ins, u64 id, u64 key, u64 i, u64 count)
{
    struct count_node e = {
        .dist = dist,
        .ins = dist->ins ? ins : 0UL,
        .id  = dist->id  ? id  : 0UL,
        .key = dist->key ? key : 0UL,
    };
    struct rb_node *rbn = NULL;
    struct count_node *n = NULL;

    if (!dist)
        return ;

    rbn = rblist__findnew(&dist->dist, &e);
    if (rbn) {
        n = rb_entry(rbn, struct count_node, rbn);
        i %= dist->hist_size;
        n->hist[i] += count;
        if (n->max < n->hist[i])
            n->max = n->hist[i];
        if (dist->max < n->hist[i])
            dist->max = n->hist[i];
        n->sum += count;
        if (i > n->hist_len)
            n->hist_len = i;
    }
}

void count_dist_print(struct count_dist *dist, print_count_node printnode, void *opaque)
{
    struct rb_node *node = NULL;
    struct count_node *n = NULL;

    if (!dist)
        return;
    if (rblist__empty(&dist->dist))
        return;

    for (node = rb_first_cached(&dist->dist.entries); node;
        node = rb_next(node)) {
        n = rb_entry(node, struct count_node, rbn);
        printnode(opaque, n);
    }
}

u64 count_dist_max(struct count_dist *dist)
{
    return dist ? dist->max : 0;
}

void count_dist_reset(struct count_dist *dist)
{
    if (!dist)
        return;
    rblist__exit(&dist->dist);
    dist->max = 0;
}

void count_dist_prune(struct count_dist *dist, bool (*keep)(void *opaque, struct count_node *node), void *opaque)
{
    struct rb_node *node, *next;

    if (!dist)
        return;
    for (node = rb_first_cached(&dist->dist.entries); node; node = next) {
        next = rb_next(node);
        if (!keep(opaque, rb_entry(node, struct count_node, rbn)))
            rblist__remove_node(&dist->dist, node);
    }
}

bool count_dist_empty(struct count_dist *dist)
{
    if (!dist)
        return true;
    return rblist__empty(&dist->dist);
}


