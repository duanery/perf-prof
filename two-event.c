#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/list.h>
#include <linux/zalloc.h>
#include <linux/rblist.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <latency_helpers.h>
#include <two-event.h>

#define TASK_COMM_LEN 16

static int two_event_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct two_event *two = container_of(rbn, struct two_event, rbnode);
    const struct two_event *e = entry;

    if (two->tp1 > e->tp1)
        return 1;
    else if (two->tp1 < e->tp1)
        return -1;
    else {
        // tp2 may be NULL
        if (two->tp2 > e->tp2)
            return 1;
        else if (two->tp2 < e->tp2)
            return -1;

        return 0;
    }
}

static struct rb_node *two_event_node_new(struct rblist *rlist, const void *new_entry)
{
    struct two_event_class *class = container_of(rlist, struct two_event_class, two_events);
    const struct two_event *e = new_entry;
    struct two_event *two = malloc(class->impl->instance_size);
    if (two) {
        memset(two, 0, class->impl->instance_size);
        RB_CLEAR_NODE(&two->rbnode);
        two->class = class;
        two->tp1 = e->tp1;
        two->tp2 = e->tp2;
        two->id = class->ids++;
        rblist__findnew(&class->two_events_byid, two);
        return &two->rbnode;
    } else
        return NULL;
}

static void two_event_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct two_event_class *class = container_of(rblist, struct two_event_class, two_events);
    struct two_event *two = container_of(rb_node, struct two_event, rbnode);
    if (!two->deleting) {
        two->deleting = true;
        /*
         * In the two_event_class_delete function, all two_event objects are deleted.
         * Therefore, the derived object is deleted here.
         */
        class->impl->object_delete(class, two);
    }
    rblist__remove_node(&class->two_events_byid, &two->rbnode_byid);
    free(two);
}

static int two_event_node_cmp_byid(struct rb_node *rbn, const void *entry)
{
    struct two_event *two = container_of(rbn, struct two_event, rbnode_byid);
    const struct two_event *e = entry;

    if (two->id > e->id)
        return 1;
    else if (two->id < e->id)
        return -1;
    else
        return 0;
}

static struct rb_node *two_event_node_new_byid(struct rblist *rlist, const void *new_entry)
{
    struct two_event *two = (struct two_event *)new_entry;
    RB_CLEAR_NODE(&two->rbnode_byid);
    return &two->rbnode_byid;
}

static void two_event_node_delete_byid(struct rblist *rblist, struct rb_node *rb_node)
{
}

static struct two_event *two_event_new(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    struct two_event entry = {
        .tp1 = tp1,
        .tp2 = tp2,
    };
    struct rb_node *rbn = rblist__findnew(&class->two_events, &entry);
    struct two_event *two = NULL;

    if (rbn) {
        two = container_of(rbn, struct two_event, rbnode);
    }
    return two;
}

static void two_event_delete(struct two_event_class *class, struct two_event *two)
{
    if (two && !two->deleting) {
        two->deleting = true;
        rblist__remove_node(&class->two_events, &two->rbnode);
    }
}

static struct two_event *two_event_find(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    struct two_event entry = {
        .tp1 = tp1,
        .tp2 = tp2,
    };
    struct rb_node *rbn = rblist__find(&class->two_events, &entry);
    struct two_event *two = NULL;

    if (rbn) {
        two = container_of(rbn, struct two_event, rbnode);
    }
    return two;
}

static struct two_event *two_event_find_byid(struct two_event_class *class, unsigned int id)
{
    struct two_event entry = {
        .id = id,
    };
    struct rb_node *rbn = NULL;
    struct two_event *two = NULL;

    rbn = rblist__find(&class->two_events_byid, &entry);

    if (rbn) {
        two = container_of(rbn, struct two_event, rbnode_byid);
    }
    return two;
}

static void dummy_two(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter) {}
static remaining_return dummy_remaining(struct two_event *two, union perf_event *event1, struct event_info *info, struct event_iter *iter) {return REMAINING_BREAK;}
static int dummy_print_header(struct two_event *two) {return 0;}
static void dummy_print(struct two_event *two) {}

static struct two_event_class *two_event_class_new(struct two_event_impl *impl, struct two_event_options *options)
{
    struct two_event_class *class = malloc(impl->class_size);

    if (!class)
        return NULL;

    memset(class, 0, impl->class_size);

    class->ids = 0;
    class->impl = impl;
    class->opts = *options;
    if (class->opts.keyname && class->opts.keylen == 0)
        class->opts.keylen = strlen(class->opts.keyname);
    rblist__init(&class->two_events);
    class->two_events.node_cmp = two_event_node_cmp;
    class->two_events.node_new = two_event_node_new;
    class->two_events.node_delete = two_event_node_delete;
    rblist__init(&class->two_events_byid);
    class->two_events_byid.node_cmp = two_event_node_cmp_byid;
    class->two_events_byid.node_new = two_event_node_new_byid;
    class->two_events_byid.node_delete = two_event_node_delete_byid;

    class->two = dummy_two;
    class->remaining = dummy_remaining;
    class->print_header = dummy_print_header;
    class->print = dummy_print;

    return class;
}

static void two_event_class_delete(struct two_event_class *class)
{
    if (class) {
        rblist__exit(&class->two_events);
        free(class);
    }
}

static void impl_init(struct two_event_impl *impl)
{
    /* class */
    if (!impl->class_size)
        impl->class_size = sizeof(struct two_event_class);
    if (!impl->class_new)
        impl->class_new = two_event_class_new;
    if (!impl->class_delete)
        impl->class_delete = two_event_class_delete;

    /* object */
    if (!impl->instance_size)
        impl->instance_size = sizeof(struct two_event);
    if (!impl->object_new)
        impl->object_new = two_event_new;
    if (!impl->object_delete)
        impl->object_delete = two_event_delete;
    if (!impl->object_find)
        impl->object_find = two_event_find;
}


/*
 * Delay between two events
 *
 * Count the maximum, minimum, and average values of each instance.
 * And can output delay heatmap.
**/

struct delay {
    struct two_event base;
    struct heatmap *heatmap;
};

struct delay_class {
    struct two_event_class base;
    int max_len1;
    int max_len2;
    struct latency_dist *lat_dist;
    bool global_comm;
};

static struct two_event *delay_new(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    struct two_event *two = two_event_new(class, tp1, tp2);
    struct delay *delay = NULL;
    struct delay_class *delay_class = NULL;

    if (two) {
        delay = container_of(two, struct delay, base);
        delay_class = container_of(two->class, struct delay_class, base);

        if (!tp2)
            return two;

        if (strlen(tp1->alias ?: tp1->name) > delay_class->max_len1)
            delay_class->max_len1 = strlen(tp1->alias ?: tp1->name);
        if (strlen(tp2->alias ?: tp2->name) > delay_class->max_len2)
            delay_class->max_len2 = strlen(tp2->alias ?: tp2->name);
        if (class->opts.heatmap) {
            char buff[1024];
            snprintf(buff, sizeof(buff), "%s-%s-%s", class->opts.heatmap, tp1->alias ?: tp1->name, tp2->alias ?: tp2->name);
            delay->heatmap = heatmap_open("ns", "ns", buff);
        }
    }
    return two;
}

static void delay_delete(struct two_event_class *class, struct two_event *two)
{
    struct delay *delay = NULL;

    if (two) {
        delay = container_of(two, struct delay, base);
        heatmap_close(delay->heatmap);
        two_event_delete(class, two);
    }
}

