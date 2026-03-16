#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/rbtree.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>

#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2

struct kmemleak_stat {
    __u64 alloc_num;
    __u64 alloc_mem;
    __u64 total_alloc;
    __u64 total_free;
};
struct kmemleak_ctx {
    struct callchain_ctx *cc;
    struct flame_graph *flame;
    struct tp_list *tp_alloc;
    struct tp_list *tp_free;
    struct rb_root alloc;
    struct kmemleak_stat stat;
    struct list_head lost_list;
    int default_flags;
    bool report_leaked_bytes;
    bool collect_comm;
};
struct perf_event_backup {
    struct rb_node rbnode;
    __u64    ptr;
    char     comm[16];
    int      callchain;
    union perf_event event;
};

struct kmemleak_lost_node {
    struct list_head lost_link;
    int ins;
    bool reclaim;
    u64 start_time;
    u64 end_time;
    u64 lost_id; // struct perf_record_lost::id
    u64 lost;
};


// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
struct sample_type_header {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    __u64   id;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
};

static __always_inline struct perf_event_backup *alloc_find(struct rb_root *root, __u64 ptr)
{
    struct rb_node *node = root->rb_node;
    while (node) {
        struct perf_event_backup *b = rb_entry(node, struct perf_event_backup, rbnode);
        if (ptr < b->ptr)
            node = node->rb_left;
        else if (ptr > b->ptr)
            node = node->rb_right;
        else
            return b;
    }
    return NULL;
}

static __always_inline int alloc_insert(struct rb_root *root, struct perf_event_backup *new)
{
    struct rb_node **p = &root->rb_node;
    struct rb_node *parent = NULL;

    while (*p) {
        struct perf_event_backup *b = rb_entry(*p, struct perf_event_backup, rbnode);
        parent = *p;
        if (new->ptr < b->ptr)
            p = &(*p)->rb_left;
        else if (new->ptr > b->ptr)
            p = &(*p)->rb_right;
        else
            return -EEXIST;
    }
    rb_link_node(&new->rbnode, parent, p);
    rb_insert_color(&new->rbnode, root);
    return 0;
}

static __always_inline void alloc_erase(struct kmemleak_ctx *ctx, struct perf_event_backup *b)
{
    size_t size = offsetof(struct perf_event_backup, event) + b->event.header.size;
    rb_erase(&b->rbnode, &ctx->alloc);
    ctx->stat.alloc_num --;
    ctx->stat.alloc_mem -= size;
    free(b);
}

static void alloc_free_all(struct kmemleak_ctx *ctx)
{
    struct rb_node *node, *next;
    struct perf_event_backup *b;
    for (node = rb_first(&ctx->alloc); node; node = next) {
        next = rb_next(node);
        b = rb_entry(node, struct perf_event_backup, rbnode);
        alloc_erase(ctx, b);
    }
}

static __always_inline struct perf_event_backup *alloc_new(struct kmemleak_ctx *ctx, __u64 ptr,
        int callchain, union perf_event *event, int pid)
{
    size_t size = offsetof(struct perf_event_backup, event) + event->header.size;
    struct perf_event_backup *b = malloc(size);
    if (b) {
        RB_CLEAR_NODE(&b->rbnode);
        b->ptr = ptr;
        b->callchain = callchain;
        if (ctx->collect_comm) {
            const char *comm = global_comm_get(pid);
            if (comm) {
                *(u64 *)(b->comm) = *(u64 *)(comm);
                *(u64 *)(b->comm+8) = *(u64 *)(comm+8);
            } else
                strcpy(b->comm, "<...>");
            b->comm[sizeof(b->comm) - 1] = '\0';
        } else
            b->comm[0] = '\0';
        memcpy(&b->event, event, event->header.size);
        ctx->stat.alloc_num ++;
        ctx->stat.alloc_mem += size;
    }
    return b;
}

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct kmemleak_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (!env->tp_alloc ||
        !env->tp_free) {
        free(ctx);
        return -1;
    }
    INIT_LIST_HEAD(&ctx->lost_list);

    tep__ref();
    global_comm_ref();

    ctx->alloc = RB_ROOT;
    memset(&ctx->stat, 0, sizeof(ctx->stat));
    ctx->report_leaked_bytes = false;

    return 0;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct kmemleak_ctx *ctx = dev->private;
    struct kmemleak_lost_node *lost, *next;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link)
        free(lost);

    alloc_free_all(ctx);
    callchain_ctx_free(ctx->cc);
    if (dev->env->flame_graph) {
        flame_graph_output(ctx->flame);
        flame_graph_close(ctx->flame);
    }
    tp_list_free(ctx->tp_alloc);
    tp_list_free(ctx->tp_free);
    global_comm_unref();
    tep__unref();
    free(ctx);
}

