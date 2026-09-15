#ifndef __KVM_EXIT_H
#define __KVM_EXIT_H

/*
 * The bpf:kvm_exit event.
 *
 * Every field here is a *final* value, and this is also the type
 * expr_filter() takes, so these names are exactly the set a --filter
 * expression may use. Nothing in this struct is a timestamp or a snapshot as
 * far as a filter or a userspace consumer is concerned: the BPF program
 * resolves all three time fields before it filters and outputs the event.
 *
 * The time fields decompose as:
 *
 *   exit_latency   kvm:kvm_exit -> kvm:kvm_entry
 *     |
 *     +-- runq_delay    the vcpu was runnable, waiting for a CPU
 *     +-- offcpu_wait   the vcpu was off-CPU but not runnable (halt-poll,
 *     |                 blocked on I/O, sleeping)
 *     +-- (the rest)    the vcpu was on-CPU, in the host
 *
 * and are reported so that the breakdown always adds up:
 *
 *   0 <= runq_delay + offcpu_wait <= exit_latency
 *
 * runq_delay is derived from sched_info.run_delay, which is accounted in
 * rq_clock() units rather than the bpf_ktime_get_ns() clock the other two use,
 * so it is clamped to the measured off-CPU time to hold that invariant. See
 * kvm_entry() in kvm_exit.bpf.c.
 *
 * runq_delay, offcpu_wait and switches are only resolved in system-wide
 * (-C cpus) mode, which is the only mode that attaches to sched_switch. In
 * per-process (-p pid) mode they are always 0, and the event is truncated at
 * offsetof(runq_delay) on output.
 *
 * Entries in the kvm_vcpu map that holds this struct between kvm_exit and
 * kvm_entry are reclaimed in BPF, by the sched_process_free program. Userspace
 * plays no part in it, so a lost perf event cannot leave a stale entry behind
 * to exhaust the map or to be matched against a recycled pid.
 */
struct kvm_vcpu_event
{
    uint32_t tgid, pid;
    uint16_t isa;   //KVM_ISA_VMX  KVM_ISA_SVM
    uint16_t switches;      /* context switches between exit and entry */
    uint32_t exit_reason;
    int64_t exit_latency;   /* kvm_exit -> kvm_entry (ns) */
    int64_t runq_delay;     /* runnable, waiting for a CPU (ns) */
    int64_t offcpu_wait;    /* off-CPU but not runnable (ns), >= 0 */
};

/*
 * Scratch aliases for the three time fields.
 *
 * Between kvm_exit and kvm_entry the BPF program has nowhere else to keep its
 * bookkeeping, so it parks intermediate values in the very fields it will
 * later overwrite with the final ones. That is what used to make the field
 * names unreadable: `latency' held a timestamp for most of its life.
 *
 * These macros do not add storage -- each one is the same field under a name
 * that says what it holds *at that point in the pipeline*. Read through the
 * macro while the value is an intermediate, and through the field name once it
 * is final. kvm_entry() performs the changeover, before expr_filter() runs, so
 * a filter and a userspace consumer only ever see final values.
 */
#define EXIT_TIME(e)        ((e)->exit_latency)  /* bpf_ktime_get_ns() at kvm_exit */
#define RUNQ_DELAY_SNAP(e)  ((e)->runq_delay)    /* sched_info.run_delay at switch-out */
#define OFFCPU_ACC(e)       ((e)->offcpu_wait)   /* off-CPU total accumulated so far */
/*
 * Same field as OFFCPU_ACC(), used while the vcpu is switched out and its
 * event lives in the kvm_vcpu hashmap: the switch-out timestamp, biased by
 * whatever had already been accumulated. Switching back in computes
 * `now - OFFCPU_TS()', which yields the new total in one subtraction.
 */
#define OFFCPU_TS(e)        ((e)->offcpu_wait)

#define KVM_ISA_VMX   1
#define KVM_ISA_SVM   2
#define KVM_ISA_ARM   3

#define EXIT_REASON_HLT        12     // intel
#define SVM_EXIT_HLT           0x078  // amd
#define ARM_EXIT_HLT           0x01   // arm64

#if defined(__aarch64__) || defined(__TARGET_ARCH_arm64)

#define ARM_EXIT_WITH_SERROR_BIT  31
#define ARM_EXCEPTION_CODE(x)     ((x) & ~(1U << ARM_EXIT_WITH_SERROR_BIT))
#define ARM_EXCEPTION_IS_TRAP(x)  (ARM_EXCEPTION_CODE((x)) == ARM_EXCEPTION_TRAP)
#define ARM_SERROR_PENDING(x)     !!((x) & (1U << ARM_EXIT_WITH_SERROR_BIT))

#define ARM_EXCEPTION_IRQ         0
#define ARM_EXCEPTION_EL1_SERROR  1
#define ARM_EXCEPTION_TRAP        2
#define ARM_EXCEPTION_IL          3
/* The hyp-stub will return this for any kvm_call_hyp() call */
#define ARM_EXCEPTION_HYP_GONE    HVC_STUB_ERR

#define HVC_STUB_ERR      0xbadca11

#define ARM_EXCEPTION_REASON(exit_code) (0x80000000 | (exit_code))

#define EXIT_REASON(exit_code, esr_ec) \
    (ARM_EXCEPTION_IS_TRAP(exit_code) ? esr_ec : ARM_EXCEPTION_REASON(exit_code))

#endif

#endif
