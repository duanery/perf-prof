#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/rblist.h>
#include <api/fs/fs.h>
#include <monitor.h>
#include <tep.h>
#include <tp_struct.h>

#define min(x, y) ({                \
    typeof(x) _min1 = (x);          \
    typeof(y) _min2 = (y);          \
    (void) (&_min1 == &_min2);      \
    _min1 < _min2 ? _min1 : _min2; })


struct runtime {
    struct rb_node rbn;
    int binding_id;
    union {
        int another;
        int cpu;
        int tid;
    };
    u64 runtime;
    u64 nr_run;
    u64 max;
    char comm[16];
};

struct oncpu_ctx {
    bool tid_to_cpumap;
    struct prof_bindings bindings;
    int nr_cpus;
    struct perf_cpu_map *prio_map;
    struct rblist runtimes;
    int *percpu_thread_siblings;
};

struct oncpu_binding {
    struct prof_binding binding;
    u64 running_time;
    int pid;
};

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW
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
    __u64       period;
    //PERF_SAMPLE_RAW
    struct {
        __u32   size;
        union {
            __u8    data[0];
            struct sched_stat_runtime runtime;
            struct sched_switch sched_switch;
        };
    } __packed raw;
};

struct runtime_entry {
    int binding_id;
    union {
        int another;
        int cpu;
        int tid;
    };
    char *comm;
};

static int runtime_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct runtime *run = rb_entry(rbn, struct runtime, rbn);
    const struct runtime_entry *e = entry;

    // tid
    if (run->binding_id > e->binding_id)
        return 1;
    else if (run->binding_id < e->binding_id)
        return -1;

    // cpu
    if (run->another > e->another)
        return 1;
    else if (run->another < e->another)
        return -1;

    return 0;
}

static int runtime_node_cmp_comm(struct rb_node *rbn, const void *entry)
{
    struct runtime *run = rb_entry(rbn, struct runtime, rbn);
    const struct runtime_entry *e = entry;

    // cpu
    if (run->binding_id > e->binding_id)
        return 1;
    else if (run->binding_id < e->binding_id)
        return -1;

    // only-comm
    if (e->another == 0)
        return strcmp(run->comm, e->comm);

    // tid
    if (run->another > e->another)
        return 1;
    else if (run->another < e->another)
        return -1;

    return 0;
}

static int runtime_binding_id_cmp(const void *entry, const struct rb_node *rbn)
{
    const struct runtime_entry *e = entry;
    struct runtime *run = rb_entry(rbn, struct runtime, rbn);

    return e->binding_id - run->binding_id;
}

static struct rb_node *runtime_node_new(struct rblist *rlist, const void *new_entry)
{
    const struct runtime_entry *e = new_entry;
    struct runtime *run = malloc(sizeof(*run));
    if (run) {
        RB_CLEAR_NODE(&run->rbn);
        run->binding_id = e->binding_id;
        run->another = e->another;
        run->runtime = 0;
        run->nr_run = 0;
        run->max = 0;
        memcpy(run->comm, e->comm, 16);
        return &run->rbn;
    }
    return NULL;
}

static void runtime_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct runtime *run = rb_entry(rb_node, struct runtime, rbn);
    free(run);
}

static void empty(struct rblist *rblist, struct rb_node *rb_node)
{
}

static int runtime_sorted_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct runtime *run = rb_entry(rbn, struct runtime, rbn);
    struct runtime *e = rb_entry(entry, struct runtime, rbn);

    if (run->binding_id > e->binding_id)
        return 1;
    else if (run->binding_id < e->binding_id)
        return -1;

    if (run->runtime > e->runtime)
        return -1;
    else if (run->runtime < e->runtime)
        return 1;

    if (run->another > e->another)
        return 1;
    else if (run->another < e->another)
        return -1;

    return 0;
}

static struct rb_node *runtime_sorted_node_new(struct rblist *rlist, const void *new_entry)
{
    struct rb_node *n = (void *)new_entry;

    RB_CLEAR_NODE(n);
    return n;
}


