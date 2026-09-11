#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#include "kvm_exit.h"
#include "perf_output.bpf.h"
#include "expr_filter.bpf.h"

#define MAX_CPUS 4096
#define MAX_VCPUS 8192
#define INT64_MAX 9223372036854775807UL

const volatile unsigned int filter_pid = 0;
unsigned char work_cpus[MAX_CPUS] = {0};
struct kvm_vcpu_event percpu_event[MAX_CPUS] = {0};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_VCPUS);
    __type(key, int);
    __type(value, struct kvm_vcpu_event);
} kvm_vcpu SEC(".maps");

/* Kernel-side expression filter for the kvm_vcpu_event event source.
 * See expr_filter.bpf.h. */
DEFINE_EXPR_FILTER(struct kvm_vcpu_event)

static __always_inline int kvm_exit_oncpu(u32 exit_reason, u32 isa)
{
    struct kvm_vcpu_event *data;
    u64 cpu = bpf_get_smp_processor_id();

    barrier_var(cpu);
    if (cpu >= MAX_CPUS || !work_cpus[cpu])
        return 0;

    data = &percpu_event[cpu];
    data->exit_reason = exit_reason;
    EXIT_TIME(data) = bpf_ktime_get_ns();

    if (!data->pid) {
        u64 id = bpf_get_current_pid_tgid();
        data->tgid = (u32)(id >> 32);
        data->pid = (u32)id;
        data->isa = isa;
    }
    return 0;
}

#ifdef __TARGET_ARCH_arm64
SEC("raw_tp/kvm_exit")
int BPF_PROG(kvm_exit, int ret, unsigned int esr_ec, unsigned long vcpu_pc)
{
    return kvm_exit_oncpu(EXIT_REASON((u32)ret, esr_ec), KVM_ISA_ARM);
}
#else
/*
 * x86 has two mutually exclusive ways to read exit_reason, and which one works
 * depends on the kernel. kvm_exit_select_prog() in bpf_kvm_exit.c autoloads
 * exactly one of the pair below.
 *
 * Up to v5.15 the tracepoint carried exit_reason as its first *argument*:
 *
 *     TP_PROTO(unsigned int exit_reason, struct kvm_vcpu *vcpu, u32 isa)
 *
 * so raw_tp/kvm_exit can take it straight from the argument list, which is
 * what kvm_exit_legacy() does.
 *
 * Since 0a62a0319abb ("KVM: x86: Get exit_reason as part of
 * kvm_x86_ops.get_exit_info"), in v5.16, the argument is gone:
 *
 *     TP_PROTO(struct kvm_vcpu *vcpu, u32 isa)
 *
 * exit_reason is now obtained inside TP_fast_assign(), via the
 * get_exit_info() callback, and only ever exists as a field of the trace
 * entry. A raw_tp program sees the *arguments*, so on v5.16+ there is no
 * exit_reason for it to read -- and no way to synthesise one either, since
 * get_exit_info() is a vendor callback (vmx/svm) that BPF cannot invoke.
 * Hence the tp/kvm/kvm_exit program, which runs later in the pipeline, after
 * TP_fast_assign() has populated the entry, and reads ctx->exit_reason.
 *
 * The struct below mirrors the head of that entry. It is not read from BTF,
 * so the three fields must stay in sync with TRACE_EVENT_KVM_EXIT() in
 * arch/x86/kvm/trace.h; fields past isa are not needed and are left out.
 *
 * Why `return 1' rather than 0: for a tp program the return value gates
 * whether the trace entry is still delivered to perf. See
 * perf_trace_run_bpf_submit() in kernel/events/core.c -- a zero from
 * trace_call_bpf() makes it drop the sample. Returning 0 would therefore
 * silently break any *other* consumer of kvm:kvm_exit (perf record, a
 * concurrent perf-prof kvm-exit) for as long as this program is attached.
 * The raw_tp variants have no such effect, which is why they can return the
 * helper's value directly.
 */
