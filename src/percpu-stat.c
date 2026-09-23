#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/rblist.h>
#include "monitor.h"
#include "trace_helpers.h"
#include "tep.h"

struct swevent_stat {
    uint64_t count;
    uint64_t diff;
};
struct swevent_binding {
    struct prof_binding binding;
    struct swevent_stat stat;
};
struct evsel_node {
    struct rb_node rbnode;
    struct evsel_node *next;
    struct perf_evsel *evsel;
    const char *name;
    int name_len;
    bool cpu_idle;
    struct prof_bindings bindings;
    struct swevent_stat total_stats;
};

struct percpu_stat_ctx {
    struct prof_bindings bindings;
    struct evsel_node *first;
    struct evsel_node **p_next;
    struct rblist evsel_list;
    struct tp_list *tp_list;
};

static int evsel_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct evsel_node *e = container_of(rbn, struct evsel_node, rbnode);
    const struct evsel_node *n = entry;

    if (e->evsel > n->evsel)
        return 1;
    else if (e->evsel < n->evsel)
        return -1;
    else
        return 0;
}

static struct rb_node *evsel_node_new(struct rblist *rlist, const void *new_entry)
{
    struct percpu_stat_ctx *ctx = container_of(rlist, struct percpu_stat_ctx, evsel_list);
    const struct evsel_node *n = new_entry;
    struct evsel_node *e = malloc(sizeof(*e));
    if (e) {
        e->next = NULL;
        e->evsel = n->evsel;
        e->name = n->name;
        e->name_len = (int)strlen(e->name);
        e->cpu_idle = n->cpu_idle;
        e->bindings = (struct prof_bindings) { .size = sizeof(struct swevent_binding) };
        e->total_stats = (struct swevent_stat) {};
        *ctx->p_next = e;
        ctx->p_next = &e->next;
        RB_CLEAR_NODE(&e->rbnode);
        return &e->rbnode;
    } else
        return NULL;
}

static void evsel_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct evsel_node *e = container_of(rb_node, struct evsel_node, rbnode);
    prof_bindings_exit(&e->bindings);
    free(e);
}

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct percpu_stat_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    ctx->bindings.size = sizeof(struct prof_binding);

    ctx->first = NULL;
    ctx->p_next = &ctx->first;

    rblist__init(&ctx->evsel_list);
    ctx->evsel_list.node_cmp = evsel_node_cmp;
    ctx->evsel_list.node_new = evsel_node_new;
    ctx->evsel_list.node_delete = evsel_node_delete;

    return 0;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct percpu_stat_ctx *ctx = dev->private;
    rblist__exit(&ctx->evsel_list);
    prof_bindings_exit(&ctx->bindings);
    free(ctx);
}

static struct perf_evsel *perf_tp_event(struct perf_evlist *evlist, const char *sys, const char *name)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .freq          = 0,
        .sample_type   = 0,
        .read_format   = 0,
        .pinned        = 0,
        .disabled      = 0,
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

static struct perf_evsel *perf_sw_event(struct perf_evlist *evlist, int config)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_SOFTWARE,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 0,
        .freq          = 0,
        .sample_type   = 0,
        .read_format   = 0,
        .pinned        = 0,
        .disabled      = 0,
    };
    struct perf_evsel *evsel;

    attr.config = config;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        return NULL;
    }
    perf_evlist__add(evlist, evsel);

    return evsel;
}

static void __evsel_name(struct percpu_stat_ctx *ctx, struct perf_evsel *evsel, const char *name, bool cpu_idle)
{
    struct evsel_node n;
    if (evsel) {
        n.evsel = evsel;
        n.name = name;
        n.cpu_idle = cpu_idle;
        rblist__add_node(&ctx->evsel_list, &n);
    }
}
#define evsel_name(evsel, name) __evsel_name(ctx, (evsel), (name), false)
static int percpu_stat_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct percpu_stat_ctx *ctx;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    if (env->interval == 0)
        env->interval = 1000;
    env->perins = true;

    //software event
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_CONTEXT_SWITCHES), " SOFT csw");
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_CPU_MIGRATIONS), "cpu-mig");
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_PAGE_FAULTS_MIN), "minflt");
    evsel_name(perf_sw_event(evlist, PERF_COUNT_SW_PAGE_FAULTS_MAJ), "majflt");
    //syscalls
    if (env->syscalls)
        evsel_name(perf_tp_event(evlist, "raw_syscalls", "sys_enter"), " syscalls");
    //irq, softirq, workqueue
    evsel_name(perf_tp_event(evlist, "irq", "irq_handler_entry"), " hardirq");
    evsel_name(perf_tp_event(evlist, "irq", "softirq_entry"), "softirq");
    evsel_name(perf_tp_event(evlist, "timer", "hrtimer_expire_entry"), "hrtimer");
    evsel_name(perf_tp_event(evlist, "workqueue", "workqueue_execute_start"), "workqueue");
    //kvm
    evsel_name(perf_tp_event(evlist, "kvm", "kvm_exit"), " KVM exit");
    //net
    evsel_name(perf_tp_event(evlist, "net", "netif_receive_skb"), " NET recv");
    evsel_name(perf_tp_event(evlist, "net", "net_dev_xmit"), " xmit");
    evsel_name(perf_tp_event(evlist, "net", "napi_gro_receive_entry"), "gro");
    //page alloc
    evsel_name(perf_tp_event(evlist, "kmem", "mm_page_alloc"), " MM alloc");
    evsel_name(perf_tp_event(evlist, "compaction", "mm_compaction_migratepages"), "compact");
    evsel_name(perf_tp_event(evlist, "vmscan", "mm_vmscan_direct_reclaim_begin"), "reclaim");
    evsel_name(perf_tp_event(evlist, "migrate", "mm_migrate_pages"), "migrate");
    //page cache
    evsel_name(perf_tp_event(evlist, "filemap", "mm_filemap_add_to_page_cache"), " PAGE cache");
    evsel_name(perf_tp_event(evlist, "writeback", "wbc_writepage"), " WB pages");
    //cpu_idle
    __evsel_name(ctx, perf_tp_event(evlist, "power", "cpu_idle"), " idle", true);

    perf_evlist__set_leader(evlist);

    return 0;
}