static int read_cpu_thread_sibling(int cpu)
{
    struct perf_cpu_map *cpumap;
    char buff[PATH_MAX];
    char *cpu_list;
    size_t len = 0;
    int err, c, idx;
    int thread_sibling = -1;

    snprintf(buff, sizeof(buff), "devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    if ((err = sysfs__read_str(buff, &cpu_list, &len)) < 0 ||
        len == 0) {
        fprintf(stderr, "failed to read %s, %d Not Supported.\n", buff, err);
        return -1;
    }
    cpu_list[len] = '\0';
    cpumap = perf_cpu_map__new(cpu_list);

    perf_cpu_map__for_each_cpu(c, idx, cpumap) {
        if (c < 0) {
            fprintf(stderr, "cpu < 0 %s, Not Supported.\n", cpu_list);
            free(cpu_list);
            return -1;
        }
        if (c == cpu)
            continue;
        thread_sibling = c;
        break;
    }
    perf_cpu_map__put(cpumap);
    free(cpu_list);
    return thread_sibling;
}

static int read_sched_vmf_sib(int thread)
{
    char path[64];
    char buf[32];
    int fd, len, vmf_sib;

    snprintf(path, sizeof(path), "/proc/%d/sched_vmf_sib", thread);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    len = (int)read(fd, buf, sizeof(buf));
    close(fd);
    if (len <= 0) return -1;
    len--;
    if (buf[len] == '\n' || len == sizeof(buf)-1)
        buf[len] = '\0';

    vmf_sib = atoi(buf);

    return vmf_sib;
}

static void oncpu_exit(struct prof_dev *dev);
static int oncpu_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct oncpu_ctx *ctx = zalloc(sizeof(*ctx));
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
        .wakeup_watermark = (dev->pages << 12) / 2,
    };
    struct perf_evsel *evsel;
    int i;

    if (!ctx)
        return -1;
    dev->private = ctx;
    ctx->tid_to_cpumap = !prof_dev_oncpu(dev);

    if (env->prio_map) {
        if (ctx->tid_to_cpumap)
            fprintf(stderr, "WARN: --prio is only valid when bound to cpu\n");
        else if (env->filter && env->filter[0]) {
            fprintf(stderr, "--prio and --filter are mutually exclusive\n");
            goto free_ctx;
        }
        ctx->prio_map = perf_cpu_map__new(env->prio_map);
        if (!ctx->prio_map)
            goto free_ctx;
    }

    if (!env->interval)
        env->interval = 1000;

    if (env->verbose)
        tep__ref();
    else
        tep__ref_light();

    ctx->bindings.size = sizeof(struct oncpu_binding);
    ctx->nr_cpus = get_present_cpus();

    rblist__init(&ctx->runtimes);
    ctx->runtimes.node_cmp = ctx->tid_to_cpumap ? runtime_node_cmp : runtime_node_cmp_comm;
    ctx->runtimes.node_new = runtime_node_new;
    ctx->runtimes.node_delete = runtime_node_delete;

    if (ctx->tid_to_cpumap && env->detail) {
        ctx->percpu_thread_siblings = calloc(ctx->nr_cpus, sizeof(int));
        if (!ctx->percpu_thread_siblings)
            goto failed;
        for (i = 0; i < ctx->nr_cpus; i++) {
            ctx->percpu_thread_siblings[i] = read_cpu_thread_sibling(i);
            if (ctx->percpu_thread_siblings[i] == -1) {
                free(ctx->percpu_thread_siblings);
                ctx->percpu_thread_siblings = NULL;
                break;
            }
        }

    }

    prof_dev_env2attr(dev, &attr);

    if (ctx->tid_to_cpumap)
        attr.config = tep__event_id("sched", "sched_stat_runtime");
    else
        attr.config = tep__event_id("sched", "sched_switch");
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    return 0;

failed:
    oncpu_exit(dev);
    return -1;
free_ctx:
    free(ctx);
    return -1;
}

static int oncpu_filter(struct prof_dev *dev)
{
    struct oncpu_ctx *ctx = dev->private;
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct perf_evsel *evsel;
    int err = 0;

    if (env->filter && env->filter[0]) {
        perf_evlist__for_each_evsel(evlist, evsel) {
            err = perf_evsel__apply_filter(evsel, env->filter);
            if (err < 0)
                break;
        }
        return err;
    }
    if (ctx->prio_map && !ctx->tid_to_cpumap) {
        // sched:sched_switch
        char *prev_filter = cpu_filter(ctx->prio_map, "prev_prio");
        char *next_filter = cpu_filter(ctx->prio_map, "next_prio");
        char *filter;

        asprintf(&filter, "(%s) || (%s)", prev_filter, next_filter);
        free(prev_filter);
        free(next_filter);
        if (env->verbose >= VERBOSE_NOTICE)
            printf("filter: %s\n", filter);

        perf_evlist__for_each_evsel(evlist, evsel) {
            err = perf_evsel__apply_filter(evsel, filter);
            if (err < 0)
                break;
        }
        free(filter);
        return err;
    }
    return 0;
}

