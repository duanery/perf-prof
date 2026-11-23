// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 duanery

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#define BREAK    0
#define CONTINUE 1


// irq_disabled
//   irq_disabled = true,  Interrupt cleared, continue.
//   irq_disabled = false, Interrupt enabled, continue.
//
// if (!((EFLAGS >> EFLAGS_IF_BIT) & 1) == irqs_disabled)
//     continue;
// else
//     break;
const volatile bool filter_irqs_disabled = false;
const volatile bool irqs_disabled = false;
#if defined(__TARGET_ARCH_x86)
#define X86_EFLAGS_IF_BIT   9
#define EFLAGS_IF_BIT   X86_EFLAGS_IF_BIT
#define EFLAGS flags
#elif defined(__TARGET_ARCH_arm)
#define ARM_cpsr   uregs[16]
#define PSR_I_BIT  0x00000080 // bit 7
#define EFLAGS_IF_BIT   7
#define EFLAGS ARM_cpsr
#elif defined(__TARGET_ARCH_arm64)
#define PSR_I_BIT  0x00000080 // bit 7
#define EFLAGS_IF_BIT   7
#define EFLAGS pstate
#endif


// tif_need_resched
//   tif_need_resched = true,  TIF_NEED_RESCHED set, continue.
//   tif_need_resched = false, TIF_NEED_RESCHED not set, continue.
//
// if (((flags >> TIF_NEED_RESCHED) & 1) == tif_need_resched)
//     continue;
// else
//     break;
const volatile bool filter_tif_need_resched = false;
const volatile bool tif_need_resched = false;
/*
 * Kernel constraint: the TIF_* bits are per arch, arch/<arch>/include/asm/
 * thread_info.h, and thread_info is the first member of task_struct only with
 * CONFIG_THREAD_INFO_IN_TASK, on x86 since 15f4eae70d36 ("x86: Move
 * thread_info into task_struct"), in v4.9. Before it the CO-RE read of
 * task->thread_info fails to relocate, the load fails.
 */
#if defined(__TARGET_ARCH_x86)
#define TIF_NEED_RESCHED    3
#elif defined(__TARGET_ARCH_arm)
#define TIF_NEED_RESCHED    1
#elif defined(__TARGET_ARCH_arm64)
#define TIF_NEED_RESCHED    1
#endif


// sched_policy
//
// if (task->policy == sched_policy)
//     continue;
// else
//     break;
const volatile bool filter_sched_policy = false;
const volatile u32  sched_policy = 0;


// sched_prio filter
//
// Only allow events from tasks with priorities in the sched_prio array
// if (task->prio is marked in sched_prio[])
//     continue;  // allow event
// else
//     break;     // filter out event
#define MAX_PRIO 140  // Linux scheduling priorities: 0-99 (RT), 100-139 (normal)
const volatile bool filter_sched_prio = false;
const volatile u32 sched_prio[MAX_PRIO] = {0};


// exclude_pid
//
// if (pid == exclude_pid)
//     break;
// else
//     continue;
const volatile bool filter_exclude_pid = false;
const volatile u32  exclude_pid = 0;


// nr_running
//   nr_running is greater than `nr_running_min', less than `nr_running_max', continue.
//
// if (nr_running_min <= nr_running <= nr_running_max)
//     continue;
// else
//     break;
const volatile bool filter_nr_running = false;
const volatile u32 nr_running_min = 0;
const volatile u32 nr_running_max = 0xffffffff;


SEC("perf_event")
int perf_event_do_filter(struct bpf_perf_event_data *ctx)
{
    struct task_struct *task;
    struct rq *rq;
    u32 nr_running = 0;
    unsigned long flags;
    u32 pid, policy, prio;

    if (filter_irqs_disabled) {
        if (((ctx->regs.EFLAGS >> EFLAGS_IF_BIT) & 1) == irqs_disabled)
            return BREAK;
    }

    task = (void*)bpf_get_current_task();

    if (filter_tif_need_resched) {
        flags = BPF_CORE_READ(task, thread_info.flags);
        if (((flags >> TIF_NEED_RESCHED) & 1) != tif_need_resched)
            return BREAK;
    }

    if (filter_sched_policy) {
        policy = BPF_CORE_READ(task, policy);
        if (policy != sched_policy)
            return BREAK;
    }

    if (filter_sched_prio) {
        prio = BPF_CORE_READ(task, prio);
        if (prio >= MAX_PRIO || !sched_prio[prio])
            return BREAK;
    }

    if (filter_exclude_pid) {
        pid = bpf_get_current_pid_tgid() >> 32;
        if (pid == exclude_pid)
            return BREAK;
    }

    if (filter_nr_running) {
        rq = BPF_CORE_READ(task, se.cfs_rq, rq);
        if (rq)
            nr_running = BPF_CORE_READ(rq, nr_running);
        else
            nr_running = BPF_CORE_READ(task, se.cfs_rq, nr_running);

        if (nr_running < nr_running_min)
            return BREAK;
        if (nr_running > nr_running_max)
            return BREAK;
    }

    return CONTINUE;
}

char LICENSE[] SEC("license") = "GPL";
