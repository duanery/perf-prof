#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <api/fs/fs.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>

#define STAGE_INIT 0
#define STAGE_MONITOR 1
struct monitor stage_init;

struct watchdog_info {
    int watchdog_running;
    void *watchdog_hrtimer;
    unsigned long watchdog_touch_ts;
    unsigned long hrtimer_interrupts;
    unsigned long hrtimer_interrupts_saved;
    __u64 hrtimer_touch_ts;

    int print_stack;
    int print_sched;
};
struct watchdog_ctx {
    int stage;
    struct prof_dev *dev_watchdog;
    struct perf_evsel *perf_evsel_hrtimer_expire_entry;
    struct callchain_ctx *cc;
    int in_guest;
    int nr_cpus;
    struct watchdog_info *watchdog;
    int nr_watchdog;
    int watchdog_thresh;
    __u64 hrtimer_expire_entry;
    __u64 hrtimer_start;
    __u64 hrtimer_cancel;
    __u64 sched_switch;
    __u32 profile_type;
};

static void monitor_ctx_exit(struct prof_dev *dev);

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct watchdog_ctx *ctx = zalloc(sizeof(*ctx));
    char *cpumask = NULL;
    char *str = NULL;
    size_t len;

    if (!ctx)
        return -1;
    dev->private = ctx;
    ctx->stage = STAGE_INIT;

    tep__ref();
    if (env->callchain) {
        ctx->cc = callchain_ctx_new(CALLCHAIN_KERNEL, stdout);
        dev->pages *= 2;
    }
    ctx->in_guest = in_guest();

    ctx->nr_cpus = get_present_cpus();
    ctx->watchdog = calloc(ctx->nr_cpus, sizeof(struct watchdog_info));
    if (ctx->watchdog == NULL)
        goto failed;
    ctx->nr_watchdog = 0;

    if (procfs__read_str("sys/kernel/watchdog_cpumask", &cpumask, &len) == 0) {
        struct perf_cpu_map *cpus = NULL;

        cpus = perf_cpu_map__new(cpumask);
        if (cpus)
            cpus = perf_cpu_map__and(cpus, dev->cpus);
        if (cpus) {
            perf_cpu_map__put(dev->cpus);
            dev->cpus = cpus;
            free(env->cpumask);
            env->cpumask = perf_cpu_map__string(cpus);
        }
        free(cpumask);
    }

    procfs__read_str("sys/kernel/watchdog_thresh", &str, &len);
    if (str) {
        ctx->watchdog_thresh = strtol(str, NULL, 10);
        free(str);
    } else
        goto failed;

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct watchdog_ctx *ctx = dev->private;

    if (dev->env->callchain) {
        callchain_ctx_free(ctx->cc);
    }
    if (ctx->watchdog)
        free(ctx->watchdog);
    ctx->watchdog = NULL;
    tep__unref();
    free(ctx);
}

static struct perf_evsel *perf_tp_event(struct perf_evlist *evlist, const char *sys, const char *name)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW,
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .wakeup_events = 1,
        //.use_clockid   = 1,
        //.clockid       = CLOCK_MONOTONIC,
    };
    struct perf_evsel *evsel;
    int id;

    id = tep__event_id(sys, name);
    if (id < 0)
        return NULL;

    attr.config = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        return NULL;
    }
    perf_evlist__add(evlist, evsel);

    return evsel;
}

