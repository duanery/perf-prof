#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/string.h>
#include <linux/rblist.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <termios.h>
#include <monitor.h>
#include <count_helpers.h>


struct hrcount_binding {
    struct prof_binding binding;
    u64 pos;
    u64 counters[];
};

struct hrcount_ctx {
    struct perf_evsel *leader;
    struct tp_list *tp_list;
    struct prof_bindings bindings;
    int ins_oncpu;
    struct count_dist *count_dist;
    int hist_size;
    u64 period;
    bool packed_display;

    u64 rounds;
    int slots;
    int round_nr;

    bool need_reset;

    bool pipe_char; // "|"
    int tp_sys_name_max_len;
    int all_counters_max_len;
    int *pertp_max_len;
};

static void hrcount_sigwinch(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    struct tp *tp;
    int i, len = 0;

    if (!dev->tty.istty)
        return ;

    ctx->packed_display = ctx->hist_size <= 5;
    if (!ctx->packed_display)
        return;
    /*
     * [INS] |tp  |tp  |
     * [002] |1 2 |2 3 |
     */
    if (dev->env->perins)
        len += ctx->ins_oncpu ? 6 /*[%03d] */ : 9 /*[%06d] */;
    for_each_real_tp(ctx->tp_list, tp, i) {
        len += ctx->pertp_max_len[i] + 1 /*|*/;
    }
    len += 1 /*|*/;

    ctx->packed_display = len <= dev->tty.col;
}
static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct hrcount_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (!env->event) {
        free(ctx);
        return -1;
    }

    if (!env->interval)
        env->interval = 1000;
    if (!env->sample_period)
        env->sample_period = env->interval * 1000000UL;

    // For stat, if you specify the --period parameter, it actually
    // refers to the reading interval.
    if (strcmp(dev->prof->name, "stat") == 0 &&
        env->interval * 1000000UL > env->sample_period) {
        int interval = env->interval;
        env->interval = env->sample_period / 1000000UL;
        env->sample_period = interval * 1000000UL;
    }

    tep__ref_light();

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    ctx->ins_oncpu = prof_dev_oncpu(dev);
    ctx->bindings.size = sizeof(struct hrcount_binding) + (ctx->tp_list->nr_tp + 1) * sizeof(u64);

    ctx->rounds = 0;
    ctx->slots = 2;
    ctx->round_nr = 0;

    if (env->interval * 1000000UL > env->sample_period)
        ctx->hist_size = env->interval * 1000000UL / env->sample_period;
    else
        ctx->hist_size = env->sample_period / (env->interval * 1000000UL);
    ctx->count_dist = count_dist_new(env->perins, true, false, ctx->hist_size * ctx->slots);
    if (!ctx->count_dist)
        goto failed;

    ctx->period = env->sample_period;
    ctx->pipe_char = ctx->hist_size > 1 || ctx->tp_list->nr_real_tp > 1;

    ctx->all_counters_max_len = 2;
    ctx->pertp_max_len = calloc(ctx->tp_list->nr_tp, sizeof(int));
    if (!ctx->pertp_max_len)
        goto failed;
    else {
        int i, len;
        struct tp *tp;
        for_each_real_tp(ctx->tp_list, tp, i) {
            ctx->pertp_max_len[i] = strlen(tp->alias ?: tp->name);
            len = ctx->hist_size * (ctx->all_counters_max_len+1/* ' ' */) - 1;
            if (ctx->pertp_max_len[i] < len)
                ctx->pertp_max_len[i] = len;

            if (tp->alias)
                len = strlen(tp->alias);
            else
                len = strlen(tp->sys) + 1 + strlen(tp->name); //sys:name
            if (len > ctx->tp_sys_name_max_len)
                ctx->tp_sys_name_max_len = len;
        }
    }
    hrcount_sigwinch(dev);

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    count_dist_free(ctx->count_dist);
    prof_bindings_exit(&ctx->bindings);
    if (ctx->pertp_max_len)
        free(ctx->pertp_max_len);
    tp_list_free(ctx->tp_list);
    tep__unref();
    free(ctx);
}

