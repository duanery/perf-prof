#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monitor.h>
#include <tp_struct.h>

static int sched_wakeup_new;
static int sched_wakeup_id;
static int sched_switch_id;
static struct running_oncpu {
    int pid;
    char comm[16];
} *percpu_running;

union sched_event {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    struct sched_wakeup sched_wakeup;
    struct sched_wakeup_no_success sched_wakeup_new;
    struct sched_switch sched_switch;
};

int sched_init(int nr_list, struct tp_list **tp_list)
{
    int i, j;
    int wakeup_id, switch_id, cpus;
    int sched_switch_without_filter = 0;
    int level = 0;
    struct tp *tp;

    wakeup_id = tep__event_id("sched", "sched_wakeup");
    for (i = 0; i < nr_list; i++) {
        for_each_real_tp(tp_list[i], tp, j) {
            if (tp->id == wakeup_id)
                goto to_check_switch;
        }
    }
    return 0;

to_check_switch:
    switch_id = tep__event_id("sched", "sched_switch");
    for (i = 0; i < nr_list; i++) {
        for_each_real_tp(tp_list[i], tp, j) {
            // The sched_switch event cannot have any filters present.
            // Otherwise, it will affect the accuracy of sched_wakeup_unnecessary().
            if (tp->id == switch_id && !(tp->filter && tp->filter[0]))
                sched_switch_without_filter = 1;
        }
    }

    level = 1;
    // Kernel constraint: no success field since 58b9987de86c, see tp_struct.h.
    sched_wakeup_new = !tep__event_has_field(wakeup_id, "success");
    sched_wakeup_id = wakeup_id;

    if (sched_switch_without_filter) {
        if (!percpu_running) {
            sched_switch_id = switch_id;
            cpus = get_present_cpus();
            percpu_running = calloc(cpus, sizeof(struct running_oncpu));
        }
        if (percpu_running)
            level = 2;
    }

    if (level) {
        printf("Trick: Enable userland unnecessary detection of sched:sched_wakeup events.\n");
    }
    return level;
}

static void sched_switch(struct sched_switch *sched_switch, int cpu)
{
    percpu_running[cpu].pid = sched_switch->next_pid;
    memcpy(percpu_running[cpu].comm, sched_switch->next_comm, 16);
}

void sched_event(int level, void *raw, int size, int cpu)
{
    union sched_event *sched = raw;

    if (level == 2 && sched->common_type == sched_switch_id) {
        sched_switch(&sched->sched_switch, cpu);
    }
}