static int add_tp_list(struct prof_dev *dev, struct tp_list *tp_list, bool callchain)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW |
                         (callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL),
        .watermark     = 1,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    prof_dev_env2attr(dev, &attr);

    for_each_real_tp(tp_list, tp, i) {

        if (!tp->mem_ptr) {
            fprintf(stderr, "%s:%s//ptr=?/ ptr attribute is not set\n", tp->sys, tp->name);
            return -1;
        }

        evsel = tp_evsel_new(tp, &attr);
        if (!evsel) {
            return -1;
        }
        perf_evlist__add(evlist, evsel);

        tp->private = tp_list;
    }
    return 0;
}

static int kmemleak_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct kmemleak_ctx *ctx;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    ctx->tp_alloc = tp_list_new(dev, env->tp_alloc);
    if (!ctx->tp_alloc)
        goto failed;

    ctx->tp_free = tp_list_new(dev, env->tp_free);
    if (!ctx->tp_free)
        goto failed;

    if (!env->callchain)
        env->callchain = (ctx->tp_alloc->nr_need_stack == ctx->tp_alloc->nr_real_tp);

    if (env->callchain || ctx->tp_alloc->nr_need_stack || ctx->tp_free->nr_need_stack) {
        ctx->default_flags = callchain_flags(dev, CALLCHAIN_KERNEL);
        ctx->cc = callchain_ctx_new(ctx->default_flags, stdout);
        if (env->flame_graph)
            ctx->flame = flame_graph_open(ctx->default_flags, env->flame_graph);
        dev->pages *= 2;
    }

    if (add_tp_list(dev, ctx->tp_alloc, env->callchain) < 0)
        goto failed;
    if (add_tp_list(dev, ctx->tp_free, false) < 0)
        goto failed;

    if (ctx->tp_alloc->nr_mem_size == ctx->tp_alloc->nr_real_tp) {
        ctx->report_leaked_bytes = true;
        ctx->collect_comm = env->comm && env->callchain;
        if (!env->verbose && !env->callchain)
            fprintf(stderr, "Support LEAKED BYTES REPORT, need -g to enable callchain.\n");
        if (!env->verbose && env->callchain && env->flame_graph)
            fprintf(stderr, "Support LEAKED BYTES REPORT, will disable flame graph.\n");
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int kmemleak_filter(struct prof_dev *dev)
{
    struct kmemleak_ctx *ctx = dev->private;
    int err;

    if ((err = tp_list_apply_filter(dev, ctx->tp_alloc)) < 0)
        return err;

    if ((err = tp_list_apply_filter(dev, ctx->tp_free)) < 0)
        return err;

    return 0;
}

static void report_kmemleak(struct prof_dev *dev);
static void kmemleak_exit(struct prof_dev *dev)
{
    report_kmemleak(dev);
    monitor_ctx_exit(dev);
}

static inline void lost_reclaim(struct prof_dev *dev)
{
    struct kmemleak_ctx *ctx = dev->private;

    if (!list_empty(&ctx->lost_list)) {
        struct kmemleak_lost_node *lost = list_first_entry(&ctx->lost_list, struct kmemleak_lost_node, lost_link);
        struct perf_record_lost lost_event = {
            .header = {PERF_RECORD_LOST, 0, sizeof(struct perf_record_lost)},
            .id = lost->lost_id,
            .lost = lost->lost,
        };
        if (!RB_EMPTY_ROOT(&ctx->alloc))
            dev->lost_print_time = 0; // force print lost now
        print_lost_fn(dev, (union perf_event *)&lost_event, lost->ins);
    }

    if (!RB_EMPTY_ROOT(&ctx->alloc)) {
        print_time(stdout);
        printf("Report memory leaks in advance due to lost\n");

        report_kmemleak(dev);
    }
}

static void kmemleak_lost(struct prof_dev *dev, union perf_event *event, int ins, u64 lost_start, u64 lost_end)
{
    struct kmemleak_ctx *ctx = dev->private;
    struct kmemleak_lost_node *pos;
    struct kmemleak_lost_node *lost;

    // When order is enabled, event loss will be sensed in advance, but it
    // needs to be processed later.
    lost = malloc(sizeof(*lost));
    if (lost) {
        lost->ins = ins;
        lost->reclaim = false;
        lost->start_time = lost_start;
        lost->end_time = lost_end;
        lost->lost_id = event->lost.id;
        lost->lost = event->lost.lost;

        list_for_each_entry(pos, &ctx->lost_list, lost_link) {
            if (pos->start_time > lost_start)
                break;
        }
        list_add_tail(&lost->lost_link, &pos->lost_link);
    }
}


struct sample_type_callchain {
    struct sample_type_header h;
    struct callchain callchain;
};
struct sample_type_raw {
    struct sample_type_header h;
    struct {
        __u32   size;
        __u8    data[0];
    } raw;
};

static void __raw_size(union perf_event *event, bool callchain, void **praw, int *psize)
{
    if (callchain) {
        struct sample_type_callchain *data = (void *)event->sample.array;
        struct {
            __u32   size;
            __u8    data[0];
        } *raw = (void *)data->callchain.ips + data->callchain.nr * sizeof(__u64);
        *praw = raw->data;
        *psize = raw->size;
    } else {
        struct sample_type_raw *raw = (void *)event->sample.array;
        *praw = raw->raw.data;
        *psize = raw->raw.size;
    }
}

static void __print_callchain(struct prof_dev *dev, union perf_event *event, bool callchain)
{
    struct kmemleak_ctx *ctx = dev->private;
    struct sample_type_callchain *data = (void *)event->sample.array;

    if (callchain) {
        struct callchain_data cd;
        perf_event_build_callchain_data(perf_event_evsel(dev, event), event, &cd);
        print_callchain_data(ctx->cc, &cd);
        if (dev->env->flame_graph) {
            if (ctx->default_flags & CALLCHAIN_USER) {
                const char *comm = tep__pid_to_comm((int)data->h.tid_entry.tid);
                flame_graph_add_callchain(ctx->flame, &cd, data->h.tid_entry.pid, !strcmp(comm, "<...>") ? NULL : comm);
            } else
                flame_graph_add_callchain(ctx->flame, &cd, 0/*only kernel stack*/, NULL);
        }
    }
}

struct comm_entry {
    const char *comm;
    unsigned long bytes;
    int n;
};
struct leaked_bytes {
    unsigned long leaked;
    int pid;
    int nr_comm;
    int max_comm;
    struct comm_entry *comms;
};

static void collect_leaked_bytes(struct prof_dev *dev, struct kmemleak_ctx *ctx,
            struct key_value_paires *kv_pairs, struct perf_event_backup *alloc)
{
    union perf_event *event = &alloc->event;
    struct sample_type_callchain *data = (void *)event->sample.array;

    if (alloc->callchain) {
        struct perf_evsel *evsel = perf_evlist__id_to_evsel(dev->evlist, data->h.id, NULL);
        struct tp *tp = tp_from_evsel(evsel, ctx->tp_alloc);
        struct leaked_bytes *leaked;
        unsigned long bytes_alloc = 0;
        void *raw;
        int size;

        if (tp && tp->mem_size_prog) {
            __raw_size(event, true, &raw, &size);
            bytes_alloc = tp_get_mem_size(tp, GLOBAL(data->h.cpu_entry.cpu, data->h.tid_entry.pid, raw, size));
        }

        if (tp->dwarf_unwind) {
            struct callchain_data cd;
            perf_event_build_callchain_data(evsel, event, &cd);
            leaked = keyvalue_pairs_add_callchain(kv_pairs, &cd);
        } else
            leaked = keyvalue_pairs_add_key(kv_pairs, (struct_key *)&data->callchain);

        leaked->leaked += bytes_alloc;
        if (leaked->pid == 0)
            leaked->pid = data->h.tid_entry.pid;

        if (ctx->collect_comm && alloc->comm[0]) {
            const char *ucomm = unique_string(alloc->comm);
            int i;
            for (i = 0; i < leaked->nr_comm; i++) {
                if (leaked->comms[i].comm == ucomm) {
                    leaked->comms[i].bytes += bytes_alloc;
                    leaked->comms[i].n++;
                    return;
                }
            }
            if (leaked->nr_comm >= leaked->max_comm) {
                int new_max = leaked->max_comm ? leaked->max_comm * 2 : 4;
                struct comm_entry *new_comms = realloc(leaked->comms, new_max * sizeof(*new_comms));
                if (!new_comms)
                    return;
                leaked->comms = new_comms;
                leaked->max_comm = new_max;
            }
            leaked->comms[leaked->nr_comm].comm = ucomm;
            leaked->comms[leaked->nr_comm].bytes = bytes_alloc;
            leaked->comms[leaked->nr_comm].n = 1;
            leaked->nr_comm++;
        }
    }
}

static int __leak_cmp(void **value1, void **value2)
{
    struct leaked_bytes *b1 = *(struct leaked_bytes **)value1;
    struct leaked_bytes *b2 = *(struct leaked_bytes **)value2;

    if (b1->leaked < b2->leaked)
        return 1;
    else if (b1->leaked > b2->leaked)
        return -1;
    else
        return b1->pid - b2->pid;
}

static int __comm_cmp(const void *a, const void *b)
{
    const struct comm_entry *ca = a, *cb = b;
    if (cb->bytes != ca->bytes)
        return cb->bytes > ca->bytes ? 1 : -1;
    return cb->n - ca->n;
}

static void __print_leak(void *opaque, struct_key *key, void *value, unsigned int n)
{
    struct kmemleak_ctx *ctx = opaque;
    struct leaked_bytes *leaked = value;
    printf("Leak of %lu bytes in %u objects allocated from:\n", leaked->leaked, n);
    if (leaked->nr_comm > 0) {
        int i;
        qsort(leaked->comms, leaked->nr_comm, sizeof(leaked->comms[0]), __comm_cmp);
        printf("    comms:");
        for (i = 0; i < leaked->nr_comm; i++)
            printf(" %s(%lu/%d)", leaked->comms[i].comm, leaked->comms[i].bytes, leaked->comms[i].n);
        printf("\n");
        free(leaked->comms);
    }
    print_callchain_common(ctx->cc, key, leaked->pid);
}

static void report_kmemleak_stat(struct kmemleak_ctx *ctx, bool from_sigusr1)
{
    if (ctx->stat.total_alloc == 0)
        return;
    print_time(stdout);
    printf("\nKMEMLEAK STATS:\n");
    if (from_sigusr1)
        printf("ALLOC LIST num %llu mem %llu\n"
           "TOTAL alloc %llu free %llu\n\n",
           ctx->stat.alloc_num, ctx->stat.alloc_mem,
           ctx->stat.total_alloc, ctx->stat.total_free);
    else
        printf("TOTAL alloc %llu free %llu\n\n",
           ctx->stat.total_alloc, ctx->stat.total_free);
}

static void sorted_insert(struct rb_root_cached *root, struct perf_event_backup *new)
{
    struct rb_node **p = &root->rb_root.rb_node;
    struct rb_node *parent = NULL;
    struct sample_type_header *new_h = (void *)new->event.sample.array;
    bool leftmost = true;

    while (*p) {
        struct perf_event_backup *b = rb_entry(*p, struct perf_event_backup, rbnode);
        struct sample_type_header *b_h = (void *)b->event.sample.array;
        parent = *p;

        if (new_h->time < b_h->time)
            p = &(*p)->rb_left;
        else if (new_h->time > b_h->time) {
            p = &(*p)->rb_right;
            leftmost = false;
        } else if (new->ptr < b->ptr)
            p = &(*p)->rb_left;
        else {
            p = &(*p)->rb_right;
            leftmost = false;
        }
    }
    rb_link_node(&new->rbnode, parent, p);
    rb_insert_color_cached(&new->rbnode, root, leftmost);
}

static void report_kmemleak(struct prof_dev *dev)
{
    struct kmemleak_ctx *ctx = dev->private;
    struct rb_node *node, *next;
    struct perf_event_backup *alloc;
    union perf_event *event;
    struct sample_type_header *data;
    void *raw;
    int size;
    struct rb_root_cached sorted = RB_ROOT_CACHED;
    struct key_value_paires *kv_pairs = NULL;
    bool selected = true;
    u64 time_ns = 0UL;
    unsigned int nr_entries;

    report_kmemleak_stat(ctx, false);

    if (RB_EMPTY_ROOT(&ctx->alloc))
        return;

    /* Move all nodes from alloc tree to sorted tree (sorted by time, ptr). */
    nr_entries = ctx->stat.alloc_num;
    for (node = rb_first(&ctx->alloc); node; node = next) {
        next = rb_next(node);
        alloc = rb_entry(node, struct perf_event_backup, rbnode);
        rb_erase(node, &ctx->alloc);
        sorted_insert(&sorted, alloc);
    }

    if (dev->env->greater_than) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_ns = tv.tv_sec * NSEC_PER_SEC + tv.tv_usec * 1000;
    }

    if (ctx->report_leaked_bytes) {
        kv_pairs = keyvalue_pairs_new(sizeof(struct leaked_bytes));
    }
    if (!kv_pairs || dev->env->verbose) {
        printf("KMEMLEAK REPORT: %u\n", nr_entries);
    }
    for (node = rb_first_cached(&sorted); node; node = next) {
        next = rb_next(node);
        alloc = rb_entry(node, struct perf_event_backup, rbnode);
        event = &alloc->event;
        data = (void *)event->sample.array;
        if (time_ns) {
            u64 realtime_ns = evclock_to_realtime_ns(dev, (evclock_t)(u64)data->time);
            selected = realtime_ns ? (time_ns - realtime_ns > dev->env->greater_than) : true;
        }

        if (kv_pairs && selected) {
            collect_leaked_bytes(dev, ctx, kv_pairs, alloc);
        }
        if ((!kv_pairs && selected) || dev->env->verbose) {
            __raw_size(event, alloc->callchain, &raw, &size);
            tep__print_event(data->time, data->cpu_entry.cpu, raw, size);
            __print_callchain(dev, event, alloc->callchain);
        }

        rb_erase_cached(node, &sorted);
        free(alloc);
    }
    /* alloc tree is empty, reset stat */
    ctx->stat.alloc_num = 0;
    ctx->stat.alloc_mem = 0;

    if (kv_pairs) {
        printf("LEAKED BYTES REPORT:\n");
        keyvalue_pairs_sorted_foreach(kv_pairs, __leak_cmp, __print_leak, ctx);
        keyvalue_pairs_free(kv_pairs);
    }
}

