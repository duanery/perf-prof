#include <stdio.h>
#include <stdlib.h>
#include "monitor.h"
#include "trace_helpers.h"
#include "stack_helpers.h"

struct hrtimer_ctx;
typedef int (*analyzer)(struct hrtimer_ctx *ctx, int instance, int nr_tp, u64 *counters);

#define BREAK 0
#define PRINT 1

static char *expression = NULL;
struct hrtimer_ctx {
    char *expression;
    struct expr_prog *prog;
    struct callchain_ctx *cc;
    struct perf_evsel *leader;
    struct tp_list *tp_list;
    u64 *counters;
    u64 *ins_counters;
    analyzer analyzer;
    struct bpf_filter filter;
    struct prof_dev *dev;
};

static int __analyzer(struct hrtimer_ctx *ctx, int instance, int nr_tp, u64 *counters)
{
    if (expr_load_data(ctx->prog, counters, (nr_tp+1)*sizeof(*counters)) != 0)
        return BREAK;
    return (int)expr_run(ctx->prog);
}

static int __analyzer_irq_off(struct hrtimer_ctx *ctx, int instance, int nr_tp, u64 *counters)
{
    if (nr_tp == 0 && counters[0] > ctx->dev->env->greater_than)
        return PRINT;
    else
        return BREAK;
}

static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct hrtimer_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;
    ctx->dev = dev;

    if (env->event) {
        tep__ref_light();

        ctx->tp_list = tp_list_new(dev, env->event);
        if (!ctx->tp_list)
            goto failed;

        ctx->counters = calloc(1, prof_dev_nr_ins(dev) * (ctx->tp_list->nr_real_tp + 1) * sizeof(u64));
        if (!ctx->counters)
            goto failed;

        ctx->ins_counters = malloc((ctx->tp_list->nr_real_tp + 1) * sizeof(u64));
        if (!ctx->ins_counters)
            goto failed;

        ctx->analyzer = __analyzer;

        if (bpf_filter_init(&ctx->filter, env))
            bpf_filter_open(&ctx->filter);

        ctx->expression = expression;
    } else if (env->greater_than) {
        ctx->counters = calloc(1, prof_dev_nr_ins(dev) * sizeof(u64));
        if (!ctx->counters)
            goto failed;

        ctx->ins_counters = malloc(sizeof(u64));
        if (!ctx->ins_counters)
            goto failed;

        ctx->analyzer = __analyzer_irq_off;

        if (bpf_filter_init(&ctx->filter, env))
            bpf_filter_open(&ctx->filter);
    }

    if (env->callchain) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL), stdout);
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct hrtimer_ctx *ctx = dev->private;

    if (dev->env->callchain) {
        callchain_ctx_free(ctx->cc);
    }

    if (ctx->counters)
        free(ctx->counters);
    if (ctx->ins_counters)
        free(ctx->ins_counters);

    if (dev->env->event) {
        bpf_filter_close(&ctx->filter);
        tp_list_free(ctx->tp_list);
        tep__unref();
    } else if (dev->env->greater_than) {
        bpf_filter_close(&ctx->filter);
    }

    free(ctx);
}

static int hrtimer_argc_init(int argc, char *argv[])
{
    if (argc >= 1)
        expression = strdup(argv[0]);
    else
        expression = NULL;
    return 0;
}

static int hrtimer_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct hrtimer_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_SOFTWARE,
        .config        = PERF_COUNT_SW_CPU_CLOCK,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = env->sample_period ?   : env->freq, //ns
        .freq          = env->sample_period ? 0 : 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = PERF_FORMAT_ID | PERF_FORMAT_GROUP,
        .pinned        = 0,
        .disabled      = 1,
        .exclude_user  = env->exclude_user,
        .exclude_kernel = env->exclude_kernel,
        .exclude_guest = env->exclude_guest,
        .exclude_host = env->exclude_host,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL),
        .watermark     = 1,
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
    int i, j;

    if (!prof_dev_ins_oncpu(dev))
        return -1;
    if (env->sample_period == 0 && env->freq == 0)
        return -1;
    if (env->event && !expression) {
        fprintf(stderr, " {expression} needs to be specified.\n");
        return -1;
    }
    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    if (!env->event && !env->greater_than) {
        // perf-prof hrtimer -C 0-1 -F 100
        // no events, no sampling, only hrtimer
        dev->pages = 0;
    }

    if (env->callchain) {
        dev->pages *= 2;
    }
    attr.wakeup_watermark = (dev->pages << 12) / 2;

    reduce_wakeup_times(dev, &attr);

    ctx->leader = evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    if (env->event) {
        struct global_var_declare *declare = NULL;
        struct tp *tp;

        declare = calloc(ctx->tp_list->nr_real_tp+2, sizeof(*declare));
        if (!declare)
            goto failed;
        i = 0;
        for_each_real_tp(ctx->tp_list, tp, j) {
            evsel = tp_evsel_new(tp, &tp_attr);
            if (!evsel) {
                goto failed;
            }
            perf_evlist__add(evlist, evsel);

            declare[i].name = tp->alias ? : tp->name;
            declare[i].offset = i * sizeof(u64);
            declare[i].size = declare[i].elementsize = sizeof(u64);
            i++;
        }
        declare[i].name = (char *)"period";
        declare[i].offset = i * sizeof(u64);
        declare[i].size = declare[i].elementsize = sizeof(u64);

        ctx->prog = expr_compile(ctx->expression, declare);
        free(declare);
        if (!ctx->prog)
            goto failed;

        if(env->verbose)
            expr_dump(ctx->prog);
    }
    perf_evlist__set_leader(evlist);

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int hrtimer_filter(struct prof_dev *dev)
{
    struct hrtimer_ctx *ctx = dev->private;
    int err;

    if (dev->env->event) {
        err = tp_list_apply_filter(dev, ctx->tp_list);
        if (err < 0)
            return err;

        if (ctx->filter.bpf_fd >= 0) {
            err = perf_evsel__set_bpf(ctx->leader, ctx->filter.bpf_fd);
            if (err < 0)
                return err;
        }
    }
    return 0;
}

