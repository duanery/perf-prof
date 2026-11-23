#ifndef __TP_STRUCT_H
#define __TP_STRUCT_H

#include <linux/list.h>
#include <linux/const.h>
#include <linux/compiler.h>


#define ALIGN(x, a)		__ALIGN_KERNEL((x), (a))

#define TP_RAW_SIZE(type) \
    (ALIGN(sizeof(type)+sizeof(u32), sizeof(u64)) - sizeof(u32))

#define COMMON_HEADER \
    unsigned short common_type;/*       offset:0;       size:2; signed:0; */ \
    unsigned char common_flags;/*       offset:2;       size:1; signed:0; */ \
    unsigned char common_preempt_count;/*       offset:3;       size:1; signed:0; */ \
    int common_pid;/*   offset:4;       size:4; signed:1; */

/*
 * Kernel constraint: the structs here are fixed copies of the trace entries,
 * the formats in tracing/events/<sys>/<name>/format, read by the profilers
 * without tep. The ones that changed are told apart at runtime, by a field
 * (tep__event_has_field()) or the raw size: see sched_wakeup_no_success.
 * The others have kept their layout from v3.10 on; a change is noticed as
 * wrong data only, check the format first.
 */
struct trace_entry {
    COMMON_HEADER
};

/* taken from kernel/trace/trace.h, already in v3.10 */
struct kprobe_trace_entry_head {
    struct trace_entry  ent;
    unsigned long       ip;
};
struct kretprobe_trace_entry_head {
    struct trace_entry  ent;
    unsigned long       func;
    unsigned long       ret_ip;
};
/* taken from kernel/trace/trace_uprobe.c, already in v3.10 */
struct uprobe_trace_entry_head {
    struct trace_entry  ent;
    unsigned long       vaddr[];
};

struct sched_wakeup {
    COMMON_HEADER

    char comm[16];//    offset:8;       size:16;        signed:1;
    pid_t pid;//        offset:24;      size:4; signed:1;
    int prio;// offset:28;      size:4; signed:1;
    int success;//      offset:32;      size:4; signed:1;
    int target_cpu;//   offset:36;      size:4; signed:1;
};

/*
 * Kernel constraint: sched:sched_wakeup and sched_wakeup_new have no success
 * field since 58b9987de86c ("sched/tracing: Remove the redundant 'success' in
 * the sched tracepoint"), in v5.14: target_cpu moves from offset 36 to 32.
 * The layout is told by the field (tep__event_has_field()) or the raw size,
 * not the kernel version.
 */
struct sched_wakeup_no_success {
    COMMON_HEADER

    char comm[16];//    offset:8;       size:16;        signed:1;
    pid_t pid;//        offset:24;      size:4; signed:1;
    int prio;// offset:28;      size:4; signed:1;
    int target_cpu;//      offset:32;      size:4; signed:1;
};

struct sched_switch {
    COMMON_HEADER

    char prev_comm[16];//       offset:8;       size:16;        signed:1;
    pid_t prev_pid;//   offset:24;      size:4; signed:1;
    int prev_prio;//    offset:28;      size:4; signed:1;
    long prev_state;//  offset:32;      size:8; signed:1;
    char next_comm[16];//       offset:40;      size:16;        signed:1;
    pid_t next_pid;//   offset:56;      size:4; signed:1;
    int next_prio;//    offset:60;      size:4; signed:1;
};

struct sched_migrate_task {
    COMMON_HEADER

    char comm[16];//    offset:8;       size:16;        signed:1;
    pid_t pid;//        offset:24;      size:4; signed:1;
    int prio;// offset:28;      size:4; signed:1;
    int orig_cpu;//     offset:32;      size:4; signed:1;
    int dest_cpu;//     offset:36;      size:4; signed:1;
};

struct sched_stat_runtime {
    COMMON_HEADER

    char comm[16];  //   offset:8;       size:16;        signed:1;
    pid_t pid;      //   offset:24;      size:4; signed:1;
    u64 runtime;    //   offset:32;      size:8; signed:0;
    u64 vruntime;   //   offset:40;      size:8; signed:0;
};

struct sched_process_free {
    COMMON_HEADER

    char comm[16];//    offset:8;       size:16;        signed:1;
    pid_t pid;//        offset:24;      size:4; signed:1;
    int prio;// offset:28;      size:4; signed:1;
};

struct sched_process_fork {
    COMMON_HEADER