static inline int kmemleak_event_lost(struct prof_dev *dev, union perf_event *event)
{
    struct kmemleak_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct kmemleak_lost_node *lost, *next;

    if (likely(list_empty(&ctx->lost_list)))
        return 0;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link) {
        // Events before lost->start_time are processed normally.
        if (data->time <= lost->start_time)
            return 0;

        if (!lost->reclaim) {
            lost_reclaim(dev);
            lost->reclaim = true;
        }

        // Within the lost range, new events are also unsafe.
        if (data->time < lost->end_time) {
            return -1;
        } else {
            // Re-process subsequent events normally.
            list_del(&lost->lost_link);
            free(lost);
        }
    }
    return 0;
}

static long kmemleak_ftrace_filter(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct kmemleak_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct perf_evsel *evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    struct tp *tp = evsel ? perf_evsel_tp(evsel) : NULL;
    bool callchain = tp ? tp->stack : !!(perf_evsel__attr(evsel)->sample_type & PERF_SAMPLE_CALLCHAIN);
    void *raw;
    int size;
    long err;
    struct expr_global *glo;

    __raw_size(event, callchain, &raw, &size);
    glo = GLOBAL(data->cpu_entry.cpu, data->tid_entry.pid, raw, size);
    if (tp) {
        if (!tp->ftrace_filter)
            return 1;
        return tp_prog_run(tp, tp->ftrace_filter, glo);
    }
    err = tp_list_ftrace_filter(dev, ctx->tp_alloc, glo);
    if (err < 0)
        err = tp_list_ftrace_filter(dev, ctx->tp_free, glo);
    return err;
}

