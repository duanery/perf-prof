#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <monitor.h>
#include <dlfcn.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>


struct trace_ctx {
    struct callchain_ctx *cc;
    struct flame_graph *flame;
    struct tp_list *tp_list;
    time_t time;
    char time_str[32];
};

static void trace_interval(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct trace_ctx *ctx;

    if (!env->event)
        return -1;

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;

    tep__ref();

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    ctx->time = 0;
    ctx->time_str[0] = '\0';
    if (env->callchain || ctx->tp_list->nr_need_stack) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), stdout);
        if (env->flame_graph) {
            ctx->flame = flame_graph_open(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), env->flame_graph);
            if (env->interval) {
                trace_interval(dev);
            }
        }
        dev->pages *= 2;
    }

    dev->private = ctx;
    return 0;

failed:
    tep__unref();
    free(ctx);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct trace_ctx *ctx = dev->private;

    if (dev->env->callchain || ctx->tp_list->nr_need_stack) {
        callchain_ctx_free(ctx->cc);
        if (ctx->flame) {
            flame_graph_output(ctx->flame);
            flame_graph_close(ctx->flame);
        }
    }
    tp_list_free(ctx->tp_list);
    tep__unref();
    free(ctx);
}

static int trace_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct trace_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD |
                         PERF_SAMPLE_RAW | (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .inherit       = env->inherit,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    if (ctx->tp_list->nr_real_tp == ctx->tp_list->nr_push_to) {
        attr.watermark = 1;
        attr.wakeup_watermark = (dev->pages << 12) / 2;
    }

    if (env->overwrite) {
        attr.write_backward = 1;
        attr.watermark      = 1;
        attr.wakeup_watermark = dev->pages << 12;
    }
    prof_dev_env2attr(dev, &attr);

    for_each_real_tp(ctx->tp_list, tp, i) {
        evsel = tp_evsel_new(tp, &attr);
        if (!evsel) {
            goto failed;
        }
        perf_evlist__add(evlist, evsel);
    }
    for_each_dev_tp(ctx->tp_list, tp, i) {
        struct prof_dev *source_dev = tp->source_dev;
        if (source_dev)
            prof_dev_forward(source_dev, dev);
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int trace_filter(struct prof_dev *dev)
{
    struct trace_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}

static void trace_exit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW
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
    __u64		period;
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

static void __raw_size(union perf_event *event, void **praw, int *psize, bool callchain)
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

static inline void __print_callchain(struct prof_dev *dev, struct tp *tp, union perf_event *event)
{
    struct trace_ctx *ctx = dev->private;
    struct sample_type_callchain *data = (void *)event->sample.array;

    if (tp->stack) {
        if (!dev->env->flame_graph) {
            struct callchain_data cd;
            perf_event_build_callchain_data(tp->evsel, event, &cd);
            print_callchain_data(ctx->cc, &cd);
        } else {
            const char *comm = tep__pid_to_comm((int)data->h.tid_entry.pid);
            flame_graph_add_callchain_at_time(ctx->flame, &data->callchain, data->h.tid_entry.pid,
                                              !strcmp(comm, "<...>") ? NULL : comm,
                                              ctx->time, ctx->time_str);
        }
    }
}

static long trace_ftrace_filter(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct trace_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct perf_evsel *evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    struct tp *tp = tp_from_evsel(evsel, ctx->tp_list);
    void *raw;
    int size;

    if (!tp) return 0;
    if (!tp->ftrace_filter)
        return 1;
    __raw_size(event, &raw, &size, tp->stack);
    return tp_prog_run(tp, tp->ftrace_filter, GLOBAL(data->cpu_entry.cpu, data->tid_entry.pid, raw, size));
}

static void trace_print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    struct trace_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct perf_evsel *evsel = NULL;
    struct tp *tp = NULL;
    void *raw;
    int size;

    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    tp = tp_from_evsel(evsel, ctx->tp_list);
    if (!tp)
        return;

    if (!(flags & OMIT_TIMESTAMP)) {
        prof_dev_print_time(dev, data->time, stdout);
        tp_print_marker(tp);
    }

    __raw_size(event, &raw, &size, tp->stack);
    tp_print_event(tp, data->time, data->cpu_entry.cpu, raw, size);

    if (tp->stack && !(flags & OMIT_CALLCHAIN)) {
        struct callchain_data cd;
        perf_event_build_callchain_data(evsel, event, &cd);
        print_callchain_data(ctx->cc, &cd);
    }
}

static void trace_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct trace_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct perf_evsel *evsel = NULL;
    struct tp *tp = NULL;
    void *raw;
    int size;

    if (event->header.type == PERF_RECORD_DEV) {
        prof_dev_print_event(dev, event, instance, 0);
        return;
    }

    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    tp = tp_from_evsel(evsel, ctx->tp_list);
    if (!tp || tp_broadcast_event(tp, event))
        return;

    prof_dev_print_time(dev, data->time, stdout);
    tp_print_marker(tp);
    __raw_size(event, &raw, &size, tp->stack);
    tp_print_event(tp, data->time, data->cpu_entry.cpu, raw, size);
    if (tp->exec_prog)
        tp_prog_run(tp, tp->exec_prog, GLOBAL(data->cpu_entry.cpu, data->tid_entry.pid, raw, size));
    __print_callchain(dev, tp, event);
}

