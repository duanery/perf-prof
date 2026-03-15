#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/zalloc.h>
#include <linux/strlist.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <two-event.h>
#include <tp_struct.h>

#define ENABLED_MAX ULLONG_MAX
#define ENABLED_TP_MAX (ULLONG_MAX-1)

struct multi_trace_ctx;
struct timeline_node {
    struct rb_node timeline_node;
    u64    time;
    struct rb_node key_node;
    u64    key;
    struct tp *tp;
    struct tp *tp1; // nested-trace, -e A,A_ret, A's tp.
    u32 unneeded : 1,
        need_find_prev : 1,
        need_backup : 1,
        need_remove_from_backup : 1,
        maybe_unpaired : 1;
    u32 ins;
    u64 seq;
    union {
        struct list_head needed;
        struct list_head pending;
    };
    union perf_event *event;
};

struct timeline_stat {
    u64 new;
    u64 delete;
    u64 unneeded;
    u64 pending;
    u64 mem_bytes;
    u64 unneeded_bytes;
    u64 pending_bytes;
};

struct __backup_stat {
    u64 new;
    u64 delete;
    u64 mem_bytes;
};

enum lost_affect {
    LOST_AFFECT_ALL_EVENT,
    LOST_AFFECT_INS_EVENT,
};

struct lost_node {
    struct list_head lost_link;
    int ins;
    bool reclaim;
    u64 start_time;
    u64 end_time;
    u64 lost_id; // struct perf_record_lost::id
    u64 lost;
};

struct multi_trace_ctx {
    struct prof_dev *dev;
    int oncpu;
    int nr_ins;
    int nr_list;
    struct tp_list **tp_list;
    struct two_event_impl *impl;
    struct two_event_class *class;
    struct rblist backup;
    struct rblist timeline;
    struct list_head *perins_list;
    struct list_head needed_list; // need_timeline
    struct list_head pending_list; // need_timeline
    bool need_timeline;
    bool nested;
    bool impl_based_on_call;
    u64 recent_time; // The most recent time for all known events.
    u64 event_handled;
    u64 sched_wakeup_unnecessary;
    struct callchain_ctx *cc;
    struct perf_thread_map *thread_map; // profiler rundelay
    bool comm; // profiler rundelay, syscalls
    int level; // level = sched_init()

    /* lost */
    enum lost_affect lost_affect;
    struct list_head timeline_lost_list; // LOST_AFFECT_ALL_EVENT. struct lost_node
    struct list_head *perins_lost_list;  // LOST_AFFECT_INS_EVENT. struct lost_node

    /* syscalls: exit, exit_group */
    struct perf_evsel *extra_evsel;
    void (*extra_sample)(struct prof_dev *dev, union perf_event *event, int instance);

    /* stat */
    struct timeline_stat tl_stat;
    struct __backup_stat backup_stat;
};

static struct timeline_node *multi_trace_first_pending(struct prof_dev *dev, struct timeline_node *tail);

static int perf_event_backup_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct timeline_node *b = container_of(rbn, struct timeline_node, key_node);
    const struct timeline_node *e = entry;

    if (b->key > e->key)
        return 1;
    else if (b->key < e->key)
        return -1;
    else
        return 0;
}

static int perf_event_backup_node_find(const void *entry, const struct rb_node *rbn)
{
    struct timeline_node *b = container_of(rbn, struct timeline_node, key_node);
    const struct timeline_node *e = entry;

    if (b->key > e->key)
        return -1;
    else if (b->key < e->key)
        return 1;
    else
        return 0;
}

static struct rb_node *perf_event_backup_node_new(struct rblist *rlist, const void *new_entry)
{
    struct multi_trace_ctx *ctx = container_of(rlist, struct multi_trace_ctx, backup);
    if (ctx->need_timeline) {
        struct timeline_node *b = (void *)new_entry;
        /*
         * With --order enabled, events are backed up in chronological order. Therefore, it
         * can be directly added to the end of the queue `needed_list' without reordering.
        **/
        list_add_tail(&b->needed, &ctx->needed_list);
        RB_CLEAR_NODE(&b->key_node);
        return &b->key_node;
    } else {
        const struct timeline_node *e = new_entry;
        union perf_event *event = e->event;
        union perf_event *new_event = perf_event_dup(event);
        struct timeline_node *b = malloc(sizeof(*b));
        if (b && new_event) {
            b->time = e->time;
            b->key = e->key;
            b->tp = e->tp;
            b->tp1 = e->tp1;
            b->unneeded = 0;
            b->need_find_prev = e->need_find_prev;
            b->need_backup = e->need_backup;
            b->need_remove_from_backup = e->need_remove_from_backup;
            b->maybe_unpaired = 0;
            b->ins = e->ins;
            b->seq = e->seq;
            b->event = new_event;
            RB_CLEAR_NODE(&b->timeline_node);
            RB_CLEAR_NODE(&b->key_node);
            INIT_LIST_HEAD(&b->needed);
            /*
             * The events for each instance are time-ordered. Therefore, it can be directly added
             * to the end of the queue without reordering.
            **/
            list_add_tail(&b->needed, &ctx->perins_list[b->ins]);

            ctx->backup_stat.new ++;
            ctx->backup_stat.mem_bytes += event->header.size;
            return &b->key_node;
        } else {
            if (b) free(b);
            if (new_event && new_event != event) free(new_event);
            return NULL;
        }
    }
}

static void perf_event_backup_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct multi_trace_ctx *ctx = container_of(rblist, struct multi_trace_ctx, backup);
    struct timeline_node *b = container_of(rb_node, struct timeline_node, key_node);
    if (ctx->need_timeline) {
        b->unneeded = 1;
        list_del_init(&b->needed);
        ctx->tl_stat.unneeded ++;
        ctx->tl_stat.unneeded_bytes += b->event->header.size;
    } else {
        list_del(&b->needed);
        ctx->backup_stat.delete ++;
        ctx->backup_stat.mem_bytes -= b->event->header.size;
        free(b->event);
        free(b);
    }
}

static int timeline_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct timeline_node *b = container_of(rbn, struct timeline_node, timeline_node);
    const struct timeline_node *e = entry;

    if (b->time > e->time)
        return 1;
    else if (b->time < e->time)
        return -1;

    if (b->key > e->key)
        return 1;
    else if (b->key < e->key)
        return -1;

    // The time and key values of different events may be equal, so they are sorted by seq.
    if (b->seq > e->seq)
        return 1;
    else if (b->seq < e->seq)
        return -1;

    return 0;
}

static struct rb_node *timeline_node_new(struct rblist *rlist, const void *new_entry)
{
    struct multi_trace_ctx *ctx = container_of(rlist, struct multi_trace_ctx, timeline);
    const struct timeline_node *e = new_entry;
    union perf_event *event = e->event;
    union perf_event *new_event = perf_event_dup(event);
    struct timeline_node *b = malloc(sizeof(*b));
    if (b && new_event) {
        b->time = e->time;
        b->key = e->key;
        b->tp = e->tp;
        b->tp1 = e->tp1;
        b->unneeded = e->unneeded;
        b->need_find_prev = e->need_find_prev;
        b->need_backup = e->need_backup;
        b->need_remove_from_backup = e->need_remove_from_backup;
        b->maybe_unpaired = 0;
        b->ins = e->ins;
        b->seq = e->seq;
        b->event = new_event;
        RB_CLEAR_NODE(&b->timeline_node);
        RB_CLEAR_NODE(&b->key_node);
        INIT_LIST_HEAD(&b->pending);
        if (!b->tp->untraced) {
            /*
             * With --order enabled, events are backed up in chronological order. Therefore, it
             * can be directly added to the end of the queue `pending_list' without reordering.
            **/
            list_add_tail(&b->pending, &ctx->pending_list);
            b->unneeded = 0;
            ctx->tl_stat.pending ++;
            ctx->tl_stat.pending_bytes += event->header.size;
        }

        ctx->tl_stat.new ++;
        if (b->unneeded) {
            ctx->tl_stat.unneeded ++;
            ctx->tl_stat.unneeded_bytes += event->header.size;
        }
        ctx->tl_stat.mem_bytes += event->header.size;

        return &b->timeline_node;
    } else {
        if (b) free(b);
        if (new_event && new_event != event) free(new_event);
        return NULL;
    }
}

static void timeline_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct multi_trace_ctx *ctx = container_of(rblist, struct multi_trace_ctx, timeline);
    struct timeline_node *b = container_of(rb_node, struct timeline_node, timeline_node);
    if (!list_empty(&b->pending)) {
        list_del(&b->pending);
        fprintf(stderr, "BUG: event is still in the pending list.\n");
    }
    ctx->tl_stat.delete ++;
    ctx->tl_stat.mem_bytes -= b->event->header.size;
    if (b->unneeded) {
        ctx->tl_stat.unneeded --;
        ctx->tl_stat.unneeded_bytes -= b->event->header.size;
    }
    free(b->event);
    free(b);
}

static void timeline_free_unneeded(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    struct rb_node *next = rb_first_cached(&ctx->timeline.entries);
    struct timeline_node *tl;
    u64 unneeded_before = 0UL;
    u64 unneeded = 0, backup = 0;

    if (env->before_event1) {
        struct timeline_node *needed_first;
        if (!list_empty(&ctx->needed_list))
            needed_first = list_first_entry(&ctx->needed_list, struct timeline_node, needed);
        else if (!list_empty(&ctx->pending_list))
            needed_first = list_first_entry(&ctx->pending_list, struct timeline_node, pending);
        else {
            struct rb_node *unneeded_last = rb_last(&ctx->timeline.entries.rb_root);
            needed_first = rb_entry_safe(unneeded_last, struct timeline_node, timeline_node);
        }
        if (needed_first && needed_first->time > env->before_event1)
            unneeded_before = needed_first->time - env->before_event1;
    }

    while (next) {
        tl = rb_entry(next, struct timeline_node, timeline_node);

        // if before_event1: before `needed_first->time - before_event1` on the timeline
        // else: unneeded
        if ((unneeded_before == 0UL && tl->unneeded) ||
            tl->time < unneeded_before) {
            /*
             * When there are events lost, the backed-up event is deleted in time,
             * see multi_trace_event_lost().
             * Do a safety check here.
            **/
            if (unlikely(tl->unneeded == 0)) {
                if (RB_EMPTY_NODE(&tl->key_node)) {
                    fprintf(stderr, "BUG: rb key_node is empty\n");
                } else
                    rblist__remove_node(&ctx->backup, &tl->key_node);
                backup ++;
            } else
                unneeded ++;

            rblist__remove_node(&ctx->timeline, next);
        } else
            break;

        next = rb_first_cached(&ctx->timeline.entries);
    }

    if (unlikely(backup)) {
        print_time(stderr);
        fprintf(stderr, "free unneeded %lu, backup %lu\n", unneeded, backup);
    }
}

static void timeline_stat(struct multi_trace_ctx *ctx)
{
    printf("TIMELINE:\n"
           "  new = %lu\n"
           "  delete = %lu\n"
           "  unneeded = %lu\n"
           "  pending = %lu\n"
           "  mem_bytes = %lu\n"
           "  unneeded_bytes = %lu\n"
           "  pending_bytes = %lu\n"
           "BACKUP:\n"
           "  nr_entries = %u\n",
           ctx->tl_stat.new, ctx->tl_stat.delete, ctx->tl_stat.unneeded, ctx->tl_stat.pending,
           ctx->tl_stat.mem_bytes, ctx->tl_stat.unneeded_bytes, ctx->tl_stat.pending_bytes,
           rblist__nr_entries(&ctx->backup));
}

