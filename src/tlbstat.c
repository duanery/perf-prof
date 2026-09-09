#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/compiler.h>
#include <api/fs/fs.h>
#include <monitor.h>
#include <tep.h>
#include <stack_helpers.h>

struct cache {
    uint64_t counter;
    uint64_t incremental;
};
struct tlbstat_ctx {
    int nr_ins;
    struct perf_evlist *evlist;
    struct perf_evsel *leader;
    struct cache *total_time_enabled;
    struct cache *total_time_running;
    struct cache *dTLB_load_misses;
    struct cache *dTLB_loads;
    struct cache *dTLB_store_misses;
    struct cache *dTLB_stores;
    __u64 dTLB_load_misses_config;
    __u64 dTLB_loads_config;
    __u64 dTLB_store_misses_config;
    __u64 dTLB_stores_config;
};

#define C(x) PERF_COUNT_HW_CACHE_##x

#define CONFIG(cache_type, cache_op, cache_result) \
    ((C(cache_result)<<16) | (C(cache_op)<<8) | C(cache_type))

static void tlbstat_exit(struct prof_dev *dev);
static int tlbstat_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct tlbstat_ctx *ctx;
    struct perf_event_attr attr = {
        .type        = PERF_TYPE_HW_CACHE,
        .config      = 0,
        .size        = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .sample_type = 0, //PERF_SAMPLE_TID | PERF_SAMPLE_CPU | PERF_SAMPLE_READ,
        .read_format = 0,
        .exclude_host = env->exclude_host,  //only guest
        .pinned        = 0,
        .disabled      = 1,
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    __u64 dTLB_load_misses = 0;
    __u64 dTLB_loads = 0;
    __u64 dTLB_store_misses = 0;
    __u64 dTLB_stores = 0;

    if (!prof_dev_ins_oncpu(dev)) {
        fprintf(stderr, "can only be bound to CPU\n");
        return -1;
    }

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (env->interval == 0)
        env->interval = 1000;

    dTLB_load_misses = CONFIG(DTLB, OP_READ, RESULT_MISS);
    dTLB_loads = CONFIG(DTLB, OP_READ, RESULT_ACCESS);
    dTLB_store_misses = CONFIG(DTLB, OP_WRITE, RESULT_MISS);
    dTLB_stores = CONFIG(DTLB, OP_WRITE, RESULT_ACCESS);

    ctx->nr_ins = perf_cpu_map__nr(dev->cpus);
    ctx->total_time_enabled = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->total_time_running = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->dTLB_load_misses = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->dTLB_loads = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->dTLB_store_misses = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->dTLB_stores = calloc(ctx->nr_ins, sizeof(struct cache));
    if (!ctx->total_time_enabled || !ctx->total_time_running ||
        !ctx->dTLB_load_misses || !ctx->dTLB_loads ||
        !ctx->dTLB_store_misses || !ctx->dTLB_stores)
        goto failed;

    // PERF_FORMAT_GROUP
    //     Use the leader event to read all counters at once.
    //
    // PERF_FORMAT_TOTAL_TIME_ENABLED
    // PERF_FORMAT_TOTAL_TIME_RUNNING
    //    Use the leader event to get the running time of all events.
    //
    attr.read_format = PERF_FORMAT_ID | PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    ctx->dTLB_load_misses_config = attr.config = dTLB_load_misses;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init dTLB_load_misses counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);
    ctx->leader = evsel;
    ctx->evlist = evlist;

    attr.read_format = PERF_FORMAT_ID;
    ctx->dTLB_loads_config = attr.config = dTLB_loads;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init dTLB_loads counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    ctx->dTLB_store_misses_config = attr.config = dTLB_store_misses;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init dTLB_store_misses counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    ctx->dTLB_stores_config = attr.config = dTLB_stores;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init dTLB_stores counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    perf_evlist__set_leader(evlist);
    return 0;

failed:
    tlbstat_exit(dev);
    return -1;
}

static void tlbstat_exit(struct prof_dev *dev)
{
    struct tlbstat_ctx *ctx = dev->private;
    if (ctx->total_time_enabled)
        free(ctx->total_time_enabled);
    if (ctx->total_time_running)
        free(ctx->total_time_running);
    if (ctx->dTLB_load_misses)
        free(ctx->dTLB_load_misses);
    if (ctx->dTLB_loads)
        free(ctx->dTLB_loads);
    if (ctx->dTLB_store_misses)
        free(ctx->dTLB_store_misses);
    if (ctx->dTLB_stores)
        free(ctx->dTLB_stores);
    free(ctx);
}