static inline void __make_buff(char *buff, int len, const char *debug_msg, s64 time, const char *reason)
{
    *buff++ = '|';
    if (unlikely(debug_msg)) {
        char *end = buff + len - 1;
        *buff++ = ' ';
        while (*debug_msg && buff < end)
            *buff++ = *debug_msg++;
    } else if (reason) {
        snprintf(buff, len - 1, " %12.3f %s", time/1000.0, reason);
        return;
    }
    *buff++ = '\0';
}

#define SCALE 50
static void delay_two(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter)
{
    struct delay *delay = NULL;
    struct delay_class *delay_class = NULL;
    struct two_event_options *opts;
    struct multi_trace_type_header *e1 = (void *)event1->sample.array;
    struct multi_trace_type_header *e2 = (void *)event2->sample.array;
    u64 key = info->key;
    u64 delta = 0;
    const char *unit, *dummy;
    void *raw;
    int size;
    int track_tid;
    char buff[27];

    if (two) {
        delay = container_of(two, struct delay, base);
        delay_class = container_of(two->class, struct delay_class, base);
        opts = &two->class->opts;

        if (e2->time > e1->time) {
            delta = e2->time - e1->time;

            latency_dist_input(delay_class->lat_dist, key, (u64)two->id, delta, opts->greater_than);

            if (delay->heatmap)
                heatmap_write(delay->heatmap, e2->time, delta);

            if ((opts->greater_than && delta > opts->greater_than) ||
                unlikely(opts->lower_than && delta < opts->lower_than)) {
                unit = opts->env->tsc ? "kcyc" : "us";

                if (iter)
                    iter->recent_cpu = e1->cpu_entry.cpu;

                // print events before event1
                if (iter && iter->start && iter->start != iter->event1) {
                    bool printed = false;
                    s64  neg;

                    event_iter_cmd(iter, CMD_RESET);
                    neg = iter->time - e1->time;
                    do {
                        if (event_need_to_print(event1, event2, info, iter)) {
                            if (!printed) printf("-Previous %.3f %s\n", neg/1000.0, unit);
                            printed = true;
                            __make_buff(buff, sizeof(buff), iter->debug_msg, iter->time - e1->time, iter->reason);
                            multi_trace_print_title(iter->event, iter->tp, buff);
                        }
                        if (!event_iter_cmd(iter, CMD_NEXT))
                            break;
                    } while (iter->curr != iter->event1);
                }

                // print event1
                if (iter)
                    event_iter_cmd(iter, CMD_EVENT1);
                multi_trace_print(event1, two->tp1);
                if (iter) {
                    // cpu tracking for event1
                    // opts->comm: rundelay, syscalls. The key is pid.
                    track_tid = opts->comm ? (int)key : e1->tid_entry.tid;
                    if (track_tid > 0) {
                        multi_trace_raw_size(event1, &raw, &size, two->tp1);
                        tp_target_cpu(two->tp1, raw, size, e1->cpu_entry.cpu, track_tid, &iter->recent_cpu, &iter->reason);
                    }
                    // tracking running time.
                    iter->curr_cpu[0] = e1->cpu_entry.cpu;
                    iter->curr_cpu[1] = e2->cpu_entry.cpu;
                    iter->curr_cpu[2] = iter->recent_cpu;
                    iter->curr_time[0] = iter->time;
                    if (!tp_oncpu(two->tp1, raw, size, &iter->curr_pid[0], &dummy))
                        iter->curr_pid[0] = e1->tid_entry.tid;
                    if (iter->curr_cpu[2] == iter->curr_cpu[0]) {
                        iter->curr_pid[2] = iter->curr_pid[0];
                        iter->curr_time[2] = iter->curr_time[0];
                    }
                }

                // print event1 to event2
                if (iter) {
                    bool first = true;
                    int hide = 0;
                    unsigned long time_scale = 0;
                    struct {
                        u64 time;
                        union perf_event *event;
                        struct tp *tp;
                        const char *debug_msg;
                        const char *reason;
                    } prev = {0, NULL, NULL, NULL};
                    snprintf(buff, sizeof(buff), "|+ %.3f %s", delta/1000.0, unit);
                    while (event_iter_cmd(iter, CMD_NEXT)) {
                        if (iter->curr == iter->event2)
                            break;
                        if (event_need_to_print(event1, event2, info, iter)) {
                            if (!first && opts->hide_than) {
                                if (iter->time - prev.time < opts->hide_than) {
                                    hide ++;
                                    goto set_prev;
                                }
                                if (hide) {
                                    if (--hide) printf("| %17d hidden\n", hide);
                                    __make_buff(buff, sizeof(buff), prev.debug_msg, prev.time - e1->time, prev.reason);
                                    multi_trace_print_title(prev.event, prev.tp, buff);
                                }
                            }
                            if (!first) {
                                if (unlikely(iter->debug_msg))
                                    __make_buff(buff, sizeof(buff), iter->debug_msg, 0 , NULL);
                                else if (iter->comm || iter->reason)
                                    __make_buff(buff, sizeof(buff), NULL,
                                                iter->comm ? iter->running_time : iter->time - e1->time,
                                                iter->comm ?                    : iter->reason);
                                else if (time_scale >= SCALE) {
                                    snprintf(buff, sizeof(buff), "|- %.3f %s", (iter->time - e1->time)/1000.0, unit);
                                    time_scale = 0;
                                } else
                                    buff[1] = '\0';
                            }
                            multi_trace_print_title(iter->event, iter->tp, buff);
                            first = false;
                            hide = 0;
                            time_scale ++;
                       set_prev:
                            prev.time = iter->time;
                            prev.event = iter->event;
                            prev.tp = iter->tp;
                            prev.debug_msg = iter->debug_msg;
                            prev.reason = iter->reason;
                        }
                    }
                    if (first)
                        printf("%s\n", buff);
                    if (hide)
                        printf("| %17d hidden\n", hide);
                }

                // print event2
                if (iter)
                    event_iter_cmd(iter, CMD_EVENT2);
                multi_trace_print(event2, two->tp2);
                if (iter) {
                    // cpu tracking for event2
                    track_tid = opts->comm ? (int)key : e1->tid_entry.tid;
                    if (track_tid > 0) {
                        multi_trace_raw_size(event2, &raw, &size, two->tp2);
                        tp_target_cpu(two->tp2, raw, size, e2->cpu_entry.cpu, track_tid, &iter->recent_cpu, &iter->reason);
                    }
                }

                // print events after event2
                if (iter && opts->env->after_event2) {
                    union perf_event *last = NULL;
                    while (event_iter_cmd(iter, CMD_NEXT)) {
                        if (iter->time - e2->time > opts->env->after_event2)
                            break;
                        if (event_need_to_print(event1, event2, info, iter)) {
                            __make_buff(buff, sizeof(buff), iter->debug_msg, iter->time - e1->time, iter->reason);
                            multi_trace_print_title(iter->event, iter->tp, buff);
                            last = iter->event;
                        } else if (last)
                            last = iter->event;
                    }
                    if (last) {
                        struct multi_trace_type_header *e = (void *)last->sample.array;
                        printf("`After %.3f %s\n", (e->time - e2->time)/1000.0, unit);
                    }
                }
            }
        }
    }
}