static int hrcount_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct hrcount_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_SOFTWARE,
        .config        = PERF_COUNT_SW_CPU_CLOCK,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0, //ns
        .freq          = 0,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ,
        .read_format   = PERF_FORMAT_ID | PERF_FORMAT_GROUP,
        .pinned        = 0,
        .disabled      = 1,
        .watermark     = 0,
    };
    struct perf_event_attr tp_attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .freq          = 0,
        .sample_type   = 0,
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 0,
        .disabled      = 0,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    // For hrcount, it can only be attached to cpu.
    // For stat, it can be attached to cpu and pid.
    if (strcmp(dev->prof->name, "hrcount") == 0 &&
        !prof_dev_oncpu(dev)) {
        fprintf(stderr, "hrcount can only be attached to cpu.\n");
        return -1;
    }

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    // Only hrcount can sample events.
    if (strcmp(dev->prof->name, "hrcount") == 0)
        attr.sample_period = ctx->period;

    attr.wakeup_events = ctx->hist_size; // Wake up every N events
    ctx->leader = evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    for_each_real_tp(ctx->tp_list, tp, i) {
        evsel = tp_evsel_new(tp, &tp_attr);
        if (!evsel) {
            goto failed;
        }
        perf_evlist__add(evlist, evsel);
    }

    perf_evlist__set_leader(evlist);

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int hrcount_filter(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}

static void hrcount_exit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}

static void hrcount_sigusr(struct prof_dev *dev, int signum)
{
    struct hrcount_ctx *ctx = dev->private;
    if (signum == SIGWINCH) {
        if (ctx->packed_display) {
            hrcount_sigwinch(dev);
        }
    }
}

static void hrcount_reset(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    struct hrcount_binding *state;
    print_time(stdout);
    printf("hrcount reset\n");
    perf_evsel__disable(ctx->leader);
    perf_evsel__enable(ctx->leader);
    count_dist_reset(ctx->count_dist);
    prof_bindings_for_each(&ctx->bindings, state)
        state->pos = 0;
    ctx->rounds = 0;
    ctx->round_nr = 0;
}

/*
 * [INS] tp  1| 20| total 21
 */
static void direct_print(void *opaque, struct count_node *node)
{
    struct prof_dev *dev = opaque;
    struct hrcount_ctx *ctx = dev->private;
    int i, h;
    char buf[512];
    struct tp *tp = &ctx->tp_list->tp[node->id];

    if (tp->alias)
        snprintf(buf, sizeof(buf), "%s", tp->alias);
    else
        snprintf(buf, sizeof(buf), "%s:%s", tp->sys, tp->name);

    if (dev->env->perins)
        printf(ctx->ins_oncpu ? "[%03d] " : "[%6d] ",
               prof_binding_id(node->ins));
    printf("%*s ", ctx->tp_sys_name_max_len, buf);

    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    printf("%*lu", ctx->all_counters_max_len, node->hist[h++]);
    for (i = 1; i < ctx->hist_size; i++) {
        printf("|%*lu", ctx->all_counters_max_len, node->hist[h++]);
    }
    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    memset(&node->hist[h], 0, sizeof(u64) * ctx->hist_size);
    printf(" | total %lu\n", node->sum);
    node->sum = 0;
}


/*
 * perf-prof stat -e 'sched:sched_wakeup//cpus="0,2"/,sched:sched_switch//cpus=1/' \
 *                --period 200ms -i 1000 -C 0-2 --perins
 * Events are attached to different cpus. When printing the count percpu, events
 * not attached to this cpu need to be skipped.
 *
 * [CPU] |sched_wakeup       |sched_switch  |
 * [000] |342 585 875 820 297|              |
 * [001] |                   |56 44 46 65 54|
 * [002] |0   0   0   0   0  |              |
 */
