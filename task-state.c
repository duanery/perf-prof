#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <monitor.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/rblist.h>
#include <linux/thread_map.h>
#include <monitor.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <latency_helpers.h>
#include <tp_struct.h>

#define TASK_RUNNING		0
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2
#define __TASK_STOPPED		4
#define __TASK_TRACED		8
/* in tsk->exit_state */
#define EXIT_ZOMBIE		16
#define EXIT_DEAD		32
/* in tsk->state again */
#define TASK_DEAD		64
#define TASK_WAKEKILL		128
#define TASK_WAKING		256
#define TASK_PARKED		512
#define TASK_STATE_MAX		1024

/* Convenience macros for the sake of set_task_state */
#define TASK_KILLABLE		(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_STOPPED		(TASK_WAKEKILL | __TASK_STOPPED)
#define TASK_TRACED		(TASK_WAKEKILL | __TASK_TRACED)


#if 0 // LINUX 4.14
/* Used in tsk->state: */
#define TASK_RUNNING			0x0000
#define TASK_INTERRUPTIBLE		0x0001
#define TASK_UNINTERRUPTIBLE		0x0002
#define __TASK_STOPPED			0x0004
#define __TASK_TRACED			0x0008
/* Used in tsk->exit_state: */
#define EXIT_DEAD			0x0010
#define EXIT_ZOMBIE			0x0020
#define EXIT_TRACE			(EXIT_ZOMBIE | EXIT_DEAD)
/* Used in tsk->state again: */
#define TASK_PARKED			0x0040
#define TASK_DEAD			0x0080

/* get_task_state(): */
#define TASK_REPORT			(TASK_RUNNING | TASK_INTERRUPTIBLE | \
					 TASK_UNINTERRUPTIBLE | __TASK_STOPPED | \
					 __TASK_TRACED | EXIT_DEAD | EXIT_ZOMBIE | \
					 TASK_PARKED)

#define TASK_REPORT_IDLE	(TASK_REPORT + 1)
#define TASK_REPORT_MAX		(TASK_REPORT_IDLE << 1)
#endif

#define TASK_REPORT (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | __TASK_STOPPED | __TASK_TRACED)
#define RUNDELAY   (TASK_STATE_MAX << 1)

#define TASK_REPORT_IDLE  0x80 // kernel 4.14 and later.
#define TASK_REPORT_MAX  0x100 // kernel 4.14 and later.

struct task_state_ctx {
    struct callchain_ctx *cc;
    struct flame_graph *flame;
    struct perf_thread_map *thread_map;
    union {
        struct perf_evsel *sched_switch;
        struct perf_evsel *sched_switch_prev;
    };
    struct perf_evsel *sched_switch_next;
    struct perf_evsel *sched_wakeup;
    struct perf_evsel *sched_wakeup_new;
    struct tp_matcher *matcher_switch, *matcher_wakeup, *matcher_wakeup_new;
    char *filter_switch, *filter_switch_next, *filter_wakeup;
    struct rblist task_states;
    struct latency_dist *lat_dist;
    struct comm_notify notify;
    int state_dead, report_max; // Compatible with different kernel release.
    int task_report;
    union {
        int mode;
        struct {
            int filter:1;
            int SD:1;
        };
    };
    bool dup;

    // lost
    struct list_head lost_list; // struct task_lost_node

    // minevtime
    u64 recent_time;

    // stat
    struct __dup_stat {
        u64 copied;
        u64 freed;
        u64 mem_bytes;
    } stat;
};

struct task_state_node {
    struct rb_node rbnode;
    int pid;
    int state;
    u64 time;
    union perf_event *event;
};

struct task_lost_node {
    struct list_head lost_link;
    int ins;
    bool reclaim;
    u64 start_time;
    u64 end_time;
};

union sched_event {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    struct sched_wakeup sched_wakeup;
    struct sched_wakeup_no_success sched_wakeup_no_success;
    struct sched_switch sched_switch;
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

static int task_state_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct task_state_node *b = container_of(rbn, struct task_state_node, rbnode);
    const struct task_state_node *e = entry;

    return b->pid - e->pid;
}
static struct rb_node *task_state_node_new(struct rblist *rlist, const void *new_entry)
{
    struct task_state_node *b = malloc(sizeof(*b));
    if (b) {
        b->pid = -1;
        b->time = 0;
        b->event = NULL;
        RB_CLEAR_NODE(&b->rbnode);
        return &b->rbnode;
    } else
        return NULL;
}
static void task_state_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct task_state_ctx *ctx = container_of(rblist, struct task_state_ctx, task_states);
    struct task_state_node *b = container_of(rb_node, struct task_state_node, rbnode);
    if (b->event) {
        ctx->stat.mem_bytes -= b->event->header.size;
        ctx->stat.freed++;
        free(b->event);
    }
    free(b);
}

