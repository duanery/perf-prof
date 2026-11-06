#!/usr/bin/env python3

from PerfProf import PerfProf, DeadLoop
from conftest import result_check
import pytest
import time

def test_multi_trace_switch(runtime, memleak_check):
    # perf-prof multi-trace -e sched:sched_switch --cycle -i 1000 --perins
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_switch', '--cycle', '-i', '1000', '--perins'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_userspace_ftrace_filter(runtime, memleak_check):
    # perf-prof multi-trace -e sched:sched_switch/prev_prio!=next_prio/ --cycle -i 1000 --perins
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_switch/prev_prio!=next_prio/', '--cycle', '-i', '1000', '--perins'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_switch_role(runtime, memleak_check):
    # perf-prof multi-trace -e sched:sched_switch//role=(next_pid?1:0)|(prev_pid?2:0)/ --cycle -i 1000 --perins
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_switch//role=(next_pid?1:0)|(prev_pid?2:0)/',
                            '--cycle', '-i', '1000', '--perins'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_hrtimer(runtime, memleak_check):
    # perf-prof multi-trace -e timer:hrtimer_expire_entry/function==0x$tick/ -e timer:hrtimer_expire_exit -i 1000
    multi_trace = PerfProf(["multi-trace"])
    tick = PerfProf.kallsyms_lookup_name("tick_sched_timer")
    multi_trace += ["-e", "timer:hrtimer_expire_entry/function==" + hex(tick) + "/",
                    "-e", "timer:hrtimer_expire_exit", "-i", "1000"]
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_workqueue(runtime, memleak_check):
    multi_trace = PerfProf(["multi-trace"])
    tick = PerfProf.kallsyms_lookup_name("vmstat_update")
    multi_trace += ["-e", "workqueue:workqueue_queue_work/function==" + hex(tick) + "/",
                    "-e", "workqueue:workqueue_execute_start/function==" + hex(tick) + "/",
                    '-k', 'work', '--order', "-i", "1000"]
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_kprobe(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")

    # perf-prof multi-trace -e kprobe:try_to_wake_up -e kretprobe:try_to_wake_up -i 1000
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'kprobe:try_to_wake_up',
                            '-e', 'kretprobe:try_to_wake_up',
                            '-i', '1000'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_softirq_timer(runtime, memleak_check):
    # perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/',
                            '-e', 'irq:softirq_exit/vec==1/',
                            '-i', '1000'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)


def test_multi_trace_softirq_pair(runtime, memleak_check):
    # perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --impl pair
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/',
                            '-e', 'irq:softirq_exit/vec==1/',
                            '-i', '1000', '--impl', 'pair'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)


def test_multi_trace_pthread1(runtime, memleak_check):
    # perf-prof multi-trace -e syscalls:sys_enter_nanosleep -e 'syscalls:sys_exit_nanosleep,task-state/-m 64/untraced/' \
    #           --order -i 1000 -N 50  -- ./pthread --loop 10 --depth 3
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'syscalls:sys_enter_nanosleep',
                            '-e', 'syscalls:sys_exit_nanosleep,task-state/-m 64/untraced/',
                            '--order', '-i', '1000', '-N', '50', '--', './pthread', '--loop', '10', '--depth', '3'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)


def test_multi_trace_pthread2(runtime, memleak_check):
    # perf-prof multi-trace -e syscalls:sys_enter_nanosleep -e 'syscalls:sys_exit_nanosleep,task-state/-m 64/untraced/' \
    #           --order -i 1000 --than 200010us --detail  -- ./pthread --loop 100 --depth 3
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'syscalls:sys_enter_nanosleep',
                            '-e', 'syscalls:sys_exit_nanosleep,task-state/-m 64/untraced/',
                            '--order', '-i', '1000', '--than', '200010us', '--detail', '--', './pthread', '--loop', '100', '--depth', '3'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)


def test_multi_trace_softirq_timer_detail(runtime, memleak_check):
    # perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --than 100us --order --detail=-1ms
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/',
                            '-e', 'irq:softirq_exit/vec==1/',
                            '-i', '1000', '--than', '100us', '--order', '--detail=-1ms'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)