static int tlbstat_read(struct prof_dev *dev, struct perf_evsel *evsel, struct perf_counts_values *count, int instance)
{
    struct tlbstat_ctx *ctx = dev->private;
    struct perf_counts {
        u64 nr;
        u64 total_time_enabled;
        u64 total_time_running;
        struct {
            u64 value;
            u64 id;
        } ctnr[0];
    } *groups = (void *)count;
    struct cache *cache;
    int i;

    #define UPDATE_COUNTER(c) \
    if (c > cache[instance].counter) { \
        cache[instance].incremental = c - cache[instance].counter; \
        cache[instance].counter = c; \
    } else \
        cache[instance].incremental = 0;

    if (evsel != ctx->leader)
        return 0;

    cache = ctx->total_time_enabled;
    UPDATE_COUNTER(groups->total_time_enabled);

    cache = ctx->total_time_running;
    UPDATE_COUNTER(groups->total_time_running);

    for (i = 0; i < groups->nr; i++) {
        __u64 config;
        u64 value = groups->ctnr[i].value;

        evsel = perf_evlist__id_to_evsel(ctx->evlist, groups->ctnr[i].id, NULL);
        if (!evsel)
            continue;

        config = perf_evsel__attr(evsel)->config;

        if (config == ctx->dTLB_load_misses_config)
            cache = ctx->dTLB_load_misses;
        else if (config == ctx->dTLB_loads_config)
            cache = ctx->dTLB_loads;
        else if (config == ctx->dTLB_store_misses_config)
            cache = ctx->dTLB_store_misses;
        else if (config == ctx->dTLB_stores_config)
            cache = ctx->dTLB_stores;
        else
            continue;

        UPDATE_COUNTER(value);
    }
    return 1;
}

static void tlbstat_interval(struct prof_dev *dev)
{
    struct tlbstat_ctx *ctx = dev->private;
    int ins;

    print_time(stdout); printf("\n");
    printf("[CPU] %9s %9s  %7s %9s %9s  %7s %7s\n", "LOADS", "MISSES", "HIT%", "STORES", "MISSES", "HIT%", "RUN%");
    for (ins = 0; ins < ctx->nr_ins; ins ++) {
        float load_hit = 0.0;
        float store_hit = 0.0;
        float run = 0.0;

        if (!prof_dev_ins_valid(dev, ins))
            continue;
        if (ctx->dTLB_loads[ins].incremental > ctx->dTLB_load_misses[ins].incremental)
            load_hit = (ctx->dTLB_loads[ins].incremental - ctx->dTLB_load_misses[ins].incremental) * 100.0 /
                        ctx->dTLB_loads[ins].incremental;
        if (ctx->dTLB_stores[ins].incremental > ctx->dTLB_store_misses[ins].incremental)
            store_hit = (ctx->dTLB_stores[ins].incremental - ctx->dTLB_store_misses[ins].incremental) * 100.0 /
                        ctx->dTLB_stores[ins].incremental;
        run = ctx->total_time_running[ins].incremental * 100.0 / ctx->total_time_enabled[ins].incremental;
        printf("[%03d] %9lu %9lu  %6.2f%% %9lu %9lu  %6.2f%% %6.2f%%\n", prof_dev_ins_cpu(dev, ins),
                ctx->dTLB_loads[ins].incremental, ctx->dTLB_load_misses[ins].incremental, load_hit,
                ctx->dTLB_stores[ins].incremental, ctx->dTLB_store_misses[ins].incremental, store_hit,
                run);
    }
}


static const char *tlbstat_desc[] = PROFILER_DESC("tlbstat",
    "[OPTION...] [--exclude-host]",
    "Monitor the dTLB state.", "",
    "EXAMPLES",
    "    "PROGRAME" tlbstat -i 1000",
    "    "PROGRAME" tlbstat -C 0-3 -i 1000");
static const char *tlbstat_argv[] = PROFILER_ARGV("tlbstat",
    "OPTION:",
    "cpus",
    "interval", "output", "usage-self",
    "version", "verbose", "quiet", "help",
    "FILTER OPTION:",
    "exclude-host");
static profiler tlbstat = {
    .name = "tlbstat",
    .desc = tlbstat_desc,
    .argv = tlbstat_argv,
    .pages = 1,
    .init = tlbstat_init,
    .deinit = tlbstat_exit,
    .interval = tlbstat_interval,
    .read = tlbstat_read,
};
PROFILER_REGISTER(tlbstat)


