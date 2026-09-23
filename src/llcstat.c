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
struct llcstat_ctx {
    int nr_ins;
    struct cpuinfo cpuinfo;
    struct perf_evlist *evlist;
    struct perf_evsel *leader;
    struct cache *total_time_enabled;
    struct cache *total_time_running;
    struct cache *l3_cache_references;
    struct cache *l3_cache_misses;
    struct cache *l3_cache_miss_latency;
    struct cache *l3_misses_by_request_type;
    __u64 l3_cache_reference_config;
    __u64 l3_cache_miss_config;
    __u64 l3_cache_miss_latency_config;
    __u64 l3_misses_by_request_type_config;
};

static void llcstat_exit(struct prof_dev *dev);
static int llcstat_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct llcstat_ctx *ctx = zalloc(sizeof(*ctx));
    struct perf_event_attr attr = {
        .type        = PERF_TYPE_HARDWARE,
        .config      = 0,
        .size        = sizeof(struct perf_event_attr),
        .sample_period = 0, //env->trigger_freq,  //每trigger_freq个计数发起一个PMI中断, 发起1个采样.
        .sample_type = 0, //PERF_SAMPLE_TID | PERF_SAMPLE_CPU | PERF_SAMPLE_READ,
        .read_format = 0,
        .exclude_host = env->exclude_host,  //only guest
        .pinned        = 0,
        .disabled      = 1,
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    int type;
    __u64 l3_cache_reference = 0;
    __u64 l3_cache_miss = 0;
    __u64 l3_cache_miss_latency = 0;
    __u64 l3_misses_by_request_type = 0;

    if (!ctx)
        return -1;
    dev->private = ctx;

    if (get_cpuinfo(&ctx->cpuinfo) < 0)
        goto failed;

    if (!prof_dev_oncpu(dev)) {
        fprintf(stderr, "can only be bound to CPU\n");
        goto failed;
    }

    if (env->interval == 0)
        env->interval = 1000;

    if (ctx->cpuinfo.vendor == X86_VENDOR_AMD) {
        int err;
        char *cpumask = NULL;
        size_t size = 0;
        struct perf_cpu_map *cpus = NULL;

        if ((err = sysfs__read_int("bus/event_source/devices/amd_l3/type", &type)) < 0) {
            fprintf(stderr, "failed to read /sys/bus/event_source/devices/amd_l3/type."
                            "Not Supported.\n");
            goto failed;
        }
        if ((err = sysfs__read_str("bus/event_source/devices/amd_l3/cpumask", &cpumask, &size)) < 0 &&
            size == 0) {
            fprintf(stderr, "failed to read /sys/bus/event_source/devices/amd_l3/cpumask."
                            "Not Supported.\n");
            goto failed;
        }
        cpus = perf_cpu_map__new(cpumask);
        dev->cpus = perf_cpu_map__and(dev->cpus, cpus);
        perf_cpu_map__put(cpus);
        free(cpumask);

        if (ctx->cpuinfo.family == 0x17) { // AMD rome
            l3_cache_reference = 0xFF0F00000040FF04UL;
            l3_cache_miss = 0xFF0F000000400104UL;
            l3_cache_miss_latency = 0xFF0F000000400090UL;
            l3_misses_by_request_type = 0xFF0F000000401F9AUL;
        } else if (ctx->cpuinfo.family == 0x19) { // AMD milan
            l3_cache_reference = 0x0300C0000040FF04UL;
            l3_cache_miss = 0x0300C00000400104UL;
            if (ctx->cpuinfo.model >= 0x10) { // AMD Genoa, bergamo
                l3_cache_miss_latency = 0x0303C00000400090UL;
                l3_misses_by_request_type = 0x0303C00000401F9AUL;
            } else {
                l3_cache_miss_latency = 0x0300C00000400090UL;
                l3_misses_by_request_type = 0x0300C00000401F9AUL;
            }
        } else
            goto failed;
    } else {
        type = PERF_TYPE_HARDWARE;
        l3_cache_reference = PERF_COUNT_HW_CACHE_REFERENCES;
        l3_cache_miss = PERF_COUNT_HW_CACHE_MISSES;
    }

    ctx->nr_ins = perf_cpu_map__nr(dev->cpus);
    ctx->total_time_enabled = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->total_time_running = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->l3_cache_references = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->l3_cache_misses = calloc(ctx->nr_ins, sizeof(struct cache));
    if (!ctx->total_time_enabled || !ctx->total_time_running ||
        !ctx->l3_cache_references || !ctx->l3_cache_misses)
        goto failed;

    // PERF_FORMAT_GROUP
    //     Use the leader event to read all counters at once. Read the l3_cache_references event
    //     first, and then read the l3_cache_misses event, which will have an increment.
    //
    // PERF_FORMAT_TOTAL_TIME_ENABLED
    // PERF_FORMAT_TOTAL_TIME_RUNNING
    //    Use the leader event to get the running time of all events.
    //
    attr.type = type;
    attr.read_format = PERF_FORMAT_ID | PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    ctx->l3_cache_reference_config = attr.config = l3_cache_reference;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init l3_cache_reference counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);
    ctx->leader = evsel;
    ctx->evlist = evlist;

    attr.read_format = PERF_FORMAT_ID;
    ctx->l3_cache_miss_config = attr.config = l3_cache_miss;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init l3_cache_miss counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    if (ctx->cpuinfo.vendor != X86_VENDOR_AMD)
        goto set_leader;

    ctx->l3_cache_miss_latency = calloc(ctx->nr_ins, sizeof(struct cache));
    ctx->l3_misses_by_request_type = calloc(ctx->nr_ins, sizeof(struct cache));
    if (!ctx->l3_cache_miss_latency || !ctx->l3_misses_by_request_type)
        goto failed;

    ctx->l3_cache_miss_latency_config = attr.config = l3_cache_miss_latency;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init l3_cache_miss_latency counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    ctx->l3_misses_by_request_type_config = attr.config = l3_misses_by_request_type;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        fprintf(stderr, "failed to init l3_misses_by_request_type counter\n");
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