static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    int i, j, stacks = 0;
    int oncpu = prof_dev_ins_oncpu(dev);
    struct two_event_options options = {
        .keyname = (oncpu && !ctx->comm) ? "CPU" : "THREAD",
        .perins = env->perins,
        .comm = ctx->comm,
        .only_print_greater_than = env->only_print_greater_than,
        .greater_than = env->greater_than,
        .lower_than = env->lower_than,
        .hide_than = env->hide_than,
        .heatmap = env->heatmap,
        .first_n = 10,
        .sort_print = ctx->nested ? false : true,
        .env = env,
    };
    const char *keyname = NULL;
    bool untraced = false;
    int min_nr_events = 2;
    int nr_pull = 0, nr_real_nonpull_tp = 0;
    int nr_ringbuffer = 0;

    ctx->dev = dev;
    ctx->oncpu = oncpu;
    INIT_LIST_HEAD(&ctx->needed_list);
    INIT_LIST_HEAD(&ctx->pending_list);
    INIT_LIST_HEAD(&ctx->timeline_lost_list);

    tep__ref();

    if (ctx->nested)
        min_nr_events = 1;
    else if (env->cycle) {
        if (!env->impl || !strcmp(env->impl, TWO_EVENT_DELAY_IMPL))
            min_nr_events = 1;
        else
            env->cycle = 0;
    }
    if (env->nr_events < min_nr_events)
        goto failed;


    ctx->nr_ins = prof_dev_nr_ins(dev);
    ctx->nr_list = env->nr_events;
    ctx->tp_list = calloc(ctx->nr_list, sizeof(*ctx->tp_list));
    if (!ctx->tp_list)
        goto failed;

    for (i = 0; i < ctx->nr_list; i++) {
        struct tp *tp;
        int nr_real_untraced = 0;

        ctx->tp_list[i] = tp_list_new(dev, env->events[i]);
        if (!ctx->tp_list[i]) {
            goto failed;
        }
        stacks += ctx->tp_list[i]->nr_need_stack;
        for_each_real_tp(ctx->tp_list[i], tp, j) {
            if (env->verbose)
                printf("name %s id %d filter %s stack %d\n", tp->name, tp->id, tp->filter, tp->stack);
            if (tp->untraced && !tp->trigger)
                untraced = true;
            if (tp->receive)
                nr_pull += env->detail ? 1 : !tp->untraced;
            else
                nr_real_nonpull_tp += env->detail ? 1 : !tp->untraced;
            if (env->key && !tp->key) {
                tp->key_prog = tp_new_prog(tp, env->key);
                if (!tp->key_prog) {
                    fprintf(stderr, "%s:%s: Cannot set key '%s'\n", tp->sys, tp->name, env->key);
                    goto failed;
                }
                tp->key = env->key;
            }
            if (tp->key && !keyname)
                keyname = tp->key;
            if (tp->untraced) {
                if ((env->samekey || env->samepid || env->sametid) &&
                    !tp_kernel(tp) && !tp->vcpu)
                    fprintf(stderr, "The event %s:%s needs the vm attr to convert the fields of the Guest events.\n",
                            tp->sys, tp->name);
                nr_real_untraced++;
            }
        }
        // Check that any -e option does not have all events marked as untraced.
        // At least one event in each -e option must be traced.
        if (ctx->tp_list[i]->nr_real_tp - nr_real_untraced == 0) {
            fprintf(stderr, "Error: All tracepoint events in -e option %d are marked as untraced.\n", i + 1);
            fprintf(stderr, "  -e %s\n", env->events[i]);
            goto failed;
        }
    }

    if (stacks) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL), stdout);
        dev->pages *= 2;
    } else
        ctx->cc = NULL;

    // Each pull event comes from an independent channel and is regarded as a different
    // ringbuffer. Need to order.
    nr_ringbuffer = nr_pull;

    if (keyname) {
        options.keyname = keyname;
        options.keylen = strlen(keyname);
        if (options.keylen < 6)
            options.keylen = 6;
        // All events are ordered only when nr_ins==1.
        nr_ringbuffer += ctx->nr_ins;
        if (nr_ringbuffer > 1 && !using_order(dev)) {
            fprintf(stderr, "Enable --key or pull= attr, also need to enable --order.\n");
            goto failed;
        }
    } else {
        // Use instance as key, cpu or pid.
        if (!ctx->oncpu) {
            ctx->comm = 1;
            options.comm = 1;
        }
        /* Use instance as key.
         * When --detail is enabled, the events of all instance need to be ordered for
         * detailed output. Each instance accumulates a ringbuffer.
         *
         * Do not enable --detail, and use the instance as the key. All events participating
         * in latency tracing are ordered on a single instance. No matter how many instances
         * there are, ringbuffer is only + 1. Because correctness can be guaranteed without
         * enabling --order.
         */
        nr_ringbuffer += env->detail ? ctx->nr_ins : !!nr_real_nonpull_tp;
        if (nr_ringbuffer > 1 && !using_order(dev)) {
            fprintf(stderr, "Enable --detail or pull= attr, also need to enable --order.\n");
            goto failed;
        }
    }

    ctx->level = sched_init(ctx->nr_list, ctx->tp_list);

    if (env->impl && impl_based_on_call(env->impl))
        ctx->impl_based_on_call = true;
    if (ctx->impl_based_on_call && !ctx->nested) {
        fprintf(stderr, "Only nested-trace can enable --impl %s.\n", env->impl);
        goto failed;
    }

    ctx->impl = impl_get(env->impl ?: TWO_EVENT_DELAY_IMPL);
    if (!ctx->impl) {
        fprintf(stderr, "--impl %s not implemented\n", env->impl);
        goto failed;
    }
    ctx->class = ctx->impl->class_new(ctx->impl, &options);

    rblist__init(&ctx->backup);
    ctx->backup.node_cmp = perf_event_backup_node_cmp;
    ctx->backup.node_new = perf_event_backup_node_new;
    ctx->backup.node_delete = perf_event_backup_node_delete;

    rblist__init(&ctx->timeline);
    ctx->timeline.node_cmp = timeline_node_cmp;
    ctx->timeline.node_new = timeline_node_new;
    ctx->timeline.node_delete = timeline_node_delete;

    ctx->perins_list = malloc(ctx->nr_ins * sizeof(struct list_head));
    if (ctx->perins_list) {
        for (i = 0; i < ctx->nr_ins; i++)
            INIT_LIST_HEAD(&ctx->perins_list[i]);
    } else
        goto failed;

    // rundelay automatically sets the key.
    if (keyname || strcmp(dev->prof->name, "rundelay") == 0) {
        ctx->lost_affect = LOST_AFFECT_ALL_EVENT;
    } else {
        // use instance as key, cpu or pid.
        ctx->lost_affect = LOST_AFFECT_INS_EVENT;

        ctx->perins_lost_list = malloc(ctx->nr_ins * sizeof(struct list_head));
        if (ctx->perins_lost_list) {
            for (i = 0; i < ctx->nr_ins; i++)
                INIT_LIST_HEAD(&ctx->perins_lost_list[i]);
        } else
            goto failed;
    }

    ctx->need_timeline = env->detail;

    if (untraced && !env->detail) {
        fprintf(stderr, "WARN: --detail parameter is not enabled. No need to add untrace events.\n");
    }
    if ((!env->greater_than && !env->lower_than) && env->detail) {
        fprintf(stderr, "WARN: --than parameter is not enabled. No need to enable the "
                        "--detail parameter.\n");
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct lost_node *lost, *next;
    int i;

    while (multi_trace_first_pending(dev, NULL)) ;

    perf_thread_map__put(ctx->thread_map);

    rblist__exit(&ctx->backup);
    rblist__exit(&ctx->timeline);

    if (ctx->perins_lost_list) {
        for (i = 0; i < ctx->nr_ins; i++)
            list_for_each_entry_safe(lost, next, &ctx->perins_lost_list[i], lost_link)
                free(lost);
        free(ctx->perins_lost_list);
    } else
        list_for_each_entry_safe(lost, next, &ctx->timeline_lost_list, lost_link)
            free(lost);

    free(ctx->perins_list);

    if (ctx->impl && ctx->class)
        ctx->impl->class_delete(ctx->class);
    callchain_ctx_free(ctx->cc);

    if (ctx->tp_list) {
        int i;
        for (i = 0; i < ctx->nr_list; i++)
            tp_list_free(ctx->tp_list[i]);
        free(ctx->tp_list);
    }

    tep__unref();
    free(ctx);
}

static int __multi_trace_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct multi_trace_ctx *ctx = dev->private;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW,
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL),
        .watermark     = 1,
    };
    int i, j;

    if (monitor_ctx_init(dev) < 0)
        return -1;

    prof_dev_env2attr(dev, &attr);

    for (i = 0; i < ctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(ctx->tp_list[i], tp, j) {
            struct perf_evsel *evsel;
            evsel = tp_evsel_new(tp, &attr);
            if (!evsel) {
                goto failed;
            }
            perf_evlist__add(evlist, evsel);
        }
        for_each_dev_tp(ctx->tp_list[i], tp, j) {
            struct prof_dev *source_dev = tp->source_dev;
            if (source_dev &&
                prof_dev_forward(source_dev, dev) == 0) {
                // The target is responsible for whether to print title
                source_dev->print_title = false;
                if (!tp->untraced) {
                    fprintf(stderr, "%s can only be untraced.\n", source_dev->prof->name);
                    ctx->tp_list[i]->nr_untraced ++;
                }
                tp->untraced = true;

                /*
                 * source_dev uses an independent ringbuffer, and order must be enabled
                 * when forwarding to multi-trace.
                 *
                 *   perf-prof multi-trace -e kvm:kvm_exit -e 'kvm:kvm_entry,task-state/-m 256/untraced/' \
                 *   -t 210673 -m 128 -i 1000 --than 80us --detail=sametid
                 */
                if (ctx->need_timeline &&
                    !using_order(dev)) {
                    fprintf(stderr, "Enable --detail and %s//, also need to enable --order.\n", source_dev->prof->name);
                    goto failed;
                }
            }
        }
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int multi_trace_init(struct prof_dev *dev)
{
    int i, j, k, n;
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    if (!ctx) {
        dev->private = ctx = zalloc(sizeof(*ctx));
        if (!ctx)
            return -1;
    }

    ctx->nested = 0;
    if (__multi_trace_init(dev) < 0)
        return -1;

    // env->cycle: from the last one back to the first.
    for (k = 0; k < ctx->nr_list - !env->cycle; k++) {
        struct tp *tp1, *tp2;
        for_each_real_tp(ctx->tp_list[k], tp1, i) {
            if (tp1->untraced)
                continue;
            // for handle remaining
            if (!ctx->impl->object_new(ctx->class, tp1, NULL))
                goto failed;
            n = (k+1) % ctx->nr_list;
            for_each_real_tp(ctx->tp_list[n], tp2, j) {
                if (tp2->untraced)
                    continue;
                if (!ctx->impl->object_new(ctx->class, tp1, tp2))
                    goto failed;
            }
        }
    }
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int multi_trace_filter(struct prof_dev *dev)
{
    struct multi_trace_ctx *ctx = dev->private;
    int i, err;

    for (i = 0; i < ctx->nr_list; i++) {
        if ((err = tp_list_apply_filter(dev, ctx->tp_list[i])) < 0)
            return err;
    }
    return 0;
}

static void multi_trace_enabled(struct prof_dev *dev)
{
    /*
     * Start sampling after the events is fully enabled.
     *
     * -e sched:sched_wakeup -e sched:sched_switch -C 0-95
     * A sched_wakeup occurs on CPU0, possibly a paired sched_switch occurs on CPU95. When enabling,
     * CPU0 is enabled first, and CPU95 is enabled last. It is possible that the sched_wakeup event
     * is only sampled on CPU0, and the sched_switch event is not sampled on CPU95.
     * It is possible that sched_wakeup will block the timeline to free unneeded events.
    **/

    /*
     * See prof_dev_atomic_enable().
     */
}

static int multi_trace_call_remaining(struct prof_dev *dev, struct timeline_node *left, remaining_reason rr)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    struct two_event *two;

    two = ctx->impl->object_find(ctx->class, left->tp, NULL);
    if (two) {
        struct event_info info = {};
        struct event_iter iter = {};
        info.tp1 = left->tp;
        info.tp2 = NULL;
        info.key = left->key;
        info.recent_time = ctx->recent_time;
        info.rr = rr;
        if (ctx->need_timeline) {
            if (env->before_event1) {
                struct timeline_node backup = {
                    .time = left->time - env->before_event1,
                    .key = left->key,
                    .seq = 0,
                };
                iter.start = rb_entry_safe(rblist__find_first(&ctx->timeline, &backup),
                                            struct timeline_node, timeline_node);
            } else
                iter.start = left;
            iter.event1 = left;
            iter.event2 = NULL;
            iter.curr = iter.start;
        }
        if (info.recent_time - left->time > env->greater_than)
            left->maybe_unpaired = 1;

        return ctx->class->remaining(two, left->event, &info, ctx->need_timeline ? &iter : NULL);
    }
    return REMAINING_CONTINUE;
}

static void multi_trace_handle_remaining(struct prof_dev *dev, remaining_reason rr)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct rb_node *next = rb_first_cached(&ctx->backup.entries);
    struct timeline_node *left;

    while (next) {
        left = rb_entry(next, struct timeline_node, key_node);
        if (multi_trace_call_remaining(dev, left, rr) == REMAINING_BREAK)
            break;
        next = rb_next(next);
    }
}