static remaining_return delay_remaining(struct two_event *two, union perf_event *event1, struct event_info *info, struct event_iter *iter)
{
    struct two_event_options *opts;
    struct multi_trace_type_header *e1 = (void *)event1->sample.array;
    const char *unit;
    u64 delta = 0;
    void *raw;
    int size;
    int track_tid;

    if (info->rr != REMAINING_LOST)
        return REMAINING_BREAK;

    if (two) {
        opts = &two->class->opts;

        if (!opts->greater_than || !iter)
            return REMAINING_BREAK;

        delta = info->recent_time - e1->time;
        if (delta > opts->greater_than) {
            unit = opts->env->tsc ? "kcyc" : "us";

            if (iter)
                iter->recent_cpu = opts->comm ? e1->cpu_entry.cpu : -1;

            // print events before event1
            if (iter && iter->start && iter->start != iter->event1) {
                struct multi_trace_type_header *e;
                bool printed = false;
                s64  neg;

                event_iter_cmd(iter, CMD_RESET);

                e = (void *)iter->event->sample.array;
                neg = e->time - e1->time;
                do {
                    if (event_need_to_print(event1, NULL, info, iter)) {
                        if (!printed) printf("-Previous %.3f %s\n", neg/1000.0, unit);
                        printed = true;
                        multi_trace_print_title(iter->event, iter->tp, "|");
                    }
                    if (!event_iter_cmd(iter, CMD_NEXT))
                        break;
                } while (iter->curr != iter->event1);
            }

            // print event1
            multi_trace_print(event1, two->tp1);
            if (iter) {
                track_tid = opts->comm ? (int)info->key : e1->tid_entry.tid;
                if (track_tid > 0) {
                    multi_trace_raw_size(event1, &raw, &size, two->tp1);
                    tp_target_cpu(two->tp1, raw, size, e1->cpu_entry.cpu, track_tid, &iter->recent_cpu, &iter->reason);
                }
            }

            // print event1 to event2
            if (iter) {
                bool first = true;
                char buff[32];
                unsigned long time_scale = 0;
                snprintf(buff, sizeof(buff), "| >= %.3f %s", delta/1000.0, unit);
                event_iter_cmd(iter, CMD_EVENT1);
                while (event_iter_cmd(iter, CMD_NEXT)) {
                    if (iter->curr == iter->event2)
                        break;
                    if (event_need_to_print(event1, NULL, info, iter)) {
                        if (!first) {
                            if ((time_scale % SCALE) == 0)
                                snprintf(buff, sizeof(buff), "|- %.3f %s", (iter->time - e1->time)/1000.0, unit);
                            else
                                buff[1] = '\0';
                        }
                        multi_trace_print_title(iter->event, iter->tp, buff);
                        first = false;
                        time_scale ++;
                    }
                }
                printf("| >= %12.3f %s, event2 may be lost.\n", delta/1000.0, unit);
            }
        }
    }
    return REMAINING_CONTINUE;
}


static void delay_print_node(void *opaque, struct latency_node *node)
{
    struct delay_class *delay_class = opaque;
    struct two_event_options *opts = &delay_class->base.opts;
    struct two_event *two = two_event_find_byid(&delay_class->base, node->key);
    double p50 = tdigest_quantile(node->td, 0.50);
    double p95 = tdigest_quantile(node->td, 0.95);
    double p99 = tdigest_quantile(node->td, 0.99);
    bool than = !!opts->greater_than;

    if (opts->perins) {
        int ret = 0;
        if (two->tp1->printkey_prog)
            ret = tp_print_key(two->tp1, node->instance);
        else if (two->tp2->printkey_prog)
            ret = tp_print_key(two->tp2, node->instance);
        if (ret > 0) {
            if (opts->keylen+1 > ret)
                printf("%-*s ", opts->keylen - ret, "");
        } else printf("%-*lu ", opts->keylen, node->instance);

        // if (comm) node->instance means pid.
        if (opts->comm && delay_class->global_comm)
            printf("%-*s ", TASK_COMM_LEN, tep__pid_to_comm((int)node->instance));
    }
    printf("%*s", delay_class->max_len1, two->tp1->alias ?: two->tp1->name);
    printf(" => %-*s", delay_class->max_len2, two->tp2->alias ?: two->tp2->name);
    printf(" %8lu %16.3f %12.3f %12.3f %12.3f %12.3f %12.3f",
        node->n, node->sum/1000.0, node->min/1000.0, p50/1000.0, p95/1000.0, p99/1000.0, node->max/1000.0);
    if (than)
        if (node->than && isatty(1))
            printf(" \033[31;1m%6lu (%3lu%s)\033[0m\n", node->than, node->than * 100 / (node->n ? : 1), "%");
        else
            printf(" %6lu (%3lu%s)\n", node->than, node->than * 100 / (node->n ? : 1), "%");
    else
        printf("\n");
}

static int delay_print_header(struct two_event *two)
{
    struct delay_class *delay_class = NULL;
    struct two_event_options *opts;
    bool than;
    int i;

    if (two) {
        delay_class = container_of(two->class, struct delay_class, base);
        opts = &two->class->opts;
        than = !!opts->greater_than;

        if (latency_dist_empty(delay_class->lat_dist))
            return 1;
        if (opts->only_print_greater_than &&
            !latency_dist_greater_than(delay_class->lat_dist, opts->greater_than))
            return 1;

        print_time(stdout);
        printf("\n");

        if (opts->perins) {
            printf("%-*s ", opts->keylen, opts->keyname);
            if (opts->comm && delay_class->global_comm)
                printf("%-*s ", TASK_COMM_LEN, "comm");
        }

        printf("%*s => %-*s", delay_class->max_len1, "start", delay_class->max_len2, "end");
        if (!opts->env->tsc)
            printf(" %8s %16s %12s %12s %12s %12s %12s", "calls", "total(us)", "min(us)", "p50(us)",
                    "p95(us)", "p99(us)", "max(us)");
        else
            printf(" %8s %16s %12s %12s %12s %12s %12s", "calls", "total(kcyc)", "min(kcyc)", "p50(kcyc)",
                    "p95(kcyc)", "p99(kcyc)", "max(kcyc)");

        if (than)
            printf("    than(reqs)\n");
        else
            printf("\n");

        if (opts->perins) {
            for (i=0; i<opts->keylen; i++) printf("-");
            printf(" ");
            if (opts->comm && delay_class->global_comm) {
                for (i=0; i<TASK_COMM_LEN; i++) printf("-");
                printf(" ");
            }
        }
        for (i=0; i<delay_class->max_len1; i++) printf("-");
        printf("    ");
        for (i=0; i<delay_class->max_len2; i++) printf("-");
        printf(" %8s %16s %12s %12s %12s %12s %12s",
                        "--------", "----------------", "------------", "------------", "------------",
                        "------------", "------------");
        if (than)
            printf(" --------------\n");
        else
            printf("\n");

        if (!opts->sort_print)
            latency_dist_print(delay_class->lat_dist, delay_print_node, delay_class);
        else
            latency_dist_print_sorted(delay_class->lat_dist, delay_print_node, delay_class);
        return 1;
    }
    return 0;
}

static void delay_print(struct two_event *two)
{
}

static struct two_event_class *delay_class_new(struct two_event_impl *impl, struct two_event_options *options)
{
    struct two_event_class *class = two_event_class_new(impl, options);
    struct delay_class *delay_class;

    if (class) {
        class->two = delay_two;
        class->remaining = delay_remaining;
        class->print_header = delay_print_header;
        class->print = delay_print;

        if (class->opts.keylen < 3)
            class->opts.keylen = 3;

        delay_class = container_of(class, struct delay_class, base);
        delay_class->max_len1 = 5; // 5 is strlen("start")
        delay_class->max_len2 = 3; // 5 is strlen("end")
        delay_class->lat_dist = latency_dist_new_quantile(options->perins, true, 0);
        delay_class->global_comm = global_comm_ref() == 0;
    }
    return class;
}

