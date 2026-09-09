#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <monitor.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <latency_helpers.h>
#include <bpf_expr_filter.h>
#include <bpf-skel/kvm_exit.h>
#include <bpf-skel/kvm_exit.skel.h>
#include <internal/xyarray.h>
#include <internal/evsel.h>
#include <linux/thread_map.h>

struct kvmexit_ctx {
    struct kvm_exit_bpf *obj;
    struct perf_evsel *evsel;
    struct comm_notify notify;
    struct latency_dist *lat_dist;
    struct latency_dist *lat_dist2; // non-HLT, per process.
    FILE *output2;
    struct perf_thread_map *thread_map;
    u64 recent_time;
    bool print_header;
    bool oncpu;
    bool thread;
};

struct extra_rundelay {
    u64 offcpu_wait;
    u64 runq_delay;
    u32 switches;
};

#if defined(__i386__) || defined(__x86_64__)
static int kvm_exit_reason_in_args(void)
{
    int version = kernel_release();

    if (version < 0) {
        fprintf(stderr, "Failed to determine kernel version\n");
        return -1;
    }
    return version < KERNEL_VERSION(5, 16, 0);
}

static int kvm_exit_select_prog(struct kvm_exit_bpf *obj)
{
    int reason_in_args = kvm_exit_reason_in_args();

    if (reason_in_args < 0)
        return -1;
    if (reason_in_args) {
        bpf_program__set_autoload(obj->progs.kvm_exit, false);
        bpf_program__set_autoload(obj->progs.kvm_exit_pid, false);
    } else {
        bpf_program__set_autoload(obj->progs.kvm_exit_legacy, false);
        bpf_program__set_autoload(obj->progs.kvm_exit_pid_legacy, false);
    }

    return 0;
}
#else
static int kvm_exit_select_prog(struct kvm_exit_bpf *obj)
{
    (void)obj;
    return 0;
}
#endif

static int comm_notify(struct comm_notify *notify, int pid, int state, u64 free_time)
{
    if (state == NOTIFY_COMM_DELETE) {
        struct kvmexit_ctx *ctx = container_of(notify, struct kvmexit_ctx, notify);
        bpf_map__delete_elem(ctx->obj->maps.kvm_vcpu, &pid, sizeof(pid), 0);
    }
    return 0;
}

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct kvmexit_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (prof_dev_ins_oncpu(dev))
        ctx->oncpu = 1;

    ctx->obj = kvm_exit_bpf__open();
    if (!ctx->obj)
        goto free_ctx;
    if (kvm_exit_select_prog(ctx->obj) < 0)
        goto destroy;

    ctx->lat_dist = latency_dist_new_quantile(env->perins, true, sizeof(struct extra_rundelay));
    if (!ctx->lat_dist)
        goto destroy;

    if (env->output2) {
        ctx->lat_dist2 = latency_dist_new(env->perins, 0, 0);
        if (!ctx->lat_dist2)
            goto free_lat;
        ctx->output2 = fopen(env->output2, "a");
        if (!ctx->output2)
            goto free_lat2;
    }

    // Only oncpu mode needs global comm service to track system-wide process changes
    if (ctx->oncpu) {
        if (global_comm_ref() < 0)
            goto close2;
        ctx->notify.notify = comm_notify;
        global_comm_register_notify(&ctx->notify);
    }
    ctx->thread = env->perins && (!ctx->oncpu || env->detail);
    return 0;

close2:
    if (ctx->output2) fclose(ctx->output2);
free_lat2:
    latency_dist_free(ctx->lat_dist2);
free_lat:
    latency_dist_free(ctx->lat_dist);
destroy:
    kvm_exit_bpf__destroy(ctx->obj);
free_ctx:
    free(ctx);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct kvmexit_ctx *ctx = dev->private;
    perf_thread_map__put(ctx->thread_map);
    if (ctx->oncpu) {
        global_comm_unregister_notify(&ctx->notify);
        global_comm_unref();
    }
    if (ctx->output2) fclose(ctx->output2);
    latency_dist_free(ctx->lat_dist2);
    latency_dist_free(ctx->lat_dist);
    kvm_exit_bpf__destroy(ctx->obj);
    free(ctx);
}

static int get_tgid(int tid)
{
    char path[256], line[256];
    FILE *fp;
    int pid = 0;

    // Read /proc/pid/status, get Tgid.
    snprintf(path, sizeof(path), "/proc/%d/status", tid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "Tgid: %d", &pid) == 1)
            break;
        pid = 0;
    }
    fclose(fp);

    return pid;
}