static void multi_trace_interval(struct prof_dev *dev)
{
    struct multi_trace_ctx *ctx = dev->private;
    int i, j, k, n;
    int header = 0;
    struct two_event *two;

    // env->cycle: from the last one back to the first.
    for (k = 0; k < ctx->nr_list - !dev->env->cycle; k++) {
        struct tp *tp1, *tp2;
        for_each_real_tp(ctx->tp_list[k], tp1, i) {
            if (tp1->untraced)
                continue;
            // for print remaining
            two = ctx->impl->object_find(ctx->class, tp1, NULL);
            if (!header) {
                header = ctx->class->print_header(two);
            }
            ctx->class->print(two);
            n = (k+1) % ctx->nr_list;
            for_each_real_tp(ctx->tp_list[n], tp2, j) {
                if (tp2->untraced)
                    continue;
                two = ctx->impl->object_find(ctx->class, tp1, tp2);
                if (!header) {
                    header = ctx->class->print_header(two);
                }
                ctx->class->print(two);
            }
        }
    }
}

static void multi_trace_exit(struct prof_dev *dev)
{
    multi_trace_interval(dev);
    monitor_ctx_exit(dev);
}

static void multi_trace_flush(struct prof_dev *dev, enum profdev_flush how)
{
    if (how == PROF_DEV_FLUSH_FINAL) {
        struct multi_trace_ctx *ctx = dev->private;

        multi_trace_handle_remaining(dev, REMAINING_EXIT);

        while (multi_trace_first_pending(dev, NULL)) ;
        rblist__exit(&ctx->backup);
        rblist__exit(&ctx->timeline);
    }
}

static u64 multi_trace_minevtime(struct prof_dev *dev)
{
    struct multi_trace_ctx *ctx = dev->private;
    u64 minevtime = ULLONG_MAX;

    if (dev->env->greater_than || dev->env->lower_than) {
        struct rb_node *rbn;
        struct timeline_node *node = NULL, *tmp;

        if (ctx->need_timeline) {
            rbn = rb_first_cached(&ctx->timeline.entries);
            node = rb_entry_safe(rbn, struct timeline_node, timeline_node);
        } else {
            int i;
            for (i = 0; i < ctx->nr_ins; i++) {
                tmp = list_first_entry_or_null(&ctx->perins_list[i], struct timeline_node, needed);
                if (tmp && (!node || tmp->time < node->time))
                    node = tmp;
            }
        }
        if (node && node->time < minevtime)
            minevtime = node->time;
    }

    if (dev->env->perins && ctx->comm) {
        u64 mintime = 0;

        if (dev->env->interval && ctx->recent_time > dev->env->interval * NSEC_PER_MSEC)
            mintime = ctx->recent_time - dev->env->interval * NSEC_PER_MSEC;

        if (mintime < minevtime)
            minevtime = mintime;
    }

    return minevtime;
}

static void multi_trace_sigusr(struct prof_dev *dev, int signum)
{
    struct multi_trace_ctx *ctx = dev->private;

    if (signum != SIGUSR1)
        return;

    if (ctx->need_timeline)
        timeline_stat(ctx);
    else {
        printf("BACKUP:\n"
               "  new = %lu\n"
               "  delete = %lu\n"
               "  nr_entries = %u\n"
               "  mem_bytes = %lu\n",
               ctx->backup_stat.new, ctx->backup_stat.delete, rblist__nr_entries(&ctx->backup),
               ctx->backup_stat.mem_bytes);
    }
    printf("SPECIAL EVENT:\n");
    printf("  sched:sched_wakeup unnecessary %lu\n", ctx->sched_wakeup_unnecessary);
}

static inline void reclaim(struct prof_dev *dev, u64 key, remaining_reason rr)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct rb_node *node, *next;
    struct timeline_node backup = {
        .key = key,
    };
    int remaining = REMAINING_CONTINUE;

    // Remove all events with the same key.
    node = rb_find_first(&backup, &ctx->backup.entries.rb_root, perf_event_backup_node_find);
    while (node) {
        next = rb_next_match(&backup, node, perf_event_backup_node_find);
        if (remaining == REMAINING_CONTINUE) {
            struct timeline_node *left = rb_entry(node, struct timeline_node, key_node);
            remaining = multi_trace_call_remaining(dev, left, rr);
        }
        rblist__remove_node(&ctx->backup, node);
        node = next;
    }
}

static inline void lost_reclaim(struct prof_dev *dev, int ins)
{
    struct multi_trace_ctx *ctx = dev->private;

    if (ctx->lost_affect == LOST_AFFECT_INS_EVENT) {
        u64 key = ctx->oncpu ? prof_dev_ins_cpu(dev, ins) : prof_dev_ins_thread(dev, ins);
        reclaim(dev, key, REMAINING_LOST);
    } else {
        multi_trace_handle_remaining(dev, REMAINING_LOST);
        rblist__exit(&ctx->backup);
    }
}

static void multi_trace_print_lost(struct prof_dev *dev, union perf_event *event, int ins)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct lost_node *lost;
    struct env *env;

    if (event)
        return print_lost_fn(dev, event, ins);

    if (ctx->lost_affect == LOST_AFFECT_INS_EVENT)
        lost = list_first_entry(&ctx->perins_lost_list[ins], struct lost_node, lost_link);
    else
        lost = list_first_entry(&ctx->timeline_lost_list, struct lost_node, lost_link);

    env = dev->env;
    if (unlikely(env->verbose >= VERBOSE_NOTICE)) {
        print_time(stderr);
        fprintf(stderr, "%s: lost %lu events on %s #%d", dev->prof->name, lost->lost,
                        ctx->oncpu ? "CPU" : "thread",
                        ctx->oncpu ? prof_dev_ins_cpu(dev, lost->ins) : prof_dev_ins_thread(dev, lost->ins));
        if (env->greater_than || env->lower_than)
            fprintf(stderr, " (%lu.%06lu, %lu.%06lu)\n", lost->start_time/NSEC_PER_SEC, (lost->start_time%NSEC_PER_SEC)/1000,
                                lost->end_time/NSEC_PER_SEC, (lost->end_time%NSEC_PER_SEC)/1000);
        else
            fprintf(stderr, "\n");
    } else {
        struct perf_record_lost lost_event = {
            .header = {PERF_RECORD_LOST, 0, sizeof(struct perf_record_lost)},
            .id = lost->lost_id,
            .lost = lost->lost,
        };
        print_lost_fn(dev, (union perf_event *)&lost_event, lost->ins);
    }
}

static void multi_trace_lost(struct prof_dev *dev, union perf_event *event, int ins, u64 lost_start, u64 lost_end)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct lost_node *pos;
    struct lost_node *lost;

    // Without order, events are processed in the order within the ringbuffer.
    // When lost, all previous events have been processed and only need to reclaim.
    if (!using_order(dev) && !dev->env->after_event2) {
        multi_trace_print_lost(dev, event, ins);
        lost_reclaim(dev, ins);
        if (ctx->need_timeline)
            timeline_free_unneeded(dev);
        return;
    }

    // When order is enabled, event loss will be sensed in advance, but it needs to
    // be processed later. Similarly, with --detail=+1ms, event loss will also be
    // seen in advance and processed later.
    lost = malloc(sizeof(*lost));
    if (lost) {
        lost->ins = ins;
        lost->reclaim = false;
        lost->start_time = lost_start;
        lost->end_time = lost_end;
        lost->lost_id = event->lost.id;
        lost->lost = event->lost.lost;

        if (ctx->lost_affect == LOST_AFFECT_INS_EVENT) {
            list_add_tail(&lost->lost_link, &ctx->perins_lost_list[ins]);
        } else {
            list_for_each_entry(pos, &ctx->timeline_lost_list, lost_link) {
                if (pos->start_time > lost_start)
                    break;
            }
            list_add_tail(&lost->lost_link, &pos->lost_link);
        }
    }
}

void multi_trace_raw_size(union perf_event *event, void **praw, int *psize, struct tp *tp)
{
    if (tp->stack) {
        struct multi_trace_type_callchain *data = (void *)event->sample.array;
        struct {
            __u32   size;
            __u8    data[0];
        } *raw = (void *)data->callchain.ips + data->callchain.nr * sizeof(__u64);
        *praw = raw->data;
        *psize = raw->size;
    } else {
        struct multi_trace_type_raw *raw = (void *)event->sample.array;
        *praw = raw->raw.data;
        *psize = raw->raw.size;
    }
}