    char parent_comm[16];//     offset:8;       size:16;        signed:1;
    pid_t parent_pid;// offset:24;      size:4; signed:1;
    char child_comm[16];//      offset:28;      size:16;        signed:1;
    pid_t child_pid;//  offset:44;      size:4; signed:1;
};

struct sched_process_exec {
    COMMON_HEADER

    short filename_offset, filename_len;//__data_loc char[] filename;       offset:8;       size:4; signed:1;
    pid_t pid;//        offset:12;      size:4; signed:1;
    pid_t old_pid;//    offset:16;      size:4; signed:1;
};

struct task_newtask {
    COMMON_HEADER

    pid_t pid;//        offset:8;       size:4; signed:1;
    char comm[16];//    offset:12;      size:16;        signed:1;
    unsigned long clone_flags;//        offset:32;      size:8; signed:0;
    short oom_score_adj;//      offset:40;      size:2; signed:1;
};

struct task_rename {
    COMMON_HEADER

    pid_t pid;//        offset:8;       size:4; signed:1;
    char oldcomm[16];// offset:12;      size:16;        signed:1;
    char newcomm[16];// offset:28;      size:16;        signed:1;
    short oom_score_adj;//      offset:44;      size:2; signed:1;
};

typedef u64 sector_t;
typedef u32 __kernel_dev_t;

struct block_getrq {
    COMMON_HEADER

    __kernel_dev_t dev;//        offset:8;       size:4; signed:0;
    sector_t sector;//  offset:16;      size:8; signed:0;
    unsigned int nr_sector;//   offset:24;      size:4; signed:0;
    char rwbs[8];//     offset:28;      size:8; signed:1;
    char comm[16];//    offset:36;      size:16;        signed:1;
};

struct block_rq_insert {
    COMMON_HEADER

    __kernel_dev_t dev;//        offset:8;       size:4; signed:0;
    sector_t sector;//  offset:16;      size:8; signed:0;
    unsigned int nr_sector;//   offset:24;      size:4; signed:0;
    unsigned int bytes;//       offset:28;      size:4; signed:0;
    char rwbs[8];//     offset:32;      size:8; signed:1;
    char comm[16];//    offset:40;      size:16;        signed:1;
    short cmd_offset, cmd_len;//__data_loc char[] cmd;    offset:56;      size:4; signed:1;
};

struct block_rq_issue {
    COMMON_HEADER

    __kernel_dev_t dev;//        offset:8;       size:4; signed:0;
    sector_t sector;//  offset:16;      size:8; signed:0;
    unsigned int nr_sector;//   offset:24;      size:4; signed:0;
    unsigned int bytes;//       offset:28;      size:4; signed:0;
    char rwbs[8];//     offset:32;      size:8; signed:1;
    char comm[16];//    offset:40;      size:16;        signed:1;
    short cmd_offset, cmd_len;//__data_loc char[] cmd;    offset:56;      size:4; signed:1;
};

struct block_rq_complete {
    COMMON_HEADER

    __kernel_dev_t dev;//        offset:8;       size:4; signed:0;
    sector_t sector;//  offset:16;      size:8; signed:0;
    unsigned int nr_sector;//   offset:24;      size:4; signed:0;
    int error;//        offset:28;      size:4; signed:1;
    char rwbs[8];//     offset:32;      size:8; signed:1;
    short cmd_offset, cmd_len;//__data_loc char[] cmd;    offset:40;      size:4; signed:1;
};


#if defined(__i386__) || defined(__x86_64__)
struct kvm_pvclock_update {
    COMMON_HEADER

    unsigned int vcpu_id;//     offset:8;       size:4; signed:0;
    __u32 version;//    offset:12;      size:4; signed:0;
    __u64 tsc_timestamp;//      offset:16;      size:8; signed:0;
    __u64 system_time;//        offset:24;      size:8; signed:0;
    __u32 tsc_to_system_mul;//  offset:32;      size:4; signed:0;
    __s8 tsc_shift;//   offset:36;      size:1; signed:1;
    __u8 flags;//       offset:37;      size:1; signed:0;
};

struct kvm_write_tsc_offset {
    COMMON_HEADER

    unsigned int vcpu_id;//     offset:8;       size:4; signed:0;
    __u64 previous_tsc_offset;//        offset:16;      size:8; signed:0;
    __u64 next_tsc_offset;//    offset:24;      size:8; signed:0;
};

#endif

#endif
