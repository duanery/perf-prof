#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/zalloc.h>
#include <linux/strlist.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <latency_helpers.h>

struct num_dist_ctx {
    int nr_ins;
    int nr_points;
    struct tp_list *tp_list;
    struct latency_dist *dist;
    int max_len;
    struct callchain_ctx *cc;
    struct heatmap **heatmaps;
    bool print_header;
};

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
        union {
            __u8    data[0];
            unsigned short common_type;
        };
    } __packed raw;
};

static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    int i, stacks = 0;
    struct tp *tp;
    struct env *env = dev->env;
    struct num_dist_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (!env->event) {
        free(ctx);
        return -1;
    }

    if (env->greater_than || env->verbose >= VERBOSE_EVENT)
        tep__ref();
    else
        tep__ref_light();

    ctx->nr_ins = prof_dev_nr_ins(dev);

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    if (ctx->tp_list->nr_num_prog == 0) {
        fprintf(stderr, "Please use the multi-trace profiler\n");
        goto failed;
    }
    if (ctx->tp_list->nr_real_tp != ctx->tp_list->nr_num_prog) {
        fprintf(stderr, "The number of 'num' attr is not equal to the number of event\n");
        goto failed;
    }

    ctx->nr_points = ctx->tp_list->nr_tp;

    ctx->dist = latency_dist_new_quantile(env->perins, true, 0);
    if (!ctx->dist)
        goto failed;

    for_each_real_tp(ctx->tp_list, tp, i) {

        if (strlen(tp->alias?:tp->name) > ctx->max_len)
            ctx->max_len = strlen(tp->alias?:tp->name);
        stacks += tp->stack;

        if (env->verbose)
            printf("name %s id %d filter %s stack %d\n", tp->name, tp->id, tp->filter, tp->stack);
    }

    if (stacks || env->callchain) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL), stdout);
        dev->pages *= 2;
    } else
        ctx->cc = NULL;

    if (env->heatmap) {
        char buff[1024];

        ctx->heatmaps = calloc(ctx->nr_points, sizeof(*ctx->heatmaps));
        if (!ctx->heatmaps)
            goto failed;
        for_each_real_tp(ctx->tp_list, tp, i) {
            snprintf(buff, sizeof(buff), "%s-%s", env->heatmap, tp->name);
            ctx->heatmaps[i] = heatmap_open("ns", "ns", buff);
        }
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct num_dist_ctx *ctx = dev->private;
    tp_list_free(ctx->tp_list);
    latency_dist_free(ctx->dist);
    callchain_ctx_free(ctx->cc);
    if (ctx->heatmaps) {
        int i;
        for (i = 0; i < ctx->nr_points; i++)
            heatmap_close(ctx->heatmaps[i]);
        free(ctx->heatmaps);
    }
    tep__unref();
    free(ctx);
}

static int num_dist_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct num_dist_ctx *ctx;
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
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL),
        .watermark     = 1,
    };
    struct tp *tp;
    int i;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    attr.wakeup_watermark = (dev->pages << 12) / 3;
    prof_dev_env2attr(dev, &attr);

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct perf_evsel *evsel;
        evsel = tp_evsel_new(tp, &attr);
        if (!evsel)
            goto failed;
        perf_evlist__add(evlist, evsel);
    }
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int num_dist_filter(struct prof_dev *dev)
{
    struct num_dist_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}

static void print_num_node(void *opaque, struct latency_node *node)
{
    struct prof_dev *dev = opaque;
    struct env *env = dev->env;
    struct num_dist_ctx *ctx = dev->private;
    int oncpu = prof_dev_ins_oncpu(dev);
    struct tp *tp = &ctx->tp_list->tp[node->key];
    double p99 = tdigest_quantile(node->td, 0.99);
    int i;

    if (ctx->print_header) {
        ctx->print_header = false;

        print_time(stdout);
        printf("\n");

        if (env->perins)
            printf(oncpu ? "[CPU] " : "[THREAD] ");
        printf("%-*s", ctx->max_len, "event");
        printf(" %8s %16s %12s %12s %12s %12s\n", "calls", "total", "min", "avg", "p99", "max");

        if (env->perins)
            printf(oncpu ? "----- " : "-------- ");
        for (i=0; i<ctx->max_len; i++) printf("-");
        printf(" %8s %16s %12s %12s %12s %12s\n",
                        "--------", "----------------", "------------", "------------", "------------", "------------");
    }

    if (env->perins) {
        if (oncpu)
            printf("[%03d] ", prof_dev_ins_cpu(dev, node->instance));
        else
            printf("%-8d ", prof_dev_ins_thread(dev, node->instance));
    }
    printf("%*s", ctx->max_len, tp->alias ?: tp->name);
    printf(" %8lu %16lu %12lu %12lu %12lu %12lu\n",
        node->n, node->sum, node->min, node->sum/node->n, (u64)p99, node->max);
}

