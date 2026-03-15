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
#include "perf_regs_mask.h"

struct page_faults_ctx {
    struct callchain_ctx *cc;
    struct callchain_ctx *print_ip;
    bool timely_sym;
    bool sample;
};

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct page_faults_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    tep__ref();
    if (dev->env->callchain) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), stdout);
        dev->pages *= 2;
    }
    ctx->print_ip = callchain_ctx_new(CALLCHAIN_KERNEL | CALLCHAIN_USER, stdout);
    callchain_ctx_config(ctx->print_ip, 0, 1, 1, 0, 0, '\n', '\n');
    return 0;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct page_faults_ctx *ctx = dev->private;
    callchain_ctx_free(ctx->print_ip);
    callchain_ctx_free(ctx->cc);
    tep__unref();
    free(ctx);
}

static int page_faults_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct page_faults_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_SOFTWARE,
        .config        = PERF_COUNT_SW_PAGE_FAULTS,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_REGS_USER : 0),
        .read_format   = 0,
        .sample_regs_user = PERF_REGS_MASK,
        .pinned        = 1,
        .disabled      = 1,
        .exclude_user  = env->exclude_user,
        .exclude_kernel = env->exclude_kernel,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    if (!attr.watermark)
        ctx->timely_sym = 1;

    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void page_faults_exit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}



// in linux/perf_event.h
// PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
// PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_REGS_USER
struct sample_type_header {
    __u64			ip;
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    __u64   addr;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    struct callchain callchain;
};

struct sample_regs_user {
    u64     abi;
    u64     regs[hweight64(PERF_REGS_MASK)];
};

static void print_regs_user(struct sample_regs_user *regs_user, u64 unused)
{
#if defined(__i386__) || defined(__x86_64__)
#define REG(r) regs_user->regs[PERF_REG_X86_##r - (PERF_REG_X86_##r > PERF_REG_X86_DS ? REG_NOSUPPORT_N : 0)]
    printf("      RIP: %016lx RSP: %016lx RFLAGS:%08lx\n", REG(IP), REG(SP), REG(FLAGS));
    printf("      RAX: %016lx RBX: %016lx RCX: %016lx\n", REG(AX), REG(BX), REG(CX));
    printf("      RDX: %016lx RSI: %016lx RDI: %016lx\n", REG(DX), REG(SI), REG(DI));

#if defined(__i386__)
    printf("      RBP: %016lx CS: %04lx SS: %04lx\n", REG(BP), REG(CS), REG(SS));
#else
    printf("      RBP: %016lx R08: %016lx R09: %016lx\n", REG(BP), REG(R8), REG(R9));
    printf("      R10: %016lx R11: %016lx R12: %016lx\n", REG(R10), REG(R11), REG(R12));
    printf("      R13: %016lx R14: %016lx R15: %016lx\n", REG(R13), REG(R14), REG(R15));
    printf("      CS: %04lx SS: %04lx\n", REG(CS), REG(SS));
#endif

#elif defined(__aarch64__)
#define REG(r) regs_user->regs[PERF_REG_ARM64_##r]
    printf("      X00: %016lx X01: %016lx X02: %016lx X03: %016lx\n", REG(X0), REG(X1), REG(X2), REG(X3));
    printf("      X04: %016lx X05: %016lx X06: %016lx X07: %016lx\n", REG(X4), REG(X5), REG(X6), REG(X7));
    printf("      X08: %016lx X09: %016lx X10: %016lx X11: %016lx\n", REG(X8), REG(X9), REG(X10), REG(X11));
    printf("      X12: %016lx X13: %016lx X14: %016lx X15: %016lx\n", REG(X12), REG(X13), REG(X14), REG(X15));
    printf("      X16: %016lx X17: %016lx X18: %016lx X19: %016lx\n", REG(X16), REG(X17), REG(X18), REG(X19));
    printf("      X20: %016lx X21: %016lx X22: %016lx X23: %016lx\n", REG(X20), REG(X21), REG(X22), REG(X23));
    printf("      X24: %016lx X25: %016lx X26: %016lx X27: %016lx\n", REG(X24), REG(X25), REG(X26), REG(X27));
    printf("      X28: %016lx X29: %016lx LR: %016lx\n", REG(X28), REG(X29), REG(LR));
    printf("      SP: %016lx PC: %016lx\n", REG(SP), REG(PC));
#endif
}

static void page_faults_print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    struct page_faults_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct sample_regs_user *regs_user;
    struct {
        __u64 nr;
        __u64 ips[2];
    } callchain;

    if (!(flags & OMIT_TIMESTAMP))
        prof_dev_print_time(dev, data->time, stdout);

    tep__update_comm(NULL, data->tid_entry.tid);
    printf("%16s %6u [%03d] %llu.%06llu: page-fault: addr %016llx ip ", tep__pid_to_comm(data->tid_entry.tid), data->tid_entry.tid,
           data->cpu_entry.cpu, data->time / NSEC_PER_SEC, (data->time % NSEC_PER_SEC)/1000, data->addr);

    if (data->ip >= START_OF_KERNEL || (ctx->sample && ctx->timely_sym)) {
        callchain.nr = 2;
        callchain.ips[0] = data->ip >= START_OF_KERNEL ? PERF_CONTEXT_KERNEL : PERF_CONTEXT_USER;
        callchain.ips[1] = data->ip;
        print_callchain(ctx->print_ip, (struct callchain *)&callchain, data->tid_entry.pid);
    } else
        printf("%016llx\n", data->ip);

    if (dev->env->callchain && !(flags & OMIT_CALLCHAIN)) {
        struct callchain_data cd;
        perf_event_build_callchain_data(perf_event_evsel(dev, event), event, &cd);
        regs_user = (struct sample_regs_user *)&data->callchain.ips[data->callchain.nr];
        print_callchain_data_cbs(ctx->cc, &cd, NULL, (callchain_cbs)print_regs_user, regs_user);
    }
}

static void page_faults_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct page_faults_ctx *ctx = dev->private;
    ctx->sample = 1;
    page_faults_print_event(dev, event, instance, 0);
    ctx->sample = 0;
}

static const char *page_faults_desc[] = PROFILER_DESC("page-faults",
    "[OPTION...] [-g]",
    "Trace page fault exception.", "",
    "Additional sampling of user-mode general registers helps analyze the",
    "page fault stack.",
    "",
    "EXAMPLES",
    "    "PROGRAME" page-faults -p 2347 -g",
    "    "PROGRAME" page-faults -C 0 -g");
static const char *page_faults_argv[] = PROFILER_ARGV("page-faults",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER, "exclude-user", "exclude-kernel",
    PROFILER_ARGV_PROFILER, "call-graph");
static profiler page_faults = {
    .name = "page-faults",
    .desc = page_faults_desc,
    .argv = page_faults_argv,
    .pages = 2,
    .init = page_faults_init,
    .deinit = page_faults_exit,
    .print_event = page_faults_print_event,
    .sample = page_faults_sample,
};
PROFILER_REGISTER(page_faults)