def test_multi_trace_softirq_timer_detail_only_than(runtime, memleak_check):
    # perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --only-than 100us --order --detail=-1ms
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/',
                            '-e', 'irq:softirq_exit/vec==1/',
                            '-i', '1000', '--only-than', '100us', '--order', '--detail=-1ms'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_softirq_timer_detail_tsc(runtime, memleak_check):
    if not PerfProf.tsc_clocksource():
        pytest.skip("--tsc needs the tsc clocksource")
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/',
                            '-e', 'irq:softirq_exit/vec==1/',
                            '-i', '1000', '--than', '100us', '--order', '--detail=-1ms', '--tsc'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_rundelay(runtime, memleak_check):
    # perf-prof multi-trace -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
    #                       -e 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/' \
    #                       -k pid -m 256 -i 1000 --order --than 100ms --detail=samekey,-100us
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/',
                            '-e', 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/',
                            '-k', 'pid', '-m', '512', '-i', '1000', '--order', '--than', '100ms', '--detail=samekey,-100us'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_rundelay_profile(runtime, memleak_check):
    # perf-prof multi-trace -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
    #                       -e 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/,profile/-F 200 --watermark 50 -m 16/untraced/' \
    #                       -k pid -m 256 -i 1000 --order --than 100ms --detail=samecpu
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/',
                            '-e', 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/,profile/-F 200 --watermark 50 -m 16/untraced/',
                            '-k', 'pid', '-m', '512', '-i', '1000', '--order', '--than', '100ms', '--detail=samecpu'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_rundelay_page_faults(runtime, memleak_check):
    # perf-prof multi-trace -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
    #                       -e 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/,page-faults/--exclude-user --watermark 50 -m 16/untraced/' \
    #                       -k pid -m 256 -i 1000 --order --than 100ms --detail=samecpu
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/',
                            '-e', 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/,page-faults/--exclude-user --watermark 50 -m 16/untraced/',
                            '-k', 'pid', '-m', '512', '-i', '1000', '--order', '--than', '100ms', '--detail=samecpu,hide<1ms'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

@pytest.fixture
def sleep_loop_tid():
    loop = DeadLoop(lambda :time.sleep(0.001))
    yield loop.tid
    loop.stop()

def test_multi_trace_tid_sleep_1ms(runtime, memleak_check, sleep_loop_tid):
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_switch/prev_pid==' + str(sleep_loop_tid) + '/',
                            '-e', 'sched:sched_wakeup/pid==' + str(sleep_loop_tid) + '/',
                            '-e', 'sched:sched_switch/next_pid==' + str(sleep_loop_tid) + '/',
                            '-i', '1000', '--order'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)


@pytest.fixture
def sleep_loop_tid_100ms():
    loop = DeadLoop(lambda :time.sleep(0.1))
    yield loop.tid
    loop.stop()

def test_multi_trace_tid_sleep_100ms(runtime, memleak_check, sleep_loop_tid_100ms):
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_switch/prev_pid==' + str(sleep_loop_tid_100ms) + '/,sched:sched_stat_runtime/runtime>0/untraced/',
                            '-e', 'sched:sched_wakeup/pid==' + str(sleep_loop_tid_100ms) + '/',
                            '-e', 'sched:sched_switch/next_pid==' + str(sleep_loop_tid_100ms) + '/',
                            '-i', '1000', '-m', '512', '--order', '--than', '105ms', '--detail=-1ms,samecpu'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_printkey_pid(runtime, memleak_check):
    # perf-prof multi-trace -e 'sched:sched_wakeup//key=pid/printkey=printf("PID:%d",key)/' -e 'sched:sched_switch//key=next_pid/' -k pid --order -i 1000 --perins
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'sched:sched_wakeup//key=pid/printkey=printf("PID:%d ",key)/',
                            '-e', 'sched:sched_switch//key=next_pid/',
                            '-k', 'pid', '--order', '-i', '1000', '--perins'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_printkey_hex(runtime, memleak_check):
    # perf-prof multi-trace -e 'irq:softirq_entry/vec==1/key=vec/printkey=printf("VEC:0x%x",key)/' -e 'irq:softirq_exit/vec==1/key=vec/' -i 1000
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/key=vec/printkey=printf("VEC:0x%x ",key)/',
                            '-e', 'irq:softirq_exit/vec==1/key=vec/',
                            '-i', '1000', '--order', '--perins'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_key_cpu(runtime, memleak_check):
    # Test --key with builtin variable _cpu
    # perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -k _cpu -i 1000 --order
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/',
                            '-e', 'irq:softirq_exit/vec==1/',
                            '-k', '_cpu', '-i', '1000', '--order'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_key_complex_expr(runtime, memleak_check):
    # Test --key with complex expression
    # perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -k '(_cpu<<16)|vec' -i 1000 --order
    multi_trace = PerfProf(["multi-trace",
                            '-e', 'irq:softirq_entry/vec==1/',
                            '-e', 'irq:softirq_exit/vec==1/',
                            '-k', '(_cpu<<16)|vec', '-i', '1000', '--order'])
    for std, line in multi_trace.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)
