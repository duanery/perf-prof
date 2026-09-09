#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <monitor.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/rblist.h>
#include <linux/const.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <latency_helpers.h>
#include <bpf-skel/kvm_exit.h>

#define ALIGN(x, a)  __ALIGN_KERNEL((x), (a))

#include "kvm_exit_reason.c"

struct sample_type_raw;

struct kvmexit_ctx {
    int nr_ins;
    u16 kvm_exit_size;
    u16 sample_size;
    struct sample_type_raw *perins_kvm_exit;
    int *perins_kvm_exit_valid;
    __u64 kvm_exit;
    __u64 kvm_entry;
    struct latency_dist *lat_dist;
    struct heatmap *heatmap;
    bool print_header;
    bool ins_oncpu;
};

#if defined(__aarch64__)
struct trace_kvm_exit_armv8 {
    unsigned short common_type;//	offset:0;	size:2;	signed:0;
	unsigned char common_flags;//	offset:2;	size:1;	signed:0;
	unsigned char common_preempt_count;//	offset:3;	size:1;	signed:0;
	int common_pid;//	offset:4;	size:4;	signed:1;

	int ret;//	offset:8;	size:4;	signed:1;
	unsigned int esr_ec;//	offset:12;	size:4;	signed:0;
	unsigned long vcpu_pc;//	offset:16;	size:8;	signed:0;
};
union trace_kvm_exit {
    struct trace_kvm_exit_armv8 e1;
};

struct trace_kvm_entry {
    unsigned short common_type;//      offset:0;       size:2; signed:0;
    unsigned char common_flags;//      offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//      offset:3;       size:1; signed:0;
    int common_pid;//  offset:4;       size:4; signed:1;

    unsigned long vcpu_id;//    offset:8;       size:8; signed:0;
};
#else
struct trace_kvm_exit1 {
    unsigned short common_type;//	offset:0;	size:2;	signed:0;
	unsigned char common_flags;//	offset:2;	size:1;	signed:0;
	unsigned char common_preempt_count;//	offset:3;	size:1;	signed:0;
	int common_pid;//	offset:4;	size:4;	signed:1;
	int common_lock_depth;//	offset:8;	size:4;	signed:1;

	unsigned int exit_reason;//	offset:12;	size:4;	signed:0;
	unsigned long guest_rip;//	offset:16;	size:8;	signed:0;
};
struct trace_kvm_exit2 {
    unsigned short common_type;//	offset:0;	size:2;	signed:0;
	unsigned char common_flags;//	offset:2;	size:1;	signed:0;
	unsigned char common_preempt_count;//	offset:3;	size:1;	signed:0;
	int common_pid;//	offset:4;	size:4;	signed:1;

	unsigned int exit_reason;//	offset:8;	size:4;	signed:0;
	unsigned long guest_rip;//	offset:16;	size:8;	signed:0;
	u32 isa;//	offset:24;	size:4;	signed:0;
	u64 info1;//	offset:32;	size:8;	signed:0;
	u64 info2;//	offset:40;	size:8;	signed:0;
};
union trace_kvm_exit {
    struct trace_kvm_exit1 e1;
    struct trace_kvm_exit2 e2;
};

struct trace_kvm_entry {
    unsigned short common_type;//      offset:0;       size:2; signed:0;
    unsigned char common_flags;//      offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//      offset:3;       size:1; signed:0;
    int common_pid;//  offset:4;       size:4; signed:1;

    unsigned int vcpu_id;//    offset:8;       size:4; signed:0;
};

#endif
// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
struct sample_type_raw {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    struct {
        __u32   size;
        union {
            __u8    data[0];
            unsigned short common_type;
            union trace_kvm_exit kvm_exit;
            struct trace_kvm_entry kvm_entry;
        };
    } __packed raw;
};