static void delay_class_delete(struct two_event_class *class)
{
    struct delay_class *delay_class;
    if (class) {
        delay_class = container_of(class, struct delay_class, base);
        latency_dist_free(delay_class->lat_dist);
        if (delay_class->global_comm) global_comm_unref();
        two_event_class_delete(class);
    }
}

static struct two_event_impl delay_impl = {
    .name = TWO_EVENT_DELAY_IMPL,
    .class_size = sizeof(struct delay_class),
    .class_new = delay_class_new,
    .class_delete = delay_class_delete,

    .instance_size = sizeof(struct delay),
    .object_new = delay_new,
    .object_delete = delay_delete,
};


/*
 * syscall delay
 *
 * Count the maximum, minimum, and average values of each instance.
 * Number of syscall errors
 * And can output delay heatmap.
 *
**/

struct sys_enter {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    unsigned char common_flags;//       offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//       offset:3;       size:1; signed:0;
    int common_pid;//   offset:4;       size:4; signed:1;

    long id;//  offset:8;       size:8; signed:1;
    unsigned long args[6];//    offset:16;      size:48;        signed:0;
};

struct sys_exit {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    unsigned char common_flags;//       offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//       offset:3;       size:1; signed:0;
    int common_pid;//   offset:4;       size:4; signed:1;

    long id;//  offset:8;       size:8; signed:1;
    long ret;// offset:16;      size:8; signed:1;
};


#undef __SYSCALL
#undef __SYSCALL_WITH_COMPAT
#define __SYSCALL(nr, sym) [nr] = #sym,
#define __SYSCALL_WITH_COMPAT(nr, sym, compat) [nr] = #sym,
const char *syscalls_table[] = {
#if defined(__i386__)
#include <asm/syscalls_32.h>
#elif defined(__x86_64__)
#include <asm/syscalls_64.h>
#else
#include <asm-generic/unistd.h>
#endif
};
const int syscalls_table_size = sizeof(syscalls_table)/sizeof(syscalls_table[0]);

static struct two_event *syscalls_new(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    if (strcmp(tp1->sys, "raw_syscalls") ||
        strcmp(tp1->name, "sys_enter") ||
        (tp2 && strcmp(tp2->sys, "raw_syscalls")) ||
        (tp2 && strcmp(tp2->name, "sys_exit"))) {
        fprintf(stderr, "Please use -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit\n");
        return NULL;
    }
    return delay_new(class, tp1, tp2);
}

static void syscalls_two(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter)
{
    struct delay *delay = NULL;
    struct delay_class *delay_class = NULL;
    struct two_event_options *opts;
    struct multi_trace_type_header *e1 = (void *)event1->sample.array;
    struct multi_trace_type_header *e2 = (void *)event2->sample.array;
    struct sys_enter *sys_enter;
    int enter_size;
    struct sys_exit *sys_exit;
    int exit_size;
    u64 delta = 0;
    struct latency_node *node;

    if (two) {
        delay = container_of(two, struct delay, base);
        delay_class = container_of(two->class, struct delay_class, base);
        opts = &two->class->opts;

        if (e2->time > e1->time) {
            delta = e2->time - e1->time;

            multi_trace_raw_size(event1, (void **)&sys_enter, &enter_size, two->tp1);
            multi_trace_raw_size(event2, (void **)&sys_exit, &exit_size, two->tp2);

            if (sys_enter->common_pid != sys_exit->common_pid ||
                sys_enter->id != sys_exit->id)
                return ;

            node = latency_dist_input(delay_class->lat_dist, sys_enter->common_pid, sys_enter->id, delta, opts->greater_than);
            node->extra[0] += IS_ERR_VALUE((unsigned long)sys_exit->ret); //error

            if (delay->heatmap)
                heatmap_write(delay->heatmap, e2->time, delta);

            if (opts->greater_than && delta > opts->greater_than) {
                multi_trace_print(event1, two->tp1);
                multi_trace_print(event2, two->tp2);
            }
        }
    }
}

static remaining_return syscalls_remaining(struct two_event *two, union perf_event *event1, struct event_info *info, struct event_iter *iter)
{
    struct delay *delay = NULL;
    struct delay_class *delay_class = NULL;
    struct two_event_options *opts;
    struct multi_trace_type_header *e1 = (void *)event1->sample.array;
    struct sys_enter *sys_enter;
    int enter_size;
    u64 delta = 0;

    if (info->rr != REMAINING_SYSCALLS)
        return REMAINING_BREAK;

    if (two) {
        delay = container_of(two, struct delay, base);
        delay_class = container_of(two->class, struct delay_class, base);
        opts = &two->class->opts;

        // info->recent_time is free_time, see syscalls_extra_sample().
        if (info->recent_time > e1->time) {
            delta = info->recent_time - e1->time;

            multi_trace_raw_size(event1, (void **)&sys_enter, &enter_size, two->tp1);

            /* node = */latency_dist_input(delay_class->lat_dist, sys_enter->common_pid, sys_enter->id, delta, opts->greater_than);
            // node->extra[0] += 0; //error

            if (delay->heatmap)
                heatmap_write(delay->heatmap, info->recent_time, delta);

            if (opts->greater_than && delta > opts->greater_than) {
                multi_trace_print(event1, two->tp1);
                prof_dev_print_time(two->tp1->dev, info->recent_time, stdout);
                tp_print_marker(two->tp1);
                printf("%16s %6u .... [%03d] %lu.%06lu: sched:sched_process_free: comm=%s pid=%d\n", tep__pid_to_comm(0), 0,
                        e1->cpu_entry.cpu, info->recent_time/NSEC_PER_SEC, (info->recent_time%NSEC_PER_SEC)/1000,
                        tep__pid_to_comm(e1->tid_entry.tid), e1->tid_entry.tid);
            }
        }
    }
    return REMAINING_CONTINUE;
}

static void syscalls_print_node(void *opaque, struct latency_node *node)
{
    struct delay_class *delay_class = opaque;
    struct two_event_options *opts = &delay_class->base.opts;
    bool than = !!opts->greater_than;
    char buf[64];

    if (opts->perins) {
        printf("%-6lu ", node->instance);
        /*
         * syscalls_two(): latency_dist_input(,sys_enter->common_pid,)
         * The default instance is pid, there is no need to judge opts->comm.
        **/
        if (delay_class->global_comm)
            printf("%-*s ", TASK_COMM_LEN, tep__pid_to_comm((int)node->instance));
    }
    if (node->key < sizeof(syscalls_table)/sizeof(syscalls_table[0])
        && syscalls_table[node->key]) {
        snprintf(buf, sizeof(buf), "%s(%lu)", syscalls_table[node->key], node->key);
        printf("%-20s", buf);
    } else
        printf("%-20lu", node->key);
    printf(" %8lu %16.3f %12.3f %12.3f %12.3f %6lu",
        node->n, node->sum/1000.0, node->min/1000.0, node->sum/node->n/1000.0, node->max/1000.0, node->extra[0]);
    if (than)
        if (node->than && isatty(1))
            printf(" \033[31;1m%6lu (%3lu%s)\033[0m\n", node->than, node->than * 100 / (node->n ? : 1), "%");
        else
            printf(" %6lu (%3lu%s)\n", node->than, node->than * 100 / (node->n ? : 1), "%");
    else
        printf("\n");
}