static int watchdog_stage_init(struct prof_dev *dev)
{
    struct watchdog_ctx *ctx;
    struct perf_evsel *evsel;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    evsel = perf_tp_event(dev->evlist, "timer", "hrtimer_expire_entry");
    if (!evsel)
        goto failed;
    ctx->perf_evsel_hrtimer_expire_entry = evsel;
    ctx->hrtimer_expire_entry = perf_evsel__attr(evsel)->config;

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void watchdog_exit(struct prof_dev *dev);
static int watchdog_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct prof_dev *dev_init;
    struct env *env_init;
    struct watchdog_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_HARDWARE,
        .config        = PERF_COUNT_HW_CPU_CYCLES,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = env->freq,
        .freq          = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 0,
        .disabled      = 1,
        .exclude_callchain_user = 1,
        .wakeup_events = 1,
    };

    struct perf_evsel *evsel;

    env_init = clone_env(env);

    dev_init = prof_dev_open_cpu_thread_map(&stage_init, env_init, NULL, NULL, dev);
    if (!dev_init)
        return -1;

    /*
     * watchdog and stage_init share the same watchdog_ctx.
     * init by stage_init, exit by watchdog.
     */
    ctx = dev_init->private;
    ctx->dev_watchdog = dev;
    dev->private = ctx;
    dev->pages = dev_init->pages;
    perf_cpu_map__put(dev->cpus);
    dev->cpus = perf_cpu_map__get(dev_init->cpus);
    dev->state = PROF_DEV_STATE_OFF; // Keep off, enable in watchdog_stage_sample().
    if (env_init->cpumask)
        env->cpumask = strdup(env_init->cpumask);

    if (ctx->in_guest) {
        attr.type = PERF_TYPE_SOFTWARE;
        attr.config = PERF_COUNT_SW_CPU_CLOCK;
    }

    if (prof_dev_enable(dev_init) < 0)
        goto failed;

    evsel = perf_tp_event(evlist, "timer", "hrtimer_expire_entry");
    if (!evsel)
        goto failed;
    ctx->hrtimer_expire_entry = perf_evsel__attr(evsel)->config;

    evsel = perf_tp_event(evlist, "timer", "hrtimer_start");
    if (!evsel)
        goto failed;
    ctx->hrtimer_start = perf_evsel__attr(evsel)->config;

    evsel = perf_tp_event(evlist, "timer", "hrtimer_cancel");
    if (!evsel)
        goto failed;
    ctx->hrtimer_cancel = perf_evsel__attr(evsel)->config;

    evsel = perf_tp_event(evlist, "sched", "sched_switch");
    if (!evsel)
        goto failed;
    ctx->sched_switch = perf_evsel__attr(evsel)->config;

    evsel = perf_evsel__new(&attr);
    if (!evsel)
        goto failed;
    perf_evlist__add(evlist, evsel);
    ctx->profile_type = attr.type;

    return 0;

failed:
    watchdog_exit(dev);
    return -1;
}


static int watchdog_stage_filter(struct prof_dev *dev)
{
    struct watchdog_ctx *ctx = dev->private;
    struct ksyms *ksyms;
    const struct ksym *ksym;
    char filter[64];
    int err = -1;

    ksyms = ksyms__load();
    if (!ksyms)
        return -1;

    ksym = ksyms__get_symbol(ksyms, "watchdog_timer_fn", NULL);
    if (!ksym)
        goto failed;

    snprintf(filter, sizeof(filter), "function==0x%lx", ksym->addr);
    err = perf_evsel__apply_filter(ctx->perf_evsel_hrtimer_expire_entry, filter);

failed:
    ksyms__free(ksyms);
    return err;
}

static int watchdog_filter(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct watchdog_ctx *ctx = dev->private;
    struct perf_evsel *evsel;
    char filter[64];
    int err;

    if (ctx->stage == STAGE_MONITOR) {
        perf_evlist__for_each_evsel(evlist, evsel) {
            if (perf_evsel__attr(evsel)->type == PERF_TYPE_TRACEPOINT) {
                __u64 config = perf_evsel__attr(evsel)->config;

                if (config == ctx->hrtimer_expire_entry ||
                    config == ctx->hrtimer_start ||
                    config == ctx->hrtimer_cancel) {
                    int cpu, idx;
                    struct perf_cpu_map *cpus = perf_evsel__cpus(evsel);
                    perf_cpu_map__for_each_cpu(cpu, idx, cpus) {
                        snprintf(filter, sizeof(filter), "hrtimer==%p", ctx->watchdog[cpu].watchdog_hrtimer);
                        perf_evsel__apply_filter_cpu(evsel, filter, idx);
                        if (dev->env->verbose)
                            printf("filter %s for cpu %d\n", filter, idx);
                    }
                } else if (config == ctx->sched_switch) {
                    // Starting from 4.19, softlockup uses cpu_stop_threads.
                    if (kernel_release() >= KERNEL_VERSION(4, 19, 0))
                        snprintf(filter, sizeof(filter), "next_comm~\"migration/*\"");
                    else
                        snprintf(filter, sizeof(filter), "next_comm~\"watchdog/*\"");
                    err = perf_evsel__apply_filter(evsel, filter);
                    if (err < 0)
                        return err;
                    if (dev->env->verbose)
                        printf("filter %s\n", filter);
                }
            }
        }
    }
    return 0;
}