void multi_trace_print_title(union perf_event *event, struct tp *tp, const char *title)
{
    struct prof_dev *dev = tp->dev;
    struct multi_trace_ctx *ctx = dev->private;
    struct multi_trace_type_callchain *data = (void *)event->sample.array;
    void *raw;
    int size;

    if (dev->print_title) {
        if (title)
            printf("%-27s", title);
        else
            prof_dev_print_time(dev, data->h.time, stdout);
        tp_print_marker(tp);
    }

    if (event->header.type == PERF_RECORD_DEV) {
        prof_dev_print_event(dev, event, 0, OMIT_TIMESTAMP);
        return;
    }

    multi_trace_raw_size(event, &raw, &size, tp);
    tp_print_event(tp, data->h.time, data->h.cpu_entry.cpu, raw, size);

    if (tp->stack) {
        struct callchain_data cd;
        perf_event_build_callchain_data(tp->evsel, event, &cd);
        print_callchain_data(ctx->cc, &cd);
    }
}

static inline bool event_comparable(union perf_event *event1, struct tp *tp1, union perf_event *event2, struct tp *tp2)
{
    // tp_kernel: Compare key values directly.
    //!tp_kernel: Rely on vm attr to convert the fields of the Guest events.
    /*
     * Guest
     *   tp_kernel(tp) == false
     *   tp->vcpu: vm= attr, convert to Host event.
     *
     * Host-kernel
     *   tp_kernel(tp) == false && tp_is_dev(tp) == false
     * Host-forward
     *   tp_kernel(tp) == false && tp_is_dev(tp) == true
     *
     * Event comparability.
     *   Guest <=> Guest
     *     tp_kernel(tp1) == tp_kernel(tp2) == false  and
     *     !!tp1->vcpu == !!tp2->vcpu
     *   Host  <=> Host
     *     tp_kernel(tp1) == tp_kernel(tp2) == true
     *
     *   Guest <=> Host
     *     tp_kernel(tp1) == false and tp_kernel(tp2) == true and
     *     !!tp1->vcpu
     *   Host  <=> Guest
     *     tp_kernel(tp1) == true and tp_kernel(tp2) == false and
     *     !!tp2->vcpu
     */
    bool tp1_host = tp1 && (tp_kernel(tp1) || !!tp1->vcpu); // event, tp maybe NULL.
    bool tp2_host = tp2 && (tp_kernel(tp2) || !!tp2->vcpu);
    return tp1_host == tp2_host;
}

bool event_need_to_print(union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter)
{
    struct prof_dev *dev = info->tp1->dev;
    struct multi_trace_ctx *ctx = dev->private;
    struct env *env = dev->env;
    struct timeline_node *curr = iter->curr;
    union perf_event *event = iter->event;
    struct multi_trace_type_header *e  = (void *)event->sample.array;
    struct multi_trace_type_header *e1 = (void *)event1->sample.array;
    struct multi_trace_type_header *e2 = event2 ? (void *)event2->sample.array : NULL;
    bool cmp_e1, cmp_e2, samecpu_cmp_e1;
    void *raw = NULL;
    int size = 0;

    if (!(env->samecpu || env->samepid || env->sametid || env->samekey))
        return true;

    cmp_e1 = event_comparable(event, curr->tp, event1, info->tp1);
    cmp_e2 = event_comparable(event, curr->tp, event2, info->tp2); // event2, tp2 maybe NULL.
    if (!cmp_e1 && !cmp_e2)
        return false;

    if (event->header.type != PERF_RECORD_DEV) {
        // Guest  Host-kernel
        multi_trace_raw_size(event, &raw, &size, curr->tp);
    } else {
        raw = event;
        size = event->header.size;
    }
    iter->reason = NULL;

    samecpu_cmp_e1 = cmp_e1;
    if (env->same1 || env->same2) {
        cmp_e1 = cmp_e1 && env->same1;
        cmp_e2 = cmp_e2 && env->same2;
    }

    // e->cpu_entry.cpu maybe -1, See block_event_convert()
    if (env->samecpu && e->cpu_entry.cpu != -1) {
        if (samecpu_cmp_e1) {
            // cpu tracking
            // ctx->comm: rundelay, syscalls. The key is pid.
            int track_tid = ctx->comm ? (int)info->key : e1->tid_entry.tid;
            if (track_tid > 0) {
                iter->debug_msg = "cpu tracking";
                if (tp_target_cpu(curr->tp, raw, size, e->cpu_entry.cpu, track_tid, &iter->recent_cpu, &iter->reason)) {
                    if (iter->recent_cpu == -1)
                        iter->debug_msg = "cpu tracking end";
                    goto TRUE;
                } else if (!ctx->comm && /* iter->recent_cpu maybe -1, See block_event_convert() */
                        e->cpu_entry.cpu != iter->recent_cpu &&
                        e->tid_entry.tid == track_tid) {
                    iter->recent_cpu = e->cpu_entry.cpu;
                    goto TRUE;
                }
            }

            iter->debug_msg = "samecpu-track";
            if (e->cpu_entry.cpu == iter->recent_cpu ||
                tp_samecpu(curr->tp, raw, size, iter->recent_cpu))
                goto TRUE;

            if (cmp_e1 && !ctx->comm) {
                iter->debug_msg = "samecpu-1";
                if (e->cpu_entry.cpu == e1->cpu_entry.cpu ||
                    tp_samecpu(curr->tp, raw, size, e1->cpu_entry.cpu))
                    goto TRUE;
            }
        }
        if (cmp_e2) {
            if (!ctx->comm) {
                iter->debug_msg = "samecpu-2";
                if (e->cpu_entry.cpu == e2->cpu_entry.cpu ||
                   (e1->cpu_entry.cpu != e2->cpu_entry.cpu &&
                        tp_samecpu(curr->tp, raw, size, e2->cpu_entry.cpu)))
                    goto TRUE;
            }
        }
    }

    if (env->samepid) {
        if (cmp_e1) {
            // ctx->comm: rundelay, syscalls. The key is pid.
            int pid = ctx->comm ? (int)info->key : e1->tid_entry.pid;
            iter->debug_msg = "samepid-1";
            if (pid > 0) // exclude 0 and e1->tid_entry.pid maybe -1
            if (e->tid_entry.pid == pid ||
                tp_samepid(curr->tp, raw, size, pid))
                goto TRUE;
        }
        if (cmp_e2 && (int)e2->tid_entry.pid > 0) {
            iter->debug_msg = "samepid-2";
            if (!ctx->comm) {
                if (e->tid_entry.pid == e2->tid_entry.pid ||
                   (e1->tid_entry.pid != e2->tid_entry.pid &&
                        tp_samepid(curr->tp, raw, size, e2->tid_entry.pid)))
                    goto TRUE;
            }
        }
    }

    if (env->sametid) {
        if (cmp_e1) {
            // ctx->comm: rundelay, syscalls. The key is pid.
            int tid = ctx->comm ? (int)info->key : e1->tid_entry.tid;
            iter->debug_msg = "sametid-1";
            if (tid > 0) // exclude 0 and e1->tid_entry.tid maybe -1
            if (e->tid_entry.tid == tid ||
                tp_samepid(curr->tp, raw, size, tid))
                goto TRUE;
        }
        if (cmp_e2 && (int)e2->tid_entry.tid > 0) {
            iter->debug_msg = "sametid-2";
            if (!ctx->comm) {
                if (e->tid_entry.tid == e2->tid_entry.tid ||
                   (e1->tid_entry.tid != e2->tid_entry.tid &&
                        tp_samepid(curr->tp, raw, size, e2->tid_entry.tid)))
                    goto TRUE;
            }
        }
    }

    iter->debug_msg = "samekey";
    if (env->samekey)
    if ((!!curr->tp->key) == (!!info->tp1->key) &&
        curr->key == info->key)
        goto TRUE;

    iter->debug_msg = NULL;
    return false;

TRUE:
    if (!env->verbose)
        iter->debug_msg = NULL;

    // samecpu
    if (env->samecpu && e->cpu_entry.cpu != -1) {
        int next_pid, cpuslot = -1, i;
        const char *prev_comm;
        iter->running_time = 0;
        iter->comm = NULL;
        // Only the sched_switch event can return 1.
        if (tp_oncpu(curr->tp, raw, size, &next_pid, &prev_comm)) {
            /* cpu tracking
             * iter->recent_cpu : indicates the CPU that `track_tid` is running on.
             * iter->curr_cpu[2]: records the pid and start time running on
             * recent_cpu, and changes with recent_cpu.
             *
             * [000]  sched_wakeup: track_tid CPU:002         | recent_cpu = 2
             * [002]  ...                                     |
             * [002]  sched_migrate_task: track_tid dst_cpu=3 | recent_cpu = 3
             * [003]  ...                                     |
             * [003]  sched_switch: xx => track_tid           | recent_cpu = 3
             * [003]  sched_switch: track_tid => py           | recent_cpu = -1
             */
            if (e->cpu_entry.cpu == iter->recent_cpu) {
                // curr_cpu[2] may be -1, and when it is different from recent_cpu,
                // curr_pid[2] and curr_time[2] are no longer correct and need to be
                // synchronized from [0], [1] or reset.
                if (iter->curr_cpu[2] != iter->recent_cpu) {
                    iter->curr_cpu[2] = iter->recent_cpu;
                    if (iter->curr_cpu[2] == iter->curr_cpu[0]) {
                        iter->curr_pid[2] = iter->curr_pid[0];
                        iter->curr_time[2] = iter->curr_time[0];
                    } else if (iter->curr_cpu[2] == iter->curr_cpu[1]) {
                        iter->curr_pid[2] = iter->curr_pid[1];
                        iter->curr_time[2] = iter->curr_time[1];
                    } else {
                        iter->curr_pid[2] = -1;
                        iter->curr_time[2] = 0;
                    }
                }
                cpuslot = 2;
            } else if (e->cpu_entry.cpu == iter->curr_cpu[2]) cpuslot = 2;
              else if (e->cpu_entry.cpu == iter->curr_cpu[0]) cpuslot = 0;
              else if (e->cpu_entry.cpu == iter->curr_cpu[1]) cpuslot = 1;

            if (cpuslot >= 0) {
                if (iter->curr_time[cpuslot] &&
                    iter->curr_pid[cpuslot] > 0 && // exclude swapper/*
                    iter->curr_pid[cpuslot] == e->tid_entry.tid) {
                    iter->running_time = iter->time - iter->curr_time[cpuslot];
                    iter->comm = prev_comm;
                }
                iter->curr_pid[cpuslot] = next_pid;
                iter->curr_time[cpuslot] = iter->time;

                for (i = 0; i < 3; i++)
                    if (cpuslot != i && iter->curr_cpu[i] == iter->curr_cpu[cpuslot]) {
                        iter->curr_pid[i] = iter->curr_pid[cpuslot];
                        iter->curr_time[i] = iter->curr_time[cpuslot];
                    }
                if (cpuslot == 2 && iter->recent_cpu == -1) {
                    iter->curr_cpu[2] = -1;
                    iter->curr_pid[2] = -1;
                    iter->curr_time[2] = 0;
                }
            }
        }
    }
    return true;
}

int event_iter_cmd(struct event_iter *iter, enum event_iter_cmd cmd)
{
    struct timeline_node *curr;
    struct rb_node *rbn;

    switch (cmd) {
        case CMD_RESET:
            curr = iter->curr = iter->start;
            break;
        case CMD_EVENT1:
        case CMD_EVENT2:
            curr = iter->curr = (cmd == CMD_EVENT1 ? iter->event1 : iter->event2);
            break;
        case CMD_PREV:
        case CMD_NEXT:
            if (iter->curr == NULL)
                return 0;

            curr = iter->curr;
            rbn = (cmd == CMD_PREV ? rb_prev : rb_next)(&curr->timeline_node);
            iter->curr = rb_entry_safe(rbn, struct timeline_node, timeline_node);
            if (!iter->curr)
                return 0;

            curr = iter->curr;
            break;
        case CMD_MAX:
        default:
            return 0;
    }

    iter->event = curr->event;
    iter->tp = curr->tp;
    iter->time = curr->time;
    return 1;
}