static int syscalls_print_header(struct two_event *two)
{
    struct delay_class *delay_class = NULL;
    struct two_event_options *opts;
    bool than;
    int i;

    if (two) {
        delay_class = container_of(two->class, struct delay_class, base);
        opts = &two->class->opts;
        than = !!opts->greater_than;

        if (latency_dist_empty(delay_class->lat_dist))
            return 1;

        print_time(stdout);
        printf("\n");

        if (opts->perins) {
            printf("thread ");
            if (delay_class->global_comm)
                printf("%-*s ", TASK_COMM_LEN, "comm");
        }

        printf("%-20s", "syscalls");
        if (!opts->env->tsc)
            printf(" %8s %16s %12s %12s %12s %6s", "calls", "total(us)", "min(us)", "avg(us)", "max(us)", "err");
        else
            printf(" %8s %16s %12s %12s %12s %6s", "calls", "total(kcyc)", "min(kcyc)", "avg(kcyc)", "max(kcyc)", "err");

        if (than)
            printf("   than(reqs)\n");
        else
            printf("\n");

        if (opts->perins) {
            printf("------ ");
            if (delay_class->global_comm) {
                for (i=0; i<TASK_COMM_LEN; i++) printf("-");
                printf(" ");
            }
        }
        for (i=0; i<20; i++) printf("-");
        printf(" %8s %16s %12s %12s %12s %6s",
                        "--------", "----------------", "------------", "------------", "------------", "------");
        if (than)
            printf(" --------------\n");
        else
            printf("\n");

        if (!opts->sort_print)
            latency_dist_print(delay_class->lat_dist, syscalls_print_node, delay_class);
        else
            latency_dist_print_sorted(delay_class->lat_dist, syscalls_print_node, delay_class);
        return 1;
    }
    return 0;
}


static struct two_event_class *syscalls_class_new(struct two_event_impl *impl, struct two_event_options *options)
{
    struct two_event_class *class = two_event_class_new(impl, options);
    struct delay_class *delay_class;

    if (class) {
        class->two = syscalls_two;
        class->remaining = syscalls_remaining;
        class->print_header = syscalls_print_header;
        class->print = delay_print;

        delay_class = container_of(class, struct delay_class, base);
        delay_class->lat_dist = latency_dist_new(options->perins, true, sizeof(u64));
        delay_class->global_comm = global_comm_ref() == 0;
    }
    return class;
}


static struct two_event_impl syscalls_impl = {
    .name = TWO_EVENT_SYSCALLS_IMPL,
    .class_size = sizeof(struct delay_class),
    .class_new = syscalls_class_new,
    .class_delete = delay_class_delete,

    .instance_size = sizeof(struct delay),
    .object_new = syscalls_new,
    .object_delete = delay_delete,
};


/*
 * Determine if two events are paired
 *
 * Print unpaired events.
 * Report the number of paired and unpaired events.
**/

struct pair {
    struct two_event base;
    union {
        u64 paired;
        u64 unpaired;
    };
};

struct pair_class {
    struct two_event_class base;
};

static void pair_two(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter)
{
    struct pair *pair;

    if (two) {
        pair = container_of(two, struct pair, base);
        pair->paired ++;
    }
}

static remaining_return pair_remaining(struct two_event *two, union perf_event *event1, struct event_info *info, struct event_iter *iter)
{
    struct pair *pair;

    if (two) {
        pair = container_of(two, struct pair, base);
        pair->unpaired ++;
        multi_trace_print(event1, two->tp1);
    }
    return REMAINING_CONTINUE;
}

static int pair_print_header(struct two_event *two)
{
    return 1;
}

static void pair_print(struct two_event *two)
{
    struct pair *pair;

    if (two) {
        pair = container_of(two, struct pair, base);
        print_time(stdout);
        if (two->tp2)
            printf("%s:%s %s:%s paired %lu\n", two->tp1->sys, two->tp1->name, two->tp2->sys, two->tp2->name,
                pair->paired);
        else
             printf("%s:%s unpaired %lu\n", two->tp1->sys, two->tp1->name, pair->unpaired);
        pair->paired = pair->unpaired = 0;
    }
}

static struct two_event_class *pair_class_new(struct two_event_impl *impl, struct two_event_options *options)
{
    struct two_event_class *class = two_event_class_new(impl, options);

    if (class) {
        class->two = pair_two;
        class->remaining = pair_remaining;
        class->print_header = pair_print_header;
        class->print = pair_print;
    }
    return class;
}

static struct two_event_impl pair_impl = {
    .name = TWO_EVENT_PAIR_IMPL,
    .class_size = sizeof(struct pair_class),
    .class_new = pair_class_new,

    .instance_size = sizeof(struct pair),
};




/*
 * Profile memory allocated and freed bytes.
 *
**/

struct mem_profile {
    struct two_event base;
    struct key_value_paires *alloc;
    struct key_value_paires *free;
    unsigned int nr_alloc;
    u64 alloc_bytes;
    unsigned int nr_free;
    u64 free_bytes;
};

struct mem_profile_class {
    struct two_event_class base;
    struct callchain_ctx *cc;
};

struct callchain_value {
    u64 bytes;
    int pid;
};

static struct two_event *mem_profile_new(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    struct two_event *two = NULL;
    struct mem_profile *profile = NULL;

    if (!tp1->mem_size_prog) {
        fprintf(stderr, "%s:%s//size=?/ size attribute is not set\n", tp1->sys, tp1->name);
        return NULL;
    }
    if (!tp1->stack) {
        fprintf(stderr, "WARN: %s:%s//stack/ without stack attribute, memory allocations "
                        "cannot be profiled based on the stack.\n", tp1->sys, tp1->name);
    }
    if (tp2 && !tp2->stack) {
        fprintf(stderr, "WARN: %s:%s//stack/ without stack attribute, memory deallocation "
                        "cannot be profiled based on the stack.\n", tp2->sys, tp2->name);
    }

    two = two_event_new(class, tp1, tp2);
    if (two) {
        profile = container_of(two, struct mem_profile, base);
        profile->alloc = keyvalue_pairs_new(sizeof(struct callchain_value));
        profile->free = keyvalue_pairs_new(sizeof(struct callchain_value));
    }
    return two;
}

static void mem_profile_delete(struct two_event_class *class, struct two_event *two)
{
    struct mem_profile *profile = NULL;

    if (two) {
        profile = container_of(two, struct mem_profile, base);
        keyvalue_pairs_free(profile->alloc);
        keyvalue_pairs_free(profile->free);
        two_event_delete(class, two);
    }
}

static void mem_profile_two(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter)
{
    struct mem_profile *profile = NULL;
    struct multi_trace_type_callchain *data;
    struct callchain_data cd;

    if (!two)
        return ;

    if (two) {
        unsigned long long bytes_alloc = 0;
        struct callchain_value *val;
        void *raw;
        int size;

        profile = container_of(two, struct mem_profile, base);

        multi_trace_raw_size(event1, &raw, &size, two->tp1);

        data = (void *)event1->sample.array;
        bytes_alloc = tp_get_mem_size(two->tp1, GLOBAL(data->h.cpu_entry.cpu, data->h.tid_entry.pid, raw, size));

        profile->nr_alloc ++;
        profile->alloc_bytes += bytes_alloc;
        if (two->tp1->stack) {
            data = (void *)event1->sample.array;
            if (two->tp1->dwarf_unwind) {
                perf_event_build_callchain_data(two->tp1->evsel, event1, &cd);
                val = keyvalue_pairs_add_callchain(profile->alloc, &cd);
            } else {
                val = keyvalue_pairs_add_key(profile->alloc, (struct_key *)&data->callchain);
            }
            val->bytes += bytes_alloc;
            if (val->pid == 0)
                val->pid = data->h.tid_entry.pid;
        }

        if (event2) {
            profile->nr_free ++;
            profile->free_bytes += bytes_alloc;
            if (two->tp2->stack) {
                data = (void *)event2->sample.array;
                if (two->tp2->dwarf_unwind) {
                    perf_event_build_callchain_data(two->tp2->evsel, event2, &cd);
                    val = keyvalue_pairs_add_callchain(profile->free, &cd);
                } else {
                    val = keyvalue_pairs_add_key(profile->free, (struct_key *)&data->callchain);
                }
                val->bytes += bytes_alloc;
                if (val->pid == 0)
                    val->pid = data->h.tid_entry.pid;
            }
        }
    }
}