static int bpf_kvm_exit_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct kvmexit_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_SOFTWARE,
        .config        = PERF_COUNT_SW_BPF_OUTPUT,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TIME | PERF_SAMPLE_RAW,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
        .wakeup_watermark = (dev->pages << 12) / 3,
    };
    int cpu, ins, i, tid, tgid;
    struct perf_thread_map *tgidmap = NULL;
    struct prof_dev *tgiddev = NULL;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    ctx->evsel = perf_evsel__new(&attr);
    if (!ctx->evsel)
        goto failed;
    perf_evlist__add(evlist, ctx->evsel);

    if (ctx->oncpu) {
        perf_cpu_map__for_each_cpu(cpu, ins, dev->cpus)
            ctx->obj->bss->work_cpus[cpu] = 1;
        for (i = 0; i < ARRAY_SIZE(ctx->obj->bss->percpu_event); i++)
            EXIT_TIME(&ctx->obj->bss->percpu_event[i]) = INT64_MAX;

        bpf_program__set_autoload(ctx->obj->progs.kvm_exit_pid, 0);
#if defined(__i386__) || defined(__x86_64__)
        bpf_program__set_autoload(ctx->obj->progs.kvm_exit_pid_legacy, 0);
#endif
        bpf_program__set_autoload(ctx->obj->progs.kvm_entry_pid, 0);
    } else {
        // can only be bound to cpu
        ctx->thread_map = dev->threads;
        // Pre-read comms to avoid costly global_comm lookups
        thread_map__read_comms(ctx->thread_map);
        perf_cpu_map__put(dev->cpus);
        dev->cpus = perf_cpu_map__new(NULL);
        dev->threads = perf_thread_map__new_dummy();

        tid = perf_thread_map__pid(ctx->thread_map, 0);
        tgid = get_tgid(tid);
        ctx->obj->rodata->filter_pid = tgid;

        bpf_program__set_autoload(ctx->obj->progs.kvm_exit, 0);
#if defined(__i386__) || defined(__x86_64__)
        bpf_program__set_autoload(ctx->obj->progs.kvm_exit_legacy, 0);
#endif
        bpf_program__set_autoload(ctx->obj->progs.kvm_entry, 0);
        bpf_program__set_autoload(ctx->obj->progs.sched_switch, 0);

        // Monitor main thread exit to auto-close device via hangup callback
        tgidmap = thread_map__new_by_tid(tgid);
        if (!tgidmap)
            goto failed;
        tgiddev = trace_dev_open("sched:sched_process_exit", NULL, tgidmap, dev, NULL, (void *)prof_dev_close);
        if (!tgiddev)
            goto failed;
        perf_thread_map__put(tgidmap);
        tgidmap = NULL;
    }

    /*
     * Install the kernel-side expression filter, if one was given. Must happen
     * before load: expr_filter() is a subprogram, and libbpf copies it into its
     * callers during load.
     */
    if (dev->env->filter &&
        bpf_expr_filter_apply(ctx->obj->obj, dev->env->filter, dev->env->verbose) < 0)
        goto failed;

    if (kvm_exit_bpf__load(ctx->obj))
        goto failed;

    return 0;

failed:
    if (tgiddev)
        prof_dev_close(tgiddev);
    if (tgidmap)
        perf_thread_map__put(tgidmap);
    monitor_ctx_exit(dev);
    return -1;
}

static void bpf_kvm_exit_fix_sample_pos(struct prof_dev *dev)
{
    dev->pos.sample_type |= PERF_SAMPLE_TID | PERF_SAMPLE_CPU;
    /* PERF_SAMPLE_TIME | PERF_SAMPLE_RAW
     *   __u64 time;
     *  struct {
     *      __u32   size;
     *      __u8    data[0];
     *  } raw;
     */
    dev->pos.tid_pos = sizeof(u64) + sizeof(u32);
    dev->pos.cpu_pos = -1;
}

#define FD(e, x, y) ((int *) xyarray__entry(e->fd, x, y))
static int bpf_kvm_exit_filter(struct prof_dev *dev)
{
    struct kvmexit_ctx *ctx = dev->private;
    struct bpf_map *perf_events = ctx->obj->maps.perf_events;
    int cpu, ins, perf_event_fd;
    int err;

    perf_cpu_map__for_each_cpu(cpu, ins, dev->cpus) {
        perf_event_fd = *FD(ctx->evsel, ins, 0);
        err = bpf_map__update_elem(perf_events, &cpu, sizeof(cpu), &perf_event_fd, sizeof(perf_event_fd), 0);
        if (err)
            return -1;
    }
    return kvm_exit_bpf__attach(ctx->obj);
}

