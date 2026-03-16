#ifndef __STACK_HELPERS_H
#define __STACK_HELPERS_H

void global_syms_stat(FILE *fp);

void function_resolver_ref(void);
void function_resolver_unref(void);
char *function_resolver(void *priv, unsigned long long *addrp, char **modp);


struct callchain {
    __u64   nr;
    __u64   ips[0];
};

struct callchain_data {
    struct callchain *callchain;
    u32 pid;
    u64 regs_abi;
    u64 *regs;
    void *stack;
    u64 stack_sz;
};

typedef void (*callchain_cbs)(void *opaque, u64 perf_context);

struct callchain_ctx;
enum {
    CALLCHAIN_KERNEL = 1,
    CALLCHAIN_USER = 2,
    CALLCHAIN_DWARF = 4,
};
struct callchain_ctx *callchain_ctx_new(int flags, FILE *fout);
void callchain_ctx_config(struct callchain_ctx *cc, bool addr, bool symbol, bool offset,
        bool dso, bool reverse, char sep, char end);
void callchain_ctx_free(struct callchain_ctx *cc);
void print_callchain(struct callchain_ctx *cc, struct callchain *callchain, u32 pid);

void print_callchain_common_cbs(struct callchain_ctx *cc, struct callchain *callchain, u32 pid,
                                callchain_cbs kernel_cb, callchain_cbs user_cb, void *opaque);
static inline
void print_callchain_common(struct callchain_ctx *cc, struct callchain *callchain, u32 pid) {
    print_callchain_common_cbs(cc, callchain, pid, NULL, NULL, NULL);
}
void print_callchain_data_cbs(struct callchain_ctx *cc, struct callchain_data *data,
                              callchain_cbs kernel_cb, callchain_cbs user_cb, void *opaque);
static inline
void print_callchain_data(struct callchain_ctx *cc, struct callchain_data *data) {
    print_callchain_data_cbs(cc, data, NULL, NULL, NULL);
}

typedef struct callchain struct_key;
struct key_value_paires;
struct key_value_paires *keyvalue_pairs_new(int value_size);
void keyvalue_pairs_free(struct key_value_paires *pairs);
void *keyvalue_pairs_add_key(struct key_value_paires *pairs, struct_key *key);
typedef void (*foreach_keyvalue)(void *opaque, struct_key *key, void *value, unsigned int n);
void keyvalue_pairs_foreach(struct key_value_paires *pairs, foreach_keyvalue f, void *opaque);
typedef int (*keyvalue_cmp)(void **value1, void **value2);
void keyvalue_pairs_sorted_firstn(struct key_value_paires *pairs, keyvalue_cmp cmp, foreach_keyvalue f, void *opaque, unsigned int n);
void keyvalue_pairs_sorted_foreach(struct key_value_paires *pairs, keyvalue_cmp cmp, foreach_keyvalue f, void *opaque);
void keyvalue_pairs_reinit(struct key_value_paires *pairs);
unsigned int keyvalue_pairs_nr_entries(struct key_value_paires *pairs);

const char *unique_string_len(const char *str, int len);
static inline const char *unique_string(const char *str) {
    return unique_string_len(str, 0);
}
void unique_string_stat(FILE *fp);


#define PERF_CONTEXT_FLAME_GRAPH  (PERF_CONTEXT_KERNEL - 1)
struct flame_graph;
struct flame_graph *flame_graph_new(int flags, FILE *fout);
void flame_graph_free(struct flame_graph *fg);
void flame_graph_add_callchain_at_time(struct flame_graph *fg, struct callchain_data *data,
                                         u32 pid, const char *comm,
                                         u64 time, const char *time_str);
static inline
void flame_graph_add_callchain(struct flame_graph *fg, struct callchain_data *data,
                                         u32 pid, const char *comm)
{
    flame_graph_add_callchain_at_time(fg, data, pid, comm, 0, NULL);
}
void flame_graph_output(struct flame_graph *fg);
struct flame_graph *flame_graph_open(int flags, const char *path);
void flame_graph_close(struct flame_graph *fg);
void flame_graph_reset(struct flame_graph *fg);


#define PERF_CONTEXT_PYSTACK  (PERF_CONTEXT_KERNEL - 2)

struct heatmap;
struct heatmap *heatmap_open(const char *time_uints, const char *latency_units, const char *path);
void heatmap_close(struct heatmap *heatmap);
void heatmap_write(struct heatmap *heatmap, unsigned long time, unsigned long latency);

#ifdef HAVE_LIBPYTHON
/*
 * Convert callchain to Python list of frame dicts.
 * Each dict: {'addr': int, 'symbol': str, 'offset': int, 'kernel': bool, 'dso': str}
 * Frames ordered from stack top to bottom.
 *
 * @callchain: The callchain to convert
 * @pid: Process ID for user space symbol resolution
 * @flags: CALLCHAIN_KERNEL and/or CALLCHAIN_USER flags
 * Returns: New reference to Python list, or NULL on error
 */
void *callchain_to_pylist(struct callchain *callchain, u32 pid, int flags);

/* Initialize/cleanup symbol resolution for callchain_to_pylist */
int callchain_pylist_init(int flags);
void callchain_pylist_exit(int flags);
#endif /* HAVE_LIBPYTHON */

#endif