static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct kvmexit_ctx *ctx = zalloc(sizeof(*ctx));
    int id;
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (env->greater_than || env->verbose)
        tep__ref();
    else
        tep__ref_light();

    ctx->nr_ins = prof_dev_nr_ins(dev);
    id = tep__event_id("kvm", "kvm_exit");
    if (id < 0)
        goto failed;
    ctx->kvm_exit_size = tep__event_size(id);
    ctx->sample_size = offsetof(struct sample_type_raw, raw.data) + ctx->kvm_exit_size;
    ctx->perins_kvm_exit = calloc(ctx->nr_ins, ctx->sample_size);
    ctx->perins_kvm_exit_valid = calloc(ctx->nr_ins, sizeof(int));
    if (!ctx->perins_kvm_exit || !ctx->perins_kvm_exit_valid)
        goto failed;

    ctx->lat_dist = latency_dist_new_quantile(env->perins, true, sizeof(u64));
    if (!ctx->lat_dist)
        goto failed;

    if (env->heatmap)
        ctx->heatmap = heatmap_open("ns", "ns", env->heatmap);

    ctx->ins_oncpu = prof_dev_ins_oncpu(dev);
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct kvmexit_ctx *ctx = dev->private;
    if (ctx->perins_kvm_exit)
        free(ctx->perins_kvm_exit);
    if (ctx->perins_kvm_exit_valid)
        free(ctx->perins_kvm_exit_valid);
    latency_dist_free(ctx->lat_dist);
    if (dev->env->heatmap)
        heatmap_close(ctx->heatmap);
    tep__unref();
    free(ctx);
}

static int kvm_exit_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct kvmexit_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
        .wakeup_watermark = (dev->pages << 12) / 3,
    };
    struct perf_evsel *evsel;
    int id;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    id = tep__event_id("kvm", "kvm_exit");
    if (id < 0)
        goto failed;
    attr.config = ctx->kvm_exit = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel)
        goto failed;
    perf_evlist__add(evlist, evsel);

    id = tep__event_id("kvm", "kvm_entry");
    if (id < 0)
        goto failed;
    attr.config = ctx->kvm_entry = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel)
        goto failed;
    perf_evlist__add(evlist, evsel);
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void print_latency_node(void *opaque, struct latency_node *node)
{
    struct prof_dev *dev = opaque;
    struct env *env = dev->env;
    struct kvmexit_ctx *ctx = dev->private;
    unsigned int exit_reason = node->key & 0xffffffff;
    u32 isa = node->key >> 32;
    double p99 = tdigest_quantile(node->td, 0.99);

    if (ctx->print_header) {
        ctx->print_header = false;
        print_time(stdout);
        printf("kvm-exit latency\n");

        if (env->perins)
            printf("%s ", ctx->ins_oncpu ? "[CPU]" : "[THREAD]");
        printf("%-*s %8s %16s %12s %12s %12s %12s %6s\n", isa == KVM_ISA_VMX ? 20 : 32, "exit_reason", "calls",
                 env->tsc ? "total(kcyc)" : "total(us)",
                 env->tsc ? "min(kcyc)" : "min(us)",
                 env->tsc ? "avg(kcyc)" : "avg(us)",
                 env->tsc ? "p99(kcyc)" : "p99(us)",
                 env->tsc ? "max(kcyc)" : "max(us)", "%gsys");

        if (env->perins)
            printf("%s ", ctx->ins_oncpu ? "-----" : "--------");
        printf("%s %8s %16s %12s %12s %12s %12s %6s\n", isa == KVM_ISA_VMX ? "--------------------" : "--------------------------------",
                "--------", "----------------", "------------", "------------", "------------", "------------", "------");
    }
    if (env->perins)
        printf("[%*d] ", ctx->ins_oncpu ? 3 : 6,
                ctx->ins_oncpu ? prof_dev_ins_cpu(dev, (int)node->instance) : prof_dev_ins_thread(dev, (int)node->instance));
    printf("%-*s %8lu %16.3f %12.3f %12.3f %12.3f %12.3f %6.2f\n", isa == KVM_ISA_VMX ? 20 : 32,
            find_exit_reason(isa, exit_reason),
            node->n, node->sum/1000.0,
            node->min/1000.0, node->sum/node->n/1000.0, p99/1000.0, node->max/1000.0,
            node->extra[0]*100.0/node->sum);
}

static void kvm_exit_interval(struct prof_dev *dev)
{
    struct kvmexit_ctx *ctx = dev->private;

    ctx->print_header = true;
    latency_dist_print_sorted(ctx->lat_dist, print_latency_node, dev);
    if (!ctx->print_header)
        printf("\n");
}

static int kvm_exit_filter(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct kvmexit_ctx *ctx = dev->private;
    struct perf_evsel *evsel;
    int err = 0;

    if (env->filter) {
        perf_evlist__for_each_evsel(evlist, evsel) {
            struct perf_event_attr *attr = perf_evsel__attr(evsel);
            if (attr->config == ctx->kvm_exit) {
                err = perf_evsel__apply_filter(evsel, env->filter);
                if (err < 0)
                    return err;
            }
        }
    }
    return 0;
}

