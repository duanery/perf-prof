#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <api/fs/fs.h>
#include <monitor.h>
#include <bpf_event.h>


void common_help(struct help_ctx *ctx, bool enabled, bool cpus, bool pids, bool interval, bool order, bool pages, bool verbose)
{
    struct env *env = ctx->env;

    if (!enabled)
        goto can_be_enabled;

    if (cpus && env->cpumask)
        printf("-C %s ", env->cpumask);
    if (pids && env->pids)
        printf("-p %s ", env->pids);
    if (interval && env->interval)
        printf("-i %d ", env->interval);
    if (order && env->order)
        printf("--order");
    if (pages && env->mmap_pages)
        printf("-m %d ", env->mmap_pages);
    if (verbose && env->verbose)
        printf("-v ");
    return;

can_be_enabled:
    if (cpus && !env->cpumask)
        printf("[-C .] ");
    if (pids && !env->pids)
        printf("[-p .] ");
    if (interval && !env->interval)
        printf("[-i .] ");
    if (order && !env->order)
        printf("[--order] ");
    if (pages && !env->mmap_pages)
        printf("[-m .] ");
    if (verbose && !env->verbose)
        printf("[-v] ");
}


static void monitor_help(struct monitor *m, struct help_ctx *ctx)
{
    if (m && m->help)
        m->help(ctx);
}

static void monitors_help(struct help_ctx *ctx)
{
    struct monitor *m = NULL;
    while((m = monitor_next(m))) {
        monitor_help(m, ctx);
    }
}

static void prinf_kprobe_uprobe_format(struct tp *tp)
{
    printf("name: %s\n", tp->name);
    printf("ID: perf %s\n", tp->sys);
    printf("format:\n"
           "\tfield:unsigned short common_type;       offset:0;       size:2; signed:0;\n"
           "\tfield:unsigned char common_flags;       offset:2;       size:1; signed:0;\n"
           "\tfield:unsigned char common_preempt_count;       offset:3;       size:1; signed:0;\n"
           "\tfield:int common_pid;   offset:4;       size:4; signed:1;\n"
           "\n");

    if (tp->id == KPROBE || tp->id == UPROBE)
    printf("\tfield:unsigned long __probe_ip; offset:8;       size:8; signed:0;\n"
           "\n"
           "print fmt: \"(%%lx)\", REC->__probe_ip\n");

    if (tp->id == KRETPROBE || tp->id == URETPROBE)
    printf("\tfield:unsigned long __probe_func;       offset:8;       size:8; signed:0;\n"
           "\tfield:unsigned long __probe_ret_ip;     offset:16;      size:8; signed:0;\n"
           "\n"
           "print fmt: \"(%%lx <- %%lx)\", REC->__probe_func, REC->__probe_ret_ip\n");
}

static void print_events_format(struct help_ctx *ctx)
{
    int i, j;
    int ret;
    char path[256];
    char *format;
    size_t size;

    for (i = 0; i < ctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(ctx->tp_list[i], tp, j) {
            printf("%s:%s\n", tp->sys, tp->name);
            snprintf(path, sizeof(path), "kernel/debug/tracing/events/%s/%s/format", tp->sys, tp->name);
            if (tp_is_bpf(tp))
                bpf_event_source_help(tp->bpf_src);
            else if (sysfs__read_str(path, &format, &size) == 0) {
                ret = write(STDOUT_FILENO, format, size);
                free(format);
                if (ret == -1)
                    return;
            } else
                prinf_kprobe_uprobe_format(tp);
            printf("\n");
        }
    }
}

static int help_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct help_ctx _ctx;
    struct help_ctx *ctx = &_ctx;
    int i;

    ctx->env = env;
    ctx->nr_list = env->nr_events + !!env->tp_alloc + !!env->tp_free;
    if (!ctx->nr_list)
        exit(0);
    ctx->tp_list = calloc(ctx->nr_list, sizeof(*ctx->tp_list));
    if (!ctx->tp_list)
        exit(-1);

    tep__ref_light();

    for (i = 0; i < env->nr_events; i++) {
        ctx->tp_list[i] = tp_list_new(dev, env->events[i]);
        if (!ctx->tp_list[i]) {
            exit(-1);
        }
    }
    if (env->tp_alloc) {
        ctx->tp_list[i++] = tp_list_new(dev, env->tp_alloc);
        if (!ctx->tp_list[i-1])
            exit(1);
    }
    if (env->tp_free) {
        ctx->tp_list[i++] = tp_list_new(dev, env->tp_free);
        if (!ctx->tp_list[i-1])
            exit(1);
    }

    printf("\n");
    if (env->help_monitor)
        monitor_help(env->help_monitor, ctx);
    else
        monitors_help(ctx);

    printf("\n");
    print_events_format(ctx);

    exit(0);
}

static void help_exit(struct prof_dev *dev)
{

}

static const char *help_desc[] = PROFILER_DESC("",
    "[profiler] [PROFILER OPTION...] help",
    "Help to quickly assemble commands.", "",
    "SYNOPSIS",
    "    Helps writing event attrs, event filters, 'help' can be added anywhere",
    "    in the command, but must be after the profiler.", "",
    "    Profiler can be omitted.", "",
    "EXAMPLES",
    "    "PROGRAME" trace -e sched:sched_wakeup help",
    "    "PROGRAME" -e sched:sched_wakeup,sched:sched_switch help");
static const char *help_argv[] = PROFILER_ARGV("help",
    "OPTION:", "help",
    PROFILER_ARGV_PROFILER, "event", "alloc", "free");
static profiler help_profiler = {
    .name = "help",
    .desc = help_desc,
    .argv = help_argv,
    .init = help_init,
    .deinit = help_exit,
};
PROFILER_REGISTER(help_profiler)