// Get process name: use global_comm in oncpu mode, thread_map in process-tracking mode
static const char *comm_get(struct prof_dev *dev, int pid)
{
    struct kvmexit_ctx *ctx = dev->private;
    if (ctx->oncpu)
        return global_comm_get(pid);
    else {
        int idx = perf_thread_map__idx(ctx->thread_map, pid);
        if (idx >= 0)
            return perf_thread_map__comm(ctx->thread_map, idx);
        else
            return "<...>";
    }
}

static void print_latency_node(void *opaque, struct latency_node *node)
{
    struct prof_dev *dev = opaque;
    struct env *env = dev->env;
    struct kvmexit_ctx *ctx = dev->private;
    unsigned int exit_reason = node->key & 0xffffffff;
    u32 isa = node->key >> 32;
    double p99 = tdigest_quantile(node->td, 0.99);
    struct extra_rundelay *extra = (void *)node->extra;

    if (ctx->print_header) {
        ctx->print_header = false;
        print_time(stdout);
        printf("kvm_exit latency\n");

        if (env->verbose >= -1) {
            if (env->perins)
                printf(ctx->thread ? "   PID THREAD COMM            " : "   PID ");
            printf("%-*s %8s %16s %12s %12s %12s %12s%s", isa == KVM_ISA_VMX ? 20 : 32, "exit_reason", "calls",
                     "total(us)", "min(us)", "avg(us)", "p99(us)", "max(us)", ctx->oncpu ? " " : "\n");
            if (ctx->oncpu)
                printf("offcpu_wait(us) runq_delay(us)\n");
        }

        if (env->verbose >= 0) {
            if (env->perins)
                printf(ctx->thread ? "------ ------ --------------- " : "------ ");
            printf("%s %8s %16s %12s %12s %12s %12s%s", isa == KVM_ISA_VMX ? "--------------------" : "--------------------------------",
                "--------", "----------------", "------------", "------------", "------------", "------------", ctx->oncpu ? " " : "\n");
            if (ctx->oncpu)
                printf("--------------- --------------\n");
        }
    }
    if (env->perins) {
        if (ctx->thread)
            printf("%6d %6d %-15s ", (int)(node->instance >> 32), (int)node->instance, comm_get(dev, (int)node->instance));
        else
            printf("%6d ", (int)node->instance);
    }
    printf("%-*s %8lu %16.3f %12.3f %12.3f %12.3f %12.3f%s", isa == KVM_ISA_VMX ? 20 : 32,
            find_exit_reason(isa, exit_reason),
            node->n, node->sum/1000.0,
            node->min/1000.0, node->sum/node->n/1000.0, p99/1000.0, node->max/1000.0, ctx->oncpu ? " " : "\n");
    if (ctx->oncpu)
        printf("%15.3f %14.3f\n", extra->offcpu_wait/1000.0, extra->runq_delay/1000.0);
}

static void output2(void *opaque, struct latency_node *node)
{
    struct prof_dev *dev = opaque;
    struct kvmexit_ctx *ctx = dev->private;

    if (ctx->print_header) {
        ctx->print_header = false;
        print_time(ctx->output2);
        fprintf(ctx->output2, "\n   PID  max-latency(us)\n");
    }
    fprintf(ctx->output2, "%6d  %.3f\n", (int)node->instance, node->max/1000.0);
}

static void bpf_kvm_exit_interval(struct prof_dev *dev)
{
    struct kvmexit_ctx *ctx = dev->private;

    ctx->print_header = true;
    latency_dist_print_sorted(ctx->lat_dist, print_latency_node, dev);
    if (!ctx->print_header)
        printf("\n");

    if (ctx->output2) {
        ctx->print_header = true;
        latency_dist_print(ctx->lat_dist2, output2, dev);
        fflush(ctx->output2);
    }
}

static void bpf_kvm_exit_deinit(struct prof_dev *dev)
{
    bpf_kvm_exit_interval(dev);
    monitor_ctx_exit(dev);
}

static void bpf_kvm_exit_print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    struct kvmexit_ctx *ctx = dev->private;
    struct kvm_vcpu_event *raw = (void *)event->sample.array + sizeof(u64) + sizeof(u32)/* u32 size; */;
    u64 *time = (void *)event->sample.array;

    if (!(flags & OMIT_TIMESTAMP))
        prof_dev_print_time(dev, *time, stdout);
    printf("%18s %8u    [%03d] %lu.%06lu: bpf:kvm_exit: %s lat %lu%s", comm_get(dev, raw->pid),
        raw->pid, prof_dev_ins_cpu(dev, instance), *time / NSEC_PER_SEC, (*time % NSEC_PER_SEC)/1000,
        find_exit_reason(raw->isa, raw->exit_reason), raw->exit_latency, ctx->oncpu ? " " : "\n");
    if (ctx->oncpu)
        printf("runq_delay %lu offcpu_wait %lu sw %u\n", raw->runq_delay, raw->offcpu_wait, raw->switches);
}