static void kvm_exit_deinit(struct prof_dev *dev)
{
    kvm_exit_interval(dev);
    monitor_ctx_exit(dev);
}

static int kvm_exit_grow(struct prof_dev *dev);

static void kvm_exit_lost(struct prof_dev *dev, union perf_event *event, int cpu, int tid, u64 lost_start, u64 lost_end)
{
    int ins = prof_dev_ins(dev, cpu, tid);

    struct kvmexit_ctx *ctx = dev->private;

    if (ins < 0 || kvm_exit_grow(dev) < 0 || ins >= ctx->nr_ins)
        return;

    print_lost_fn(dev, event, cpu, tid);

    if (using_order(dev)) {
        fprintf(stderr, "%s: the correctness when lost cannot be guaranteed.\n", dev->prof->name);
        return;
    }

    ctx->perins_kvm_exit_valid[ins] = 0;
}

static inline int __exit_reason(struct kvmexit_ctx *ctx, struct sample_type_raw *raw, unsigned int *exit_reason,
                                     u32 *isa, unsigned long *guest_rip)
{
    unsigned short common_type = raw->raw.common_type;

    if (common_type == ctx->kvm_exit) {
        switch (raw->raw.size) {
#if defined(__aarch64__)
        case ALIGN(sizeof(struct trace_kvm_exit_armv8)+sizeof(u32), sizeof(u64)) - sizeof(u32):
            if (raw->raw.kvm_exit.e1.ret == ARM_EXCEPTION_TRAP)
                *exit_reason = raw->raw.kvm_exit.e1.esr_ec;
            else
                *exit_reason = ARM_EXCEPTION_REASON(raw->raw.kvm_exit.e1.ret);
            *isa = KVM_ISA_ARM;
            *guest_rip = raw->raw.kvm_exit.e1.vcpu_pc;
            break;
        default:
            return -1;
#else
        case ALIGN(sizeof(struct trace_kvm_exit1)+sizeof(u32), sizeof(u64)) - sizeof(u32):
            *exit_reason = raw->raw.kvm_exit.e1.exit_reason;
            *isa = KVM_ISA_VMX;
            *guest_rip = raw->raw.kvm_exit.e1.guest_rip;
            break;
        default:
            if (raw->raw.size < ctx->kvm_exit_size)
                return -1;
            *exit_reason = raw->raw.kvm_exit.e2.exit_reason;
            *isa = raw->raw.kvm_exit.e2.isa;
            *guest_rip = raw->raw.kvm_exit.e2.guest_rip;
            break;
#endif
        }
    }
    return 0;
}

static void __print_raw(struct prof_dev *dev, struct sample_type_raw *raw, const char *str)
{
    if (dev->print_title) prof_dev_print_time(dev, raw->time, stdout);
    if (str)
        printf("%s", str);
    tep__print_event(raw->time, raw->cpu_entry.cpu, raw->raw.data, raw->raw.size);
}

static void __process_fast(struct prof_dev *dev, struct sample_type_raw *rkvm_exit, struct sample_type_raw *rkvm_entry, int instance)
{
    struct env *env = dev->env;
    struct kvmexit_ctx *ctx = dev->private;
    unsigned int exit_reason = -1, hlt = EXIT_REASON_HLT;
#if defined(__aarch64__)
    u32 isa = KVM_ISA_ARM;
#else
    u32 isa = KVM_ISA_VMX;
#endif
    unsigned long guest_rip = 0;
    __u64 delta = rkvm_entry->time - rkvm_exit->time;
    u64 key = 0;
    struct latency_node *node;

    if (__exit_reason(ctx, rkvm_exit, &exit_reason, &isa, &guest_rip) < 0)
        return;
    if (isa == KVM_ISA_SVM) {
        hlt = SVM_EXIT_HLT;
    }
    if (isa == KVM_ISA_ARM) {
        hlt = ARM_EXIT_HLT;
    }

    key = ((u64)isa<<32)|exit_reason;
    node = latency_dist_input(ctx->lat_dist, env->perins?instance:0, key, delta, env->greater_than);
    if (node && guest_rip >= START_OF_KERNEL)
        node->extra[0] += delta;

    if (env->heatmap)
        heatmap_write(ctx->heatmap, rkvm_exit->time, delta);

    if (env->greater_than &&
        exit_reason != hlt &&
        delta > env->greater_than) {
        __print_raw(dev, rkvm_exit, NULL);
        __print_raw(dev, rkvm_entry, NULL);
    }
}