static void hrtimer_exit(struct prof_dev *dev)
{
    struct hrtimer_ctx *ctx = dev->private;
    if (ctx->expression)
        free(ctx->expression);
    if (ctx->prog)
        expr_destroy(ctx->prog);
    monitor_ctx_exit(dev);
}

static void hrtimer_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct env *env = dev->env;
    struct hrtimer_ctx *ctx = dev->private;
    // in linux/perf_event.h
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ | PERF_SAMPLE_CALLCHAIN
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
    int n = env->event ? ctx->tp_list->nr_real_tp : 0;
    u64 *jcounter = ctx->counters + instance * (n + 1);
    u64 counter, cpu_clock = 0;
    u64 i, j = 0, print = BREAK;
    int verbose = env->verbose;
    int header_end = 0;
    struct tp *tp;

    if (verbose) {
        if (dev->print_title) prof_dev_print_time(dev, data->time, stdout);
        printf("    pid %6d tid %6d [%03d] %llu.%06llu: %s: cpu-clock ", data->tid_entry.pid, data->tid_entry.tid,
                data->cpu_entry.cpu, data->time/NSEC_PER_SEC, (data->time%NSEC_PER_SEC)/1000, dev->prof->name);
    }

    for (i = 0; i < data->groups.nr; i++) {
        struct perf_evsel *evsel;
        evsel = perf_evlist__id_to_evsel(dev->evlist, data->groups.ctnr[i].id, NULL);
        if (!evsel)
            continue;
        if (evsel == ctx->leader) {
            cpu_clock = data->groups.ctnr[i].value - jcounter[n];
            jcounter[n] = data->groups.ctnr[i].value;
            ctx->ins_counters[n] = cpu_clock;
            if (verbose) {
                if (!header_end) {
                    printf(" %lu ns\n", cpu_clock);
                    header_end = 1;
                } else
                    printf("  cpu-clock: %lu ns\n", cpu_clock);
            }
            continue;
        }

        if (ctx->tp_list) {
            tp = tp_from_evsel(evsel, ctx->tp_list);
            if (tp) {
                counter = data->groups.ctnr[i].value - jcounter[j];
                jcounter[j] = data->groups.ctnr[i].value;
                ctx->ins_counters[j] = counter;
                j++;
                if (verbose) {
                    if (!header_end) {
                        printf("\n");
                        header_end = 1;
                    }
                    printf("  %s:%s %lu\n", tp->sys, tp->name, counter);
                }
            }
        }
    }

    if (data->groups.nr == n + 1)
        print = ctx->analyzer(ctx, instance, n, ctx->ins_counters);

    if (print == PRINT || verbose) {
        if (!verbose) {
            if (dev->print_title) prof_dev_print_time(dev, data->time, stdout);
            printf("    pid %6d tid %6d [%03d] %llu.%06llu: %s: cpu-clock %lu ns\n", data->tid_entry.pid, data->tid_entry.tid,
                data->cpu_entry.cpu, data->time/NSEC_PER_SEC, (data->time%NSEC_PER_SEC)/1000, dev->prof->name, cpu_clock);
            if (ctx->tp_list)
            for_each_real_tp(ctx->tp_list, tp, i) {
                printf("    %s %lu\n", tp->alias ? : tp->name, ctx->ins_counters[i]);
            }
        }
        if (dev->env->callchain) {
            struct callchain_data cd;
            perf_event_build_callchain_data(perf_event_evsel(dev, event), event, &cd);
            print_callchain_data(ctx->cc, &cd);
        }
    }
}