static void oncpu_exit(struct prof_dev *dev)
{
    struct oncpu_ctx *ctx = dev->private;
    rblist__exit(&ctx->runtimes);
    prof_bindings_exit(&ctx->bindings);
    if (ctx->prio_map)
        perf_cpu_map__put(ctx->prio_map);
    if (ctx->percpu_thread_siblings)
        free(ctx->percpu_thread_siblings);
    tep__unref();
    free(ctx);
}

static int oncpu_del_thread(struct prof_dev *dev, pid_t tid)
{
    struct oncpu_ctx *ctx = dev->private;
    prof_bindings_remove_thread(&ctx->bindings, tid);
    return 0;
}

static void oncpu_lost(struct prof_dev *dev, union perf_event *event, int cpu, int tid, u64 lost_start, u64 lost_end)
{
    struct oncpu_ctx *ctx = dev->private;
    struct oncpu_binding *state = prof_binding_find(&ctx->bindings, cpu, tid);

    print_lost_fn(dev, event, cpu, tid);

    if (using_order(dev)) {
        fprintf(stderr, "%s: the correctness when lost cannot be guaranteed.\n", dev->prof->name);
        return;
    }

    if (ctx->tid_to_cpumap) {
        // sched:sched_stat_runtime
    } else {
        // sched:sched_switch
        if (state)
            state->running_time = 0;
    }
}

static struct runtime *find_first_sib(struct oncpu_ctx *ctx, int binding_id)
{
    struct rb_node *rbn;
    struct runtime_entry entry = {.binding_id = binding_id,};

    rbn = rb_find_first(&entry, &ctx->runtimes.entries.rb_root, runtime_binding_id_cmp);
    return rb_entry_safe(rbn, struct runtime, rbn);
}

#define for_each_runtime(first, run, member, cmp_member) \
    for(run = first; \
        run && run->cmp_member == first->cmp_member; \
        run = rb_entry_safe((rb_next(&run->member)), typeof(*run), member))

static void print_cpumap(struct prof_dev *dev, struct runtime *first)
{
    struct oncpu_ctx *ctx = dev->private;
    struct runtime *run;
    u64 sum = 0;

    for_each_runtime(first, run, rbn, binding_id)
        sum += run->runtime;

    printf("%-6d %-16s %-7lu ", first->binding_id, first->comm, sum/1000000);

    if (ctx->percpu_thread_siblings) {
        u64 co = 0;
        int sibling_tid = read_sched_vmf_sib(first->binding_id);
        if (sibling_tid >= 0) {
            for_each_runtime(first, run, rbn, binding_id) {
                struct runtime *first_sib = find_first_sib(ctx, sibling_tid);
                struct runtime *sib;
                for_each_runtime(first_sib, sib, rbn, binding_id) {
                    if (ctx->percpu_thread_siblings[sib->cpu] == run->cpu) {
                        co += min(run->runtime, sib->runtime);
                        break;
                    }
                }
            }
        }
        printf("%-6lu %-5lu  ", co/1000000, co*100/sum);
    }

    for_each_runtime(first, run, rbn, binding_id)
        printf("%d(%lums) ", run->cpu, run->runtime/1000000);

    if (ctx->percpu_thread_siblings) {
        printf(", ");
        for_each_runtime(first, run, rbn, binding_id)
            printf("%d ", ctx->percpu_thread_siblings[run->cpu]);
    }
    printf("\n");
}

static void print_tidmap(struct prof_dev *dev, struct runtime *first)
{
    struct runtime *run;
    u64 sum = 0;
    int nr_run = 0;
    int cpu = first->binding_id;

    for_each_runtime(first, run, rbn, binding_id) {
        sum += run->runtime;
        nr_run += run->nr_run;
    }

    if (dev->env->detail) {
        char buff[32];
        snprintf(buff, sizeof(buff), "%lums/%d", sum/1000000, nr_run);
        printf("%03d %-11s ", cpu, buff);
    } else
        printf("%03d %-7lu ", cpu, sum/1000000);

    if (dev->env->only_comm) {
        for_each_runtime(first, run, rbn, binding_id)
            if (dev->env->detail)
                printf("%s(%.1fms/%lu/%.1fms) ", run->comm, run->runtime/1000000.0, run->nr_run, run->max/1000000.0);
            else
                printf("%s(%.1fms) ", run->comm, run->runtime/1000000.0);
    } else {
        for_each_runtime(first, run, rbn, binding_id)
            if (dev->env->detail)
                printf("%s:%d(%.1fms/%lu/%.1fms) ", run->comm, run->tid, run->runtime/1000000.0, run->nr_run, run->max/1000000.0);
            else
                printf("%s:%d(%.1fms) ", run->comm, run->tid, run->runtime/1000000.0);
    }
    printf("\n");
}

