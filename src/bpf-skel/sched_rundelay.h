#ifndef __SCHED_RUNDELAY_H
#define __SCHED_RUNDELAY_H

/*
 * The bpf:sched_rundelay event, see sched_rundelay.bpf.c. Shared with
 * userspace, where the skeleton names it.
 */

/* How the task became runnable, when the delay started. */
enum sched_rundelay_reason {
    RUNDELAY_WAKEUP,        /* sched_wakeup */
    RUNDELAY_WAKEUP_NEW,    /* sched_wakeup_new: a new task */
    RUNDELAY_PREEMPT,       /* sched_switch: switched out still runnable */
};

/*
 * One delay: from becoming runnable to being switched in. Output by the
 * sched_switch that switches the task in, so in the context of the task
 * switched out: the comm and pid of the sample are not this task's.
 */
struct sched_rundelay_event {
    uint32_t tgid, pid;
    int32_t prio;
    uint32_t policy;
    uint64_t rundelay;                  /* ns */
    enum sched_rundelay_reason reason;
    int32_t runnable_cpu;               /* the cpu it became runnable on; the sample cpu is where it runs */
    char comm[16];
};

#endif