static void hrtimer_help(struct help_ctx *hctx)
{
    int i, j;
    struct env *env = hctx->env;

    printf(PROGRAME " hrtimer ");
    printf("-e \"");
    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(hctx->tp_list[i], tp, j) {
            printf("%s:%s/%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".");
            if (i != hctx->nr_list - 1 ||
                j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
    }
    printf("\" ");

    if (env->sample_period)
        printf("--period %lu ", env->sample_period);
    if (env->freq)
        printf("-F %d ", env->freq);
    if (env->callchain)
        printf("-g ");
    common_help(hctx, true, true, false, false, true, true, false);

    if (!env->sample_period)
        printf("[--period ns] ");
    if (!env->freq)
        printf("[-F freq] ");
    if (!env->callchain)
        printf("[-g] ");
    common_help(hctx, false, true, false, false, true, true, false);
    printf("\n");
}


static const char *hrtimer_desc[] = PROFILER_DESC("hrtimer",
    "[OPTION...] [-e EVENT[...]] [-F freq] [--period ns] [-g] {expression}",
    "High-resolution conditional sampling profiler.",
    "",
    "SYNOPSIS",
    "    hrtimer is a high-resolution conditional sampling profiler that uses an internal",
    "    kernel hrtimer (specified by --period or -F) to periodically sample the counts of",
    "    events specified by -e. At each sampling interval, user-space calculates the number",
    "    of event occurrences within that interval and evaluates {expression} to determine",
    "    whether to output the sample. If the expression evaluates to true, the sample is",
    "    printed.",
    "",
    "    The expression uses event names (or aliases) as variables, each representing the",
    "    count of occurrences within the sampling interval. Additionally, a 'period' variable",
    "    is available, representing the actual timer interval in nanoseconds.",
    "",
    "    Events specified by -e are grouped under the hrtimer leader. On timer expiration,",
    "    PERF_SAMPLE_READ captures all event counters in the group atomically. Each CPU's",
    "    timer fires independently and samples simultaneously, preserving count accuracy",
    "    across all CPUs.",
    "",
    "    Can only be attached to CPUs (-C), not to processes (-p).",
    "",
    "EXAMPLES",
    "    "PROGRAME" hrtimer -e sched:sched_switch -C 0 --period 50ms 'sched_switch==0'",
    "        Sample every 50ms, output when no context switches occurred.",
    "",
    "    "PROGRAME" hrtimer -e sched:sched_switch,sched:sched_wakeup -C 0-5 --period 50ms -g \\",
    "        'sched_switch==0 && sched_wakeup==0 && period > 50000000'",
    "        Sample every 50ms with callchain, output when no switches and no wakeups",
    "        occurred and the actual period exceeded 50ms.");
static const char *hrtimer_argv[] = PROFILER_ARGV("hrtimer",
    "OPTION:",
    "cpus", "watermark", "output", "mmap-pages", "exit-N", "usage-self",
    "version", "verbose", "quiet", "help",
    PROFILER_ARGV_FILTER,
    PROFILER_ARGV_PROFILER, "event", "freq", "period", "call-graph");
static profiler hrtimer = {
    .name = "hrtimer",
    .desc = hrtimer_desc,
    .argv = hrtimer_argv,
    .pages = 2,
    .help = hrtimer_help,
    .argc_init = hrtimer_argc_init,
    .init = hrtimer_init,
    .filter = hrtimer_filter,
    .deinit = hrtimer_exit,
    .sample = hrtimer_sample,
};
PROFILER_REGISTER(hrtimer);


static const char *irq_off_desc[] = PROFILER_DESC("irq-off",
    "[OPTION...] --period ns --than ns [-g]",
    "Interrupt-off detection tool.",
    "",
    "SYNOPSIS",
    "    irq-off is an interrupt-off detection tool that uses an internal hrtimer timer.",
    "    The --period option specifies the timer interval, and it samples the timer's count",
    "    value. User-space calculates the actual timer interval (in nanoseconds). If the",
    "    actual interval exceeds the threshold specified by --than, it indicates that",
    "    interrupts may have been disabled during that period, and the sample point along",
    "    with its callchain (if -g is specified) is printed.",
    "",
    "    The --than value should be greater than --period to detect interrupt-off conditions.",
    "    Based on hrtimer. See '"PROGRAME" hrtimer -h' for more information.",
    "",
    "EXAMPLES",
    "    "PROGRAME" irq-off --period 10ms --than 20ms -g",
    "        Detect interrupt-off conditions with 10ms timer interval, output when actual",
    "        interval exceeds 20ms, with callchain.",
    "",
    "    "PROGRAME" irq-off -C 0 --period 10ms --than 20ms -g",
    "        Detect interrupt-off conditions on CPU 0 only.");
static const char *irq_off_argv[] = PROFILER_ARGV("irq-off",
    "OPTION:",
    "cpus", "watermark", "output", "mmap-pages", "exit-N", "usage-self",
    "version", "verbose", "quiet", "help",
    PROFILER_ARGV_FILTER,
    PROFILER_ARGV_PROFILER, "period", "than", "call-graph");
struct monitor irq_off = {
    .name = "irq-off",
    .desc = irq_off_desc,
    .argv = irq_off_argv,
    .pages = 2,
    .init = hrtimer_init,
    .filter = hrtimer_filter,
    .deinit = hrtimer_exit,
    .sample = hrtimer_sample,
};
MONITOR_REGISTER(irq_off);