static void oncpu_interval(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct oncpu_ctx *ctx = dev->private;
    struct rb_node *next = rb_first_cached(&ctx->runtimes.entries);
    struct runtime *first, *run;
    struct rblist sorted;

    if (rblist__empty(&ctx->runtimes))
        return ;

    if (!ctx->tid_to_cpumap) {
        // sort by cpu(from small to big), runtime(from big to small), tid.

        rblist__init(&sorted);
        sorted.node_cmp = runtime_sorted_node_cmp;
        sorted.node_new = runtime_sorted_node_new;
        sorted.node_delete = runtime_node_delete;
        ctx->runtimes.node_delete = empty; //empty, not really delete

        /* sort, remove from `ctx->runtimes', add to `sorted'. */
        do {
            struct rb_node *rbn = rblist__entry(&ctx->runtimes, 0);
            rblist__remove_node(&ctx->runtimes, rbn);
            rblist__add_node(&sorted, rbn);
        } while (!rblist__empty(&ctx->runtimes));

        next = rblist__entry(&sorted, 0);
    }

    print_time(stdout);
    printf("\n");
    if (ctx->tid_to_cpumap)
        printf("THREAD %-16s %-7s %sCPUS(ms) %s\n", "COMM", "SUM(ms)",
            ctx->percpu_thread_siblings ? "CO(ms) CO(%)  " : "",
            ctx->percpu_thread_siblings ? ", SIBLINGS" : "");
    else {
        if (env->detail)
            printf("CPU %-11s COMM%s(ms/sws/max_ms)\n", "SUM(ms/sws)", env->only_comm ? "" : ":TID");
        else
            printf("CPU %-7s COMM%s(ms)\n", "SUM(ms)", env->only_comm ? "" : ":TID");
    }

    first = rb_entry_safe(next, struct runtime, rbn);
    while (first) {
        (ctx->tid_to_cpumap ? print_cpumap : print_tidmap)(dev, first);
        for_each_runtime(first, run, rbn, binding_id);
        first = run;
    }

    if (!ctx->tid_to_cpumap) {
        rblist__exit(&sorted);
        ctx->runtimes.node_delete = runtime_node_delete;
    } else
        rblist__exit(&ctx->runtimes);
}