static remaining_return mem_profile_remaining(struct two_event *two, union perf_event *event1, struct event_info *info, struct event_iter *iter)
{
    mem_profile_two(two, event1, NULL, NULL, NULL);
    return REMAINING_CONTINUE;
}

static int mem_profile_print_header(struct two_event *two)
{
    return 1;
}

static int __cmp(void **value1, void **value2)
{
    u64 *b1 = *(u64 **)value1;
    u64 *b2 = *(u64 **)value2;

    if (*b1 < *b2)
        return 1;
    else if (*b1 > *b2)
        return -1;
    else
        return 0;
}

static void __print_alloc(void *opaque, struct_key *key, void *value, unsigned int n)
{
    struct mem_profile *profile = opaque;
    struct mem_profile_class *mpclass = container_of(profile->base.class, struct mem_profile_class, base);
    struct callchain_value *val = value;
    printf("Allocate %lu (%.1f%%) bytes on %u (%.1f%%) objects:\n", val->bytes, val->bytes * 100.0 / profile->alloc_bytes,
                                                                     n, n * 100.0 / profile->nr_alloc);
    print_callchain_common(mpclass->cc, key, val->pid);
}

static void __print_free(void *opaque, struct_key *key, void *value, unsigned int n)
{
    struct mem_profile *profile = opaque;
    struct mem_profile_class *mpclass = container_of(profile->base.class, struct mem_profile_class, base);
    struct callchain_value *val = value;
    printf("Free %lu (%.1f%%) bytes on %u (%.1f%%) objects:\n", val->bytes, val->bytes * 100.0 / profile->free_bytes,
                                                                 n, n * 100.0 / profile->nr_free);
    print_callchain_common(mpclass->cc, key, val->pid);
}

static void mem_profile_print(struct two_event *two)
{
    struct mem_profile *profile = NULL;
    unsigned int nr_entries;

    if (two) {
        profile = container_of(two, struct mem_profile, base);

        if (!two->tp2) {
            if (profile->nr_alloc) {
                print_time(stdout);
                printf("\n%s:%s total alloc %lu bytes on %u objects but not freed\n", two->tp1->sys, two->tp1->name, profile->alloc_bytes, profile->nr_alloc);
                goto print_alloc;
            } else
                return ;
        }

        print_time(stdout);
        printf("\n%s:%s => %s:%s\n", two->tp1->sys, two->tp1->name, two->tp2->sys, two->tp2->name);
        printf("%s:%s total alloc %lu bytes on %u objects\n", two->tp1->sys, two->tp1->name, profile->alloc_bytes, profile->nr_alloc);
print_alloc:
        keyvalue_pairs_sorted_firstn(profile->alloc, __cmp, __print_alloc, profile, two->class->opts.first_n);
        nr_entries = keyvalue_pairs_nr_entries(profile->alloc);
        if (nr_entries > two->class->opts.first_n)
            printf("Skipping alloc numbered %u..%u\n", two->class->opts.first_n+1, nr_entries);

        if (!two->tp2)
            goto reset;

        printf("%s:%s total free %lu bytes on %u objects\n", two->tp2->sys, two->tp2->name, profile->free_bytes, profile->nr_free);
        keyvalue_pairs_sorted_firstn(profile->free, __cmp, __print_free, profile, two->class->opts.first_n);
        nr_entries = keyvalue_pairs_nr_entries(profile->free);
        if (nr_entries > two->class->opts.first_n)
            printf("Skipping free numbered %u..%u\n", two->class->opts.first_n+1, nr_entries);

reset:
        printf("\n");

        //reset
        profile->nr_alloc = 0;
        profile->nr_free = 0;
        profile->alloc_bytes = 0;
        profile->free_bytes = 0;
        keyvalue_pairs_reinit(profile->alloc);
        keyvalue_pairs_reinit(profile->free);
    }
}

static struct two_event_class *mem_profile_class_new(struct two_event_impl *impl, struct two_event_options *options)
{
    struct two_event_class *class = two_event_class_new(impl, options);
    struct mem_profile_class *mpclass;

    if (class) {
        mpclass = container_of(class, struct mem_profile_class, base);
        class->two = mem_profile_two;
        class->remaining = mem_profile_remaining;
        class->print_header = mem_profile_print_header;
        class->print = mem_profile_print;
        mpclass->cc = callchain_ctx_new(CALLCHAIN_KERNEL | CALLCHAIN_USER, stdout);
    }
    return class;
}

static void mem_profile_class_delete(struct two_event_class *class)
{
    struct mem_profile_class *mpclass;
    if (class) {
        mpclass = container_of(class, struct mem_profile_class, base);
        callchain_ctx_free(mpclass->cc);
        two_event_class_delete(class);
    }
}


static struct two_event_impl mem_profile_impl = {
    .name = TWO_EVENT_MEM_PROFILE,
    .class_size = sizeof(struct mem_profile_class),
    .class_new = mem_profile_class_new,
    .class_delete = mem_profile_class_delete,

    .instance_size = sizeof(struct mem_profile),
    .object_new = mem_profile_new,
    .object_delete = mem_profile_delete,
};



/*
 * Analyze function calls.
 *
 * two(A, B), in function A, call function B.
 * A() {
 *     B()
 * }
 *
 * Print function calls.
 *
 * sys_perf_event_open
 *   |-perf_event_alloc
 *   |   |-perf_init_event
 *   |   |   |-perf_try_init_event
 *   |-perf_install_in_context
 *
**/

struct caller {
    struct two_event base;
    struct tp *tp2;
    int depth;
    bool recursive;
    struct caller *parent;
    struct list_head callee_head;
    struct list_head caller_link;
    struct list_head class_link;
};

struct caller_iterator {
    void (*begin)(struct caller_iterator *iter);
    void (*iterator)(struct caller_iterator *iter, struct two_event *two);
    void (*end)(struct caller_iterator *iter);
};

struct call_class {
    struct two_event_class base;
    struct list_head caller_head;
    u64 calls;
    int max_depth;
    struct caller_iterator iter_print;
};

static void call_iterate(struct two_event *two, struct caller_iterator *iter)
{
    struct caller *caller = container_of(two, struct caller, base);
    struct caller *callee;

    iter->iterator(iter, two);

    list_for_each_entry(callee, &caller->callee_head, caller_link) {
        call_iterate(&callee->base, iter);
    }
}

static void call_class_iterate(struct two_event *two, struct caller_iterator *iter)
{
    struct call_class *call_class = container_of(two->class, struct call_class, base);
    struct caller *caller;

    if (iter->begin)
        iter->begin(iter);

    list_for_each_entry(caller, &call_class->caller_head, class_link) {
        call_iterate(&caller->base, iter);
    }

    if (iter->end)
        iter->end(iter);
}

static inline void call_class_print(struct two_event *two)
{
    struct call_class *call_class = container_of(two->class, struct call_class, base);

    if (call_class->calls) {
        call_class_iterate(two, &call_class->iter_print);
        call_class->calls = 0;
    }
}