static void trace_interval(struct prof_dev *dev)
{
    struct trace_ctx *ctx = dev->private;
    if (ctx->flame) {
        ctx->time = time(NULL);
        strftime(ctx->time_str, sizeof(ctx->time_str), "%Y-%m-%d;%H:%M:%S", localtime(&ctx->time));
        flame_graph_output(ctx->flame);
        flame_graph_reset(ctx->flame);
    }
}

static void trace_help(struct help_ctx *hctx)
{
    int i, j;
    struct env *env = hctx->env;

    printf(PROGRAME " trace ");
    printf("-e \"");
    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(hctx->tp_list[i], tp, j) {
            printf("%s:%s/%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".");
            if (!env->callchain)
                printf("[stack/]");
            if (i != hctx->nr_list - 1 ||
                j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
    }
    printf("\" ");

    if (env->callchain)
        printf("-g ");
    if (env->flame_graph)
        printf("--flame-graph %s ", env->flame_graph);
    common_help(hctx, true, true, true, true, true, true, false);

    if (!env->callchain)
        printf("[-g] ");
    if (!env->flame_graph)
        printf("[--flame-graph .] ");
    common_help(hctx, false, true, true, true, true, true, false);
    printf("\n");
}

static const char *trace_desc[] = PROFILER_DESC("trace",
    "[OPTION...] -e EVENT [--overwrite] [-g [--flame-graph file [-i INT]]]",
    "Trace and print events.",
    "",
    "SYNOPSIS",
    "    The most basic and flexible profiler for sampling and analyzing tracepoint",
    "    events directly. Use this when other profilers don\'t meet your requirements.",
    "    Output raw event data for custom script analysis, observe event formats, or",
    "    generate flamegraphs.",
    "",
    "EXAMPLES",
    "    "PROGRAME" trace -e sched:sched_wakeup -C 0 -g",
    "    "PROGRAME" trace -e sched:sched_wakeup -g --flame-graph wakeup",
    "    "PROGRAME" trace -e 'oom:mark_victim//exec=system(\"cat /proc/%d/stat\", pid)/'",
    "    "PROGRAME" trace -e sched:sched_wakeup,sched:sched_switch --ptrace -- ls");
static const char *trace_argv[] = PROFILER_ARGV("trace",
    PROFILER_ARGV_OPTION, "inherit",
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "overwrite", "call-graph", "flame-graph", "ptrace");
static profiler trace = {
    .name = "trace",
    .desc = trace_desc,
    .argv = trace_argv,
    .pages = 2,
    .help = trace_help,
    .init = trace_init,
    .filter = trace_filter,
    .deinit = trace_exit,
    .interval = trace_interval,
    .ftrace_filter = trace_ftrace_filter,
    .print_event = trace_print_event,
    .sample = trace_sample,
};
PROFILER_REGISTER(trace);


struct tracepoint_private {
    struct tp_list *tp_list;
    void *parent;
    void (*cb)(void *parent, void *raw);
    void (*hangup)(void *parent);
};

static void tracepoint_deinit(struct prof_dev *dev)
{
    struct tracepoint_private *p = dev->private;
    tp_list_free(p->tp_list);
    free(p);
}

static int tracepoint_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_RAW,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 0,
        .wakeup_events = 1,
    };
    struct tracepoint_private *p;
    struct tp *tp;
    struct perf_evsel *evsel;
    int i;

    if (!env->event) return -1;

    p = zalloc(sizeof(*p));
    if (!p) return -1;

    dev->private = p;
    dev->silent = true;
    p->tp_list = tp_list_new(dev, env->event);
    if (!p->tp_list)
        goto failed;

    for_each_real_tp(p->tp_list, tp, i) {
        evsel = tp_evsel_new(tp, &attr);
        if (!evsel)
            goto failed;
        perf_evlist__add(evlist, evsel);
    }
    return 0;

failed:
    tracepoint_deinit(dev);
    return -1;
}

static void tracepoint_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct tracepoint_private *p = dev->private;
    struct {
        __u32   size;
        __u8    data[0];
    } *raw = (void *)event->sample.array;

    if (p->cb)
        p->cb(p->parent, raw->data);
}

static void tracepoint_hungup(struct prof_dev *dev)
{
    struct tracepoint_private *p = dev->private;
    if (p->hangup)
        p->hangup(p->parent);
}

static profiler tracepoint = {
    .name = "tracepoint",
    .pages = 1,
    .init = tracepoint_init,
    .deinit = tracepoint_deinit,
    .hangup = tracepoint_hungup,
    .sample = tracepoint_sample,
};

struct prof_dev *trace_dev_open(const char *event, struct perf_cpu_map *cpu_map, struct perf_thread_map *thread_map,
                 struct prof_dev *parent, void (*cb)(void *parent, void *raw), void (*hangup)(void *parent))
{
    struct prof_dev *dev;
    struct env *e;
    char *ev;

    e = zalloc(sizeof(*e)); // free in prof_dev_close()
    if (!e) return NULL;

    e->nr_events = 1;
    e->events = calloc(e->nr_events, sizeof(*e->events));
    ev = strdup(event);
    if (!e->events || !ev) {
        if (e->events) free(e->events);
        free(e);
        return NULL;
    }
    e->event = e->events[0] = ev;

    dev = prof_dev_open_cpu_thread_map(&tracepoint, e, cpu_map, thread_map, parent);

    if (dev) {
        struct tracepoint_private *p = dev->private;
        p->parent = parent;
        p->cb = cb;
        p->hangup = hangup;
    }
    return dev;
}