static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct task_state_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;
    INIT_LIST_HEAD(&ctx->lost_list);

    tep__ref();
    if (env->callchain) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), stdout);
        if (env->flame_graph)
            ctx->flame = flame_graph_open(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), env->flame_graph);
        dev->pages *= 2;
    }
    rblist__init(&ctx->task_states);
    ctx->task_states.node_cmp = task_state_node_cmp;
    ctx->task_states.node_new = task_state_node_new;
    ctx->task_states.node_delete = task_state_node_delete;

    if (prof_dev_is_cloned(dev)) {
        struct task_state_ctx *pctx = prof_dev_is_cloned(dev)->private;
        ctx->lat_dist = latency_dist_ref(pctx->lat_dist);
    } else {
        ctx->lat_dist = latency_dist_new_quantile(env->perins, true, 0);
        if (!ctx->lat_dist)
            goto failed;
    }
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct task_state_ctx *ctx = dev->private;
    struct task_lost_node *lost, *next;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link)
        free(lost);

    if (ctx->filter_switch) free(ctx->filter_switch);
    if (ctx->filter_switch_next) free(ctx->filter_switch_next);
    if (ctx->filter_wakeup) free(ctx->filter_wakeup);

    perf_thread_map__put(ctx->thread_map);
    rblist__exit(&ctx->task_states);
    if (dev->env->callchain) {
        callchain_ctx_free(ctx->cc);
        if (ctx->flame) {
            flame_graph_output(ctx->flame);
            flame_graph_close(ctx->flame);
        }
    }
    latency_dist_free(ctx->lat_dist);
    if ((ctx->mode == 1 && dev->env->filter) || ctx->mode == 3)
        global_comm_unregister_notify(&ctx->notify);
    tep__unref();
    free(ctx);
}

static int task_state_notify(struct comm_notify *notify, int pid, int state, u64 free_time)
{
    if (state == NOTIFY_COMM_DELETE) {
        struct task_state_ctx *ctx = container_of(notify, struct task_state_ctx, notify);
        struct task_state_node *task, tmp;
        struct rb_node *rbn;

        tmp.pid = pid;
        rbn = rblist__find(&ctx->task_states, &tmp);
        if (rbn) {
            task = rb_entry(rbn, struct task_state_node, rbnode);
            if (task->time < free_time)
                rblist__remove_node(&ctx->task_states, rbn);
        }
    }
    return 0;
}