static struct rb_node *multi_trace_find_prev(struct prof_dev *dev, struct timeline_node *backup)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct rb_node *rbn;
    rb_for_each(rbn, backup, &ctx->backup.entries.rb_root, perf_event_backup_node_find) {
        if (!backup->tp)
            return rbn;
        else {
            struct timeline_node *prev;
            prev = container_of(rbn, struct timeline_node, key_node);
            if (prev->tp == backup->tp)
                return rbn;
        }
    }
    return NULL;
}

static void multi_trace_tryto_call_two(struct prof_dev *dev, struct timeline_node *tl_event, bool *need_free)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    union perf_event *event = tl_event->event;
    struct tp *tp = tl_event->tp;
    struct tp *tp1 = tl_event->tp1;
    bool need_find_prev = tl_event->need_find_prev;
    bool need_remove_from_backup = tl_event->need_remove_from_backup;
    u64 key = tl_event->key;

    // find prev event, not include untraced
    if (need_find_prev) {
        struct timeline_node backup = {
            .key = key,
            .tp = tp1,
        };
        struct two_event *two;
        struct rb_node *rbn = multi_trace_find_prev(dev, &backup);
        if (rbn) {
            struct timeline_node *prev;
            prev = container_of(rbn, struct timeline_node, key_node);
            prev->maybe_unpaired = 0;
            two = ctx->impl->object_find(ctx->class, prev->tp, tp);
            if (two && prev->time < tl_event->time/* Out of order */) {
                struct event_info info = {};
                info.tp1 = prev->tp;
                info.tp2 = tp;
                info.key = key;
                info.recent_time = ctx->recent_time;
                if (ctx->need_timeline) {
                    struct event_iter iter = {};
                    if (env->before_event1) {
                        backup.time = prev->time - env->before_event1;
                        backup.seq = 0;
                        iter.start = rb_entry_safe(rblist__find_first(&ctx->timeline, &backup),
                                                    struct timeline_node, timeline_node);
                    } else
                        iter.start = prev;
                    iter.event1 = prev;
                    iter.event2 = tl_event;
                    iter.curr = iter.start;
                    ctx->class->two(two, prev->event, event, &info, &iter);
                } else
                    ctx->class->two(two, prev->event, event, &info, NULL);
            }

            if (need_remove_from_backup) {
                rblist__remove_node(&ctx->backup, rbn);

                // ctx->backup no longer references an event, prev.unneeded = 1,
                // releasing unneeded events on the timeline in time.
                *need_free = true;
            }
        } else if (ctx->impl_based_on_call) {
            two = ctx->impl->object_find(ctx->class, tp, NULL);
            if (two) {
                // two(A, NULL), first call A.
                struct event_info info = {};
                info.tp1 = tp;
                info.tp2 = NULL;
                info.key = key;
                ctx->class->two(two, event, NULL, &info, NULL);
            }
        }
    }
}

static int multi_trace_tryto_backup(struct prof_dev *dev, struct timeline_node *tl_event, bool *need_free)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    bool need_backup = tl_event->need_backup;
    unsigned int nr_entries;
    struct rb_node *rbn;
    int ret = -1;

    // backup events, exclude untraced events.
    if (need_backup) {
    retry:
        nr_entries = rblist__nr_entries(&ctx->backup);
        rbn = rblist__findnew(&ctx->backup, tl_event);
        if (rbn) {
            if (nr_entries == rblist__nr_entries(&ctx->backup)) {
                struct timeline_node *exist;
                exist = rb_entry(rbn, struct timeline_node, key_node);
                // Out of order
                if (unlikely(tl_event->time < exist->time))
                    goto unneeded;
                /*
                 * The same event occurs multiple times, only the last event is backed up.
                 * Previous events will be marked as unneeded and released on the timeline in time.
                **/
                if (env->verbose > VERBOSE_NOTICE)
                    multi_trace_print_title(exist->event, exist->tp, "EEXIST");
                rblist__remove_node(&ctx->backup, rbn);
                *need_free = true;

                /*
                 * tl_event->unneeded is equal to 0, but not added to ctx->backup, tl_event->key_node
                 * is empty, `timeline_free_unneeded' cannot be called immediately.
                **/
                goto retry;
            } else
                ret = 0;
        } else {
        unneeded:
            tl_event->unneeded = 1;
            ctx->tl_stat.unneeded ++;
            ctx->tl_stat.unneeded_bytes += tl_event->event->header.size;
            *need_free = true;
        }
    } else
        // Events at the last level are unneeded.
        *need_free = true;

    return ret;
}

static struct timeline_node *multi_trace_first_pending(struct prof_dev *dev, struct timeline_node *tail)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    struct timeline_node *first;
    u64 deadline = -1UL;

    if (list_empty(&ctx->pending_list))
        return NULL;

    first = list_first_entry(&ctx->pending_list, struct timeline_node, pending);

    if (env->after_event2 && tail)
        deadline = tail->time - env->after_event2;

    if (first->time <= deadline) {
        list_del_init(&first->pending);
        first->unneeded = !first->need_backup;
        ctx->tl_stat.pending --;
        ctx->tl_stat.pending_bytes -= first->event->header.size;
        if (first->unneeded) {
            ctx->tl_stat.unneeded ++;
            ctx->tl_stat.unneeded_bytes += first->event->header.size;
        }
        return first;
    }
    return NULL;
}

static inline void multi_trace_event_lost(struct prof_dev *dev, struct timeline_node *tl_event)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct lost_node *lost, *next;
    struct list_head *head;

    if (ctx->lost_affect == LOST_AFFECT_INS_EVENT)
        head = &ctx->perins_lost_list[tl_event->ins];
    else
        head = &ctx->timeline_lost_list;

    if (list_empty(head))
        return;

    list_for_each_entry_safe(lost, next, head, lost_link) {
        // Events before lost->start_time are processed normally.
        if (tl_event->time < lost->start_time)
            return;

        /*          lost
         * - - - -|= = = =|- - - -
         *        `start_time, the last unlost event.
         * Events at start_time position only _find_prev, not _backup.
         * Subsequent events have been lost, and the backup will be reclaimed immediately.
         */
        if (tl_event->time == lost->start_time) {
            if (tl_event->need_backup) {
                tl_event->need_backup = false;
                tl_event->unneeded = true;
                ctx->tl_stat.unneeded ++;
                ctx->tl_stat.unneeded_bytes += tl_event->event->header.size;
            }
            return;
        }

        /*          lost
         * - - - -|= = = =|- - - -
         *         `Events in the lost range.
         * Within the lost range, all backup events are unsafe. Immediately reclaim events
         * in ctx->backup to avoid blocking the timeline for a long time.
         *
         * two(A, B), A in ctx->backup, B may be lost and another event B may occur.
         * two(A, B) is unsafe. Immediately delete A from ctx->backup to avoid unsafe
         * two(A, B).
         *
         * For example:
         *          lost
         * - - -A-|=B= =A=|-B- - -
         *      '___________' unsafe two(A, B).
         */
        if (!lost->reclaim) {
            u64 recent_time = ctx->recent_time;
            // Ensure that the output of multi_trace_call_remaining() is also correct.
            ctx->recent_time = lost->start_time;
            multi_trace_print_lost(dev, NULL, tl_event->ins);
            // delete A
            lost_reclaim(dev, tl_event->ins);
            ctx->recent_time = recent_time;
            lost->reclaim = true;
        }

        // Within the lost range, new events are also unsafe, neither _find_prev nor _backup.
        if (tl_event->time < lost->end_time) {
            tl_event->need_find_prev = false;
            if (tl_event->need_backup) {
                tl_event->need_backup = false;
                // Events obtained from ctx->pending_list are not counted as `unneeded' only
                // when their `need_backup' is true. See multi_trace_first_pending().
                tl_event->unneeded = true;
                ctx->tl_stat.unneeded ++;
                ctx->tl_stat.unneeded_bytes += tl_event->event->header.size;
            }
            return;
        } else {
            /*          lost
             * - - - -|= = = =|- - - -
             *                `end_time, the first event after lost.
             * Re-process subsequent events normally.
             */
            list_del(&lost->lost_link);
            free(lost);
        }
    }
}

static long multi_trace_ftrace_filter(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct multi_trace_ctx *ctx = dev->private;
    struct multi_trace_type_header *hdr = (void *)event->sample.array;
    struct perf_evsel *evsel = perf_evlist__id_to_evsel(dev->evlist, hdr->id, NULL);
    struct tp *tp = evsel ? perf_evsel_tp(evsel) : NULL;
    int i, j;
    void *raw;
    int size;

    if (tp) {
    found:
        if (!tp->ftrace_filter)
            return 1;
        multi_trace_raw_size(event, &raw, &size, tp);
        return tp_prog_run(tp, tp->ftrace_filter, GLOBAL(hdr->cpu_entry.cpu, hdr->tid_entry.pid, raw, size));
    }

    for (i = 0; i < ctx->nr_list; i++)
        for_each_real_tp(ctx->tp_list[i], tp, j)
            if (tp->evsel == evsel)
                goto found;
    return 0;
}

static void multi_trace_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    struct multi_trace_type_header *hdr = (void *)event->sample.array;
    struct perf_record_dev *event_dev = NULL;
    struct tp *tp = NULL, *tp1 = NULL;
    struct timeline_node current;
    struct perf_evsel *evsel;
    void *raw = NULL;
    int size = 0;
    int i, j;
    bool need_find_prev, need_backup, need_remove_from_backup;
    u64 key;

    if (hdr->time > ctx->recent_time)
        ctx->recent_time = hdr->time;

    if (event->header.type == PERF_RECORD_DEV) {
        event_dev = (void *)event;
        evsel = (void *)prof_dev_top_cloned(event_dev->dev);
    } else {
        evsel = perf_evlist__id_to_evsel(dev->evlist, hdr->id, NULL);
    }
    if (!evsel)
        goto not_found;

    for (i = 0; i < ctx->nr_list; i++) {
        tp1 = NULL;
        for_each_tp(ctx->tp_list[i], tp, j) {
            if (tp->evsel == evsel)
                goto found;
            if (!tp->untraced)
                tp1 = tp;
        }
    }

    if (evsel == ctx->extra_evsel) {
        ctx->extra_sample(dev, event, instance);
    }

not_found:
    return;