static void num_dist_interval(struct prof_dev *dev)
{
    struct num_dist_ctx *ctx = dev->private;

    ctx->print_header = true;
    latency_dist_print(ctx->dist, print_num_node, dev);
}

static void num_dist_exit(struct prof_dev *dev)
{
    num_dist_interval(dev);
    monitor_ctx_exit(dev);
}

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

static void __print_callchain(struct num_dist_ctx *ctx, union perf_event *event, struct tp *tp)
{
    struct callchain_data cd;
    perf_event_build_callchain_data(tp->evsel, event, &cd);
    print_callchain_data(ctx->cc, &cd);
}

static long num_dist_ftrace_filter(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct num_dist_ctx *ctx = dev->private;
    struct sample_type_header *hdr = (void *)event->sample.array;
    struct perf_evsel *evsel = perf_evlist__id_to_evsel(dev->evlist, hdr->id, NULL);
    struct tp *tp = tp_from_evsel(evsel, ctx->tp_list);
    void *raw;
    int size;

    if (!tp) return 0;
    if (!tp->ftrace_filter)
        return 1;
    __raw_size(event, &raw, &size, tp->stack);
    return tp_prog_run(tp, tp->ftrace_filter, GLOBAL(hdr->cpu_entry.cpu, hdr->tid_entry.pid, raw, size));
}

static void num_dist_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct num_dist_ctx *ctx = dev->private;
    struct env *env = dev->env;
    struct sample_type_header *hdr = (void *)event->sample.array;
    struct perf_evsel *evsel;
    struct tp *tp;
    void *raw;
    int size;
    __u64 delta;

    evsel = perf_evlist__id_to_evsel(dev->evlist, hdr->id, NULL);
    tp = tp_from_evsel(evsel, ctx->tp_list);
    if (!tp)
        return;

    __raw_size(event, &raw, &size, tp->stack || env->callchain);
    delta = tp_get_num(tp, GLOBAL(hdr->cpu_entry.cpu, hdr->tid_entry.pid, raw, size));
    latency_dist_input(ctx->dist, env->perins?instance:0, tp->idx, delta, env->greater_than);

    if (env->heatmap)
        heatmap_write(ctx->heatmaps[tp->idx], hdr->time, delta);

    if ((env->greater_than && delta > env->greater_than) ||
        env->verbose >= VERBOSE_EVENT) {
        if (dev->print_title) prof_dev_print_time(dev, hdr->time, stdout);
        tep__update_comm(NULL, hdr->tid_entry.tid);
        tp_print_event(tp, hdr->time, hdr->cpu_entry.cpu, raw, size);
        __print_callchain(ctx, event, tp);
    }
}

static void num_dist_help(struct help_ctx *hctx)
{
    int i, j;
    struct env *env = hctx->env;

    printf(PROGRAME " num-dist ");
    printf("-e \"");
    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(hctx->tp_list[i], tp, j) {
            printf("%s:%s/%s/num=%s/alias=%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".",
                             tp->num?:".", tp->alias?:".");
            if (!env->callchain)
                printf("[stack/]");
            if (i != hctx->nr_list - 1 ||
                j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
    }
    printf("\" ");

    if (env->perins)
        printf("--perins ");
    if (env->greater_than)
        printf("--than %lu ", env->greater_than);
    if (env->heatmap)
        printf("--heatmap %s ", env->heatmap);
    common_help(hctx, true, true, true, true, false, false, true);

    if (!env->perins)
        printf("[--perins] ");
    if (!env->greater_than)
        printf("[--than .] ");
    if (!env->heatmap)
        printf("[--heatmap .] ");
    common_help(hctx, false, true, true, true, false, false, true);
    printf("\n");
}


static const char *num_dist_desc[] = PROFILER_DESC("num-dist",
    "[OPTION...] -e EVENT [--perins] [--than ns] [--heatmap file] [-g]",
    "Numerical distribution.", "",
    "EXAMPLES",
    "    "PROGRAME" num-dist -e sched:sched_stat_runtime help",
    "    "PROGRAME" num-dist -e sched:sched_stat_runtime//num=runtime/ -C 0 -i 1000",
    "    "PROGRAME" num-dist -e 'sched:sched_stat_runtime//num=\"runtime/1000\"/alias=runtime(us)/' -C 0 -i 1000");
static const char *num_dist_argv[] = PROFILER_ARGV("num-dist",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "perins", "than", "heatmap", "call-graph");
static profiler num_dist = {
    .name = "num-dist",
    .desc = num_dist_desc,
    .argv = num_dist_argv,
    .pages = 64,
    .help = num_dist_help,
    .init = num_dist_init,
    .filter = num_dist_filter,
    .deinit = num_dist_exit,
    .interval = num_dist_interval,
    .ftrace_filter = num_dist_ftrace_filter,
    .sample = num_dist_sample,
};
PROFILER_REGISTER(num_dist)