static inline void packed_skip(void *opaque, u64 id)
{
    struct {
        u64 ins;
        u64 id;
        int line_len;
        struct prof_dev *dev;
    } *iter = opaque;
    struct prof_dev *dev = iter->dev;
    struct hrcount_ctx *ctx = dev->private;

    if (iter->ins != ~0UL && iter->id < id) {
        for (; iter->id < id; iter->id++) {
            struct tp *tp = &ctx->tp_list->tp[iter->id];
            if (!tp_is_dev(tp)) {
                printf("%s%-*s", ctx->pipe_char ? "|" : "", ctx->pertp_max_len[iter->id], "");
            }
        }
        if (id == ctx->tp_list->nr_tp)
            printf("%s\n", ctx->pipe_char ? "|" : "");
    }
}

/*
 * [INS] |tp  |tp  |
 * [002] |1 2 |2 3 |
 */
static void packed_print(void *opaque, struct count_node *node)
{
    int i, h, len = 0, max_len;
    u64 max = node->max;
    struct {
        u64 ins;
        u64 id;
        int line_len;
        struct prof_dev *dev;
    } *iter = opaque;
    struct prof_dev *dev = iter->dev;
    struct hrcount_ctx *ctx = dev->private;

    node->max = 0;
    len = strsize(max);
    max_len = ctx->hist_size * (len+1/* ' ' */) - 1;
    if (ctx->pertp_max_len[node->id] < max_len)
        ctx->pertp_max_len[node->id] = max_len;
    max_len = ((ctx->pertp_max_len[node->id] + 1) / ctx->hist_size) - 1 /* ' ' */;

    if (dev->env->perins && iter->ins != node->ins) {
        packed_skip(opaque, ctx->tp_list->nr_tp);
        iter->ins = node->ins;
        iter->id = 0;
        iter->line_len = printf(ctx->ins_oncpu ? "[%03d] " : "[%6d] ",
            prof_binding_id(node->ins));
    }

    packed_skip(opaque, node->id);

    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    if (ctx->pipe_char)
        len = printf("|%-*lu", max_len, node->hist[h++]) - 1/* '|' */;
    else
        len = printf("%-*lu", max_len, node->hist[h++]);
    for (i = 1; i < ctx->hist_size; i++) {
        len += printf(" %-*lu", max_len, node->hist[h++]);
    }
    h = (ctx->rounds % ctx->slots) * ctx->hist_size;
    memset(&node->hist[h], 0, sizeof(u64) * ctx->hist_size);
    iter->line_len += len;

    iter->line_len += printf("%*s", ctx->pertp_max_len[node->id] - len, "");
    iter->id ++;

    if (iter->id == ctx->tp_list->nr_tp) {
        if (ctx->pipe_char)
            iter->line_len += printf("|");
        printf("\n");
        if (dev->tty.istty && iter->line_len > dev->tty.col)
            ctx->packed_display = false;
    }
}

static void __hrcount_interval(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    int len, i;
    u64 print_pos = (ctx->rounds + 1) * ctx->hist_size;
    u64 max_pos = 0;
    struct hrcount_binding *state;

    // Determine if all instances are complete
    prof_bindings_for_each(&ctx->bindings, state) {
        if (state->pos < print_pos)
            return ;
        if (state->pos > max_pos)
            max_pos = state->pos;
    }
    if (ctx->bindings.nr >= 2 && ctx->hist_size >= 2 &&
        max_pos - print_pos >= ctx->hist_size/2)
        ctx->need_reset = true;

    len = strsize(count_dist_max(ctx->count_dist));
    if (len > ctx->all_counters_max_len)
        ctx->all_counters_max_len = len;

    print_time(stdout);
    printf("\n");

    if (ctx->packed_display) {
        struct {
            u64 ins;
            u64 id;
            int line_len;
            struct prof_dev *dev;
        } iter;
        struct tp *tp;
        if (dev->env->perins)
            printf(ctx->ins_oncpu ? "[CPU] " : "[THREAD] ");
        for_each_real_tp(ctx->tp_list, tp, i) {
            printf("%s%-*s", ctx->pipe_char ? "|" : "", ctx->pertp_max_len[i], tp->alias ?: tp->name);
        }
        printf("%s\n", ctx->pipe_char ? "|" : "");
        iter.ins = ~0UL;
        iter.id = 0;
        iter.line_len = 0;
        iter.dev = dev;
        count_dist_print(ctx->count_dist, packed_print, &iter);
        packed_skip(&iter, ctx->tp_list->nr_tp);
    } else
        count_dist_print(ctx->count_dist, direct_print, dev);

    ctx->rounds ++;
}