found:
    if (unlikely(tp->trigger)) {
        multi_trace_interval(dev);
    }

    tp_broadcast_event(tp, event);
    if (unlikely(env->verbose >= VERBOSE_EVENT || tp->trigger)) {
        multi_trace_print_title(event, tp, NULL);
    }

    if (!event_dev)
        multi_trace_raw_size(event, &raw, &size, tp);

    if (!ctx->nested) {
        bool event_is_sched_wakeup_and_unnecessary = false;
        union {
            unsigned long role;
            struct {
                unsigned long as_event1 : 1,
                              as_event2 : 1;
            };
        } role = {.role = 3};
        if (!event_dev) {
            sched_event(ctx->level, raw, size, hdr->cpu_entry.cpu);
            event_is_sched_wakeup_and_unnecessary = sched_wakeup_unnecessary(ctx->level, raw, size);
            if (event_is_sched_wakeup_and_unnecessary) ctx->sched_wakeup_unnecessary ++;

            if (tp->role_prog)
                role.role = tp_get_role(tp, GLOBAL(hdr->cpu_entry.cpu, hdr->tid_entry.pid, raw, size));
        }
        /*
         * The role ATTR can only change the `need_find_prev' and `need_backup' variables
         * from 1 to 0.
         * For --cyele, role=3 for all events, you can use role ATTR to make some events
         * only as event1 or only as event2.
         */
        need_find_prev = (i != 0 || env->cycle) && role.as_event2;
        need_backup = (i != ctx->nr_list - 1 ? !event_is_sched_wakeup_and_unnecessary : env->cycle) &&
                      (role.as_event1);
        need_remove_from_backup = 1;
        // no need to use tp1
        tp1 = NULL;

        if (env->verbose > VERBOSE_NOTICE &&
            i != ctx->nr_list - 1 && event_is_sched_wakeup_and_unnecessary)
            multi_trace_print_title(event, tp, "UNNECESSARY");
    } else {
        need_find_prev = ctx->impl_based_on_call || tp1 != NULL;
        need_backup = tp1 == NULL;
        need_remove_from_backup = tp1 != NULL;
    }

    // get key, include untraced events.
    // !untraced: tp->key || env->key
    //  untraced: tp->key
    if (tp->key_prog) {
        key = tp_get_key(tp, GLOBAL(hdr->cpu_entry.cpu, hdr->tid_entry.pid, raw, size));
    } else
        key = ctx->oncpu ? prof_dev_ins_cpu(dev, instance) : prof_dev_ins_thread(dev, instance);

    current.time = hdr->time;
    current.key = key;
    current.tp = tp;
    current.tp1 = tp1;
    current.unneeded = (!need_backup) || tp->untraced; // untraced means unneeded
    current.need_find_prev = need_find_prev;
    current.need_backup = need_backup;
    current.need_remove_from_backup = need_remove_from_backup;
    current.ins = instance;
    current.seq = ctx->event_handled++;
    current.event = event;

    // insert events to Timeline, include untraced events.
    if (ctx->need_timeline) {
        bool need_free = current.unneeded;

        if (rblist__empty(&ctx->timeline) && current.unneeded &&
            env->before_event1 == 0 && env->after_event2 == 0)
            goto not_found;
        else {
            int ret = rblist__add_node(&ctx->timeline, &current);
            if (ret != 0) {
                multi_trace_print_title(event, tp, ret == -EEXIST ? "ADD:-EEXIST" : "ADD:-ENOMEM");
                goto not_found;
            }
        }

        while (1) {
            // Only !untraced events are added to the ctx->pending_list, which is sorted
            // chronologically. See timeline_node_new.
            struct timeline_node *first = multi_trace_first_pending(dev, &current);
            if (!first)
                break;

            /*
             * --detail=samecpu,+1ms
             * With this option, there will also be events in the pending_list, which are
             * sampled before lost->start_time. Lost must be checked when processing these
             * events, not when inserting pending_list.
             */
            multi_trace_event_lost(dev, first);

            // Only handles !untraced events.
            multi_trace_tryto_call_two(dev, first, &need_free);
            multi_trace_tryto_backup(dev, first, &need_free);
        }

        if (need_free)
            timeline_free_unneeded(dev);
    } else {
        bool dummy = false;

        if (tp->untraced)
            goto not_found;

        multi_trace_event_lost(dev, &current);

        // Only handles !untraced events.
        multi_trace_tryto_call_two(dev, &current, &dummy);
        if (multi_trace_tryto_backup(dev, &current, &dummy) < 0)
            goto not_found;
    }
}

static void multi_trace_print_dev(struct prof_dev *dev, int indent)
{
    struct multi_trace_ctx *ctx = dev->private;

    two_event_class_print(ctx->class, indent);
    if (ctx->need_timeline) {
        dev_printf("TIMELINE:\n");
        dev_printf("    entries: %u\n", rblist__nr_entries(&ctx->timeline));
        dev_printf("    backup: %u\n", rblist__nr_entries(&ctx->backup));
        dev_printf("    unneeded: %lu\n", ctx->tl_stat.unneeded);
        dev_printf("    pending: %lu\n", ctx->tl_stat.pending);
        dev_printf("    mem_bytes: %lu\n", ctx->tl_stat.mem_bytes);
    } else {
        dev_printf("BACKUP:\n");
        dev_printf("    entries: %u\n", rblist__nr_entries(&ctx->backup));
        dev_printf("    mem_bytes: %lu\n", ctx->backup_stat.mem_bytes);
    }
    if (ctx->sched_wakeup_unnecessary) {
        dev_printf("sched:sched_wakeup unnecessary: %lu\n", ctx->sched_wakeup_unnecessary);
    }
}

static void __help_events(struct help_ctx *hctx, const char *impl, bool *has_key)
{
    int i, j;
    struct env *env = hctx->env;

    if (strcmp(impl, TWO_EVENT_SYSCALLS_IMPL) == 0) {
        printf("-e raw_syscalls:sys_enter/./ -e raw_syscalls:sys_exit/./ ");
        return;
    }

    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        printf("-e \"");
        for_each_real_tp(hctx->tp_list[i], tp, j) {
            printf("%s:%s/%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".");
            if (!env->key || tp->key)
                printf("key=%s/", tp->key?:".");
            if (tp->key)
                *has_key = true;
            if (tp->role)
                printf("role=%s/", tp->role?:".");
            if (strcmp(impl, TWO_EVENT_MEM_PROFILE) == 0)
                printf("ptr=%s/size=%s/", tp->mem_ptr?:".", tp->mem_size?:".");
            if (strcmp(impl, TWO_EVENT_PAIR_IMPL) != 0)
                printf("stack/");
            if (tp->untraced)
                printf("untraced/");
            if (tp->trigger)
                printf("trigger/");
            if (tp->alias)
                printf("alias=%s/", tp->alias);
            if (!tp->role)
                printf("[role=./]");
            if (!tp->untraced)
                printf("[untraced/]");
            if (!tp->trigger)
                printf("[trigger/]");
            if (!tp->alias)
                printf("[alias=./]");
            if (j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
        printf("\" ");
    }
}

static void __multi_trece_help(struct help_ctx *hctx, const char *common, const char *impl, bool impl_default, bool nested)
{
    struct env *env = hctx->env;
    bool has_key = false;
    int min_nr_events = 2;

    if (env->impl && strcmp(env->impl, impl))
        return;
    if (env->cycle && strcmp(impl, TWO_EVENT_DELAY_IMPL))
        return;

    if (nested)
        min_nr_events = 1;
    else if (env->cycle)
        min_nr_events = 1;

    if (strcmp(impl, TWO_EVENT_DELAY_IMPL) == 0)
        min_nr_events = 1;
    else if (strcmp(impl, TWO_EVENT_SYSCALLS_IMPL) == 0)
        min_nr_events = 0;

    if (hctx->nr_list < min_nr_events)
        return;

    printf("%s ", common);
    __help_events(hctx, impl, &has_key);

    if (env->key)
        printf("-k %s --order ", env->key);
    else if (has_key)
        printf("--order ");
    if (!impl_default)
        printf("--impl %s ", impl);
    if (strcmp(impl, TWO_EVENT_DELAY_IMPL) == 0 ||
        strcmp(impl, TWO_EVENT_SYSCALLS_IMPL) == 0 ||
        strcmp(impl, TWO_EVENT_CALL_DELAY_IMPL) == 0) {
        if (env->perins)
            printf("--perins ");
        if (env->greater_than)
            printf("--than %lu ", env->greater_than);
        if (env->detail) {
            if (env->before_event1 || env->samecpu || env->samepid) {
                int len = 0;
                printf("--detail=");
                if (env->before_event1)
                    len += printf("-%lu", env->before_event1);
                if (env->samecpu)
                    len += printf("%ssamecpu", len > 0 ? "," : "");
                if (env->samepid)
                    len += printf("%ssamepid", len > 0 ? "," : "");
                printf(" ");
            } else
                printf("--detail ");
        }
        if (env->heatmap)
            printf("--heatmap %s ", env->heatmap);
        if (!nested)
        if (env->cycle ||
            (strcmp(impl, TWO_EVENT_DELAY_IMPL) == 0 && hctx->nr_list == 1))
            printf("--cycle ");
    }
    common_help(hctx, true, true, true, true, false, true, true);

    if (!env->key && !has_key)
        printf("[-k . --order] ");
    else if (!env->key)
        printf("[-k .] ");
    if (strcmp(impl, TWO_EVENT_DELAY_IMPL) == 0 ||
        strcmp(impl, TWO_EVENT_SYSCALLS_IMPL) == 0 ||
        strcmp(impl, TWO_EVENT_CALL_DELAY_IMPL) == 0) {
        if (!env->perins)
            printf("[--perins] ");
        if (!env->greater_than)
            printf("[--than .] ");
        if (!env->detail)
            printf("[--detail[=-N,+N,samecpu,samepid]] ");
        if (!env->heatmap)
            printf("[--heatmap .] ");
        if (!nested)
        if (!env->cycle &&
            (strcmp(impl, TWO_EVENT_DELAY_IMPL) == 0 && hctx->nr_list > 1))
            printf("[--cycle] ");
    }
    common_help(hctx, false, true, true, true, false, true, true);

    printf("\n");
}

#define NUM(ary) (sizeof(ary)/sizeof(ary[0]))
static void multi_trece_help(struct help_ctx *hctx)
{
    const char *common = PROGRAME " multi-trace";
    const char *impl_str[] = {TWO_EVENT_DELAY_IMPL, TWO_EVENT_PAIR_IMPL, TWO_EVENT_MEM_PROFILE, TWO_EVENT_SYSCALLS_IMPL};
    int impl;

    for (impl = 0; impl < NUM(impl_str); impl++)
        __multi_trece_help(hctx, common, impl_str[impl], false, false);
}

static const char *multi_trace_desc[] = PROFILER_DESC("multi-trace",
    "[OPTION...] -e EVENT [-e ...] [-k key] [--impl impl] [--than|--only-than ns] [--detail] [--perins] [--heatmap file] [--cycle]",
    "Comprehensive multi-event relationship analyzer.",
    "",
    "SYNOPSIS",
    "    Multi-trace converts complex multi-event relationships into pairwise two-event analysis.",
    "    Events are associated through the `key` ATTR. Supports multiple TWO-EVENT implementations.",
    "    Primarily used for latency analysis with --than to filter high-latency events, and --detail",
    "    to examine intermediate events during delays. The `untraced` ATTR allows inserting auxiliary",
    "    events to reconstruct delay details. Can serve as a joint analysis hub, incorporating other",
    "    profilers as auxiliary event sources.",
    "",
    "    Derived profilers: rundelay, syscalls, kmemprof, nested-trace.",
    "",
    "    Applicable scenarios:",
    "      - Process scheduling delay analysis",
    "      - System call latency analysis",
    "      - Memory allocation lifecycle tracking",
    "      - Interrupt/softirq latency analysis",
    "",
    "TWO-EVENT IMPLEMENTATIONS",
    "    delay    - Latency analysis between event pairs",
    "    pair     - Event pairing (alloc/free, open/close, etc.)",
    "    kmemprof - Memory profiling (allocation and free bytes)",
    "    syscalls - System call latency analysis",
    "",
    "EXAMPLES",
    "    "PROGRAME" multi-trace -e sched:sched_switch//role=\"(next_pid?1:0)|(prev_pid?2:0)\"/ --cycle -i 1000",
    "    "PROGRAME" multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000",
    "    "PROGRAME" multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --impl pair",
    "    "PROGRAME" multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --than 100us",
    "    "PROGRAME" multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --than 100us --order --detail=-1ms",
    "    "PROGRAME" multi-trace -e 'sched:sched_wakeup/comm~\"java\"/key=pid/' \\",
    "                          -e 'sched:sched_switch/next_comm~\"java\"/key=next_pid/,sched:sched_migrate_task/comm~\"java\"/untraced/stack/' \\",
    "                          -i 1000 --order --only-than 10ms --detail=samepid",
    "    "PROGRAME" multi-trace -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \\",
    "                          -e 'sched:sched_switch//key=next_pid/,profile/-F 200 --watermark 50 -m 16/untraced/' -k pid -m 128 \\",
    "                          -i 1000 --order --than 20ms --detail=samecpu");
static const char *multi_trace_argv[] = PROFILER_ARGV("multi-trace",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "key", "impl", "than", "only-than", "lower", "detail", "perins", "heatmap", "cycle");
static profiler multi_trace = {
    .name = "multi-trace",
    .desc = multi_trace_desc,
    .argv = multi_trace_argv,
    .pages = 64,
    .help = multi_trece_help,
    .init = multi_trace_init,
    .filter = multi_trace_filter,
    .enabled = multi_trace_enabled,
    .deinit = multi_trace_exit,
    .flush = multi_trace_flush,
    .sigusr = multi_trace_sigusr,
    .print_dev = multi_trace_print_dev,
    .interval = multi_trace_interval,
    .minevtime = multi_trace_minevtime,
    .lost = multi_trace_lost,
    .ftrace_filter = multi_trace_ftrace_filter,
    .sample = multi_trace_sample,
};
PROFILER_REGISTER(multi_trace);


static int kmemprof_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    if (env->impl)
        free(env->impl);
    env->impl = strdup(TWO_EVENT_MEM_PROFILE);
    return multi_trace_init(dev);
}

static void kmemprof_help(struct help_ctx *hctx)
{
    struct env *env = hctx->env;
    const char *common = PROGRAME " kmemprof";
    char *oldimpl = env->impl;
    env->impl = strdup(TWO_EVENT_MEM_PROFILE);
    __multi_trece_help(hctx, common, TWO_EVENT_MEM_PROFILE, true, false);
    free(env->impl);
    env->impl = oldimpl;
}

static const char *kmemprof_desc[] = PROFILER_DESC("kmemprof",
    "[OPTION...] -e alloc -e free [-k key]",
    "Memory allocation profiler for user/kernel allocators.",
    "",
    "SYNOPSIS",
    "    Specialized memory allocation profiler derived from multi-trace. Two -e options specify",
    "    memory allocation and free events respectively. Tracks total allocated/freed bytes and",
    "    object counts, aggregates output by call stack to show top N allocation hotspots.",
    "",
    "    Used for memory allocation hotspot analysis and allocation pattern identification.",
    "    Equivalent to: "PROGRAME" multi-trace --impl kmemprof",
    "",
    "    Applicable scenarios:",
    "      - Kernel memory allocation hotspot analysis (kmalloc, page_alloc)",
    "      - User-space allocator profiling (malloc, mmap)",
    "      - Memory allocation pattern identification",
    "      - High-frequency allocation path optimization",
    "",
    "EVENT ATTRIBUTES",
    "    size=EXPR   - Calculate allocation size (required for alloc events)",
    "    key=EXPR    - Object identifier for alloc/free correlation (e.g., ptr, page)",
    "    stack       - Enable call stack recording for allocation hotspot analysis",
    "",
    "EXAMPLES",
    "    "PROGRAME" kmemprof -e kmem:kmalloc//size=bytes_alloc/stack/ -e kmem:kfree -m 128 --order -k ptr",
    "    "PROGRAME" kmemprof -e kmem:kmalloc//size=bytes_alloc/stack/,kmem:kmalloc_node//size=bytes_alloc/stack/ -e kmem:kfree --order -k ptr",
    "    "PROGRAME" kmemprof -e 'kmem:mm_page_alloc//size=4096<<order/key=page/stack/' -e kmem:mm_page_free//key=page/stack/ -m 256 --order"
    );
static const char *kmemprof_argv[] = PROFILER_ARGV("kmemprof",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "key");
static profiler kmemprof = {
    .name = "kmemprof",
    .desc = kmemprof_desc,
    .argv = kmemprof_argv,
    .pages = 64,
    .help = kmemprof_help,
    .init = kmemprof_init,
    .filter = multi_trace_filter,
    .enabled = multi_trace_enabled,
    .deinit = multi_trace_exit,
    .flush = multi_trace_flush,
    .sigusr = multi_trace_sigusr,
    .print_dev = multi_trace_print_dev,
    .interval = multi_trace_interval,
    .minevtime = multi_trace_minevtime,
    .lost = multi_trace_lost,
    .ftrace_filter = multi_trace_ftrace_filter,
    .sample = multi_trace_sample,
};
PROFILER_REGISTER(kmemprof);


static void syscalls_extra_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct multi_trace_type_raw *raw = (void *)event->sample.array;
    struct sched_process_free *proc_free = (void *)raw->raw.data;

    reclaim(dev, proc_free->pid, REMAINING_SYSCALLS);
}

