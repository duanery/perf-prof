#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "monitor.h"
#include "trace_helpers.h"
#include "tep.h"
#include "stack_helpers.h"

struct profile_ctx {
    int nr_ins;
    uint64_t *counter;
    uint64_t *cycles;
    struct {
        uint64_t start_time;
        uint64_t num;
    }*stat;
    struct callchain_ctx *cc;
    struct flame_graph *flame;
    struct bpf_filter filter;
    struct perf_evsel *evsel;
    time_t time;
    char time_str[32];
    int in_guest;
    int tsc_khz;
    int vendor;
    bool only_flame_graph;
};

static void monitor_ctx_exit(struct prof_dev *dev);
static void profile_interval(struct prof_dev *dev);

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ | PERF_SAMPLE_CALLCHAIN
struct sample_type_data {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    __u64 counter;
    struct callchain callchain;
};

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct profile_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    tep__ref();
    ctx->nr_ins = prof_dev_nr_ins(dev);
    ctx->counter = calloc(ctx->nr_ins, sizeof(uint64_t));
    if (!ctx->counter)
        goto failed;

    ctx->cycles = calloc(ctx->nr_ins, sizeof(uint64_t));
    if (!ctx->cycles)
        goto failed;

    ctx->stat = calloc(ctx->nr_ins, sizeof(*ctx->stat));
    if (!ctx->stat)
        goto failed;

    ctx->time = 0;
    ctx->time_str[0] = '\0';
    if (env->callchain) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), stdout);
        if (env->flame_graph) {
            ctx->flame = flame_graph_open(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), env->flame_graph);
            if (env->interval) {
                ctx->only_flame_graph = (env->flame_graph[0] == '\0');
                profile_interval(dev);
            }
        }
    }

    if (bpf_filter_init(&ctx->filter, env)) {
        if (bpf_filter_open(&ctx->filter) < 0)
            goto failed;
    }

    ctx->in_guest = in_guest();
    ctx->vendor = get_cpu_vendor();
    if (!ctx->in_guest && ctx->vendor == X86_VENDOR_INTEL)
        ctx->tsc_khz = get_tsc_khz();

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct profile_ctx *ctx = dev->private;

    if (ctx->counter) free(ctx->counter);
    if (ctx->cycles) free(ctx->cycles);
    if (ctx->stat) free(ctx->stat);
    bpf_filter_close(&ctx->filter);
    if (dev->env->callchain) {
        callchain_ctx_free(ctx->cc);
        if (ctx->flame) {
            flame_graph_output(ctx->flame);
            flame_graph_close(ctx->flame);
        }
    }
    tep__unref();
    free(ctx);
}

static int profile_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct profile_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_HARDWARE,
        .config        = PERF_COUNT_HW_CPU_CYCLES,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = env->freq,
        .freq          = env->freq ? 1 : 0,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_READ |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = 0,
        .pinned        = 0,
        .disabled      = 1,
        .watermark     = 1,
        .exclude_user  = env->exclude_user,
        .exclude_kernel = env->exclude_kernel,
        .exclude_guest = env->exclude_guest,
        .exclude_host = env->exclude_host,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
    };
    struct perf_evsel *evsel;

    if (env->exclude_guest && env->exclude_host)
        return -1;
    if (env->exclude_user && env->exclude_kernel)
        return -1;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    if (ctx->in_guest) {
        attr.type = PERF_TYPE_SOFTWARE;
        attr.config = PERF_COUNT_SW_CPU_CLOCK;
        attr.exclude_idle = 1;
    } else if (ctx->vendor == X86_VENDOR_INTEL) {
        if (ctx->tsc_khz > 0 && env->freq > 0) {
            attr.config = PERF_COUNT_HW_REF_CPU_CYCLES;
            attr.freq = 0;
            attr.sample_period = ctx->tsc_khz * 1000ULL / env->freq;
        }
    }

    if (env->callchain)
        dev->pages *= 2;

    if (env->verbose) {
        printf("tsc_khz = %d\n", ctx->tsc_khz);
    }

    prof_dev_env2attr(dev, &attr);

    evsel = perf_evsel__new(&attr);
    if (!evsel)
        goto failed;

    perf_evlist__add(evlist, evsel);
    ctx->evsel = evsel;
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int profile_filter(struct prof_dev *dev)
{
    struct profile_ctx *ctx = dev->private;
    int err;

    if (ctx->filter.bpf_fd >= 0) {
        err = perf_evsel__set_bpf(ctx->evsel, ctx->filter.bpf_fd);
        if (err < 0)
            return err;
    }
    return 0;
}