static void watchdog_stage_deinit(struct prof_dev *dev)
{

}

static void watchdog_exit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW,
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

static void watchdog_stage_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct watchdog_ctx *ctx = dev->private;
    struct sample_type_raw *data = (void *)event->sample.array;

    tep__update_comm(NULL, data->h.tid_entry.tid);
    tep__print_event(data->h.time, data->h.cpu_entry.cpu, data->raw.data, data->raw.size);

    if (ctx->watchdog[data->h.cpu_entry.cpu].watchdog_hrtimer == NULL) {
        struct tep_record record;
    	struct trace_seq s;
        struct tep_event *e;
        unsigned long long hrtimer;

        memset(&record, 0, sizeof(record));
        record.ts = data->h.time/1000;
        record.cpu = data->h.cpu_entry.cpu;
        record.size = data->raw.size;
        record.data = data->raw.data;

        trace_seq_init(&s);
        e = tep_find_event_by_record(tep__ref(), &record);
        tep__unref();
        if (tep_get_field_val(&s, e, "hrtimer", &record, &hrtimer, 1) < 0) {
            trace_seq_putc(&s, '\n');
            trace_seq_do_fprintf(&s, stderr);
            trace_seq_destroy(&s);
            return;
        }
        trace_seq_destroy(&s);
        ctx->watchdog[data->h.cpu_entry.cpu].watchdog_hrtimer = (void *)hrtimer;
        ctx->nr_watchdog ++;
        if (ctx->nr_watchdog == perf_cpu_map__nr(dev->cpus)) {
            print_time(stdout);
            printf(" == collect all %d watchdog hrtimer\n", ctx->nr_watchdog);

            ctx->stage = STAGE_MONITOR;
            ctx->perf_evsel_hrtimer_expire_entry = NULL;
            prof_dev_close(dev);
            watchdog_filter(ctx->dev_watchdog);
            prof_dev_enable(ctx->dev_watchdog);
        }
    }
}