static int syscalls_init(struct prof_dev *dev)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW,
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
    };
    int sched_process_free;

    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;
    ctx->comm = 1; // for multi_trace_minevtime(), not used in syscalls_print_header().

    if (env->impl)
        free(env->impl);
    env->impl = strdup(TWO_EVENT_SYSCALLS_IMPL);

    if (env->key)
        free(env->key);
    if (prof_dev_ins_oncpu(dev)) {
        env->key = strdup("common_pid");
        env->order = 1;
    } else
        env->key = NULL;

    if (multi_trace_init(dev) < 0)
        return -1;

    prof_dev_env2attr(dev, &attr);

    sched_process_free = tep__event_id("sched", "sched_process_free");
    if (sched_process_free < 0)
        goto failed;

    attr.config = sched_process_free;
    ctx->extra_evsel = perf_evsel__new(&attr);
    if (!ctx->extra_evsel)
        goto failed;
    perf_evlist__add(dev->evlist, ctx->extra_evsel);

    ctx->extra_sample = syscalls_extra_sample;

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void syscalls_help(struct help_ctx *hctx)
{
    struct env *env = hctx->env;
    const char *common = PROGRAME " syscalls";
    char *oldimpl = env->impl;
    env->impl = strdup(TWO_EVENT_SYSCALLS_IMPL);
    __multi_trece_help(hctx, common, TWO_EVENT_SYSCALLS_IMPL, true, false);
    free(env->impl);
    env->impl = oldimpl;
}

static const char *syscalls_desc[] = PROFILER_DESC("syscalls",
    "[OPTION...] -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit [--than ns] [--perins] [--heatmap file]",
    "System call latency analyzer derived from multi-trace.",
    "",
    "SYNOPSIS",
    "    Syscalls is a specialized system call latency analyzer derived from multi-trace.",
    "    It analyzes the complete lifecycle of system calls, from sys_enter to sys_exit.",
    "",
    "    Key features:",
    "      - Automatically uses common_pid as key for event correlation",
    "      - Groups statistics by syscall ID (system call number)",
    "      - Provides latency distribution for each syscall type",
    "      - Tracks error counts (ret < 0) per syscall",
    "",
    "OUTPUT FORMAT",
    "    Statistics are grouped by syscall ID, showing:",
    "      - Syscall name and ID",
    "      - Call count and latency distribution (min/avg/max)",
    "      - Error count (syscalls with ret < 0)",
    "",
    "EXAMPLES",
    "    "PROGRAME" syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit -p 1 --perins",
    "    "PROGRAME" syscalls -e raw_syscalls:sys_enter/id==0/ -e raw_syscalls:sys_exit/id==0/ -C 0 --than 10ms");
static const char *syscalls_argv[] = PROFILER_ARGV("syscalls",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "than", "perins", "heatmap");
static profiler syscalls = {
    .name = "syscalls",
    .desc = syscalls_desc,
    .argv = syscalls_argv,
    .compgen = "-e 'raw_syscalls:sys_enter' -e 'raw_syscalls:sys_exit'",
    .pages = 64,
    .help = syscalls_help,
    .init = syscalls_init,
    .filter = multi_trace_filter,
    .enabled = multi_trace_enabled,
    .deinit = multi_trace_exit,
    .flush = multi_trace_flush,
    .sigusr = multi_trace_sigusr,
    .print_dev = multi_trace_print_dev,
    .interval = multi_trace_interval,
    .minevtime = multi_trace_minevtime,
    .lost = multi_trace_lost,
    .ftrace_filter = multi_trace_ftrace_filter,
    .sample = multi_trace_sample,
};
PROFILER_REGISTER(syscalls);


/* nested-trace
 *
 * perf-prof nested-trace -e A,A_ret -e B,B_ret -e C,C_ret [--impl call|call-delay]
 *
 * A call B, B call C.
 *
 * timeline: A, B, C, C_ret, B_ret, A_ret
 *
 *  pid1 | pid2  | pid3
 * ______|_C_B_A_|______
 *       |
 *       `stack-like. A comes first, B next, and C last.
 *
 * pid2 events:
 *     two(A, NULL),  A comes first.   A is the root node.
 *     two(A, B),     B next.          A call B, B is a descendant of A.
 *     two(B, C),     C last.          B call C, C is a descendant of B.
 *     two(C, C_ret), first remove C.  C return, get the execution time of C.
 *     two(B, B_ret), then remove B.   B return, get the execution time of B.
 *     two(A, A_ret), last remove A.   A return, get the execution time of A.
**/
static int nested_perf_event_backup_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct timeline_node *b = container_of(rbn, struct timeline_node, key_node);
    const struct timeline_node *e = entry;

    if (b->key > e->key)
        return 1;
    else if (b->key < e->key)
        return -1;

    //time reverse order
    if (b->time > e->time)
        return -1;
    else if (b->time < e->time)
        return 1;

    return 0;
}