static void profile_exit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}

static int profile_read(struct prof_dev *dev, struct perf_evsel *evsel, struct perf_counts_values *count, int instance)
{
    struct profile_ctx *ctx = dev->private;
    uint64_t cycles = 0;
    const char *str_in[] = {"host,guest", "host", "guest", "error"};
    const char *str_mode[] = {"all", "usr", "sys", "error"};
    int in, mode, oncpu;

    if (count->val > ctx->cycles[instance]) {
        cycles = count->val - ctx->cycles[instance];
        ctx->cycles[instance] = count->val;
    }
    if (cycles) {
        in = (dev->env->exclude_host << 1) | dev->env->exclude_guest;
        mode = (dev->env->exclude_user << 1) | dev->env->exclude_kernel;
        print_time(stdout);
        oncpu = prof_dev_ins_oncpu(dev);
        if (ctx->vendor == X86_VENDOR_INTEL && ctx->tsc_khz > 0)
            printf("%s %d [%s] %.2f%% [%s] %lu cycles\n", oncpu ? "cpu" : "thread",
                    oncpu ? prof_dev_ins_cpu(dev, instance) : prof_dev_ins_thread(dev, instance),
                    str_in[in],
                    (float)cycles * 100 / (ctx->tsc_khz * (__u64)dev->env->interval),
                    str_mode[mode], cycles);
        else
            printf("%s %d [%s] [%s] %lu cycles\n", oncpu ? "cpu" : "thread",
                    oncpu ? prof_dev_ins_cpu(dev, instance) : prof_dev_ins_thread(dev, instance),
                    str_in[in], str_mode[mode], cycles);
    }
    return 0;
}

static void profile_print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    struct profile_ctx *ctx = dev->private;
    struct sample_type_data *data = (void *)event->sample.array;
    uint64_t counter = 0;

    if (data->counter > ctx->counter[instance])
        counter = data->counter - ctx->counter[instance];
    ctx->counter[instance] = data->counter;

    if (!(flags & OMIT_TIMESTAMP))
        prof_dev_print_time(dev, data->time, stdout);

    tep__update_comm(NULL, data->tid_entry.tid);
    printf("%16s %6u [%03d] %llu.%06llu: profile: %lu cpu-cycles\n", tep__pid_to_comm(data->tid_entry.tid), data->tid_entry.tid,
                    data->cpu_entry.cpu, data->time / NSEC_PER_SEC, (data->time % NSEC_PER_SEC)/1000, counter);

    if (dev->env->callchain && !(flags & OMIT_CALLCHAIN)) {
        struct callchain_data cd;
        perf_event_build_callchain_data(ctx->evsel, event, &cd);
        print_callchain_data(ctx->cc, &cd);
    }
}

static void profile_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct profile_ctx *ctx = dev->private;
    struct sample_type_data *data = (void *)event->sample.array;
    uint64_t counter = 0;
    int print = 1;

    if (data->counter > ctx->counter[instance])
        counter = data->counter - ctx->counter[instance];
    ctx->counter[instance] = data->counter;

    if (dev->env->greater_than) {
        uint64_t time = ctx->stat[instance].start_time;
        ctx->stat[instance].num ++;
        if (data->time - time >= NSEC_PER_SEC) {
            print = 0;
            ctx->stat[instance].start_time = data->time;
            ctx->stat[instance].num = 1;
        } else {
            int x = (dev->env->freq * dev->env->greater_than + 99) / 100;
            if (ctx->stat[instance].num < x)
                print = 0;
        }
    }

    if (print) {
        if (!ctx->only_flame_graph) {
            prof_dev_print_time(dev, data->time, stdout);
            tep__update_comm(NULL, data->tid_entry.tid);
            printf("%16s %6u [%03d] %llu.%06llu: profile: %lu cpu-cycles\n", tep__pid_to_comm(data->tid_entry.tid), data->tid_entry.tid,
                            data->cpu_entry.cpu, data->time / NSEC_PER_SEC, (data->time % NSEC_PER_SEC)/1000, counter);
        }
        if (dev->env->callchain) {
            if (!dev->env->flame_graph) {
                struct callchain_data cd;
                perf_event_build_callchain_data(ctx->evsel, event, &cd);
                print_callchain_data(ctx->cc, &cd);
            } else {
                const char *comm = tep__pid_to_comm((int)data->tid_entry.pid);
                flame_graph_add_callchain_at_time(ctx->flame, &data->callchain, data->tid_entry.pid,
                                                  !strcmp(comm, "<...>") ? NULL : comm,
                                                  ctx->time, ctx->time_str);
            }
        }
    }
}