static struct two_event *call_new(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    struct two_event *two = two_event_new(class, tp1, NULL);
    struct caller *caller = NULL;

    if (tp2) {
        // tp2 is not used.
    }

    if (two) {
        caller = container_of(two, struct caller, base);
        caller->tp2 = tp2;
        caller->parent = NULL;
        INIT_LIST_HEAD(&caller->callee_head);
        INIT_LIST_HEAD(&caller->caller_link);
        INIT_LIST_HEAD(&caller->class_link);
    }
    return two;
}

static void call_delete(struct two_event_class *class, struct two_event *two)
{
    struct caller *caller = NULL, *callee, *next;

    if (two) {
        caller = container_of(two, struct caller, base);
        list_for_each_entry_safe(callee, next, &caller->callee_head, caller_link) {
            list_del_init(&callee->caller_link);
        }
        list_del_init(&caller->caller_link);
        list_del_init(&caller->class_link);
        two_event_delete(class, two);
    }
}

static struct two_event *call_find(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    // tp2 is not used.
    return two_event_find(class, tp1, NULL);
}

static void call_update_depth(struct two_event *two)
{
    struct call_class *call_class = container_of(two->class, struct call_class, base);
    struct caller *caller = container_of(two, struct caller, base);
    struct caller *callee;

    if (caller->parent == NULL)
        caller->depth = 0;
    else
        caller->depth = caller->parent->depth + 1;

    if (caller->depth > call_class->max_depth)
        call_class->max_depth = caller->depth;

    list_for_each_entry(callee, &caller->callee_head, caller_link) {
        call_update_depth(&callee->base);
    }
}

static inline bool call_recursive(struct caller *caller, struct caller *callee)
{
    // two(A, A), recursive call.
    // two(A, B) .. two(B, A), recursive call.
    while (caller) {
        if (caller == callee)
            return true;
        caller = caller->parent;
    }
    return false;
}

static bool call_link(struct two_event *two, struct tp *tp2)
{
    struct caller *caller;
    struct caller *callee;
    struct call_class *call_class;

    if (two) {
        caller = container_of(two, struct caller, base);
        call_class = container_of(two->class, struct call_class, base);

        if (!tp2) {
            /*
             * two(A, NULL), first call A.
             * A is the root node, remove 'caller_link', add to 'caller_head'.
            **/
            if (!list_empty(&caller->caller_link)) {
                call_class_print(two);
                list_del_init(&caller->caller_link);
                caller->parent = NULL;
                call_update_depth(two);
            }
            if (list_empty(&caller->class_link))
                list_add_tail(&caller->class_link, &call_class->caller_head);
            call_class->calls ++;
            return true;
        }
        /*
         * two(A, B), in function A, call function B.
         * Use tp2(B) to find the callee.
        **/
        two = two_event_find(two->class, tp2, NULL);
        if (two) {
            callee = container_of(two, struct caller, base);

            caller->recursive = call_recursive(caller, callee);
            /*
             * callee->parent == NULL: first call.
             * callee->parent != NULL: different parents call the same function.
            **/
            if (callee->parent != caller && !caller->recursive) {
                if (callee->parent != NULL || !list_empty(&callee->class_link))
                    call_class_print(two);
                list_move_tail(&callee->caller_link, &caller->callee_head);
                list_del_init(&callee->class_link);
                callee->parent = caller;
                call_update_depth(two);
            }
            call_class->calls ++;
            return true;
        }
    }
    return false;
}

static void call_two(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter)
{
    // iter is not used.
    call_link(two, info->tp2);
}

static void call_iterator_print(struct caller_iterator *iter, struct two_event *two)
{
    struct caller *caller = container_of(two, struct caller, base);

    if (caller->depth > 0) {
        int i;
        for (i = 0; i < caller->depth - 1; i++)
            printf("  | ");
        printf("  |-");
    }
    printf("%s%s\n", two->tp1->alias ?: two->tp1->name, caller->recursive ? " R" : "");
}

static int call_print_header(struct two_event *two)
{
    if (two)
        call_class_print(two);

    return 1;
}

static struct two_event_class *call_class_new(struct two_event_impl *impl, struct two_event_options *options)
{
    struct two_event_class *class = two_event_class_new(impl, options);
    struct call_class *call_class = NULL;

    if (class) {
        call_class = container_of(class, struct call_class, base);
        class->two = call_two;
        class->print_header = call_print_header;

        INIT_LIST_HEAD(&call_class->caller_head);

        call_class->iter_print.begin = NULL;
        call_class->iter_print.iterator = call_iterator_print;
        call_class->iter_print.end = NULL;
    }
    return class;
}

static struct two_event_impl call_impl = {
    .name = TWO_EVENT_CALL_IMPL,
    .class_size = sizeof(struct call_class),
    .class_new = call_class_new,

    .instance_size = sizeof(struct caller),
    .object_new = call_new,
    .object_delete = call_delete,
    .object_find = call_find,
};



/*
 * Analyze function calls. Also analyze function time.
 *
 * two(A, B), in function A, call function B.
 * two(B, B_ret), B to B_ret, get function time.
 *
 * A() {
 *     B() {retirn;}
 * }
 *
 * Print function calls and time statistics.
 *
 *                       function call    calls        total(us)      min(us)      avg(us)      max(us)
 * ----------------------------------- -------- ---------------- ------------ ------------ ------------
 * sys_perf_event_open                        8         1503.710      102.922      187.963      477.550
 *   |-perf_event_alloc                       8         1263.778       76.350      157.972      439.987
 *   |   |-perf_init_event                    8         1238.025       73.837      154.753      433.907
 *   |   |   |-perf_try_init_event          504          929.110        0.407        1.843      354.204
 *   |-perf_install_in_context                8          184.217       17.710       23.027       27.144
 *
**/

struct call_delay {
    struct caller base;
    struct two_event *delay;
};

struct call_delay_class {
    struct call_class base;
    struct two_event_class *delay_class;
};

static struct two_event *call_delay_new(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    struct two_event *two = call_new(class, tp1, tp2);
    struct call_delay *call_delay;
    struct call_delay_class *call_delay_class = NULL;

    if (two) {
        call_delay = container_of(two, struct call_delay, base.base);
        call_delay_class = container_of(class, struct call_delay_class, base.base);
        call_delay->delay = delay_impl.object_new(call_delay_class->delay_class, tp1, tp2);
        if (!call_delay->delay) {
            call_delete(class, two);
            two = NULL;
        }
    }
    return two;
}

static void call_delay_delete(struct two_event_class *class, struct two_event *two)
{
    struct call_delay *call_delay;
    struct call_delay_class *call_delay_class = NULL;

    if (two) {
        call_delay = container_of(two, struct call_delay, base.base);
        call_delay_class = container_of(class, struct call_delay_class, base.base);
        delay_impl.object_delete(call_delay_class->delay_class, call_delay->delay);
    }
    call_delete(class, two);
}

static struct two_event *call_delay_find(struct two_event_class *class, struct tp *tp1, struct tp *tp2)
{
    return call_find(class, tp1, tp2);
}

static void call_delay_two(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter)
{
    struct call_delay *call_delay;
    struct call_delay_class *call_delay_class;

    if (two) {
        call_delay = container_of(two, struct call_delay, base.base);
        call_delay_class = container_of(two->class, struct call_delay_class, base.base);

        /*
         * two(A, B), in function A, call function B.
         * Use tp2(B) to find the callee.
         *
         * two(B, B_ret), B to B_ret, get function time.
         * Use tp2(B_ret) cannot find the callee, count the function time.
         * `iter' is used.
        **/
        if(!call_link(two, info->tp2)) {
            if (delay_impl.object_find(call_delay_class->delay_class, info->tp1, info->tp2) != call_delay->delay) {
                fprintf(stderr, "BUG: in %s, %s:%s and %s:%s are mismatched.\n", __FUNCTION__,
                        info->tp1->sys, info->tp1->name, info->tp2->sys, info->tp2->name);
                return;
            }
            call_delay_class->delay_class->two(call_delay->delay, event1, event2, info, iter);
        }
    }
}