static int get_softlockup_thresh(struct watchdog_ctx *ctx)
{
	return ctx->watchdog_thresh * 2;
}
static unsigned long get_timestamp(__u64 time)
{
	return time >> 30LL;  /* 2^30 ~= 10^9 */
}
static unsigned long hrtimer_sample_period(struct watchdog_ctx *ctx)
{
	/*
	 * convert watchdog_thresh from seconds to ns
	 * the divide by 5 is to give hrtimer several chances (two
	 * or three with the current relation between the soft
	 * and hard thresholds) to increment before the
	 * hardlockup detector generates a warning
	 */
	return get_softlockup_thresh(ctx) * ((__u64)NSEC_PER_SEC / 5);
}
static unsigned long sample_period(struct watchdog_ctx *ctx)
{
	/*
	 * convert watchdog_thresh from seconds to ns
	 * the divide by 5 is to give hrtimer several chances (two
	 * or three with the current relation between the soft
	 * and hard thresholds) to increment before the
	 * hardlockup detector generates a warning
	 */
	/*get_softlockup_thresh() * ((u64)NSEC_PER_SEC / 5);*/
	return get_softlockup_thresh(ctx) / 5;
}
static int will_hardlockup(struct prof_dev *dev, __u32 cpu, __u64 now)
{
    struct watchdog_ctx *ctx = dev->private;
    unsigned long thresh;

    if (!ctx->watchdog[cpu].watchdog_running ||
        !ctx->watchdog[cpu].hrtimer_touch_ts)
        return 0;

    thresh = hrtimer_sample_period(ctx) + (NSEC_PER_SEC / 5);

    if (ctx->watchdog[cpu].hrtimer_interrupts == ctx->watchdog[cpu].hrtimer_interrupts_saved &&
        now - ctx->watchdog[cpu].hrtimer_touch_ts > thresh) {
        print_time(stdout);
        printf("WILL: hard lockup - CPU#%u [%llu - %llu > %lu]\n", cpu, now,
                    ctx->watchdog[cpu].hrtimer_touch_ts, thresh);
        return 1;
    } else if (dev->env->verbose) {
        print_time(stdout);
        printf("DEBUG: hard lockup - CPU#%u [%llu - %llu <= %lu]\n", cpu, now,
                    ctx->watchdog[cpu].hrtimer_touch_ts, thresh);
    }
    return 0;
}
static int will_softlockup(struct prof_dev *dev, __u32 cpu, __u64 now)
{
    struct watchdog_ctx *ctx = dev->private;

    if (!ctx->watchdog[cpu].watchdog_running ||
        !ctx->watchdog[cpu].watchdog_touch_ts)
        return 0;

    if (get_timestamp(now) - ctx->watchdog[cpu].watchdog_touch_ts > sample_period(ctx)) {
        print_time(stdout);
        printf("WILL: soft lockup - CPU#%u [%lu - %lu > %lu]\n", cpu, get_timestamp(now),
                    ctx->watchdog[cpu].watchdog_touch_ts, sample_period(ctx));
        return 1;
    } else if (dev->env->verbose) {
        print_time(stdout);
        printf("DEBUG: soft lockup - CPU#%u [%lu - %lu <= %lu]\n", cpu, get_timestamp(now),
                    ctx->watchdog[cpu].watchdog_touch_ts, sample_period(ctx));
    }
    return 0;
}

static void __print_callchain(struct prof_dev *dev, union perf_event *event)
{
    struct watchdog_ctx *ctx = dev->private;
    struct sample_type_callchain *data = (void *)event->sample.array;

    if (dev->env->callchain) {
        print_callchain_common(ctx->cc, &data->callchain, 0/*only kernel stack*/);
    }
}

static void watchdog_sample(struct prof_dev *dev, union perf_event *event, int bind_cpu, int bind_tid)
{
    struct watchdog_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct perf_evsel *evsel;
    __u32 cpu = data->cpu_entry.cpu;
    __u32 type;
    __u64 config;

    evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    if (unlikely(!evsel))
        return ;

    type = perf_evsel__attr(evsel)->type;
    config = perf_evsel__attr(evsel)->config;

    if (type == ctx->profile_type) {
        if (will_hardlockup(dev, cpu, data->time)) {
            ctx->watchdog[cpu].print_stack = 1;
        } else {
            ctx->watchdog[cpu].print_stack = 0;
        }
        ctx->watchdog[cpu].hrtimer_interrupts_saved = ctx->watchdog[cpu].hrtimer_interrupts;

        if (ctx->watchdog[cpu].print_stack ||
            ctx->watchdog[cpu].print_sched ||
            dev->env->verbose >= VERBOSE_EVENT) {
            if (dev->print_title) prof_dev_print_time(dev, data->time, stdout);
            printf("%16s %6u [%03d] %llu.%06llu: watchdog: cpu-cycles\n", tep__pid_to_comm(data->tid_entry.tid), data->tid_entry.tid,
                    data->cpu_entry.cpu, data->time / NSEC_PER_SEC, (data->time % NSEC_PER_SEC)/1000);
            __print_callchain(dev, event);
            fflush(stdout);
            fsync(fileno(stdout));
        }
    } else if (type == PERF_TYPE_TRACEPOINT) {
        if (config == ctx->hrtimer_expire_entry) {
            ctx->watchdog[cpu].watchdog_running = 1;
            ctx->watchdog[cpu].hrtimer_interrupts ++;
            ctx->watchdog[cpu].hrtimer_touch_ts = data->time;
            if (will_softlockup(dev, cpu, data->time)) {
                ctx->watchdog[cpu].print_sched = 1;
            } else {
                ctx->watchdog[cpu].print_sched = 0;
            }
        } else if (config == ctx->hrtimer_start) {
            ctx->watchdog[cpu].watchdog_running = 1;
            ctx->watchdog[cpu].hrtimer_touch_ts = data->time;
        } else if (config == ctx->hrtimer_cancel) {
            ctx->watchdog[cpu].watchdog_running = 0;
        }

        if (ctx->watchdog[cpu].print_sched ||
            dev->env->verbose >= VERBOSE_EVENT) {
            struct sample_type_raw *raw = (void *)event->sample.array;
            if (dev->print_title) prof_dev_print_time(dev, data->time, stdout);
            tep__update_comm(NULL, data->tid_entry.tid);
            tep__print_event(data->time, data->cpu_entry.cpu, raw->raw.data, raw->raw.size);
            fflush(stdout);
            fsync(fileno(stdout));
        }

        if (config == ctx->sched_switch) {
            ctx->watchdog[cpu].watchdog_touch_ts = get_timestamp(data->time);
            ctx->watchdog[cpu].print_sched = 0; // No more softlockup.
        }
    }
}