struct kvm_exit_trace_ctx {
    struct trace_entry ent;
    unsigned int exit_reason;
    unsigned long guest_rip;
    u32 isa;
};

/* v5.16+: exit_reason only exists in the trace entry. */
SEC("tp/kvm/kvm_exit")
int kvm_exit(struct kvm_exit_trace_ctx *ctx)
{
    kvm_exit_oncpu(ctx->exit_reason, ctx->isa);
    return 1; /* keep delivering the entry to other perf consumers */
}

/* <= v5.15: exit_reason is the first tracepoint argument. */
SEC("raw_tp/kvm_exit")
int BPF_PROG(kvm_exit_legacy, u32 exit_reason, void *vcpu, u32 isa)
{
    return kvm_exit_oncpu(exit_reason, isa);
}
#endif

SEC("raw_tp/kvm_entry")
int BPF_PROG(kvm_entry) // int vcpu_id | unsigned long vcpu_pc
{
    struct kvm_vcpu_event *data;
    u64 cpu = bpf_get_smp_processor_id();

    barrier_var(cpu);
    if (cpu >= MAX_CPUS || !work_cpus[cpu])
        return 0;

    data = &percpu_event[cpu];
    data->exit_latency = bpf_ktime_get_ns() - EXIT_TIME(data);
    /*
     * A vcpu first seen at sched_switch has no kvm_exit to pair with and gets
     * an INT64_MAX exit time, which comes out negative here. Drop those before
     * the filter runs, so an expression never sees a bogus first event.
     */
    if (data->exit_latency > 0) {
        /*
         * Turn the scratch values into the final runq_delay and offcpu_wait
         * before filtering, not just before output: until this runs,
         * RUNQ_DELAY_SNAP() is the raw sched_info.run_delay snapshot taken at
         * sched_switch rather than a delta, and OFFCPU_ACC() is the total
         * off-CPU time, which still includes the runqueue wait. An expression
         * referring to either field would otherwise see an intermediate.
         */
        if (data->switches) {
            struct task_struct *task = (void *)bpf_get_current_task();
            u64 run_delay = BPF_CORE_READ(task, sched_info.run_delay);
            s64 offcpu = OFFCPU_ACC(data);
            s64 runq = run_delay - RUNQ_DELAY_SNAP(data);

            /*
             * Both values below must fit inside exit_latency, but only one of
             * them is measured on the same clock as exit_latency.
             *
             * offcpu is a sum of bpf_ktime_get_ns() deltas, so it cannot
             * exceed exit_latency. runq comes from sched_info.run_delay, which
             * the kernel accumulates in rq_clock() units: sched_info_queued()
             * timestamps against the rq the vcpu switched *out* on, and
             * sched_info_arrive() against the rq it switched *in* on. Those are
             * per-CPU sched_clock() reads, not NTP-corrected and not mutually
             * synchronised, so once the vcpu migrates the delta can come out
             * either short or long -- long enough to exceed exit_latency and
             * print a runqueue wait longer than the exit that contains it.
             *
             * Clamping runq into [0, offcpu] is not just cosmetic: time spent
             * runnable-but-waiting is by definition part of the time spent
             * off-CPU, so offcpu is the true upper bound. That keeps the
             * reported breakdown self-consistent,
             *
             *     0 <= runq_delay + offcpu_wait == offcpu <= exit_latency
             *
             * and makes offcpu_wait exact rather than something that needed a
             * negative-value guard of its own.
             */
            if (offcpu < 0)
                offcpu = 0;
            if (runq < 0)
                runq = 0;
            else if (runq > offcpu)
                runq = offcpu;

            data->runq_delay = runq;
            data->offcpu_wait = offcpu - runq;
        } else {
            data->runq_delay = 0;
            data->offcpu_wait = 0;
        }
        if (expr_filter(data))
            perf_output(ctx, data, sizeof(*data));
    }
    data->switches = 0;
    return 0;
}