static void call_delay_begin(struct caller_iterator *iter)
{
    struct call_delay_class *call_delay_class;
    struct delay_class *delay_class;
    struct call_class *call_class;
    int i, flen;

    call_class = container_of(iter, struct call_class, iter_print);
    call_delay_class = container_of(call_class, struct call_delay_class, base);
    delay_class = container_of(call_delay_class->delay_class, struct delay_class, base);

    print_time(stdout);
    printf("\n");

    flen = call_class->max_depth * 4 + delay_class->max_len1;
    if (flen < 13) flen = 13; // 13 is strlen("function call");
    printf("%*s R", flen, "function call");
    if (!call_class->base.opts.env->tsc)
        printf(" %8s %16s %12s %12s %12s %12s %12s\n", "calls", "total(us)", "min(us)", "p50(us)",
                "p95(us)", "p99(us)", "max(us)");
    else
        printf(" %8s %16s %12s %12s %12s %12s %12s\n", "calls", "total(kcyc)", "min(kcyc)", "p50(kcyc)",
                "p95(kcyc)", "p99(kcyc)", "max(kcyc)");

    for (i=0; i<flen; i++) printf("-");
    printf(" -");
    printf(" %8s %16s %12s %12s %12s %12s %12s\n",
                    "--------", "----------------", "------------", "------------", "------------",
                    "------------", "------------");
}

static void call_delay_iterator(struct caller_iterator *iter, struct two_event *two)
{
    struct call_delay *call_delay;
    struct call_delay_class *call_delay_class;
    struct delay_class *delay_class;
    struct latency_node *node;
    struct caller *caller = container_of(two, struct caller, base);
    int len = 0, flen;
    double p50, p95, p99;

    call_delay = container_of(two, struct call_delay, base.base);
    call_delay_class = container_of(two->class, struct call_delay_class, base.base);
    delay_class = container_of(call_delay_class->delay_class, struct delay_class, base);
    node = latency_dist_find(delay_class->lat_dist, 0/*unused*/, call_delay->delay->id);

    if (caller->depth > 0) {
        int i;
        for (i = 0; i < caller->depth - 1; i++)
            len += printf("  | ");
        len += printf("  |-");
    }
    flen = call_delay_class->base.max_depth * 4 + delay_class->max_len1;
    if (flen < 13) flen = 13;
    printf("%-*s %s", flen-len, two->tp1->alias ?: two->tp1->name, caller->recursive ? "R" : " ");

    if (node) {
        p50 = tdigest_quantile(node->td, 0.50);
        p95 = tdigest_quantile(node->td, 0.95);
        p99 = tdigest_quantile(node->td, 0.99);
        printf(" %8lu %16.3f %12.3f %12.3f %12.3f %12.3f %12.3f\n",
            node->n, node->sum/1000.0, node->min/1000.0, p50/1000.0, p95/1000.0, p99/1000.0, node->max/1000.0);
    } else
        printf("\n");
}

static void call_delay_end(struct caller_iterator *iter)
{
    struct call_delay_class *call_delay_class;
    struct delay_class *delay_class;
    struct call_class *call_class;

    call_class = container_of(iter, struct call_class, iter_print);
    call_delay_class = container_of(call_class, struct call_delay_class, base);
    delay_class = container_of(call_delay_class->delay_class, struct delay_class, base);
    latency_dist_reset(delay_class->lat_dist);
}

static struct two_event_class *call_delay_class_new(struct two_event_impl *impl, struct two_event_options *options)
{
    struct two_event_class *class = call_class_new(impl, options);
    struct call_delay_class *call_delay_class = NULL;
    struct call_class *call_class;

    if (class) {
        call_delay_class = container_of(class, struct call_delay_class, base.base);
        call_class = &call_delay_class->base;

        class->two = call_delay_two;
        call_class->iter_print.begin = call_delay_begin;
        call_class->iter_print.iterator = call_delay_iterator;
        call_class->iter_print.end = call_delay_end;

        /*
         * call_delay does not support per-instance display.
        **/
        class->opts.perins = false;
        call_delay_class->delay_class = delay_impl.class_new(&delay_impl, &class->opts);
    }
    return class;
}

static void call_delay_class_delete(struct two_event_class *class)
{
    struct call_delay_class *call_delay_class;
    struct two_event_class *delay_class;

    if (class) {
        call_delay_class = container_of(class, struct call_delay_class, base.base);
        delay_class = call_delay_class->delay_class;
        two_event_class_delete(class);
        delay_impl.class_delete(delay_class);
    }
}

static struct two_event_impl call_delay_impl = {
    .name = TWO_EVENT_CALL_DELAY_IMPL,
    .class_size = sizeof(struct call_delay_class),
    .class_new = call_delay_class_new,
    .class_delete = call_delay_class_delete,

    .instance_size = sizeof(struct call_delay),
    .object_new = call_delay_new,
    .object_delete = call_delay_delete,
    .object_find = call_delay_find,
};


struct two_event_impl *impl_get(const char *name)
{
    struct two_event_impl *impl = NULL;

    if (strcmp(name, delay_impl.name) == 0)
        impl = &delay_impl;
    else if (strcmp(name, pair_impl.name) == 0)
        impl = &pair_impl;
    else if (strcmp(name, mem_profile_impl.name) == 0)
        impl = &mem_profile_impl;
    else if (strcmp(name, syscalls_impl.name) == 0)
        impl = &syscalls_impl;
    else if (strcmp(name, call_impl.name) == 0)
        impl = &call_impl;
    else if (strcmp(name, call_delay_impl.name) == 0) {
        impl_init(&delay_impl);
        impl = &call_delay_impl;
    }

    if (impl)
        impl_init(impl);
    return impl;
}

bool impl_based_on_call(const char *name)
{
    return strcmp(name, TWO_EVENT_CALL_IMPL) == 0 ||
           strcmp(name, TWO_EVENT_CALL_DELAY_IMPL) == 0;
}

void two_event_class_print(struct two_event_class *class, int indent)
{
    struct rb_node *node;
    struct two_event *two, *first = NULL;
    bool call = impl_based_on_call(class->impl->name);
    struct tp *tp2;
    int n = 0;

    dev_printf("impl: %s\n", class->impl->name);

    for (node = rb_first_cached(&class->two_events.entries); node; node = rb_next(node)) {
        two = rb_entry(node, struct two_event, rbnode);
        tp2 = two->tp2 ?: (call ? container_of(two, struct caller, base)->tp2 : NULL);
        if (tp2 && ++n == 1)
            first = two;
    }
    if (n == 0)
        return;

    if (n == 1) {
        tp2 = first->tp2 ?: (call ? container_of(first, struct caller, base)->tp2 : NULL);
        dev_printf("two: %s => %s\n", first->tp1->name, tp2->name);
    } else {
        dev_printf("two:\n");
        for (node = rb_first_cached(&class->two_events.entries); node; node = rb_next(node)) {
            two = rb_entry(node, struct two_event, rbnode);
            tp2 = two->tp2 ?: (call ? container_of(two, struct caller, base)->tp2 : NULL);
            if (tp2)
                dev_printf("    %s => %s\n", two->tp1->name, tp2->name);
        }
    }
}