static void percpu_stat_exit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}

static int percpu_stat_del_thread(struct prof_dev *dev, pid_t tid)
{
    struct percpu_stat_ctx *ctx = dev->private;
    struct evsel_node *e;

    for (e = ctx->first; e; e = e->next)
        prof_bindings_remove_thread(&e->bindings, tid);
    prof_bindings_remove_thread(&ctx->bindings, tid);
    return 0;
}

static int percpu_stat_read(struct prof_dev *dev, struct perf_evsel *evsel, struct perf_counts_values *count, int cpu, int tid)
{
    struct percpu_stat_ctx *ctx = dev->private;
    struct evsel_node n = {.evsel = evsel};
    struct rb_node *rbn = rblist__find(&ctx->evsel_list, &n);
    struct evsel_node *e = rbn ? container_of(rbn, struct evsel_node, rbnode) : NULL;
    struct swevent_binding *state;

    if (e == NULL)
        return 0;
    if (!prof_binding_get(&ctx->bindings, cpu, tid))
        return 0;
    state = prof_binding_get(&e->bindings, cpu, tid);
    if (!state)
        return 0;

    state->stat.diff = 0;
    if (count->val > state->stat.count) {
        state->stat.diff = count->val - state->stat.count;
        state->stat.count = count->val;
        if (e->cpu_idle) {
            //cpu_idle, contains enter and exit, must be divided by 2
            state->stat.diff /= 2;
        }
    }
    e->total_stats.diff += state->stat.diff;
    return 0;
}

static void percpu_stat_interval(struct prof_dev *dev)
{
    struct percpu_stat_ctx *ctx = dev->private;
    struct evsel_node *next = ctx->first;
    struct prof_binding *binding;

    print_time(stdout);
    printf("\n[CPU] ");
    while (next) {
        printf("%s ", next->name);
        next = next->next;
    }

    if (dev->env->perins)
    prof_bindings_for_each(&ctx->bindings, binding) {
        printf("\n[%03d] ", binding->cpu >= 0 ? binding->cpu : binding->tid);
        next = ctx->first;
        while (next) {
            struct swevent_binding *state = prof_binding_find(&next->bindings, binding->cpu, binding->tid);
            printf("%*lu ", next->name_len, state ? state->stat.diff : 0);
            next = next->next;
        }
    }

    printf("\n[ALL] ");
    next = ctx->first;
    while (next) {
        printf("%*lu ", next->name_len, next->total_stats.diff);
        next->total_stats.diff = 0;
        next = next->next;
    }
    printf("\n");
}

static void percpu_stat_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
}


static const char *percpu_stat_desc[] = PROFILER_DESC("percpu-stat",
    "[OPTION...] [--syscalls]",
    "Handpicked event statistics.", "",
    "TRACEPOINT",
    "    PERF_COUNT_SW_CONTEXT_SWITCHES  PERF_COUNT_SW_CPU_MIGRATIONS",
    "    PERF_COUNT_SW_PAGE_FAULTS_MIN   PERF_COUNT_SW_PAGE_FAULTS_MAJ",
    "    irq:irq_handler_entry           irq:softirq_entry",
    "    timer:hrtimer_expire_entry      workqueue:workqueue_execute_start",
    "    kvm:kvm_exit                    net:netif_receive_skb",
    "    net:net_dev_xmit                net:napi_gro_receive_entry      ",
    "    kmem:mm_page_alloc              compaction:mm_compaction_migratepages",
    "    migrate:mm_migrate_pages        vmscan:mm_vmscan_direct_reclaim_begin",
    "    writeback:wbc_writepage         filemap:mm_filemap_add_to_page_cache",
    "    raw_syscalls:sys_enter          power:cpu_idle", "",
    "EXAMPLES",
    "    "PROGRAME" percpu-stat -C 0",
    "    "PROGRAME" percpu-stat --syscalls -i 2000");
static const char *percpu_stat_argv[] = PROFILER_ARGV("percpu-stat",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "syscalls");
static profiler percpu_stat = {
    .name = "percpu-stat",
    .desc = percpu_stat_desc,
    .argv = percpu_stat_argv,
    .pages = 0,
    .init = percpu_stat_init,
    .deinit = percpu_stat_exit,
    .del_thread = percpu_stat_del_thread,
    .interval = percpu_stat_interval,
    .read   = percpu_stat_read,
    .sample = percpu_stat_sample,
};
PROFILER_REGISTER(percpu_stat);
