#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/compiler.h>
#include <monitor.h>

struct counter {
    uint64_t accum;
    uint64_t increased;
};
struct hw_counter {
    struct counter total_time_enabled;
    struct counter total_time_running;
    struct counter cycles;
    struct counter insns;
};

struct hwstat_ctx {
    int nr_ins;
    struct perf_evsel *leader;
    struct hw_counter *hwc;
};

static void hwstat_exit(struct prof_dev *dev);
static int hwstat_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct hwstat_ctx *ctx;
    struct perf_event_attr attr = {
        .type        = PERF_TYPE_HARDWARE,
        .config      = 0,
        .size        = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .sample_type = 0,
        .read_format = 0,
        .exclude_host = env->exclude_host,  //only guest
        .pinned       = 0,
        .disabled     = 1,
    };
    struct perf_evsel *evsel;

    if (!prof_dev_oncpu(dev)) {
        fprintf(stderr, "can only be bound to CPU\n");
        return -1;
    }

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (env->interval == 0)
        env->interval = 1000;

    ctx->nr_ins = perf_cpu_map__nr(dev->cpus);
    ctx->hwc = calloc(ctx->nr_ins, sizeof(*ctx->hwc));
    if (!ctx->hwc)
        goto failed;

    // PERF_FORMAT_GROUP
    //     Use the leader event to read all counters at once.
    //
    // PERF_FORMAT_TOTAL_TIME_ENABLED
    // PERF_FORMAT_TOTAL_TIME_RUNNING
    //    Use the leader event to get the running time of all events.
    attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    evsel = perf_evsel__new(&attr);
    if (!evsel) goto failed;
    perf_evlist__add(evlist, evsel);
    ctx->leader = evsel;

    attr.read_format = 0;
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    evsel = perf_evsel__new(&attr);
    if (!evsel) goto failed;
    perf_evlist__add(evlist, evsel);

    perf_evlist__set_leader(evlist);
    return 0;

failed:
    hwstat_exit(dev);
    return -1;
}

static void hwstat_exit(struct prof_dev *dev)
{
    struct hwstat_ctx *ctx = dev->private;
    if (ctx->hwc)
        free(ctx->hwc);
    free(ctx);
}

#define UPDATE(c) \
if (groups->c > hwc->c.accum) {  \
    hwc->c.increased = groups->c - hwc->c.accum; \
    hwc->c.accum = groups->c; \
} else \
     hwc->c.increased = 0; \


static int hwstat_read(struct prof_dev *dev, struct perf_evsel *evsel, struct perf_counts_values *count, int cpu, int tid)
{
    int cpu_index = perf_cpu_map__idx(dev->cpus, cpu);
    struct hwstat_ctx *ctx = dev->private;
    struct perf_counts {
        u64 nr;
        u64 total_time_enabled;
        u64 total_time_running;
        u64 cycles;
        u64 insns;
    } *groups = (void *)count;
    struct hw_counter *hwc;

    if (cpu_index < 0)
        return 0;
    hwc = &ctx->hwc[cpu_index];

    if (evsel != ctx->leader)
        return 0;

    UPDATE(total_time_enabled);
    UPDATE(total_time_running);
    UPDATE(cycles);
    UPDATE(insns);
    return 1;
}

static void hwstat_interval(struct prof_dev *dev)
{
    struct hwstat_ctx *ctx = dev->private;
    int ins;

    print_time(stdout); printf("\n");
    printf("[CPU] %10s %10s   %4s  %7s\n", "CYCLES", "INSNS", "IPC", "RUN%");
    for (ins = 0; ins < ctx->nr_ins; ins ++) {
        float ipc = 0.0;
        float run = 0.0;

        ipc = ctx->hwc[ins].insns.increased * 1.0 / ctx->hwc[ins].cycles.increased;
        run = ctx->hwc[ins].total_time_running.increased * 100.0 / ctx->hwc[ins].total_time_enabled.increased;
        printf("[%03d] %10lu %10lu   %4.2f  %6.2f%%\n", perf_cpu_map__cpu(dev->cpus, ins),
                ctx->hwc[ins].cycles.increased, ctx->hwc[ins].insns.increased, ipc, run);
    }
}

static const char *hwstat_desc[] = PROFILER_DESC("hwstat",
    "[OPTION...] [--exclude-host]",
    "Monitor the hardware state: cycles, IPC.", "",
    "EXAMPLES",
    "    "PROGRAME" hwstat -i 1000",
    "    "PROGRAME" hwstat -C 0-3 -i 1000");
static const char *hwstat_argv[] = PROFILER_ARGV("hwstat",
    "OPTION:",
    "cpus",
    "interval", "output", "usage-self",
    "version", "verbose", "quiet", "help",
    "FILTER OPTION:",
    "exclude-host");
static profiler hwstat = {
    .name = "hwstat",
    .desc = hwstat_desc,
    .argv = hwstat_argv,
    .pages = 0,
    .init = hwstat_init,
    .deinit = hwstat_exit,
    .interval = hwstat_interval,
    .read = hwstat_read,
};
PROFILER_REGISTER(hwstat)