static void bpf_kvm_exit_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct kvmexit_ctx *ctx = dev->private;
    struct env *env = dev->env;
    struct latency_node *node;
    // in linux/perf_event.h
    // PERF_SAMPLE_TIME | PERF_SAMPLE_RAW
    struct kvm_vcpu_event *raw = (void *)event->sample.array + sizeof(u64) + sizeof(u32)/* u32 size; */;
    u64 *time = (void *)event->sample.array;
    u64 key = ((u64)raw->isa<<32) | raw->exit_reason;
    s64 delta = raw->exit_latency;
    u64 ins = 0;
    u32 hlt;

    if (*time > ctx->recent_time)
        ctx->recent_time = *time;

    switch (raw->isa) {
        case KVM_ISA_VMX: hlt = EXIT_REASON_HLT; break;
        case KVM_ISA_SVM: hlt = SVM_EXIT_HLT; break;
        default: hlt = ARM_EXIT_HLT; break;
    }

    if (env->perins) {
        if (!ctx->oncpu)
            ins = (u64)raw->tgid << 32 | raw->pid;
        else if (env->detail)
            ins = (u64)raw->tgid << 32 | (raw->exit_reason != hlt ? raw->pid : raw->tgid);
        else
            ins = raw->tgid;
    }
    node = latency_dist_input(ctx->lat_dist, ins, key, delta, 0);
    if (node && ctx->oncpu) {
        struct extra_rundelay *extra = (void *)node->extra;
        extra->offcpu_wait += raw->offcpu_wait;
        extra->runq_delay += raw->runq_delay;
        extra->switches += raw->switches;
    }

    if (ctx->lat_dist2)
        latency_dist_input(ctx->lat_dist2, raw->tgid, 0, raw->exit_reason != hlt ? delta : raw->runq_delay, 0);

    if (unlikely(env->verbose >= VERBOSE_EVENT))
        goto print_event;

    if (env->greater_than &&
        (raw->exit_reason != hlt ? delta : raw->runq_delay) > env->greater_than) {
    print_event:
        bpf_kvm_exit_print_event(dev, event, instance, 0);
    }
}

static void bpf_kvm_exit_print_dev(struct prof_dev *dev, int indent)
{
    struct kvmexit_ctx *ctx = dev->private;
    int pid = 0;
    int n = 0;

    dev_printf("kvm_vcpu:\n");
    while (bpf_map__get_next_key(ctx->obj->maps.kvm_vcpu,
                pid == 0 ? NULL : &pid, &pid, sizeof(pid)) == 0) {
        dev_printf("%6u %s\n", pid, comm_get(dev, pid));
        n++;
    }
}

static void bpf_kvm_exit_sigusr(struct prof_dev *dev, int signum)
{
    struct kvmexit_ctx *ctx = dev->private;

    if (signum == SIGUSR1) {
        if (ctx->output2)
            ctx->output2 = freopen(dev->env->output2, "a", ctx->output2);
    }
}

static u64 bpf_kvm_exit_minevtime(struct prof_dev *dev)
{
    struct kvmexit_ctx *ctx = dev->private;
    struct env *env = dev->env;
    u64 minevtime = ULLONG_MAX;

    if (ctx->thread && env->interval) {
        if (ctx->recent_time > env->interval * NSEC_PER_MSEC)
            minevtime = ctx->recent_time - env->interval * NSEC_PER_MSEC;
    }
    return minevtime;
}