static int kvm_exit_grow(struct prof_dev *dev)
{
    struct kvmexit_ctx *ctx = dev->private;
    int n = prof_dev_nr_ins(dev);

    if (n <= ctx->nr_ins)
        return 0;
    if (mem_grow_zero((void **)&ctx->perins_kvm_exit, ctx->nr_ins, n, ctx->sample_size) ||
        mem_grow_zero((void **)&ctx->perins_kvm_exit_valid, ctx->nr_ins, n, sizeof(int)))
        return -1;
    ctx->nr_ins = n;
    return 0;
}

static void kvm_exit_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    int instance = prof_dev_ins(dev, cpu, tid);
    struct kvmexit_ctx *ctx = dev->private;
    /* PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW */
    struct sample_type_raw *raw = (void *)event->sample.array;
    unsigned short common_type = raw->raw.common_type;
    unsigned int exit_reason;
    u32 isa;
    unsigned long guest_rip;

    if (instance < 0 || kvm_exit_grow(dev) < 0 || instance >= ctx->nr_ins)
        return;

    if (dev->env->verbose >= VERBOSE_EVENT) {
        if (dev->print_title) prof_dev_print_time(dev, raw->time, stdout);
        tep__print_event(raw->time, raw->cpu_entry.cpu, raw->raw.data, raw->raw.size);
    }

    if (common_type == ctx->kvm_exit) {
        if (__exit_reason(ctx, raw, &exit_reason, &isa, &guest_rip) < 0)
            return;
        ctx->perins_kvm_exit_valid[instance] = 1;
        memcpy(&ctx->perins_kvm_exit[instance], raw, ctx->sample_size);
    } else if (common_type == ctx->kvm_entry) {
        if (ctx->perins_kvm_exit_valid[instance] == 1) {
            struct sample_type_raw *raw_kvm_exit = &ctx->perins_kvm_exit[instance];
            if (raw->tid_entry.tid == raw_kvm_exit->tid_entry.tid &&
                raw->time > raw_kvm_exit->time) {
                __process_fast(dev, raw_kvm_exit, raw, instance);
                ctx->perins_kvm_exit_valid[instance] = 0;
            } else {
                if (raw->tid_entry.tid != raw_kvm_exit->tid_entry.tid) {
                    if (dev->env->verbose >= VERBOSE_NOTICE) {
                        __print_raw(dev, raw_kvm_exit, "WARN");
                        __print_raw(dev, raw, "WARN");
                    }
                }
            }
        }
    }
}

static const char *kvm_exit_desc[] = PROFILER_DESC("kvm-exit",
    "[OPTION...] [--perins] [--than ns] [--heatmap file] [--filter filter]",
    "KVM virtualization exit latency analyzer.",
    "",
    "SYNOPSIS",
    "    Run on the Host to trace the latency from kvm_exit to kvm_entry. Aggregates by",
    "    exit_reason to show latency distribution (min/avg/p99/max) for each exit reasons.",
    "    Used for VM performance troubleshooting and virtualization overhead analysis.",
    "",
    "    Use --than to display events exceeding the threshold (excludes HLT exits).",
    "",
    "TRACEPOINT",
    "    kvm:kvm_exit, kvm:kvm_entry",
    "",
    "EXAMPLES",
    "    "PROGRAME" kvm-exit -p 2347 -i 1000",
    "    "PROGRAME" kvm-exit -C 1-4 -i 1000 --perins",
    "    "PROGRAME" kvm-exit -p 2347 --filter 'exit_reason==48' --than 1ms -i 1000");
static const char *kvm_exit_argv[] = PROFILER_ARGV("kvm-exit",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "perins", "than", "heatmap", "filter");
struct monitor kvm_exit = {
    .name = "kvm-exit",
    .desc = kvm_exit_desc,
    .argv = kvm_exit_argv,
    .pages = 64,
    .init = kvm_exit_init,
    .filter = kvm_exit_filter,
    .deinit = kvm_exit_deinit,
    .interval = kvm_exit_interval,
    .lost = kvm_exit_lost,
    .sample = kvm_exit_sample,
};
MONITOR_REGISTER(kvm_exit)