static int task_state_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct task_state_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
    };
    struct perf_evsel *evsel;
    int id;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    /**
     * sched:sched_switch and sched:sched_wakeup are not suitable for binding to threads
    **/
    if (!prof_dev_ins_oncpu(dev)) {
        ctx->thread_map = dev->threads;
        perf_cpu_map__put(dev->cpus);
        dev->cpus = perf_cpu_map__new(NULL);
        dev->threads = perf_thread_map__new_dummy();
    }

    ctx->task_report = TASK_REPORT;
    if (env->interruptible_set && !env->interruptible)
        ctx->task_report &= ~TASK_INTERRUPTIBLE;

    if (kernel_release() >= KERNEL_VERSION(4, 14, 0)) {
        ctx->state_dead = EXIT_ZOMBIE|EXIT_DEAD;
        ctx->report_max = TASK_REPORT_MAX;
        ctx->task_report |= TASK_REPORT_IDLE;
    } else {
        ctx->state_dead = EXIT_ZOMBIE|EXIT_DEAD|TASK_DEAD;
        ctx->report_max = TASK_STATE_MAX;
    }

    if (env->greater_than && using_order(dev))
        ctx->dup = true;

    /* |    mode       |
     * |      filter   |  event
     * | S/D  pid/comm | sched_switch                       sched_switch     sched_wakeup   sched_wakeup_new
     *    0      0       //                                 none             //             //
     *    0      1       /prev_pid==xx/                     /next_pid==xx/   /pid==xx/      /pid==xx/
     *    1      0       /prev_state==xx/                   none             //             none
     *    1      1       /prev_state==xx && prev_pid==xx/   none             /pid==xx/      none
     */

    ctx->SD = !!(env->interruptible || env->uninterruptible);
    ctx->filter = !!(ctx->thread_map || env->filter);

    /*
     * Mode 1 filtering comm will be affected by task_rename.
     *   --filter "sh"
     *     sched_switch: next_comm=sh next_pid=1045
     *     task_rename: pid=1045 oldcomm=sh newcomm=awk
     *   pid=1045 will remain on the ctx->task_states list until the pid is reused.
     *
     * Mode 3 is affected by task_switch.
     *   -S --filter "sh"
     *     311612.197341 sched:sched_switch: prev_pid=91319 prev_comm=sh prev_state=S
     *     311612.197354 sched:sched_switch: next_pid=91319 next_comm=sh
     *   pid=91319 sleeps, but there is no sched_wakeup and starts running directly.
     *   After this, pid=91319 will remain on the ctx->task_states list until the pid is reused.
     *
     * These two modes track the process free and delete the corresponding task_state_node from
     * the ctx->task_states list. This doesn't completely solve the problem, just alleviates it.
     */
    if ((ctx->mode == 1 && env->filter) || ctx->mode == 3) {
        ctx->notify.notify = task_state_notify;
        global_comm_register_notify(&ctx->notify);
    }

    // sched:sched_switch//
    // sched:sched_switch/prev_pid==xx/
    // sched:sched_switch/prev_comm==xx/
    // sched:sched_switch/prev_state==xx/
    // sched:sched_switch/prev_state==xx && prev_pid==xx/
    // sched:sched_switch/prev_state==xx && prev_comm==xx/
    id = tep__event_id("sched", "sched_switch");
    if (id < 0)
        goto failed;
    attr.config = id;
    evsel = ctx->sched_switch = perf_evsel__new(&attr);
    if (!evsel)
        goto failed;
    perf_evlist__add(evlist, evsel);
    ctx->matcher_switch = tp_matcher_find(NULL, "sched", "sched_switch");

    // sched:sched_switch/next_pid==xx/
    // sched:sched_switch/next_comm==xx/
    if (ctx->mode == 1) {
        evsel = ctx->sched_switch_next = perf_evsel__new(&attr);
        if (!evsel)
            goto failed;
        perf_evlist__add(evlist, evsel);
    } else
        ctx->sched_switch_next = NULL;

    // sched:sched_wakeup//
    // sched:sched_wakeup/pid==xx/
    id = tep__event_id("sched", "sched_wakeup");
    if (id < 0)
        goto failed;
    attr.config = id;
    evsel = ctx->sched_wakeup = perf_evsel__new(&attr);
    if (!evsel)
        goto failed;
    perf_evlist__add(evlist, evsel);
    ctx->matcher_wakeup = tp_matcher_find(NULL, "sched", "sched_wakeup");

    // sched:sched_wakeup_new//
    // sched:sched_wakeup_new/pid==xx/
    if (ctx->mode == 0 || (ctx->mode == 1 && env->filter)) {
        id = tep__event_id("sched", "sched_wakeup_new");
        if (id < 0)
            goto failed;
        attr.config = id;
        evsel = ctx->sched_wakeup_new = perf_evsel__new(&attr);
        if (!evsel)
            goto failed;
        perf_evlist__add(evlist, evsel);
        ctx->matcher_wakeup_new = tp_matcher_find(NULL, "sched", "sched_wakeup_new");
    } else
        ctx->sched_wakeup_new = NULL;

    /*
     * mode 1, mode 3, only for workload or using_ptrace.
     *
     * For trace_dev_open(), if the event is attached to `thread_map' and sched
     * switching is frequent, the CPU utilization of these threads will increase.
     * Therefore, we use ptrace.
     */
    if (ctx->thread_map && !env->filter && (env->workload.pid > 0 ||
                                            env->using_ptrace)) {
        ptrace_attach(ctx->thread_map, dev);
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int task_state_filter(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct task_state_ctx *ctx = dev->private;
    char filter[4096];
    struct perf_evsel *evsel;
    int err = 0;

    perf_evlist__for_each_evsel(evlist, evsel) {
        if (evsel == ctx->sched_switch) {
            struct tp_filter *prev_filter = NULL;

            prev_filter = tp_filter_new(ctx->thread_map, "prev_pid", env->filter, "prev_comm");

            if (env->interruptible && env->uninterruptible) {
                if (prev_filter) {
                    snprintf(filter, sizeof(filter), "(prev_state==%d || prev_state==%d || prev_state==%d) && (%s)",
                            TASK_INTERRUPTIBLE, TASK_UNINTERRUPTIBLE, TASK_KILLABLE, prev_filter->filter);
                } else
                    snprintf(filter, sizeof(filter), "prev_state==%d || prev_state==%d || prev_state==%d",
                            TASK_INTERRUPTIBLE, TASK_UNINTERRUPTIBLE, TASK_KILLABLE);
            } else if (env->interruptible) {
                if (prev_filter)
                    snprintf(filter, sizeof(filter), "prev_state==%d && (%s)",
                            TASK_INTERRUPTIBLE, prev_filter->filter);
                else
                    snprintf(filter, sizeof(filter), "prev_state==%d", TASK_INTERRUPTIBLE);
            } else if (env->uninterruptible) {
                if (prev_filter)
                    snprintf(filter, sizeof(filter), "(prev_state==%d || prev_state==%d) && (%s)",
                            TASK_UNINTERRUPTIBLE, TASK_KILLABLE, prev_filter->filter);
                else
                    snprintf(filter, sizeof(filter), "prev_state==%d || prev_state==%d",
                            TASK_UNINTERRUPTIBLE, TASK_KILLABLE);
            } else if (prev_filter) {
                snprintf(filter, sizeof(filter), "%s", prev_filter->filter);
            } else {
                filter[0] = '\0';
            }

            tp_filter_free(prev_filter);

            if (filter[0]) {
                err = perf_evsel__apply_filter(evsel, filter);
                if (!err)
                    ctx->filter_switch = strdup(filter);
            }

            if (err < 0 || env->verbose >= VERBOSE_NOTICE)
                fprintf(err < 0 ? stderr : stdout, "sched:sched_switch filter \"%s\"\n", filter);

            if (err < 0) return err;
        } else if (evsel == ctx->sched_switch_next) {
            struct tp_filter *next_filter = NULL;

            next_filter = tp_filter_new(ctx->thread_map, "next_pid", env->filter, "next_comm");
            if (next_filter) {
                err = perf_evsel__apply_filter(evsel, next_filter->filter);
                if (!err)
                    ctx->filter_switch_next = strdup(next_filter->filter);
            }

            if (err < 0 || env->verbose >= VERBOSE_NOTICE)
                fprintf(err < 0 ? stderr : stdout, "sched:sched_switch filter \"%s\"\n", next_filter ? next_filter->filter : "");

            tp_filter_free(next_filter);
            if (err < 0) return err;
        } else if (evsel == ctx->sched_wakeup || evsel == ctx->sched_wakeup_new) {
            struct tp_filter *tp_filter = NULL;

            tp_filter = tp_filter_new(ctx->thread_map, "pid", env->filter, "comm");
            if (tp_filter) {
                err = perf_evsel__apply_filter(evsel, tp_filter->filter);
                if (!err && !ctx->filter_wakeup)
                    ctx->filter_wakeup = strdup(tp_filter->filter);
            }

            if (err < 0 || env->verbose >= VERBOSE_NOTICE)
                fprintf(err < 0 ? stderr : stdout, "sched:sched_wakeup%s filter \"%s\"\n",
                        evsel == ctx->sched_wakeup ? "" : "_new", tp_filter ? tp_filter->filter : "");

            tp_filter_free(tp_filter);
            if (err < 0) return err;
        }
    }
    return 0;
}

static void task_state_enabled(struct prof_dev *dev)
{
    if (prof_dev_is_cloned(dev)) {
        struct task_state_ctx *ctx = dev->private;
        int idx, pid;
        if (!ctx->thread_map)
            return;
        perf_thread_map__for_each_thread(pid, idx, ctx->thread_map) {
            if (kill(pid, 0) == 0)
                return;
        }
        if (dev->env->verbose) {
            pid = perf_thread_map__pid(ctx->thread_map, 0);
            print_time(stdout);
            printf("%s close %d in task_state_enabled()\n", dev->prof->name, pid);
        }
        prof_dev_close(dev);
    }
}

static void task_print_node(void *opaque, struct latency_node *node)
{
    struct prof_dev *dev = opaque;
    double p50 = tdigest_quantile(node->td, 0.50);
    double p95 = tdigest_quantile(node->td, 0.95);
    double p99 = tdigest_quantile(node->td, 0.99);
    const char *state = NULL;

    switch (node->key) {
        case TASK_RUNNING: state = "R "; break;
        case TASK_INTERRUPTIBLE: state = "S "; break;
        case TASK_UNINTERRUPTIBLE: state = "D "; break;
        case __TASK_STOPPED: state = "T "; break;
        case __TASK_TRACED: state = "t "; break;
        case TASK_REPORT_IDLE: state = "I "; break;
        case RUNDELAY: state = "RD"; break;
        default: return;
    }

    if (dev->env->perins) {
        int pid = (int)node->instance;
        printf("%6d %-16s ", pid, tep__pid_to_comm(pid));
    }
    printf("%s %8lu %16.3f %12.3f %12.3f %12.3f %12.3f %12.3f\n", state,
        node->n, node->sum/1000.0, node->min/1000.0, p50/1000.0, p95/1000.0, p99/1000.0, node->max/1000.0);
}

static void task_state_interval(struct prof_dev *dev)
{
    struct task_state_ctx *ctx = dev->private;

    if (!prof_dev_at_top(dev))
        return;

    if (latency_dist_empty(ctx->lat_dist))
        return;

    print_time(stdout);
    printf("\n");

    if (dev->env->perins)
        printf("thread %-*s ", 16, "comm");
    printf("St %8s %16s %12s %12s %12s %12s %12s\n", "calls", "total(us)", "min(us)", "p50(us)",
        "p95(us)", "p99(us)", "max(us)");

    if (dev->env->perins)
        printf("------ ---------------- ");
    printf("-- %8s %16s %12s %12s %12s %12s %12s\n",
                "--------", "----------------", "------------", "------------", "------------",
                "------------", "------------");
    latency_dist_print(ctx->lat_dist, task_print_node, dev);
}

static void task_state_deinit(struct prof_dev *dev)
{
    task_state_interval(dev);
    monitor_ctx_exit(dev);
}

static u64 task_state_minevtime(struct prof_dev *dev)
{
    struct task_state_ctx *ctx = dev->private;
    struct env *env = dev->env;
    u64 minevtime = ULLONG_MAX;

    /*
     * The processes on the ctx->task_states rblist are all alive, and
     * there is no need to obtain their minevtime.
     */
    if (env->perins) {
        u64 mintime = 0;

        if (env->interval && ctx->recent_time > env->interval * NSEC_PER_MSEC)
            mintime = ctx->recent_time - env->interval * NSEC_PER_MSEC;

        if (mintime < minevtime)
            minevtime = mintime;
    }

    return minevtime;
}

static void task_state_lost(struct prof_dev *dev, union perf_event *event, int ins, u64 lost_start, u64 lost_end)
{
    struct task_state_ctx *ctx = dev->private;
    struct task_lost_node *pos;
    struct task_lost_node *lost;

    print_lost_fn(dev, event, ins);

    // task-state serves as the forwarding source device.
    if (unlikely(!prof_dev_is_final(dev)))
        return;

    // Order is enabled by default.
    // When order is enabled, event loss will be sensed in advance, but it
    // needs to be processed later.
    lost = malloc(sizeof(*lost));
    if (lost) {
        lost->ins = ins;
        lost->reclaim = false;
        lost->start_time = lost_start;
        lost->end_time = lost_end;

        list_for_each_entry(pos, &ctx->lost_list, lost_link) {
            if (pos->start_time > lost_start)
                break;
        }
        list_add_tail(&lost->lost_link, &pos->lost_link);
    }
}

static void __raw_size(struct prof_dev *dev, union perf_event *event, void **praw, int *psize)
{
    if (dev->env->callchain) {
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

static inline void __print_callchain(struct prof_dev *dev, union perf_event *event)
{
    struct task_state_ctx *ctx = dev->private;
    struct sample_type_callchain *data = (void *)event->sample.array;

    if (dev->env->callchain) {
        struct callchain_data cd;
        perf_event_build_callchain_data(perf_event_evsel(dev, event), event, &cd);
        if (!dev->env->flame_graph) {
            print_callchain_data(ctx->cc, &cd);
        } else {
            const char *comm = tep__pid_to_comm((int)data->h.tid_entry.pid);
            flame_graph_add_callchain(ctx->flame, &cd, data->h.tid_entry.pid, !strcmp(comm, "<...>") ? NULL : comm);
        }
    }
}
static void print_event(struct prof_dev *dev, union perf_event *event)
{
    struct sample_type_header *data = (void *)event->sample.array;
    void *raw;
    int size;

    __raw_size(dev, event, &raw, &size);
    prof_dev_print_time(dev, data->time, stdout);
    tep__print_event(data->time, data->cpu_entry.cpu, raw, size);
    __print_callchain(dev, event);
}

static inline int task_state_event_lost(struct prof_dev *dev, union perf_event *event)
{
    struct task_state_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct task_lost_node *lost, *next;

    if (likely(list_empty(&ctx->lost_list)))
        return 0;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link) {
        // Events before lost->start_time are processed normally.
        if (data->time <= lost->start_time)
            return 0;

        /*
         * Not sure which events are lost, we can only delete all process states
         * in `ctx->task_states'. Restart collection after lost.
         */
        if (!lost->reclaim) {
            rblist__exit(&ctx->task_states);
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

static void task_state_print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    struct task_state_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    void *raw;
    int size;

    if (!(flags & OMIT_TIMESTAMP))
        prof_dev_print_time(dev, data->time, stdout);

    __raw_size(dev, event, &raw, &size);
    tep__print_event(data->time, data->cpu_entry.cpu, raw, size);

    if (dev->env->callchain && !(flags & OMIT_CALLCHAIN)) {
        struct callchain_data cd;
        perf_event_build_callchain_data(perf_event_evsel(dev, event), event, &cd);
        print_callchain_data(ctx->cc, &cd);
    }
}

static void task_state_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct env *env = dev->env;
    struct task_state_ctx *ctx = dev->private;
    // in linux/perf_event.h
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
    struct sample_type_header *data = (void *)event->sample.array;
    struct task_state_node *task, tmp;
    union sched_event *sched_event;
    struct sched_switch *sw = NULL;
    struct perf_evsel *evsel;
    struct rb_node *rbn;
    void *raw;
    int size;

    if (data->time > ctx->recent_time)
        ctx->recent_time = data->time;

    if (unlikely(env->verbose >= VERBOSE_EVENT))
        task_state_print_event(dev, event, instance, 0);

    if (unlikely(task_state_event_lost(dev, event) < 0))
        goto free_event;

    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    if (!evsel)
        goto free_event;

    __raw_size(dev, event, &raw, &size);
    sched_event = raw;

    /* |    mode       |
     * |      filter   |  event
     * | S/D  pid/comm | sched_switch                      | sched_switch    | sched_wakeup  | sched_wakeup_new
     * - - - - - - - - | - - - - - - - - - - - - - - - - - | - - - - - - - - | - - - - - - - | - - - - - - - - -
     *    0      0     | //                                | none            | //            | //
     *                 | RUNNING                           |                 | S/D/T/t       | to RUNDELAY
     *                 | to S/D/T/t                        |                 | to RUNDELAY   |
     *                 | RUNDELAY                          |                 |               |
     *                 | to RUNNING                        |                 |               |
     *                 |                                   |                 |               |
     * - - - - - - - - | - - - - - - - - - - - - - - - - - | - - - - - - - - | - - - - - - - | - - - - - - - - -
     *    0      1     | /prev_pid==xx/                    | /next_pid==xx/  | /pid==xx/     | /pid==xx/
     *                 | RUNNING                           | RUNDELAY        | S/D/T/t       | to RUNDELAY
     *                 | to S/D/T/t                        | to RUNNING      | to RUNDELAY   |
     *                 |                                   |                 |               |
     * - - - - - - - - | - - - - - - - - - - - - - - - - - | - - - - - - - - | - - - - - - - | - - - - - - - - -
     *    1      0     | /prev_state==xx/                  | none            | //            | none
     *                 | to S/D                            |                 | S/D           |
     *                 |                                   |                 |               |
     * - - - - - - - - | - - - - - - - - - - - - - - - - - | - - - - - - - - | - - - - - - - | - - - - - - - - -
     *    1      1     | /prev_state==xx && prev_pid==xx/  | none            | /pid==xx/     | none
     *                 | to S/D                            |                 | S/D           |
     */

    if (evsel == ctx->sched_switch) {
        sw = &sched_event->sched_switch;

        if (sw->prev_pid > 0) {
            tmp.pid = sw->prev_pid;
            rbn = rblist__findnew(&ctx->task_states, &tmp);
            task = rb_entry_safe(rbn, struct task_state_node, rbnode);
            if (task) {
                if (task->pid != -1 && data->time > task->time) {
                    // RUNNING
                    if (task->state == TASK_RUNNING) {
                        latency_dist_input(ctx->lat_dist, task->pid, TASK_RUNNING, data->time - task->time, env->greater_than);
                    }
                }
                // to INTERRUPTIBLE/UNINTERRUPTIBLE/STOPPED/TRACED
                // to S/D/T/t
                task->pid = sw->prev_pid;
                task->state = sw->prev_state == ctx->report_max ? TASK_RUNNING : sw->prev_state;
                task->time = data->time;

                if (sw->prev_state & ctx->state_dead)
                    rblist__remove_node(&ctx->task_states, rbn);
                else if (ctx->dup) {
                    if (task->event) {
                        ctx->stat.mem_bytes -= task->event->header.size;
                        ctx->stat.freed++;
                        free(task->event);
                    }
                    task->event = memdup(event, event->header.size);
                    ctx->stat.mem_bytes += event->header.size;
                    ctx->stat.copied++;
                }
            }
        }

        if (ctx->mode == 0)
            goto parse_next;
    } else if (evsel == ctx->sched_switch_next) {
        sw = &sched_event->sched_switch;
parse_next:
        if (sw->next_pid > 0) {
            tmp.pid = sw->next_pid;
            rbn = rblist__findnew(&ctx->task_states, &tmp);
            task = rb_entry_safe(rbn, struct task_state_node, rbnode);
            if (task) {
                if (task->pid != -1 && data->time > task->time) {
                    // RUNDELAY: sched_wakeup -> sched_switch
                    if (task->state == TASK_RUNNING) {
                        u64 delta = data->time - task->time;
                        latency_dist_input(ctx->lat_dist, task->pid, RUNDELAY, delta, env->greater_than);
                        if (env->greater_than && delta > env->greater_than &&
                            task->event) {
                            if (dev->print_title) print_time(stdout);
                            printf(" task-state: %d %s RUNDELAY %lu ms\n", task->pid, tep__pid_to_comm(task->pid), delta/NSEC_PER_MSEC);
                            print_event(dev, task->event);
                            print_event(dev, event);
                        }
                    }
                }
                // to RUNNING
                task->pid = sw->next_pid;
                task->state = TASK_RUNNING;
                task->time = data->time;
                if (ctx->dup) {
                    if (task->event) {
                        ctx->stat.mem_bytes -= task->event->header.size;
                        ctx->stat.freed++;
                        free(task->event);
                    }
                    task->event = NULL;
                }
                // goto free_event;
            }
        }
    } else if (evsel == ctx->sched_wakeup || evsel == ctx->sched_wakeup_new) {
        struct sched_wakeup *wakeup = &sched_event->sched_wakeup;

        tmp.pid = wakeup->pid;
        if (ctx->SD)
             rbn = rblist__find(&ctx->task_states, &tmp);
        else rbn = rblist__findnew(&ctx->task_states, &tmp);
        task = rb_entry_safe(rbn, struct task_state_node, rbnode);
        if (task) {
            if (task->pid != -1 && data->time > task->time) {
                // S/D/T/t/I
                int state = task->state & ctx->task_report;
                if (state) {
                    u64 delta = data->time - task->time;
                    latency_dist_input(ctx->lat_dist, task->pid, state, delta, env->greater_than);
                    if (env->greater_than && delta > env->greater_than &&
                        task->event) {
                        if (dev->print_title) print_time(stdout);
                        printf(" task-state: %d %s WAIT %lu ms\n", task->pid, tep__pid_to_comm(task->pid), delta/NSEC_PER_MSEC);
                        print_event(dev, task->event);
                        print_event(dev, event);
                    }
                }
            }

            if (ctx->SD) {
                rblist__remove_node(&ctx->task_states, rbn);
                goto free_event;
            }

            // to RUNDELAY
            if (task->state != TASK_RUNNING || task->pid == -1 || evsel == ctx->sched_wakeup_new)
                task->time = data->time;
            task->pid = wakeup->pid;
            task->state = TASK_RUNNING;
            if (ctx->dup) {
                if (task->event) {
                    ctx->stat.mem_bytes -= task->event->header.size;
                    ctx->stat.freed++;
                    free(task->event);
                }
                task->event = memdup(event, event->header.size);
                ctx->stat.mem_bytes += event->header.size;
                ctx->stat.copied++;
            }
        }
    }

free_event:
    // do nothing
    return;
}

static void task_state_sigusr(struct prof_dev *dev, int signum)
{
    struct task_state_ctx *ctx = dev->private;
    if (signum == SIGUSR1) {
        print_time(stdout);
        printf("task-state\n");
        printf("  copied: %lu\n"
               "  freed: %lu\n"
               "  mem_bytes: %lu\n"
               "  tasks %d\n",
               ctx->stat.copied, ctx->stat.freed, ctx->stat.mem_bytes,
               rblist__nr_entries(&ctx->task_states));
    }
}

static void task_state_print_dev(struct prof_dev *dev, int indent)
{
    struct task_state_ctx *ctx = dev->private;
    dev_printf("filter:\n");
    if (ctx->sched_switch)
        dev_printf("    sched_switch: %s\n", ctx->filter_switch ?: "");
    if (ctx->sched_switch_next)
        dev_printf("    sched_switch: %s\n", ctx->filter_switch_next ?: "");
    if (ctx->sched_wakeup)
        dev_printf("    sched_wakeup: %s\n", ctx->filter_wakeup ?: "");
    if (ctx->sched_wakeup_new)
        dev_printf("    sched_wakeup_new: %s\n", ctx->filter_wakeup ?: "");

    if (!prof_dev_is_final(dev))
        return;

    dev_printf("task_states: %d\n", rblist__nr_entries(&ctx->task_states));
    if (ctx->dup) {
        dev_printf("copied: %lu\n", ctx->stat.copied);
        dev_printf("freed: %lu\n", ctx->stat.freed);
        dev_printf("mem_bytes: %lu\n", ctx->stat.mem_bytes);
    }
}

static const char *task_state_desc[] = PROFILER_DESC("task-state",
    "[OPTION...] [-S] [-D] [--than ns] [--filter comm] [--perins] [-g [--flame-graph file]]",
    "Monitor task state: R S D T t I RD.",
    "",
    "SYNOPSIS",
    "    A comprehensive analyzer for profiling system-wide or specified process runtime",
    "    characteristics. Analyzes scheduling events (sched_switch, sched_wakeup) to compute",
    "    latencies across seven states (R/S/D/T/t/I/RD), including runtime, sleep time, and",
    "    scheduling rundelays (RD). Outputs latency distribution (min/p50/p95/p99/max) for",
    "    each state.",
    "",
    "    Suitable for diagnosing process scheduling delays, IO wait, and process hangs.",
    "    Use --than option to filter and output raw events exceeding a specified latency",
    "    threshold. Also supports call stack output, flame graphs, and state filtering.",
    "",
    "TRACEPOINT",
    "    sched:sched_switch, sched:sched_wakeup, sched:sched_wakeup_new",
    "",
    "EXAMPLES",
    "    "PROGRAME" task-state -i 1000 --no-interruptible",
    "    "PROGRAME" task-state -p 2347 --perins --ptrace",
    "    "PROGRAME" task-state -p 2347 -SD --than 20ms -g",
    "    "PROGRAME" task-state --filter 'java,python*' -S --than 100ms -g",
    "    "PROGRAME" task-state -- ip link show eth0");
static const char *task_state_argv[] = PROFILER_ARGV("task-state",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "interruptible", "uninterruptible", "than", "filter\nFilter process comm",
    "perins", "call-graph", "flame-graph", "ptrace");
struct monitor task_state = {
    .name = "task-state",
    .desc = task_state_desc,
    .argv = task_state_argv,
    .pages = 8,
    .order = 1,
    .init = task_state_init,
    .filter = task_state_filter,
    .enabled = task_state_enabled,
    .deinit = task_state_deinit,
    .sigusr = task_state_sigusr,
    .print_dev = task_state_print_dev,
    .interval = task_state_interval,
    .minevtime = task_state_minevtime,
    .lost = task_state_lost,
    .print_event = task_state_print_event,
    .sample = task_state_sample,
};
MONITOR_REGISTER(task_state);


struct matcher_result {
    struct tp_matcher *matcher;
    void *true_raw;
    int true_size;
};

/*
 * task-state itself does not use tp. But when it is used as a forwarding source,
 * tp will be assigned to it in the forwarding target device.
 */
static void task_state_matcher(struct tp *tp, void *raw, int size, struct matcher_result *result)
{
    union perf_event *event = raw;
    struct prof_dev *dev;
    struct task_state_ctx *ctx;
    struct sample_type_header *data;
    struct perf_evsel *evsel;

    if (!tp_is_dev(tp))
        return;

    if (event->header.type == PERF_RECORD_DEV) {
        struct perf_record_dev *event_dev = (void *)event;
        event = event_dev->event;
        dev = event_dev->dev;
    } else
        dev = tp->source_dev;

    ctx = dev->private;
    data = (void *)event->sample.array;
    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    if (!evsel)
        return;

    __raw_size(dev, event, &result->true_raw, &result->true_size);

    if (evsel == ctx->sched_switch)
        result->matcher = ctx->matcher_switch;
    else if (evsel == ctx->sched_switch_next)
        result->matcher = ctx->matcher_switch;
    else if (evsel == ctx->sched_wakeup)
        result->matcher = ctx->matcher_wakeup;
    else if (evsel == ctx->sched_wakeup_new)
        result->matcher = ctx->matcher_wakeup_new;
}

static bool __task_state_samecpu(struct tp *tp, void *raw, int size, int cpu)
{
    struct matcher_result result = {};
    task_state_matcher(tp, raw, size, &result);
    return tp_matcher_samecpu(result.matcher, tp, result.true_raw, result.true_size, cpu);
}

static bool __task_state_samepid(struct tp *tp, void *raw, int size, int pid)
{
    struct matcher_result result = {};
    task_state_matcher(tp, raw, size, &result);
    return tp_matcher_samepid(result.matcher, tp, result.true_raw, result.true_size, pid);
}

static bool __task_state_target_cpu(struct tp *tp, void *raw, int size, int cpu, int pid, int *target_cpu, const char **reason)
{
    struct matcher_result result = {};
    task_state_matcher(tp, raw, size, &result);
    return tp_matcher_target_cpu(result.matcher, tp, result.true_raw, result.true_size, cpu, pid, target_cpu, reason);
}

TP_MATCHER_REGISTER5(NULL, "task-state", __task_state_samecpu, __task_state_samepid, __task_state_target_cpu);