static void kmemleak_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct kmemleak_ctx *ctx = dev->private;
    // in linux/perf_event.h
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
    struct sample_type_header *data = (void *)event->sample.array;
    struct perf_evsel *evsel;
    struct perf_event_backup *b;
    struct tp *tp = NULL;
    void *ptr = NULL;
    int rc;
    void *raw;
    int size;
    bool is_alloc;
    bool callchain;
    struct expr_global *glo;

    /* PERF_SAMPLE_ID:
     * alloc need stack, free does not need stack, PERF_SAMPLE_ID must be set.
     * Because, there is no way to know whether there is a callchain in the perf_event sample.
     *
     * You can use the common_type of traceevent, but you need to get the raw location first.
     * To get the raw location, you must know whether there is a callchain in perf_event.
     */
    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    tp = tp_from_evsel(evsel, ctx->tp_alloc);
    if (!tp) {
        tp = tp_from_evsel(evsel, ctx->tp_free);
        if (!tp)
            return;
    }

    is_alloc = tp->private == ctx->tp_alloc;
    callchain = tp->stack;
    __raw_size(event, callchain, &raw, &size);

    if (dev->env->verbose >= VERBOSE_EVENT) {
        tep__update_comm(NULL, data->tid_entry.tid);
        tep__print_event(data->time, data->cpu_entry.cpu, raw, size);
        __print_callchain(dev, event, callchain);
    }

    if (kmemleak_event_lost(dev, event) < 0)
        return;

    glo = GLOBAL(data->cpu_entry.cpu, data->tid_entry.pid, raw, size);
    if (is_alloc) {
        ptr = tp_get_mem_ptr(tp, glo);

        b = alloc_new(ctx, (__u64)ptr, callchain, event, data->tid_entry.pid);
        if (!b)
            return;
        rc = alloc_insert(&ctx->alloc, b);
        if (rc == -EEXIST) {
            // Two alloc events with the same ptr implies a free in between.
            // This happens when the free has the same timestamp as the next
            // alloc and gets processed after it due to ordering.
            struct perf_event_backup *old = alloc_find(&ctx->alloc, (__u64)ptr);
            alloc_erase(ctx, old);
            alloc_insert(&ctx->alloc, b);
        }
        ctx->stat.total_alloc ++;
    } else {
        ptr = tp_get_mem_ptr(tp, glo);

        b = alloc_find(&ctx->alloc, (__u64)ptr);
        if (b)
            alloc_erase(ctx, b);
        ctx->stat.total_free ++;
    }
}