/*

perf-prof multi-trace -e 'sched:sched_wakeup//stack/,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
                      -e 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/' -k pid -m 256 -i 1000 --order  \
                      -o rundelay.log --than 90ms --detail=samekey,-1ms,+1ms

----------------------------------------------------------------------------------------------------------------------------------------------------------

CASE 1:
 2023-06-08 14:53:11.508825             <idle>      0 d... [024] 11607.012798: sched:sched_switch: swapper/24:0 [120] R ==> health_check:89329 [120]
 UNNECESSARY                      health_check  89347 d... [072] 11607.012807: sched:sched_wakeup: health_check:89329 [120] success=1 CPU:024
     ffffffff810c9175 ttwu_do_wakeup+0xb5 ([kernel.kallsyms])
     ffffffff810cc296 try_to_wake_up+0x326 ([kernel.kallsyms])
     ffffffff810cc3e2 default_wake_function+0x12 ([kernel.kallsyms])
     ffffffff810914da child_wait_callback+0x5a ([kernel.kallsyms])
     ffffffff810c039b __wake_up_common+0x5b ([kernel.kallsyms])
     ffffffff810c4fe4 __wake_up_sync_key+0x44 ([kernel.kallsyms])
     ffffffff81093726 __wake_up_parent+0x26 ([kernel.kallsyms])
     ffffffff810a3b64 do_notify_parent+0x1a4 ([kernel.kallsyms])
     ffffffff810932a2 do_exit+0x6e2 ([kernel.kallsyms])
     ffffffff8109367f do_group_exit+0x3f ([kernel.kallsyms])
     ffffffff810936f4 sys_exit_group+0x14 ([kernel.kallsyms])
     ffffffff816cf3be tracesys+0xe3 ([kernel.kallsyms])
 2023-06-08 14:53:11.509475        AdamPlugins  89329 d... [024] 11607.012923: sched:sched_switch: health_check:89329 [120] S ==> swapper/48:0 [120]


 11607.012798: sched:sched_switch:  89329 is running.

 11607.012806:                      89329 -> waitpid -> do_wait -> set_current_state(TASK_INTERRUPTIBLE), but not scheduled.

 11607.012807: sched:sched_wakeup:  wakeup 89329
                                    try_to_wake_up {
                                        if (!(p->state & state)) // p->state = TASK_INTERRUPTIBLE @ 11607.012806
                                            goto unlock;
                                        if (p->on_rq && ttwu_remote(p, wake_flags)) // 89329 has not been scheduled yet, so p->on_rq = 1, do ttwu_remote.
                                            goto unlock;
                                    }
                                    ttwu_remote {
                                        rq_lock;
                                        p->state = TASK_RUNNING;
                                        trace_sched_wakeup(p);
                                        rq_unlock;
                                    }

 11607.012808:                      89329 __schedule {
                                        rq_lock;
                                        if (!preempt && prev->state) {  // p->state = TASK_RUNNING @ 11607.012807 ttwu_remote
                                            if (signal_pending_state(prev->state, prev)) {
                                                prev->state = TASK_RUNNING;
                                            }
                                        }
                                        // 89329 was put_prev_task into the runqueue and pick_next_task again.
                                        if (likely(prev != next)) {
                                        } else {
                                            rq_unlock;
                                            // 89329 not switch out.
                                        }
                                    }
                                    do_wait // continue to execute.

 11607.012923: sched:sched_switch:  89329 scheduled.

 At time 11607.012807, an isolated sched_wakeup event was generated. For the multi-trace profiler, the sched_wakeup event will be backed up to &ctx.backup.
 The event will always stay in &ctx.backup, blocking the free of events on the timeline, see timeline_free_unneeded().

----------------------------------------------------------------------------------------------------------------------------------------------------------

CASE 2:
 2023-06-08 14:53:07.134227             <idle>      0 d... [072] 11603.394386: sched:sched_switch: swapper/72:0 [120] R ==> sshd:164337 [120]
 UNNECESSARY                              sshd 164337 d... [072] 11603.394431: sched:sched_wakeup: sshd:164337 [120] success=1 CPU:072
     ffffffff810c9175 ttwu_do_wakeup+0xb5 ([kernel.kallsyms])
     ffffffff810cc296 try_to_wake_up+0x326 ([kernel.kallsyms])
     ffffffff810cc3e2 default_wake_function+0x12 ([kernel.kallsyms])
     ffffffff810c039b __wake_up_common+0x5b ([kernel.kallsyms])
     ffffffff810c4f89 __wake_up+0x39 ([kernel.kallsyms])
     ffffffff81407295 tty_wakeup+0x35 ([kernel.kallsyms])
     ffffffff81413635 pty_write+0x65 ([kernel.kallsyms])
     ffffffff8140c8b7 n_tty_write+0x2d7 ([kernel.kallsyms])
     ffffffff81409eab tty_write+0x14b ([kernel.kallsyms])
     ffffffff81213920 vfs_write+0xc0 ([kernel.kallsyms])
     ffffffff8121473f sys_write+0x7f ([kernel.kallsyms])
     ffffffff816cf3be tracesys+0xe3 ([kernel.kallsyms])
 2023-06-08 14:53:07.134491               sshd 164337 d... [072] 11603.394436: sched:sched_switch: sshd:164337 [120] S ==> kworker/72:1:52616 [120]

 164337 Waking itself again while running. Similar to CASE 1, an isolated sched:sched_wakeup event will still be generated.


 For the multi-trace profiler, when an isolated sched:sched_wakeup event is detected, it is no longer backed up to &ctx.backup.

 */
bool sched_wakeup_unnecessary(int level, void *raw, int size)
{
    union sched_event *sched = raw;

    if (!level)
        return false;

    if (sched->common_type == sched_wakeup_id) {
        int target_cpu = sched_wakeup_new ? sched->sched_wakeup_new.target_cpu : sched->sched_wakeup.target_cpu;
        if (sched->sched_wakeup.common_pid == sched->sched_wakeup.pid ||
            (level == 2 &&
             percpu_running[target_cpu].pid == sched->sched_wakeup.pid))
            return true;
    }
    return false;
}