static void watchdog_throttle(struct prof_dev *dev, union perf_event *event, int bind_cpu, int bind_tid)
{
    struct watchdog_ctx *ctx = dev->private;
    struct perf_evsel *evsel;
    int cpu;
    const char *str;
    __u32 type;
    __u64 time;

    if (!dev->env->verbose)
        return;

    evsel = perf_evlist__id_to_evsel(dev->evlist, event->throttle.id, &cpu);
    if (!evsel)
        return;

    type = event->header.type;
    time = event->throttle.time;
    if (type == PERF_RECORD_THROTTLE) {
        str = "throttle";
    } else if (type == PERF_RECORD_UNTHROTTLE) {
        str = "unthrottle";
    } else
        return;

    type = perf_evsel__attr(evsel)->type;
    if (type == ctx->profile_type) {
        prof_dev_print_time(dev, time, stdout);
        printf("==> [%03d] %llu.%06llu: %s\n", cpu, time / NSEC_PER_SEC, (time % NSEC_PER_SEC)/1000, str);
    } else if (type == PERF_TYPE_TRACEPOINT) {
        /* This won't happen */
    }
}

struct monitor stage_init = {
    .name = "watchdog-init",
    .pages = 2,
    .init = watchdog_stage_init,
    .filter = watchdog_stage_filter,
    .deinit = watchdog_stage_deinit,
    .sample = watchdog_stage_sample,
};

static const char *watchdog_desc[] = PROFILER_DESC("watchdog",
    "[OPTION...] [-F freq] [-g]",
    "Detect hard lockup and soft lockup.", "",
    "EXAMPLES",
    "    "PROGRAME" watchdog -F 1 -g",
    "    "PROGRAME" watchdog -C 0 -F 1 -g -v");
static const char *watchdog_argv[] = PROFILER_ARGV("watchdog",
    "OPTION:",
    "cpus", "output", "mmap-pages", "exit-N", "usage-self",
    "version", "verbose", "quiet", "help",
    PROFILER_ARGV_PROFILER, "freq", "call-graph");
struct monitor watchdog = {
    .name = "watchdog",
    .desc = watchdog_desc,
    .argv = watchdog_argv,
    .pages = 2,
    .init = watchdog_init,
    .filter = watchdog_filter,
    .deinit = watchdog_exit,
    .sample = watchdog_sample,
    .throttle = watchdog_throttle,
    .unthrottle = watchdog_throttle,
};
MONITOR_REGISTER(watchdog)

