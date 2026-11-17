#ifndef __COUNT_HELPERS
#define __COUNT_HELPERS

struct count_dist;

struct count_node {
    union {
        struct rb_node rbn;
        struct count_dist *dist;
    };
    u64 ins;
    u64 id;
    u64 key;

    u64 max;
    u64 sum;

    int hist_len;
    u64 hist[0];
};

struct count_dist *count_dist_new(bool ins, bool id, bool key, int hist_size);
void count_dist_free(struct count_dist *dist);
void count_dist_input(struct count_dist *dist, u64 ins, u64 id, u64 key, u64 count);
void count_dist_insert(struct count_dist *dist, u64 ins, u64 id, u64 key, u64 i, u64 count);
typedef void (*print_count_node)(void *opaque, struct count_node *node);
void count_dist_print(struct count_dist *dist, print_count_node printnode, void *opaque);
u64 count_dist_max(struct count_dist *dist);
void count_dist_reset(struct count_dist *dist);
/* Delete the nodes @keep returns false for. */
void count_dist_prune(struct count_dist *dist, bool (*keep)(void *opaque, struct count_node *node), void *opaque);
bool count_dist_empty(struct count_dist *dist);


#endif