static void kmemleak_sigusr(struct prof_dev *dev, int signum)
{
    if (signum == SIGUSR1)
        report_kmemleak_stat(dev->private, true);
}

static void kmemleak_help(struct help_ctx *hctx)
{
    int j;
    struct env *env = hctx->env;
    struct tp_list *tp_alloc, *tp_free;
    struct tp *tp;

    if (hctx->nr_list != 2)
        return ;

    tp_alloc = hctx->tp_list[0];
    tp_free = hctx->tp_list[1];
    printf(PROGRAME " kmemleak ");
    printf("--alloc \"");
    for_each_real_tp(tp_alloc, tp, j) {
        printf("%s:%s/%s/ptr=%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".",
                         tp->mem_ptr?:".");
        if (tp->mem_size)
            printf("size=%s/", tp->mem_size);
        else
            printf("[size=./]");
        if (!env->callchain)
            printf("[stack/]");
        if (j != tp_alloc->nr_tp - 1)
            printf(",");
    }
    printf("\" ");

    printf("--free \"");
    for_each_real_tp(tp_free, tp, j) {
        printf("%s:%s/%s/ptr=%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".",
                         tp->mem_ptr?:".");
        if (j != tp_free->nr_tp - 1)
            printf(",");
    }
    printf("\" ");

    if (env->callchain)
        printf("-g ");
    if (env->flame_graph)
        printf("--flame-graph %s ", env->flame_graph);
    common_help(hctx, true, true, true, false, true, true, true);

    if (!env->callchain)
        printf("[-g] ");
    if (!env->flame_graph)
        printf("[--flame-graph .] ");
    common_help(hctx, false, true, true, false, true, true, true);
    printf("\n");
}