set_leader:
    perf_evlist__set_leader(evlist);
    return 0;

failed:
    llcstat_exit(dev);
    return -1;
}

static void llcstat_exit(struct prof_dev *dev)
{
    struct llcstat_ctx *ctx = dev->private;
    if(ctx->total_time_enabled) free(ctx->total_time_enabled);
    if(ctx->total_time_running) free(ctx->total_time_running);
    if(ctx->l3_cache_references) free(ctx->l3_cache_references);
    if(ctx->l3_cache_misses) free(ctx->l3_cache_misses);
    if(ctx->l3_cache_miss_latency) free(ctx->l3_cache_miss_latency);
    if(ctx->l3_misses_by_request_type) free(ctx->l3_misses_by_request_type);
    free(ctx);
}

static int llcstat_read(struct prof_dev *dev, struct perf_evsel *evsel, struct perf_counts_values *count, int cpu, int tid)
{
    int cpu_index = perf_cpu_map__idx(dev->cpus, cpu);
    struct llcstat_ctx *ctx = dev->private;
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
    if (c > cache[cpu_index].counter) { \
        cache[cpu_index].incremental = c - cache[cpu_index].counter; \
        cache[cpu_index].counter = c; \
    } else \
        cache[cpu_index].incremental = 0;

    if (cpu_index < 0 || evsel != ctx->leader)
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

        if (config == ctx->l3_cache_reference_config)
            cache = ctx->l3_cache_references;
        else if (config == ctx->l3_cache_miss_config)
            cache = ctx->l3_cache_misses;
        else if (config == ctx->l3_cache_miss_latency_config)
            cache = ctx->l3_cache_miss_latency;
        else if (config == ctx->l3_misses_by_request_type_config)
            cache = ctx->l3_misses_by_request_type;
        else
            continue;

        UPDATE_COUNTER(value);
    }
    return 1;
}

static void llcstat_interval(struct prof_dev *dev)
{
    struct llcstat_ctx *ctx = dev->private;
    int ins;

    print_time(stdout); printf("\n");
    printf("[CPU] L3 %9s %9s  %6s %7s  %12s\n", "REFERENCE", "MISSES", "HIT%", "RUN%", "MISS-LATENCY");
    for (ins = 0; ins < ctx->nr_ins; ins ++) {
        float hit = 0.0;
        float run = 0.0;

        if (ctx->l3_cache_references[ins].incremental > ctx->l3_cache_misses[ins].incremental)
            hit = (ctx->l3_cache_references[ins].incremental - ctx->l3_cache_misses[ins].incremental) * 100.0 /
                   ctx->l3_cache_references[ins].incremental;
        run = ctx->total_time_running[ins].incremental * 100.0 / ctx->total_time_enabled[ins].incremental;
        printf("[%03d]    %9lu %9lu  %5.2f%% %6.2f%%  ", perf_cpu_map__cpu(dev->cpus, ins),
                ctx->l3_cache_references[ins].incremental, ctx->l3_cache_misses[ins].incremental,
                hit, run);
        if (ctx->cpuinfo.vendor == X86_VENDOR_AMD) {
            uint64_t latency = 0;
            if (ctx->l3_misses_by_request_type[ins].incremental > 0)
                latency = ctx->l3_cache_miss_latency[ins].incremental * 16 /
                          ctx->l3_misses_by_request_type[ins].incremental;
            printf("%12lu\n", latency);
        } else
            printf("<not supported>\n");
    }
}


static const char *llcstat_desc[] = PROFILER_DESC("llcstat",
    "[OPTION...] [--exclude-host]",
    "Monitor the last level cache state.", "",
    "EXAMPLES",
    "    "PROGRAME" llcstat -i 1000",
    "    "PROGRAME" llcstat -C 0-3 -i 1000");
static const char *llcstat_argv[] = PROFILER_ARGV("llcstat",
    "OPTION:",
    "cpus",
    "interval", "output", "usage-self",
    "version", "verbose", "quiet", "help",
    "FILTER OPTION:",
    "exclude-host");
static profiler llcstat = {
    .name = "llcstat",
    .desc = llcstat_desc,
    .argv = llcstat_argv,
    .pages = 1,
    .init = llcstat_init,
    .deinit = llcstat_exit,
    .interval = llcstat_interval,
    .read = llcstat_read,
};
PROFILER_REGISTER(llcstat)