static void hrcount_interval(struct prof_dev *dev)
{
    struct hrcount_ctx *ctx = dev->private;
    __hrcount_interval(dev);
    if (ctx->need_reset) {
        ctx->need_reset = false;
        hrcount_reset(dev);
    }
}

static struct hrcount_binding *hrcount_binding(struct prof_dev *dev, int cpu, int tid)
{
    struct hrcount_ctx *ctx = dev->private;
    struct hrcount_binding *state = prof_binding_find(&ctx->bindings, cpu, tid);

    if (!state) {
        state = prof_binding_get(&ctx->bindings, cpu, tid);
        if (state)
            state->pos = ctx->rounds * ctx->hist_size;
    }
    return state;
}

static int hrcount_del_thread(struct prof_dev *dev, pid_t tid)
{
    struct hrcount_ctx *ctx = dev->private;
    prof_bindings_remove_thread(&ctx->bindings, tid);
    return 0;
}

static void hrcount_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    u64 binding = prof_binding_key(cpu, tid);
    struct hrcount_ctx *ctx = dev->private;
    struct hrcount_binding *state = hrcount_binding(dev, cpu, tid);
    /* PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ */
    struct sample_type_data {
        struct {
            __u32    pid;
            __u32    tid;
        }    tid_entry;
        __u64  time;
        struct {
            __u32    cpu;
            __u32    reserved;
        }    cpu_entry;
        struct {
            __u64 nr;
            struct {
                __u64 value;
                __u64 id;
            } ctnr[0];
        } groups;
    } *data = (void *)event->sample.array;
    int n = ctx->tp_list->nr_tp;
    u64 *ins_counter;
    u64 counter, cpu_clock = 0;
    u64 i, j;
    int verbose = dev->env->verbose;
    u64 print_pos = (ctx->rounds + 1) * ctx->hist_size;

    if (!state)
        return;
    ins_counter = state->counters;

    for (i = 0; i < data->groups.nr; i++) {
        struct perf_evsel *evsel;
        struct tp *tp;
        evsel = perf_evlist__id_to_evsel(dev->evlist, data->groups.ctnr[i].id, NULL);
        if (!evsel)
            continue;
        if (evsel == ctx->leader) {
            cpu_clock = data->groups.ctnr[i].value - ins_counter[n];
            ins_counter[n] = data->groups.ctnr[i].value;
            if (cpu_clock >= ctx->period * 2) {
                state->pos += cpu_clock / ctx->period - 1;
                verbose = VERBOSE_NOTICE;
            }
            continue;
        }
        tp = tp_from_evsel(evsel, ctx->tp_list);
        if (tp) {
            j = tp->idx;
            counter = data->groups.ctnr[i].value - ins_counter[j];
            ins_counter[j] = data->groups.ctnr[i].value;
            count_dist_insert(ctx->count_dist, binding, j, 0, state->pos, counter);
        }
    }

    state->pos ++;

    if (verbose) {
        if (dev->print_title) prof_dev_print_time(dev, data->time, stdout);
        printf(" %6d/%-6d [%03d]  %llu.%06llu: %s: cpu-clock: %lu ns\n", data->tid_entry.pid, data->tid_entry.tid,
                data->cpu_entry.cpu, data->time/NSEC_PER_SEC, (data->time%NSEC_PER_SEC)/1000, dev->prof->name, cpu_clock);
    }

    /* KERNEL BUG: maybe stuck
     *
     * perf_swevent_hrtimer ->
     *   __perf_event_overflow(throttle=1) ->
     *     __perf_event_account_interrupt ->
     *       perf_log_throttle
     *
     * After the perf event is throttled, it needs to wait for a tick to resume.
     * However, tick may be closed by nohz. It takes a long time to be unthrottled.
    **/
    if (state->pos >= print_pos) {
        ctx->round_nr ++;
        if (ctx->round_nr >= ctx->bindings.nr) {
            ctx->round_nr = 0;
            __hrcount_interval(dev);
        }
    }
}