SEC("raw_tp/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev, struct task_struct *next)
{
    struct kvm_vcpu_event *curr, *prev_event, *next_event;
    u64 cpu = bpf_get_smp_processor_id();
    u32 next_pid;
    u64 time, run_delay;

    barrier_var(cpu);
    if (cpu >= MAX_CPUS || !work_cpus[cpu])
        return 0;

    time = 0;
    curr = &percpu_event[cpu];
    if (curr->pid) {
        time = bpf_ktime_get_ns();
        run_delay = BPF_CORE_READ(prev, sched_info.run_delay);
        prev_event = bpf_map_lookup_elem(&kvm_vcpu, &curr->pid);
        if (!prev_event) {
            curr->switches = 0;
            OFFCPU_TS(curr) = time;
            RUNQ_DELAY_SNAP(curr) = run_delay;
            bpf_map_update_elem(&kvm_vcpu, &curr->pid, curr, BPF_ANY);
        } else {
            prev_event->exit_reason = curr->exit_reason;
            EXIT_TIME(prev_event) = EXIT_TIME(curr);
            /*
             * From kvm_exit to kvm_entry, the vcpu may have multiple sched_switches
             * and sched_migrations.
             *
             * RUNQ_DELAY_SNAP(): Save it here, use it in kvm_entry and clean it up.
             *            Depends on CONFIG_SCHED_INFO, CONFIG_SCHEDSTATS
             *
             * curr->switches: Keeps the number of switches since kvm_exit.
             * OFFCPU_ACC(curr): Cumulative value, the delay between vcpu switching
             *     out and switching in, since kvm_exit.
             *
             * OFFCPU_TS(prev_event): The switch-out timestamp, biased by the
             *     off-CPU time already accumulated. In the kvm_vcpu hashmap.
             */
            if (curr->switches == 0) {
                prev_event->switches = 0;
                OFFCPU_TS(prev_event) = time;
                RUNQ_DELAY_SNAP(prev_event) = run_delay;
            } else {
                prev_event->switches = curr->switches;
                OFFCPU_TS(prev_event) = time - OFFCPU_ACC(curr);
            }
        }
        /*
         *  CPU     0                1
         *      vcpu=>idle
         *  (1) idle=>awk        idle=>vcpu(load kvm_vcpu, update percpu_event[1])
         *  (2) awk =>idle       vcpu=>idle(update kvm_vcpu)
         *      idle=>vcpu
         *
         * Assigning pid = 0 can avoid (1) (2) setting the old exit_reason/exit time
         * to the kvm_vcpu hashmap.
         */
        curr->pid = 0;
    }

    next_pid = BPF_CORE_READ(next, pid);
    if (next_pid) {
        next_event = bpf_map_lookup_elem(&kvm_vcpu, &next_pid);
        if (next_event) {
            curr->tgid = next_event->tgid;
            curr->pid = next_event->pid;
            curr->isa = next_event->isa;
            curr->exit_reason = next_event->exit_reason;
            EXIT_TIME(curr) = EXIT_TIME(next_event);
            curr->switches = next_event->switches + 1;
            RUNQ_DELAY_SNAP(curr) = RUNQ_DELAY_SNAP(next_event);
            OFFCPU_ACC(curr) = (time ?: bpf_ktime_get_ns()) - OFFCPU_TS(next_event);
        } else {
            /*
             * A newly generated vCPU has no kvm_exit to pair with, so give it
             * an exit time far in the future: kvm_entry subtracts it from the
             * current time, and the resulting negative value is what marks the
             * event as bogus for anything that looks at it.
             */
            EXIT_TIME(curr) = INT64_MAX;
        }
    }
    return 0;
}

/*
 * Reclaim the kvm_vcpu entry of a dead vcpu thread. Attached in both modes:
 * oncpu inserts from sched_switch, per-process from kvm_exit_track_pid().
 *
 * It must be sched_process_free (release_task()) rather than
 * sched_process_exit (do_exit()): sched_process_exit fires *before* the task's
 * final schedule(), so the sched_switch that follows -- with the dying vcpu as
 * prev and curr->pid still set -- would immediately reinsert what was just
 * deleted. sched_process_free is past that last switch, so nothing can
 * resurrect the entry.
 *
 * Deliberately not gated on work_cpus[] or filter_pid: the map is global, a
 * vcpu may well be reaped on a CPU we do not monitor, and deleting a key that
 * is not there costs less than the lookup needed to decide to skip it.
 */
SEC("raw_tp/sched_process_free")
int BPF_PROG(sched_process_free, struct task_struct *p)
{
    u32 pid = BPF_CORE_READ(p, pid);

    bpf_map_delete_elem(&kvm_vcpu, &pid);
    return 0;
}

static __always_inline int kvm_exit_track_pid(u32 exit_reason, u32 isa)
{
    static struct kvm_vcpu_event zero;
    struct kvm_vcpu_event *data;
    u64 id = bpf_get_current_pid_tgid();
    u32 pid;

    if (filter_pid && (u32)(id >> 32) != filter_pid)
        return 0;

    pid = (u32)id;
    data = bpf_map_lookup_elem(&kvm_vcpu, &pid);
    if (!data) {
        bpf_map_update_elem(&kvm_vcpu, &pid, &zero, BPF_NOEXIST);
        data = bpf_map_lookup_elem(&kvm_vcpu, &pid);
        if (data) {
            data->tgid = (u32)(id >> 32);
            data->pid = (u32)id;
            data->isa = isa;
        } else
            return 0;
    }
    data->exit_reason = exit_reason;
    EXIT_TIME(data) = bpf_ktime_get_ns();
    return 0;
}

#ifdef __TARGET_ARCH_arm64
SEC("raw_tp/kvm_exit")
int BPF_PROG(kvm_exit_pid, int ret, unsigned int esr_ec, unsigned long vcpu_pc)
{
    return kvm_exit_track_pid(EXIT_REASON((u32)ret, esr_ec), KVM_ISA_ARM);
}
#else
/* Same v5.16 split as kvm_exit()/kvm_exit_legacy() above, including the
 * `return 1'. See the comment there. */
SEC("tp/kvm/kvm_exit")
int kvm_exit_pid(struct kvm_exit_trace_ctx *ctx)
{
    kvm_exit_track_pid(ctx->exit_reason, ctx->isa);
    return 1; /* keep delivering the entry to other perf consumers */
}

SEC("raw_tp/kvm_exit")
int BPF_PROG(kvm_exit_pid_legacy, u32 exit_reason, void *vcpu, u32 isa)
{
    return kvm_exit_track_pid(exit_reason, isa);
}
#endif

SEC("raw_tp/kvm_entry")
int BPF_PROG(kvm_entry_pid) // int vcpu_id | unsigned long vcpu_pc
{
    u64 id = bpf_get_current_pid_tgid();
    struct kvm_vcpu_event *data;
    u32 pid;

    if (filter_pid && (u32)(id >> 32) != filter_pid)
        return 0;

    pid = (u32)id;
    data = bpf_map_lookup_elem(&kvm_vcpu, &pid);
    if (data) {
        data->exit_latency = bpf_ktime_get_ns() - EXIT_TIME(data);
        /*
         * This path never attaches to sched_switch, so switches, runq_delay
         * and offcpu_wait are always 0 and the event is truncated before
         * runq_delay on output. A filter must not rely on them here.
         * The map entry only exists once kvm_exit_track_pid() has run, which
         * is why no exit-time validity check is needed.
         */
        if (expr_filter(data))
            perf_output(ctx, data, offsetof(struct kvm_vcpu_event, runq_delay));
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";