static int nested_trace_init(struct prof_dev *dev)
{
    int i, k;
    struct multi_trace_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    ctx->nested = 1;
    if (!dev->env->impl)
        dev->env->impl = strdup(TWO_EVENT_CALL_DELAY_IMPL);
    if (__multi_trace_init(dev) < 0)
        return -1;

    ctx->backup.node_cmp = nested_perf_event_backup_node_cmp;

    for (k = 0; k < ctx->nr_list; k++) {
        struct tp *tp1 = NULL;
        struct tp *tp2 = NULL;
        struct tp *tp;

        /*
         * f1 --------> f1_ret
         *    f2 -> f2_ret
         *       ..
         *
         * -e f1,f1_ret -e f2,f2_ret ..
        **/
        if (ctx->tp_list[k]->nr_tp - ctx->tp_list[k]->nr_untraced != 2) {
            fprintf(stderr, "-e ");
            for_each_real_tp(ctx->tp_list[k], tp, i) {
                fprintf(stderr, "%s%s:%s%s", i == 0 ? "" : ",", tp->sys, tp->name, tp->untraced ? "//untraced/" : "");
            }
            fprintf(stderr, " are unpaired\n");
            goto failed;
        }

        for_each_real_tp(ctx->tp_list[k], tp, i) {
            if (tp->untraced)
                continue;
            if (!tp1)
                tp1 = tp;
            else if (!tp2)
                tp2 = tp;
        }

        if (!ctx->impl->object_new(ctx->class, tp1, tp2))
            goto failed;
    }
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void nested_trace_interval(struct prof_dev *dev)
{
    struct multi_trace_ctx *ctx = dev->private;
    int i, k;
    int header = 0;

    for (k = 0; k < ctx->nr_list; k++) {
        struct tp *tp1 = NULL;
        struct tp *tp2 = NULL;
        struct tp *tp;
        struct two_event *two;

        for_each_real_tp(ctx->tp_list[k], tp, i) {
            if (tp->untraced)
                continue;
            if (!tp1)
                tp1 = tp;
            else if (!tp2)
                tp2 = tp;
        }
        two = ctx->impl->object_find(ctx->class, tp1, tp2);
        if (!header) {
            header = ctx->class->print_header(two);
        }
        ctx->class->print(two);
    }
}

static void nested_trace_exit(struct prof_dev *dev)
{
    nested_trace_interval(dev);
    monitor_ctx_exit(dev);
}

static void nested_trace_help(struct help_ctx *hctx)
{
    const char *common = PROGRAME " nested-trace";
    const char *impl_str[] = {TWO_EVENT_DELAY_IMPL, TWO_EVENT_CALL_IMPL, TWO_EVENT_CALL_DELAY_IMPL};
    int impl;

    for (impl = 0; impl < NUM(impl_str); impl++)
        __multi_trece_help(hctx, common, impl_str[impl], false, true);
}

static const char *nested_trace_desc[] = PROFILER_DESC("nested-trace",
    "[OPTION...] -e E,E_ret [-e ...] [-k str] [--impl impl] [--than ns] [--detail] [--perins] [--heatmap file]",
    "Profile nested-event: function calls.", "",
    "SYNOPSIS", "",
    "    Function calls, interrupts, etc. are possible nested events.",
    "    Based on multi-trace. See '"PROGRAME" multi-trace -h' for more information.", "",
    "TWO-EVENT",
    "    delay - analyze function time",
    "    call - analyze function calls",
    "    call-delay - Analyze function calls. Also analyze function time.", "",
    "EXAMPLES",
    "    "PROGRAME" nested-trace -e irq:softirq_entry/vec==1/,irq:softirq_exit/vec==1/ -i 1000",
    "    "PROGRAME" nested-trace -e irq:softirq_entry/vec==1/,irq:softirq_exit/vec==1/ -i 1000 --impl call-delay",
    "    "PROGRAME" nested-trace -e irq:softirq_entry/vec==1/,irq:softirq_exit/vec==1/ -e "
                                   "timer:timer_expire_entry,timer:timer_expire_exit -i 1000 --impl call-delay");
static const char *nested_trace_argv[] = PROFILER_ARGV("nested-trace",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "key", "impl", "than", "detail", "perins", "heatmap");
static profiler nested_trace = {
    .name = "nested-trace",
    .desc = nested_trace_desc,
    .argv = nested_trace_argv,
    .pages = 64,
    .help = nested_trace_help,
    .init = nested_trace_init,
    .filter = multi_trace_filter,
    .enabled = multi_trace_enabled,
    .deinit = nested_trace_exit,
    .flush = multi_trace_flush,
    .sigusr = multi_trace_sigusr,
    .print_dev = multi_trace_print_dev,
    .interval = nested_trace_interval,
    .minevtime = multi_trace_minevtime,
    .lost = multi_trace_lost,
    .ftrace_filter = multi_trace_ftrace_filter,
    .sample = multi_trace_sample,
};
PROFILER_REGISTER(nested_trace);


static int rundelay_init(struct prof_dev *dev)
{
    struct multi_trace_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (!prof_dev_ins_oncpu(dev)) {
        /**
         * sched:sched_switch and sched:sched_wakeup are not suitable for binding to threads
        **/
        ctx->thread_map = dev->threads;
        perf_cpu_map__put(dev->cpus);
        dev->cpus = perf_cpu_map__new(NULL);
        dev->threads = perf_thread_map__new_dummy();
    }
    ctx->comm = 1;

    return multi_trace_init(dev);
}
#define TASK_REPORT_MAX  0x100 // kernel 4.14 and later.
static int rundelay_filter(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct multi_trace_ctx *ctx = dev->private;
    int i, j;
    int sched_wakeup = tep__event_id("sched", "sched_wakeup");
    int sched_wakeup_new = tep__event_id("sched", "sched_wakeup_new");
    int sched_switch = tep__event_id("sched", "sched_switch");
    int match = 0;

    for (i = 0; i < ctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(ctx->tp_list[i], tp, j) {
            if (!tp->untraced &&
                (tp->id == sched_wakeup || tp->id == sched_wakeup_new || tp->id == sched_switch)) {
                struct tp_filter *tp_filter = NULL;
                char buff[4096];
                char *filter = NULL;

                if (tp->id == sched_wakeup || tp->id == sched_wakeup_new) {
                    if (i == 0) {
                        if (tp_set_key(tp, "pid") == 0)
                            match ++;
                        tp_filter = tp_filter_new(ctx->thread_map, "pid", env->filter, "comm");
                    }
                } else if (tp->id == sched_switch) {
                    if (i == 0) {
                        int preempt = kernel_release() >= KERNEL_VERSION(4, 14, 0) ? TASK_REPORT_MAX : 0;

                        if (tp_set_key(tp, "prev_pid") == 0)
                            match ++;
                        tp_filter = tp_filter_new(ctx->thread_map, "prev_pid", env->filter, "prev_comm");
                        if (tp_filter) {
                            snprintf(buff, sizeof(buff), "prev_state==%d && (%s)", preempt, tp_filter->filter);
                            filter = buff;
                        } else {
                            snprintf(buff, sizeof(buff), "prev_state==%d&&prev_pid>0", preempt);
                            tp_update_filter(tp, buff);
                        }
                    }
                    if (i == 1) {
                        if (tp_set_key(tp, "next_pid") == 0)
                            match ++;
                        tp_filter = tp_filter_new(ctx->thread_map, "next_pid", env->filter, "next_comm");
                    }
                }

                if (tp_filter) {
                    if (!filter)
                        filter = tp_filter->filter;
                    tp_update_filter(tp, filter);
                    tp_filter_free(tp_filter);
                }
                if (env->verbose >= VERBOSE_NOTICE)
                    printf("%s:%s filter \"%s\"\n", tp->sys, tp->name, tp->filter ? : "");
            }
        }
    }

    if (match != 4) {
        fprintf(stderr, "rundelay filter failed, found %d matching events.\n", match);
        return -1;
    }
    ctx->level = sched_init(ctx->nr_list, ctx->tp_list);
    return multi_trace_filter(dev);
}

static void rundelay_help(struct help_ctx *hctx)
{
    struct env *env = hctx->env;
    const char *common = PROGRAME " rundelay";
    char *oldimpl = env->impl;

    // Only make the simplest conditions.
    if (hctx->nr_list < 2 ||
        hctx->tp_list[0]->nr_real_tp < 3)
        return ;

    env->impl = strdup(TWO_EVENT_DELAY_IMPL);
    __multi_trece_help(hctx, common, TWO_EVENT_DELAY_IMPL, true, false);
    free(env->impl);
    env->impl = oldimpl;
}

static const char *rundelay_desc[] = PROFILER_DESC("rundelay",
    "[OPTION...] -e sched:sched_wakeup*,sched:sched_switch \\\n"
    "        -e sched:sched_switch [--filter comm] [--than ns] [--detail] [--perins] [--heatmap file]",
    "Scheduling delay analyzer derived from multi-trace.",
    "",
    "SYNOPSIS",
    "    Rundelay is a specialized scheduling delay analyzer derived from multi-trace.",
    "    It automatically configures sched event keys and filters based on --filter and -p options,",
    "    simplifying the setup compared to manual multi-trace configuration.",
    "",
    "    Automatic configuration:",
    "      --filter comm  Automatically sets trace event filters and keys for the specified process comm.",
    "      -p pid         Automatically sets trace event filters and keys for the specified PID.",
    "",
    "    Beyond the automatic setup, rundelay inherits all multi-trace capabilities including:",
    "      - Latency distribution statistics and heatmap generation",
    "      - Detailed intermediate event analysis with --detail",
    "      - `untraced` ATTR support for reconstructing delay details",
    "      - Joint analysis with auxiliary profilers (e.g., profile)",
    "      - See '"PROGRAME" multi-trace -h' for more information.",
    "",
    "AUTOMATIC FILTER GENERATION",
    "    When using --filter or -p, rundelay automatically generates:",
    "      - Trace event filters for sched:sched_wakeup*, sched:sched_switch",
    "      - Key expressions (key=pid, key=next_pid, key=prev_pid) for event correlation",
    "",
    "EXAMPLES",
    "    "PROGRAME" rundelay -e sched:sched_wakeup*,sched:sched_switch \\",
    "                       -e sched:sched_switch -p 1 -i 1000 --than 4ms",
    "    "PROGRAME" rundelay -e sched:sched_wakeup*,sched:sched_switch \\",
    "                       -e sched:sched_switch//stack/ --filter java -i 1000 --than 4ms",
    "    "PROGRAME" rundelay -e sched:sched_wakeup*,sched:sched_switch \\",
    "                       -e sched:sched_switch,sched:sched_migrate_task//untraced/stack/ \\",
    "                       -i 1000 --than 10ms --detail=samecpu",
    "    "PROGRAME" rundelay -e sched:sched_wakeup*,sched:sched_switch \\",
    "                       -e 'sched:sched_switch,profile/-F 500 --watermark 50 -g/untraced/' \\",
    "                       --filter python -i 1000 --than 100ms --detail=samecpu");
static const char *rundelay_argv[] = PROFILER_ARGV("rundelay",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "than", "detail", "perins", "heatmap", "filter\nFilter process comm");
static profiler rundelay = {
    .name = "rundelay",
    .desc = rundelay_desc,
    .argv = rundelay_argv,
    .compgen = "-e 'sched:sched_wakeup*,sched:sched_switch' -e 'sched:sched_switch'",
    .pages = 64,
    .order = 1,
    .help = rundelay_help,
    .init = rundelay_init,
    .filter = rundelay_filter,
    .enabled = multi_trace_enabled,
    .deinit = multi_trace_exit,
    .flush = multi_trace_flush,
    .sigusr = multi_trace_sigusr,
    .print_dev = multi_trace_print_dev,
    .interval = multi_trace_interval,
    .minevtime = multi_trace_minevtime,
    .lost = multi_trace_lost,
    .ftrace_filter = multi_trace_ftrace_filter,
    .sample = multi_trace_sample,
};
PROFILER_REGISTER(rundelay);