static void profile_interval(struct prof_dev *dev)
{
    struct profile_ctx *ctx = dev->private;

    if (ctx->flame) {
        if (!ctx->only_flame_graph) {
            ctx->time = time(NULL);
            strftime(ctx->time_str, sizeof(ctx->time_str), "%Y-%m-%d;%H:%M:%S", localtime(&ctx->time));
        }
        flame_graph_output(ctx->flame);
        flame_graph_reset(ctx->flame);
    }
}

static const char *profile_desc[] = PROFILER_DESC("profile",
    "[OPTION...] -F freq [-g [--flame-graph file [-i INT]]] [--than percent]",
    "CPU sampling profiler for performance analysis.",
    "",
    "SYNOPSIS",
    "    A CPU sampling profiler that periodically samples at a specified frequency.",
    "    Supports kernel-level filtering (PMU, eBPF, callchain) to filter sampled events.",
    "    Supports call stack recording and flame graph generation.",
    "    eBPF filtering enables precise sampling within problem boundaries.",
    "",
    "EXAMPLES",
    "    "PROGRAME" profile -F 997 -p 2347 -g --flame-graph cpu",
    "            Sample specific process, generate flame graph",
    "    "PROGRAME" profile -F 997 -C 0-3 --than 30 -g --flame-graph cpu",
    "            Sample CPUs 0-3, show only hotspots (>=30% usage)",
    "    "PROGRAME" profile -F 997 --exclude-user -g --flame-graph kernel",
    "            Sample kernel only, exclude user space",
    "    "PROGRAME" profile -F 997 --prio 0-99 -g --flame-graph rt_cpu",
    "            Sample real-time priority processes only",
    "    "PROGRAME" profile -F 997 --irqs_disabled -g --flame-graph irqoff",
    "            Sample when interrupts are disabled",
    "    "PROGRAME" profile -F 997 --tif_need_resched -g --flame-graph needresched",
    "            Sample when TIF_NEED_RESCHED is set");
static const char *profile_argv[] = PROFILER_ARGV("profile",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_FILTER,
    PROFILER_ARGV_PROFILER, "freq", "call-graph", "flame-graph",
    "than\nPercentage, at the frequency specified by --freq",
    "prio\nebpf, schedule priority, 0-139");
struct monitor profile = {
    .name = "profile",
    .desc = profile_desc,
    .argv = profile_argv,
    .pages = 2,
    .init = profile_init,
    .filter = profile_filter,
    .deinit = profile_exit,
    .interval = profile_interval,
    .print_event = profile_print_event,
    .sample = profile_sample,
};
PROFILER_REGISTER(profile);

static int cpu_util_init(struct prof_dev *dev)
{
    if (in_guest()) {
        fprintf(stderr, "cpu-util not support in guest\n");
        return -1;
    }
    return profile_init(dev);
}

static const char *cpu_util_desc[] = PROFILER_DESC("cpu-util",
    "[OPTION...] [--exclude-*] [-G]",
    "Report CPU utilization for guest or host.", "",
    "SYNOPSIS",
    "    Based on profile. See '"PROGRAME" profile -h' for more information.", "",
    "EXAMPLES",
    "    "PROGRAME" cpu-util -C 1-4 -i 1000",
    "    "PROGRAME" cpu-util -C 1-4 -G -i 1000");
static const char *cpu_util_argv[] = PROFILER_ARGV("cpu-util",
    PROFILER_ARGV_OPTION,
    "FILTER OPTION:",
    "exclude-host", "exclude-guest", "exclude-user", "exclude-kernel");
struct monitor cpu_util = {
    .name = "cpu-util",
    .desc = cpu_util_desc,
    .argv = cpu_util_argv,
    .pages = 0,
    .init = cpu_util_init,
    .deinit = profile_exit,
    .read   = profile_read,
};
PROFILER_REGISTER(cpu_util);