static void __common_help(struct help_ctx *hctx, const char *name)
{
    int i, j;
    struct env *env = hctx->env;

    printf(PROGRAME " %s ", name);
    printf("-e \"");
    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(hctx->tp_list[i], tp, j) {
            printf("%s:%s/%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".");
            if (tp->alias)
                printf("alias=%s/", tp->alias);
            if (!tp->alias)
                printf("[");
            if (!tp->alias)
                printf("alias=./");
            if (!tp->alias)
                printf("]");
            if (i != hctx->nr_list - 1 ||
                j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
    }
    printf("\" ");

    if (env->sample_period)
        printf("--period %lu ", env->sample_period);
    common_help(hctx, true, true, false, true, false, true, true);

    if (!env->sample_period)
        printf("[--period ns] ");
    common_help(hctx, false, true, false, true, false, true, true);
    printf("\n");
}

static void hrcount_help(struct help_ctx *hctx)
{
    __common_help(hctx, "hrcount");
}


static const char *hrcount_desc[] = PROFILER_DESC("hrcount",
    "[OPTION...] -e EVENT[...] [--period ns] [-C cpus] [-i ms] [--perins]",
    "High-resolution periodic counter with microsecond granularity.",
    "",
    "SYNOPSIS",
    "    hrcount is a high-resolution counter that internally relies on an kernel hrtimer",
    "    (PERF_COUNT_SW_CPU_CLOCK). The --period option specifies the sampling interval,",
    "    and periodically samples the counters of events specified by -e. User-space",
    "    calculates the event occurrence count within each sampling interval (this is the",
    "    high-resolution counter), and outputs all counters at the interval specified by -i.",
    "",
    "    Events specified by -e are grouped under the hrtimer leader. On timer expiration,",
    "    PERF_SAMPLE_READ captures all event counters in the group atomically. Each CPU's",
    "    timer fires independently and samples simultaneously, preserving count accuracy",
    "    across all CPUs.",
    "",
    "    Can only be attached to CPUs (-C), not to processes (-p).",
    "",
    "EXAMPLES",
    "    "PROGRAME" hrcount -e sched:sched_switch -C 0 --period 50ms -i 1000",
    "    "PROGRAME" hrcount -e sched:sched_switch,sched:sched_wakeup//alias=wakeup/ -C 0-5 --period 50ms -i 1000",
    "    "PROGRAME" hrcount -e sched:sched_switch,sched:sched_wakeup//cpus=\"3-4\"/ -C 0-5 --period 200ms -i 1000 --perins",
    "    "PROGRAME" hrcount -e sched:sched_switch -C 0-3 --period 10ms -i 500 --perins");
static const char *hrcount_argv[] = PROFILER_ARGV("hrcount",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "event", "period", "perins");
static profiler hrcount = {
    .name = "hrcount",
    .desc = hrcount_desc,
    .argv = hrcount_argv,
    .pages = 2,
    .help = hrcount_help,
    .init = hrcount_init,
    .filter = hrcount_filter,
    .deinit = hrcount_exit,
    .del_thread = hrcount_del_thread,
    .sigusr = hrcount_sigusr,
    .interval = hrcount_interval,
    .sample = hrcount_sample,
};
PROFILER_REGISTER(hrcount);

static void stat_help(struct help_ctx *hctx)
{
    __common_help(hctx, "stat");
}

static int stat_read(struct prof_dev *dev, struct perf_evsel *leader, struct perf_counts_values *count, int cpu, int tid)
{
    struct hrcount_ctx *ctx = dev->private;
    struct hrcount_binding *state = hrcount_binding(dev, cpu, tid);
    u64 binding = prof_binding_key(cpu, tid);
    struct perf_counts {
        u64 nr;
        struct {
            u64 value;
            u64 id;
        } ctnr[0];
    } *groups = (void *)count;
    int n = ctx->tp_list->nr_tp;
    u64 *ins_counter;
    u64 counter, cpu_clock;
    u64 i, j;

    if (leader != ctx->leader)
        return 0;
    if (!state)
        return 0;
    ins_counter = state->counters;

    for (i = 0; i < groups->nr; i++) {
        struct perf_evsel *evsel;
        struct tp *tp;
        evsel = perf_evlist__id_to_evsel(dev->evlist, groups->ctnr[i].id, NULL);
        if (!evsel)
            continue;
        if (evsel == ctx->leader) {
            cpu_clock = groups->ctnr[i].value - ins_counter[n];
            ins_counter[n] = groups->ctnr[i].value;
            if (cpu_clock >= ctx->period * 2) {
                state->pos += cpu_clock / ctx->period - 1;
            }
            continue;
        }
        tp = tp_from_evsel(evsel, ctx->tp_list);
        if (tp) {
            j = tp->idx;
            counter = groups->ctnr[i].value - ins_counter[j];
            ins_counter[j] = groups->ctnr[i].value;
            count_dist_insert(ctx->count_dist, binding, j, 0, state->pos, counter);
        }
    }

    state->pos ++;
    return 1;
}


static const char *stat_desc[] = PROFILER_DESC("stat",
    "[OPTION...] -e EVENT[...] [--period ns] [-C cpus] [-p pid] [-i ms] [--perins]",
    "Low-resolution periodic counter, complementary to hrcount.",
    "",
    "SYNOPSIS",
    "    stat is a low-resolution counter that internally uses a user-space timer. The",
    "    --period option specifies the timer interval, and periodically reads the counters",
    "    of events specified by -e. It calculates the event occurrence count within each",
    "    timer interval, and outputs all counters at the interval specified by -i.",
    "",
    "    Unlike hrcount which samples all CPU counters simultaneously via kernel hrtimer,",
    "    stat reads each CPU's or process's counters one by one. This sequential reading",
    "    causes some precision loss, but allows attachment to both CPUs (-C) and processes",
    "    (-p/-t).",
    "",
    "    Forms a complementary pair with hrcount: use hrcount for high-precision CPU-only",
    "    monitoring, use stat for process-level monitoring with acceptable precision loss.",
    "",
    "EXAMPLES",
    "    "PROGRAME" stat -e sched:sched_switch -C 0 -i 1000 --period 100ms",
    "    "PROGRAME" stat -e sched:sched_switch,sched:sched_wakeup//alias=wakeup/ -C 0-5 -i 1000",
    "    "PROGRAME" stat -e sched:sched_switch,sched:sched_wakeup//cpus=\"3-4\"/ -C 0-5 -i 1000 --period 200ms --perins",
    "    "PROGRAME" stat -e sched:sched_switch -p 1234 -i 1000 --period 200ms --perins");
static const char *stat_argv[] = PROFILER_ARGV("stat",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "event", "perins", "period");
static profiler stat = {
    .name = "stat",
    .desc = stat_desc,
    .argv = stat_argv,
    .pages = 1,
    .help = stat_help,
    .init = hrcount_init,
    .filter = hrcount_filter,
    .deinit = hrcount_exit,
    .del_thread = hrcount_del_thread,
    .sigusr = hrcount_sigusr,
    .interval = hrcount_interval,
    .read = stat_read,
};
PROFILER_REGISTER(stat);