static const char *kmemleak_desc[] = PROFILER_DESC("kmemleak",
    "[OPTION...] --alloc EVENT[...] --free EVENT[...] [-g [--flame-graph file]]",
    "Generic memory leak analyzer for user/kernel allocators.",
    "",
    "SYNOPSIS",
    "    Memory leak: Allocated memory that is never freed.",
    "",
    "    This is a generic memory leak analyzer supporting arbitrary user-space and kernel-space",
    "    allocators. --alloc specifies memory allocation events, --free specifies memory free",
    "    events. Allocation and free events are matched via `ptr` ATTR. After extended collection,",
    "    unmatched allocation events are considered leaks.",
    "",
    "    Supports call stack aggregation, leak event reports, leaked bytes statistics, and flame",
    "    graph generation.",
    "",
    "EXAMPLES",
    "    "PROGRAME" kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/ --free kmem:kfree//ptr=ptr/ -m 128 -g",
    "    "PROGRAME" kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/,kmem:kmalloc_node//ptr=ptr/size=bytes_alloc/stack/ \\",
    "                       --free kmem:kfree//ptr=ptr/ -m 128 -g");
static const char *kmemleak_argv[] = PROFILER_ARGV("kmemleak",
    PROFILER_ARGV_OPTION,
    "FILTER OPTION:", "user-callchain", "kernel-callchain",
    PROFILER_ARGV_PROFILER, "event", "alloc", "free", "call-graph", "flame-graph",
    "comm\nShow alloc events comm.",
    "than\nMemory allocation exceeded the specified time.");
struct monitor kmemleak = {
    .name = "kmemleak",
    .desc = kmemleak_desc,
    .argv = kmemleak_argv,
    .pages = 4,
    .order = 1,
    .help = kmemleak_help,
    .init = kmemleak_init,
    .filter = kmemleak_filter,
    .deinit = kmemleak_exit,
    .sigusr = kmemleak_sigusr,
    .lost = kmemleak_lost,
    .ftrace_filter = kmemleak_ftrace_filter,
    .sample = kmemleak_sample,
};
MONITOR_REGISTER(kmemleak)