static void oncpu_sample(struct prof_dev *dev, union perf_event *event, int bind_cpu, int bind_tid)
{
    struct oncpu_ctx *ctx = dev->private;
    struct oncpu_binding *state = prof_binding_get(&ctx->bindings, bind_cpu, bind_tid);
    struct env *env = dev->env;
    struct sample_type_data *data = (void *)event->sample.array;
    struct runtime_entry entry;
    struct rb_node *rbn;
    struct runtime *run;
    int tid, cpu;
    u64 runtime;
    char *comm;

    if (!state)
        return;

    if (env->verbose >= VERBOSE_EVENT)
        tep__print_event(data->time, data->cpu_entry.cpu, data->raw.data, data->raw.size);

    if (ctx->tid_to_cpumap) {
        // sched:sched_stat_runtime

        tid = data->tid_entry.tid;
        cpu = data->cpu_entry.cpu;
        runtime = data->raw.runtime.runtime;
        comm = data->raw.runtime.comm;
    } else {
        /*
         * sched:sched_switch
         *
         *        ps   1214 d... [000]  2359.771892: sched:sched_switch: ps:1214 [120] R ==> sap1001:112746 [120]
         *   sap1001 112746 d... [000]  2359.772143: sched:sched_switch: sap1001:112746 [120] S ==> ps:1214 [120]
         *
         * The runtime of sap1001:112746 is equal to 2359.772143 minus 2359.771892.
        **/
        if (state->running_time == 0 ||
            state->pid != data->raw.sched_switch.prev_pid) {
            state->running_time = data->time;
            state->pid = data->raw.sched_switch.next_pid;
            return;
        }
        tid = data->raw.sched_switch.prev_pid;
        cpu = data->cpu_entry.cpu;
        runtime = data->time - state->running_time;
        comm = data->raw.sched_switch.prev_comm;
        state->running_time = data->time;
        state->pid = data->raw.sched_switch.next_pid;

        // exclude swapper
        if (tid == 0)
            return;

        // exclude those not in prio_map
        if (ctx->prio_map &&
            perf_cpu_map__idx(ctx->prio_map, data->raw.sched_switch.prev_prio) < 0)
            return;
    }

	/*
	 * CPU 24/KVM  89720 d... [179] 4925560.039977: sched:sched_stat_runtime: comm=CPU 90/KVM pid=89786 runtime=951502 [ns] vruntime=52818652842246 [ns]
	 *	ffffffff810d6157 update_curr+0x167 ([kernel.kallsyms])
	 *	ffffffff810d804d enqueue_entity+0x3d ([kernel.kallsyms])
	 *	ffffffff810d8bc9 enqueue_task_fair+0x59 ([kernel.kallsyms])
	 *	ffffffff810c67b6 enqueue_task+0x56 ([kernel.kallsyms])
	 *	ffffffff810c9543 activate_task+0x23 ([kernel.kallsyms])
	 *	ffffffff810c9893 ttwu_do_activate.constprop.119+0x33 ([kernel.kallsyms])
	 *	ffffffff810ccb3d try_to_wake_up+0x18d ([kernel.kallsyms])
	 *	ffffffff810cce22 default_wake_function+0x12 ([kernel.kallsyms])
	 *	ffffffff810b7938 autoremove_wake_function+0x18 ([kernel.kallsyms])
	 *	ffffffff810c04bb __wake_up_common+0x5b ([kernel.kallsyms])
	 *	ffffffff810c55c9 __wake_up+0x39 ([kernel.kallsyms])
	 *
	 * When a process is woken up to the specified cpu x, update_curr will be called on
	 * the current cpu, and sched:sched_stat_runtime will be recorded on the current cpu
	 * instead of cpu x. Will cause data->tid_entry.tid != data->raw.runtime.pid.
	 * As in the above example, 89720 != 89786.
	**/
    if (ctx->tid_to_cpumap &&
        data->tid_entry.tid != data->raw.runtime.pid) {
        // print unhandled event
        if (env->verbose == VERBOSE_NOTICE && data->raw.runtime.runtime >= env->greater_than)
            tep__print_event(0, data->cpu_entry.cpu, data->raw.data, data->raw.size);

        // A similar problem exists with attaching to a process.
        return;
    }

    entry.binding_id = bind_cpu >= 0 ? bind_cpu : bind_tid;
    entry.another = ctx->tid_to_cpumap ? cpu : (env->only_comm ? 0 : tid);
    entry.comm = comm;
    rbn = rblist__findnew(&ctx->runtimes, &entry);
    if (rbn) {
        run = rb_entry(rbn, struct runtime, rbn);
        run->runtime += runtime;
        run->nr_run += 1;
        if (runtime > run->max)
            run->max = runtime;
    }
}

static const char *oncpu_desc[] = PROFILER_DESC("oncpu",
    "[OPTION...] [--detail] [--filter filter] [--only-comm] [--prio n]",
    "Monitor the process running on the CPU.",
    "",
    "SYNOPSIS",
    "    CPU process execution monitor for real-time tracking of running processes and their",
    "    execution time statistics. Supports two monitoring modes:",
    "      - Thread-based CPU distribution monitoring (-p option)",
    "      - CPU-based process monitoring (-C option/default, with --prio priority filtering)",
    "    Suitable for CPU resource contention analysis and process scheduling behavior observation.",
    "",
    "TRACEPOINT",
    "    sched:sched_switch, sched:sched_stat_runtime",
    "",
    "EXAMPLES",
    "    "PROGRAME" oncpu -p 2347            # Monitor thread CPU distribution",
    "    "PROGRAME" oncpu -C 0-3 --only-comm # Monitor processes on CPUs 0-3",
    "    "PROGRAME" oncpu --only-comm --detail",
    "    "PROGRAME" oncpu --prio 1-99        # Filter by real-time priority");
static const char *oncpu_argv[] = PROFILER_ARGV("oncpu",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "detail\nMore detailed information output", "filter", "only-comm", "prio");
static profiler oncpu = {
    .name = "oncpu",
    .desc = oncpu_desc,
    .argv = oncpu_argv,
    .pages = 4,
    .init = oncpu_init,
    .filter = oncpu_filter,
    .deinit = oncpu_exit,
    .del_thread = oncpu_del_thread,
    .interval = oncpu_interval,
    .lost = oncpu_lost,
    .sample = oncpu_sample,
};
PROFILER_REGISTER(oncpu)
