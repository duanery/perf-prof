#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
#include <time.h>

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

/* What an evsel is for; its evsels are replaced, the role stays. */
enum {
    ROLE_SWITCH,        // sched_switch, the prev side
    ROLE_SWITCH_NEXT,   // sched_switch, the next side
    ROLE_WAKEUP,        // sched_wakeup
    ROLE_WAKEUP_NEW,    // sched_wakeup_new
    ROLE_MAX,
};

static const char *role_name[ROLE_MAX] = {"sched_switch", "sched_switch", "sched_wakeup", "sched_wakeup_new"};
static const char *role_pid[ROLE_MAX] = {"prev_pid", "next_pid", "pid", "pid"};
static const char *role_comm[ROLE_MAX] = {"prev_comm", "next_comm", "comm", "comm"};

struct task_state_role {
    struct perf_evsel *evsel;   // the current one
    char *filter;               // not following the threads
    /* Following the threads: the kernel filter over their pids, sharing the ones of ROLE_SWITCH. */
    struct pid_filter *pf;
};

struct task_state_ctx {
    struct callchain_ctx *cc;
    struct flame_graph *flame;
    /* The threads asked for; the ones followed now are in the pid filters (ptrace adds and removes). */
    struct perf_thread_map *thread_map;
    struct task_state_role role[ROLE_MAX];
    struct tp_matcher *matcher_switch, *matcher_wakeup, *matcher_wakeup_new;
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
    /*
     * Follow the threads of thread_map: the kernel filter lets in a superset
     * of them, ftrace_filter keeps the exact ones.
     */
    bool follow;
    bool warned;
    int nr_ftrace_filter;       // the roles letting in more than the followed threads

    // lost
    struct list_head lost_list; // struct task_lost_node

    // minevtime
    u64 recent_time;

    // stat
    struct __dup_stat {
        u64 copied;
        u64 freed;
        u64 mem_bytes;
        u64 replaced;
        u64 replace_failed;
        u64 dropped;
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
        b->state = 0;
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

    ctx->lat_dist = latency_dist_new_quantile(env->perins, true, 0);
    if (!ctx->lat_dist)
        goto failed;
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct task_state_ctx *ctx = dev->private;
    struct task_lost_node *lost, *next;
    int i;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link)
        free(lost);

    for (i = 0; i < ROLE_MAX; i++) {
        free(ctx->role[i].filter);
        pid_filter_free(ctx->role[i].pf);
    }

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

/* The role is the evsel cookie + 1: 0 is an evsel of no role. */
static inline int task_state_role(struct perf_evsel *evsel)
{
    return evsel ? (int)perf_evsel_cookie(evsel) - 1 : -1;
}

static struct perf_evsel *task_state_evsel_new(int role, struct perf_event_attr *attr)
{
    struct perf_evsel *evsel = perf_evsel__new(attr);