static const char *bpf_kvm_exit_desc[] = PROFILER_DESC("bpf:kvm_exit",
    "[OPTION...] [-C cpus | -p pid] [-i interval] [--perins] [--than ns] [--filter expr]",
    "Generate bpf:kvm_exit event.",
    "",
    "SYNOPSIS",
    "    BPF tool for KVM exit latency analysis. Processes kvm_exit and kvm_entry events",
    "    in kernel space to generate bpf:kvm_exit events with detailed latency breakdown.",
    "",
    "    Aggregates exit latency distribution (min/avg/p99/max) by exit_reason, outputs",
    "    at intervals specified by -i. Use --filter to drop events in BPF (kernel), use",
    "    --than to display events exceeding a threshold (excludes HLT exits).",
    "",
    "FILTER",
    "    --filter takes a C expression over the BPF-event fields below, compiled to BPF",
    "    and evaluated in the kernel; an event is kept when it evaluates non-zero. Most",
    "    C operators work, but string and symbol builtins do not -- an unsupported",
    "    construct is reported at startup rather than silently ignored.",
    "",
    "    Fields may also be assigned, both to rewrite what is reported and, since the",
    "    expression language has no variables of its own, to hold a temporary in an",
    "    otherwise unused field.",
    "",
    "Attach modes",
    "    -C cpus    System-wide monitoring on specified CPUs",
    "    -p pid     Process-specific monitoring (tracks all vcpu threads)",
    "",
    "TRACEPOINTS AND COMPATIBILITY",
    "    Requires kvm:kvm_exit and kvm:kvm_entry; system-wide mode also uses",
    "    sched:sched_switch. On x86 kernels before Linux 5.16, exit_reason is",
    "    read from raw tracepoint arguments. On Linux 5.16 and later, fields",
    "    are read from the trace event entry. arm64 keeps the raw tracepoint path.",
    "",
    "BPF-EVENT FIELDS",
    "    Every field is a final value: the BPF program resolves all three time",
    "    fields before it filters, so an expression never sees an intermediate.",
    "",
    "    u32 tgid          # Thread group id of the vcpu thread",
    "    u32 pid           # Pid of the vcpu thread",
    "    u16 isa           # 1=VMX, 2=SVM, 3=ARM",
    "    u32 exit_reason   # VM exit reason code",
    "    u64 exit_latency  # Exit latency: kvm:kvm_exit => kvm:kvm_entry (ns)",
    "    u16 switches      # Context switch count during the exit",
    "    u64 runq_delay    # Within exit_latency: runnable, waiting for a CPU (ns)",
    "    u64 offcpu_wait   # Within exit_latency: off-CPU but not runnable (ns)",
    "",
    "    The breakdown always adds up: runq_delay + offcpu_wait <= exit_latency.",
    "    runq_delay comes from sched_info.run_delay, which the kernel accounts on",
    "    a different clock, so it is clamped to the measured off-CPU time.",
    "",
    "    switches, runq_delay and offcpu_wait are only resolved in system-wide",
    "    (-C cpus) mode, the only mode that attaches to sched:sched_switch. In",
    "    per-process (-p pid) mode they are always 0.",
    "",
    "EXAMPLES",
    "    "PROGRAME" bpf:kvm_exit -p 2347 -i 1000 --than 50ms",
    "        # Monitor VM process 2347, output every 1s, show events > 50ms",
    "",
    "    "PROGRAME" bpf:kvm_exit -C 1-4 -i 1000 --perins",
    "        # Monitor CPUs 1-4, output per-instance stats every 1s",
    "",
    "    "PROGRAME" bpf:kvm_exit -C 0-7 -i 1000 --filter 'exit_latency > 10000000' --than 20ms",
    "        # Drop exits < 10ms in BPF, display exits > 20ms (non-HLT)",
    "",
    "    "PROGRAME" bpf:kvm_exit -C 0-7 -i 1000 --filter 'exit_reason != 12'",
    "        # Ignore HLT exits entirely, in the kernel",
    "",
    "    "PROGRAME" bpf:kvm_exit -C 0-7 -i 1000 --filter 'exit_latency > 1000000 && switches > 0'",
    "        # Exits over 1ms that were also preempted",
    "",
    "    "PROGRAME" bpf:kvm_exit -C 0-7 -i 1000 --filter 'runq_delay > 1000000'",
    "        # Exits that spent over 1ms runnable, waiting for a host CPU");
static const char *bpf_kvm_exit_argv[] = PROFILER_ARGV("bpf:kvm_exit",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "perins", "than",
    "filter     Expression filter, evaluated in the kernel",
    "detail     Display per-thread stat",
    "output2    Output the non-HLT maximum latency for each process");
struct monitor bpf_kvm_exit = {
    .name = "bpf:kvm_exit",
    .desc = bpf_kvm_exit_desc,
    .argv = bpf_kvm_exit_argv,
    .pages = 4,
    .init = bpf_kvm_exit_init,
    .fix_sample_pos = bpf_kvm_exit_fix_sample_pos,
    .filter = bpf_kvm_exit_filter,
    .deinit = bpf_kvm_exit_deinit,
    .interval = bpf_kvm_exit_interval,
    .print_event = bpf_kvm_exit_print_event,
    .sample = bpf_kvm_exit_sample,
    .print_dev = bpf_kvm_exit_print_dev,
    .sigusr = bpf_kvm_exit_sigusr,
    .minevtime = bpf_kvm_exit_minevtime,
};
MONITOR_REGISTER(bpf_kvm_exit)
