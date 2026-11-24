// SPDX-License-Identifier: GPL-2.0
/*
 * bpf:sched_rundelay, the scheduling delays: from a task becoming runnable
 * (sched_wakeup, sched_wakeup_new, or switched out still runnable by
 * sched_switch) to the sched_switch that switches it in.
 *
 * Everything a BPF event source needs is here: the perf_events map and
 * perf_output() from perf_output.bpf.h, BPF_EVENT_DESC(), the filter
 * placeholder DEFINE_EXPR_FILTER() naming the event struct, and the whole
 * struct output at once. Tracing a process is configured by
 * src/bpf_sched_rundelay.c. See src/bpf_event.c.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#include "sched_rundelay.h"
#include "perf_output.bpf.h"
#include "expr_filter.bpf.h"
#include "bpf_event.bpf.h"

BPF_EVENT_DESC("Scheduling delay: from wakeup, wakeup_new or preemption to running.");

#define MAX_CPUS 4096

/* Set by prepare: the tasks of this process only, 0 for all. */
const volatile u32 filter_tgid = 0;

struct runnable {
    u64 time;
    u32 reason;
    s32 cpu;
};

/* pid -> since when it is runnable; removed when it runs, or exits. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, u32);
    __type(value, struct runnable);
} runnable SEC(".maps");

/* The event is built here: an expression may write to it, which needs writable memory. */
struct sched_rundelay_event percpu_event[MAX_CPUS] = {};

DEFINE_EXPR_FILTER(struct sched_rundelay_event)

/*
 * Kernel constraint: task_struct::state became __state in 2f064a59a11f
 * ("sched: Change task_struct::state"), in v5.14. vmlinux.h has the one of
 * the build machine, so both are read through flavors.
 */
struct task_struct___new {
    unsigned int __state;
} __attribute__((preserve_access_index));
struct task_struct___old {
    long state;
} __attribute__((preserve_access_index));

static __always_inline long task_state(struct task_struct *t)
{
    if (bpf_core_field_exists(((struct task_struct___new *)t)->__state))
        return BPF_CORE_READ((struct task_struct___new *)t, __state);
    return BPF_CORE_READ((struct task_struct___old *)t, state);
}

static __always_inline int task_cpu(struct task_struct *t)
{
    /* Only with CONFIG_THREAD_INFO_IN_TASK, as on x86 and arm64. */
    if (bpf_core_field_exists(t->cpu))
        return BPF_CORE_READ(t, cpu);
    return -1;
}

static __always_inline void set_runnable(struct task_struct *p, u32 reason, int cpu)
{
    struct runnable r;
    u32 pid = BPF_CORE_READ(p, pid);

    /* The idle tasks are never waited for. */
    if (!pid)
        return;
    if (filter_tgid && BPF_CORE_READ(p, tgid) != filter_tgid)
        return;

    r.time = bpf_ktime_get_ns();
    r.reason = reason;
    r.cpu = cpu;
    bpf_map_update_elem(&runnable, &pid, &r, BPF_ANY);
}

/* The woken task is on its target cpu by now: task_cpu(p) is the target_cpu of the tracepoint. */
SEC("raw_tp/sched_wakeup")
int BPF_PROG(sched_wakeup, struct task_struct *p)
{
    set_runnable(p, RUNDELAY_WAKEUP, task_cpu(p));
    return 0;
}

SEC("raw_tp/sched_wakeup_new")
int BPF_PROG(sched_wakeup_new, struct task_struct *p)
{
    set_runnable(p, RUNDELAY_WAKEUP_NEW, task_cpu(p));
    return 0;
}

/*
 * Kernel constraint: the first arguments of sched_switch are (bool preempt,
 * prev, next) since c73464b1c843 ("sched/core: Fix trace_sched_switch()"),
 * in v4.4; prev_state was appended in fa2c3254d7cf ("sched/tracing: Don't
 * re-read p->state when emitting sched_switch event"), in v5.18, and is not
 * read. A preempted prev is still on the runqueue whatever its state, an
 * unpreempted one only if its state is TASK_RUNNING (0), e.g. after yield().
 */
SEC("raw_tp/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev, struct task_struct *next)
{
    u32 cpu = bpf_get_smp_processor_id();
    struct sched_rundelay_event *e;
    struct runnable *r;
    u32 pid;

    if (preempt || task_state(prev) == 0)
        set_runnable(prev, RUNDELAY_PREEMPT, cpu);

    pid = BPF_CORE_READ(next, pid);
    r = bpf_map_lookup_elem(&runnable, &pid);
    if (!r || cpu >= MAX_CPUS)
        return 0;

    e = &percpu_event[cpu];
    e->tgid = BPF_CORE_READ(next, tgid);
    e->pid = pid;
    e->prio = BPF_CORE_READ(next, prio);
    e->policy = BPF_CORE_READ(next, policy);
    e->rundelay = bpf_ktime_get_ns() - r->time;
    e->reason = r->reason;
    e->runnable_cpu = r->cpu;
    BPF_CORE_READ_STR_INTO(&e->comm, next, comm);
    bpf_map_delete_elem(&runnable, &pid);

    if (expr_filter(e))
        perf_output(ctx, e, sizeof(*e));
    return 0;
}

/* A task woken but killed before it ran leaves no entry behind. */
SEC("raw_tp/sched_process_free")
int BPF_PROG(sched_process_free, struct task_struct *p)
{
    u32 pid = BPF_CORE_READ(p, pid);

    bpf_map_delete_elem(&runnable, &pid);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