    if (evsel && perf_evsel_set_cookie(evsel, role + 1) < 0) {
        perf_evsel__delete(evsel);
        evsel = NULL;
    }
    return evsel;
}

static int task_state_new_evsel(struct prof_dev *dev, int role, struct perf_event_attr *attr)
{
    struct task_state_ctx *ctx = dev->private;
    struct perf_evsel *evsel = task_state_evsel_new(role, attr);

    if (!evsel)
        return -1;
    perf_evlist__add(dev->evlist, evsel);
    ctx->role[role].evsel = evsel;
    return 0;
}

static int task_state_init(struct prof_dev *dev)
{
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
    int id;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    /**
     * sched:sched_switch and sched:sched_wakeup are not suitable for binding to threads
    **/
    if (!prof_dev_oncpu(dev)) {
        ctx->thread_map = dev->threads;
        perf_cpu_map__put(dev->cpus);
        dev->cpus = perf_cpu_map__new(NULL);
        dev->threads = perf_thread_map__new_dummy();
        /*
         * The kernel pid filter is a superset: the gaps are merged to fit
         * the length limit, and a fd's filter cannot be changed (SET_FILTER
         * returns EEXIST), the evsel is replaced to follow new threads.
         * ftrace_filter matches the threads exactly, on until the filters
         * are set. --filter matches the comm instead of the pid.
         */
        if (!env->filter) {
            ctx->follow = true;
            dev->ftrace_filter = 1;
        }
    }

    ctx->task_report = TASK_REPORT;
    if (env->interruptible_set && !env->interruptible)
        ctx->task_report &= ~TASK_INTERRUPTIBLE;

    /*
     * Kernel constraint: the prev_state of sched:sched_switch,
     * __trace_sched_switch_state() in include/trace/events/sched.h. Up to
     * v4.13 it is p->state, a dying task is TASK_DEAD. Since 1593baab910d
     * ("sched/debug: Implement consistent task-state printing"), in v4.14, it
     * is (p->state | p->exit_state) & TASK_REPORT: a dying task is EXIT_*, no
     * TASK_DEAD; and TASK_IDLE is TASK_REPORT_IDLE, since 06eb61844d84
     * ("sched/debug: Add explicit TASK_IDLE printing"), in v4.14. In v4.14
     * alone the value is the index of the state, fls(), until 3f5fe9fef5b2
     * ("sched/debug: Fix task state recording/printout"), in v4.15: S and D
     * are the same, the others are not told apart.
     *
     * The kernel version is compared: the states are not in the format as
     * fields, only in __print_flags() of the print fmt.
     */
    if (kernel_release() >= KERNEL_VERSION(4, 14, 0)) {
        ctx->state_dead = EXIT_ZOMBIE|EXIT_DEAD;
        ctx->task_report |= TASK_REPORT_IDLE;
    } else {
        ctx->state_dead = EXIT_ZOMBIE|EXIT_DEAD|TASK_DEAD;
    }
    // The prev_state of a preempted task, see tep__sched_switch_preempt_state().
    ctx->report_max = tep__sched_switch_preempt_state();

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
    if (task_state_new_evsel(dev, ROLE_SWITCH, &attr) < 0)
        goto failed;
    ctx->matcher_switch = tp_matcher_find(NULL, "sched", "sched_switch");

    // sched:sched_switch/next_pid==xx/
    // sched:sched_switch/next_comm==xx/
    if (ctx->mode == 1) {
        if (task_state_new_evsel(dev, ROLE_SWITCH_NEXT, &attr) < 0)
            goto failed;
    }

    // sched:sched_wakeup//
    // sched:sched_wakeup/pid==xx/
    id = tep__event_id("sched", "sched_wakeup");
    if (id < 0)
        goto failed;
    attr.config = id;
    if (task_state_new_evsel(dev, ROLE_WAKEUP, &attr) < 0)
        goto failed;
    ctx->matcher_wakeup = tp_matcher_find(NULL, "sched", "sched_wakeup");

    // sched:sched_wakeup_new//
    // sched:sched_wakeup_new/pid==xx/
    if (ctx->mode == 0 || (ctx->mode == 1 && env->filter)) {
        id = tep__event_id("sched", "sched_wakeup_new");
        if (id < 0)
            goto failed;
        attr.config = id;
        if (task_state_new_evsel(dev, ROLE_WAKEUP_NEW, &attr) < 0)
            goto failed;
        ctx->matcher_wakeup_new = tp_matcher_find(NULL, "sched", "sched_wakeup_new");
    }

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

/*
 * The filter of @role around @who, the pid or comm part. NULL: no filter,
 * or out of memory with @who.
 */
static char *task_state_compose(struct prof_dev *dev, int role, const char *who)
{
    struct env *env = dev->env;
    char states[128] = "";
    char *filter = NULL;
    int ret = 0;

    if (role == ROLE_SWITCH) {
        if (env->interruptible && env->uninterruptible)
            snprintf(states, sizeof(states), "prev_state==%d || prev_state==%d || prev_state==%d",
                     TASK_INTERRUPTIBLE, TASK_UNINTERRUPTIBLE, TASK_KILLABLE);
        else if (env->interruptible)
            snprintf(states, sizeof(states), "prev_state==%d", TASK_INTERRUPTIBLE);
        else if (env->uninterruptible)
            snprintf(states, sizeof(states), "prev_state==%d || prev_state==%d",
                     TASK_UNINTERRUPTIBLE, TASK_KILLABLE);
    }
    if (states[0] && who)
        ret = asprintf(&filter, strstr(states, "||") ? "(%s) && (%s)" : "%s && (%s)", states, who);
    else if (states[0])
        ret = asprintf(&filter, "%s", states);
    else if (who)
        ret = asprintf(&filter, "%s", who);
    return ret < 0 ? NULL : filter;
}

/*
 * ftrace_filter is needed while a kernel filter lets in more than the
 * followed threads. Not needed, the records skip it.
 */
static void task_state_ftrace_filter_update(struct prof_dev *dev)
{
    struct task_state_ctx *ctx = dev->private;
    bool need = ctx->nr_ftrace_filter > 0;

    if (dev->ftrace_filter != need && dev->env->verbose >= VERBOSE_NOTICE) {
        print_time(stdout);
        printf("task-state: ftrace_filter %s\n", need ? "on" : "off");
    }
    dev->ftrace_filter = need;
}

static int task_state_pf_role(struct task_state_ctx *ctx, struct pid_filter *pf)
{
    int role;

    for (role = 0; role < ROLE_MAX; role++)
        if (ctx->role[role].pf == pf)
            break;
    return role;
}

/*
 * Put the filter of a role in the kernel: applied before the evsel is
 * enabled, then the evsel is replaced. On error the old one keeps working,
 * pid_filter retries later.
 */
static int task_state_set_filter(struct pid_filter *pf, void *opaque, const char *filter, bool initial)
{
    struct prof_dev *dev = opaque;
    struct task_state_ctx *ctx = dev->private;
    int role = task_state_pf_role(ctx, pf);
    struct task_state_role *r = &ctx->role[role];
    int err;

    if (initial) {
        err = perf_evsel__apply_filter(r->evsel, filter);
        if (err < 0 || dev->env->verbose >= VERBOSE_NOTICE)
            fprintf(err < 0 ? stderr : stdout, "sched:%s filter \"%s\"\n", role_name[role], filter);
        return err;
    }
    if (dev->inclose)
        return -EBUSY;
    err = prof_dev_replace_filter(dev, &r->evsel, filter);
    if (err < 0) {
        ctx->stat.replace_failed++;
        if (!ctx->warned) {
            fprintf(stderr, "task-state: replace sched:%s filter failed, retry later: %s\n",
                    role_name[role], strerror(-err));
            ctx->warned = true;
        }
        return err;
    }
    ctx->stat.replaced++;
    if (dev->env->verbose >= VERBOSE_NOTICE) {
        print_time(stdout);
        printf("task-state: replace sched:%s filter \"%s\"\n", role_name[role], filter);
    }
    return 0;
}

/* The roles letting in more are counted: one needs ftrace_filter, all do not. */
static void task_state_need_ftrace_filter(struct pid_filter *pf, void *opaque, bool need)
{
    struct prof_dev *dev = opaque;
    struct task_state_ctx *ctx = dev->private;

    ctx->nr_ftrace_filter += need ? 1 : -1;
    task_state_ftrace_filter_update(dev);
}

static const struct pid_filter_ops task_state_pf_ops = {
    .set = task_state_set_filter,
    .ftrace_filter = task_state_need_ftrace_filter,
};

static int task_state_filter(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct task_state_ctx *ctx = dev->private;
    int role, err = 0;

    for (role = 0; role < ROLE_MAX; role++) {
        struct task_state_role *r = &ctx->role[role];
        char *filter = NULL;

        if (!r->evsel)
            continue;
        if (ctx->follow) {
            char *states = task_state_compose(dev, role, NULL);

            // The roles share the pids of ROLE_SWITCH, the first one.
            r->pf = pid_filter_new(role_pid[role], states,
                                   role == ROLE_SWITCH ? NULL : ctx->role[ROLE_SWITCH].pf,
                                   &task_state_pf_ops, dev);
            free(states);
            if (!r->pf) {
                err = -ENOMEM;
                break;
            }
            if (role == ROLE_SWITCH)
                err = pid_filter_add_threads(r->pf, ctx->thread_map);
            if (err == 0)
                err = pid_filter_start(r->pf);
            if (err < 0)
                break;
            continue;
        } else if (env->filter) {
            struct tp_filter *tp_filter = tp_filter_new(NULL, role_pid[role], env->filter, role_comm[role]);
            filter = task_state_compose(dev, role, tp_filter ? tp_filter->filter : NULL);
            tp_filter_free(tp_filter);
        } else
            filter = task_state_compose(dev, role, NULL);

        if (filter)
            err = perf_evsel__apply_filter(r->evsel, filter);

        if (err < 0 || env->verbose >= VERBOSE_NOTICE)
            fprintf(err < 0 ? stderr : stdout, "sched:%s filter \"%s\"\n", role_name[role], filter ?: "");

        if (err < 0) {
            free(filter);
            break;
        }
        free(r->filter);
        r->filter = filter;
    }
    if (ctx->follow)
        task_state_ftrace_filter_update(dev);
    return err < 0 ? err : 0;
}

static int task_state_add_thread(struct prof_dev *dev, pid_t pid)
{
    struct task_state_ctx *ctx = dev->private;
    int role, ret;

    /* No tid filter to follow the thread: the default. */
    if (!ctx->thread_map)
        return perf_evlist__add_thread(dev->evlist, pid);
    /* The events stay on all cpus, only the pid filters take the thread. */
    if (pid < 0 || !ctx->follow)
        return 0;
    if (pid_filter_follows(ctx->role[ROLE_SWITCH].pf, pid))
        return 0;

    if (dev->env->verbose >= VERBOSE_NOTICE) {
        print_time(stdout);
        printf("task-state: add thread %d\n", pid);
    }

    /*
     * A role not letting in the thread replaces its evsel at once. ptrace
     * keeps the thread stopped until this returns, no event of it is missed.
     */
    for (role = 0; role < ROLE_MAX; role++) {
        if (!ctx->role[role].pf)
            continue;
        ret = pid_filter_add(ctx->role[role].pf, pid);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/*
 * Force-settle a node whose thread is gone: account the pending
 * S/D/T/t/I latency and remove the node after all exit events were drained.
 */
static void task_state_settle_node(struct prof_dev *dev, int pid)
{
    struct task_state_ctx *ctx = dev->private;
    struct task_state_node tmp;
    struct rb_node *rbn;

    tmp.pid = pid;
    rbn = rblist__find(&ctx->task_states, &tmp);
    if (rbn) {
        struct task_state_node *task = rb_entry(rbn, struct task_state_node, rbnode);
        int state = task->pid != -1 ? (task->state & ctx->task_report) : 0;

        if (state) {
            struct timespec ts;
            evclock_t evtime = {.clock = task->time};
            u64 start, end, delta;

            clock_gettime(CLOCK_REALTIME, &ts);
            end = ts.tv_sec * (u64)NSEC_PER_SEC + ts.tv_nsec;
            start = evclock_to_realtime_ns(dev, evtime);
            delta = end > start ? end - start : 0;
            latency_dist_input(ctx->lat_dist, task->pid, state, delta, dev->env->greater_than);
        }
        rblist__remove_node(&ctx->task_states, rbn);
    }
}

static int task_state_del_thread(struct prof_dev *dev, pid_t pid)
{
    struct task_state_ctx *ctx = dev->private;
    int role;

    if (!ctx->follow || pid < 0)
        return 0;
    if (!pid_filter_follows(ctx->role[ROLE_SWITCH].pf, pid))
        return 0;

    task_state_settle_node(dev, pid);

    if (dev->env->verbose >= VERBOSE_NOTICE) {
        print_time(stdout);
        printf("task-state: del thread %d\n", pid);
    }
    /*
     * The filters still let in the tid, a new task may get it after a pid
     * wrap: ftrace_filter drops its records from now on. The filters are
     * tightened once the exits settle, then ftrace_filter may be off again.
     */
    for (role = 0; role < ROLE_MAX; role++) {
        if (ctx->role[role].pf)
            pid_filter_del(ctx->role[role].pf, pid);
    }
    return 0;
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
    struct task_state_ctx *ctx = dev->private;

    task_state_interval(dev);
    if (ctx->follow && dev->env->verbose >= VERBOSE_NOTICE)
        printf("task-state: replaced %lu replace_failed %lu dropped %lu twins %lu\n",
               ctx->stat.replaced, ctx->stat.replace_failed, ctx->stat.dropped, dev->replace_stat.dups);

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

static void task_state_lost(struct prof_dev *dev, union perf_event *event, int cpu, int tid, u64 lost_start, u64 lost_end)
{
    struct task_state_ctx *ctx = dev->private;
    struct task_lost_node *pos;
    struct task_lost_node *lost;

    print_lost_fn(dev, event, cpu, tid);

    // task-state serves as the forwarding source device.
    if (unlikely(!prof_dev_is_final(dev)))
        return;

    // Order is enabled by default.
    // When order is enabled, event loss will be sensed in advance, but it
    // needs to be processed later.
    lost = malloc(sizeof(*lost));
    if (lost) {
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

/*
 * Keep the records of the followed threads only, for both sample() and
 * forwarding; the kernel filter lets in a superset. Each role checks the pid
 * it is about. pid_filter_match() counts the dropped ones, too many
 * tighten the kernel filter.
 */
static long task_state_ftrace_filter(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct task_state_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct perf_evsel *evsel;
    struct task_state_role *r;
    union sched_event *sched_event;
    void *raw;
    int size, role;
    pid_t pid;

    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    role = task_state_role(evsel);
    if (role < 0)
        return 1;
    __raw_size(dev, event, &raw, &size);
    sched_event = raw;
    if (role == ROLE_SWITCH)
        pid = sched_event->sched_switch.prev_pid;
    else if (role == ROLE_SWITCH_NEXT)
        pid = sched_event->sched_switch.next_pid;
    else
        pid = sched_event->sched_wakeup.pid;
    r = &ctx->role[role];
    if (!r->pf || pid_filter_match(r->pf, pid))
        return 1;

    ctx->stat.dropped++;
    return 0;
}

/*
 * The evsels share one sample layout: build by the current sched_switch one,
 * the evsel of a kept record may be replaced and deleted since.
 */
static inline void task_state_callchain_data(struct prof_dev *dev, union perf_event *event,
                                             struct callchain_data *cd)
{
    struct task_state_ctx *ctx = dev->private;

    perf_event_build_callchain_data(ctx->role[ROLE_SWITCH].evsel, event, cd);
}

static inline void __print_callchain(struct prof_dev *dev, union perf_event *event)
{
    struct task_state_ctx *ctx = dev->private;
    struct sample_type_callchain *data = (void *)event->sample.array;

    if (dev->env->callchain) {
        struct callchain_data cd;
        task_state_callchain_data(dev, event, &cd);
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

static void task_state_print_event(struct prof_dev *dev, union perf_event *event, int cpu, int tid, int flags)
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
        task_state_callchain_data(dev, event, &cd);
        print_callchain_data(ctx->cc, &cd);
    }
}

static void task_state_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
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
    int size, role;

    if (data->time > ctx->recent_time)
        ctx->recent_time = data->time;

    if (unlikely(env->verbose >= VERBOSE_EVENT))
        task_state_print_event(dev, event, cpu, tid, 0);

    if (unlikely(task_state_event_lost(dev, event) < 0))
        goto free_event;

    /* By the role: the evsels of a role are replaced, the old one is still read. */
    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    role = task_state_role(evsel);
    if (role < 0)
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

    if (role == ROLE_SWITCH) {
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
    } else if (role == ROLE_SWITCH_NEXT) {
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
    } else if (role == ROLE_WAKEUP || role == ROLE_WAKEUP_NEW) {
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
            if (task->state != TASK_RUNNING || task->pid == -1 || role == ROLE_WAKEUP_NEW)
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
        if (ctx->follow)
            printf("  replaced: %lu\n"
                   "  replace_failed: %lu\n"
                   "  dropped: %lu\n"
                   "  twins: %lu\n",
                   ctx->stat.replaced, ctx->stat.replace_failed, ctx->stat.dropped, dev->replace_stat.dups);
    }
}

static void task_state_print_dev(struct prof_dev *dev, int indent)
{
    struct task_state_ctx *ctx = dev->private;
    int role;

    dev_printf("filter:\n");
    for (role = 0; role < ROLE_MAX; role++) {
        if (ctx->role[role].evsel)
            dev_printf("    %s: %s\n", role_name[role],
                       (ctx->follow ? pid_filter_string(ctx->role[role].pf) : ctx->role[role].filter) ?: "");
    }
    if (ctx->follow) {
        dev_printf("replaced: %lu\n", ctx->stat.replaced);
        dev_printf("replace_failed: %lu\n", ctx->stat.replace_failed);
        dev_printf("dropped: %lu\n", ctx->stat.dropped);
        dev_printf("twins: %lu\n", dev->replace_stat.dups);
    }

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
    .ftrace_filter = task_state_ftrace_filter,
    .add_thread = task_state_add_thread,
    .del_thread = task_state_del_thread,
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

    switch (task_state_role(evsel)) {
    case ROLE_SWITCH:
    case ROLE_SWITCH_NEXT:
        result->matcher = ctx->matcher_switch;
        break;
    case ROLE_WAKEUP:
        result->matcher = ctx->matcher_wakeup;
        break;
    case ROLE_WAKEUP_NEW:
        result->matcher = ctx->matcher_wakeup_new;
        break;
    default:
        break;
    }
}

static bool __task_state_refer_cpu(struct tp *tp, void *raw, int size, int cpu)
{
    struct matcher_result result = {};
    task_state_matcher(tp, raw, size, &result);
    return tp_matcher_refer_cpu(result.matcher, tp, result.true_raw, result.true_size, cpu);
}

static bool __task_state_refer_tid(struct tp *tp, void *raw, int size, int tid)
{
    struct matcher_result result = {};
    task_state_matcher(tp, raw, size, &result);
    return tp_matcher_refer_tid(result.matcher, tp, result.true_raw, result.true_size, tid);
}

static bool __task_state_task_cpu(struct tp *tp, void *raw, int size, int cpu, int tid, int *new_cpu, const char **reason)
{
    struct matcher_result result = {};
    task_state_matcher(tp, raw, size, &result);
    return tp_matcher_task_cpu(result.matcher, tp, result.true_raw, result.true_size, cpu, tid, new_cpu, reason);
}

TP_MATCHER_REGISTER5(NULL, "task-state", __task_state_refer_cpu, __task_state_refer_tid, __task_state_task_cpu);
