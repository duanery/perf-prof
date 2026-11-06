#!/usr/bin/env python3

from PerfProf import PerfProf
from conftest import result_check
import ctypes.util
import os
import pytest

def test_sched_wakeup(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_sched_wakeup_watermark(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup --watermark 50
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '--watermark', '50', '-m', '8'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_sched_wakeup_tsc(runtime, memleak_check):
    if not PerfProf.tsc_clocksource():
        pytest.skip("--tsc needs the tsc clocksource")
    #perf-prof trace -e sched:sched_wakeup -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '64', '--tsc', '-N', '20'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_sched_wakeup_clock_offset(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '64', '--clock-offset', '0xff', '-N', '20'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_userspace_ftrace_filter(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup,sched:sched_switch -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup/pid<prio/stack/,sched:sched_switch/next_pid<next_prio&&next_pid>0/', '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_flame_graph(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0 -g
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '64', '-g', '--flame-graph', 'wakeup'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_flame_graph_stdout(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0 -g
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '64', '-g', '--flame-graph', '/dev/stdout'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_sched_wakeup_perfeval_cpus(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '64', '--perfeval-cpus', '0', '-i', '1000'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_sched_wakeup_perfeval_limit(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '64', '--perfeval-cpus', '0', '-i', '1000', '--sampling-limit', '1000'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_overwrite(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup,sched:sched_switch --overwrite
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup,sched:sched_switch', '-m', '1', '--overwrite'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_profiler(runtime, memleak_check):
    #perf-prof trace -e 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/' --order -- cat /proc/self/maps
    prof = PerfProf(['trace', '-e', 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/', '--order', '--', 'cat', '/proc/self/maps'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_attr_cpus0(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup/target_cpu==0/,sched:sched_migrate_task/dest_cpu==0/,sched:sched_switch//cpus=0/ -m 128 --order -i 1000 -N 100000
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup/target_cpu==0/,sched:sched_migrate_task/dest_cpu==0/,sched:sched_switch//cpus=0/', '-m', '128', '--order', '-i', '1000', '-N', '100000'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_attr_cpus1(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup/target_cpu==1/,sched:sched_migrate_task/dest_cpu==1/,sched:sched_switch//cpus=1/ -m 128 --order -i 1000 -N 100000
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup/target_cpu==1/,sched:sched_migrate_task/dest_cpu==1/,sched:sched_switch//cpus=1/', '-m', '128', '--order', '-i', '1000', '-N', '100000'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_task_state1(runtime, memleak_check):
    #perf-prof trace -e 'syscalls:sys_enter_nanosleep,task-state/--perins/' --order -- ./pthread --loop 5 --depth 1
    prof = PerfProf(['trace', '-e', 'syscalls:sys_enter_nanosleep,task-state/--perins -m 64/', '-m', '128', '--order', '--', './pthread', '--loop', '5', '--depth', '1'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_task_state2(runtime, memleak_check):
    #perf-prof trace -e 'syscalls:sys_enter_nanosleep,task-state/--perins/' --order -- ./pthread --daemonize --depth 2
    prof = PerfProf(['trace', '-e', 'syscalls:sys_enter_nanosleep,task-state/--perins -m 64/', '-m', '128', '--order', '--', './pthread', '--daemonize', '--depth', '2'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_task_state3(runtime, memleak_check):
    #perf-prof trace -e 'syscalls:sys_enter_nanosleep,task-state/--perins/' --order -- ./pthread --depth 2
    prof = PerfProf(['trace', '-e', 'syscalls:sys_enter_nanosleep,task-state/--perins -m 64/', '-m', '128', '--order', '--', './pthread', '--depth', '2'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_ptrace(runtime, memleak_check):
    #perf-prof trace -e 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/' --order --ptrace -- cat /proc/self/maps
    prof = PerfProf(['trace', '-e', 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/', '--order', '--ptrace', '--', 'cat', '/proc/self/maps'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_ptrace1(runtime, memleak_check):
    #perf-prof trace -e 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/' --order --ptrace -- ./pthread --loop 5 --depth 1
    prof = PerfProf(['trace', '-e', 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/', '--order', '--ptrace', '--', './pthread', '--loop', '5', '--depth', '1'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_ptrace2(runtime, memleak_check):
    #perf-prof trace -e 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/' --order --ptrace -- ./pthread --daemonize --depth 2
    prof = PerfProf(['trace', '-e', 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/', '--order', '--ptrace', '--', './pthread', '--daemonize', '--depth', '2'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_ptrace3(runtime, memleak_check):
    #perf-prof trace -e 'task-state/--ptrace/,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/' --order -- ./pthread --loop 5 --depth 1
    prof = PerfProf(['trace', '-e', 'task-state/--ptrace/,page-faults/-N 10/,raw_syscalls:sys_enter,profile/-F 5000 -N 10/', '--order', '--', './pthread', '--loop', '5', '--depth', '1'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_kprobe(runtime, memleak_check):
    exist = PerfProf.pmu_exists('kprobe')
    if not exist:
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e kprobe:try_to_wake_up
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up/cpu==0/', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_kprobe1(runtime, memleak_check):
    exist = PerfProf.pmu_exists('kprobe')
    if not exist:
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e kprobe:try_to_wake_up
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up/common_pid+1>10/', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_kprobe_tsc(runtime, memleak_check):
    if not PerfProf.tsc_clocksource():
        pytest.skip("--tsc needs the tsc clocksource")
    exist = PerfProf.pmu_exists('kprobe')
    if not exist:
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e kprobe:try_to_wake_up -C 0
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up', '-C', '0', '-m', '64', '--tsc', '-N', '20'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_kretprobe(runtime, memleak_check):
    exist = PerfProf.pmu_exists('kprobe')
    if not exist:
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e kretprobe:try_to_wake_up -C 0
    prof = PerfProf(['trace', '-e', 'kretprobe:try_to_wake_up', '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_uprobe(runtime, memleak_check):
    exist = PerfProf.pmu_exists('uprobe')
    if not exist:
        pytest.skip("'uprobe' does not exist")

    libc_name = ctypes.util.find_library('c')
    libc_path = "/lib64/" + libc_name
    if not os.path.exists(libc_path):
        pytest.skip(libc_path + " does not exist")

    e = 'uprobe:' + libc_path + ':printf/cpu==0/'
    #perf-prof trace -e 'uprobe:/lib64/libc.so.6:printf/cpu==0/'
    prof = PerfProf(['trace', '-e', e, '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_uretprobe(runtime, memleak_check):
    exist = PerfProf.pmu_exists('uprobe')
    if not exist:
        pytest.skip("'uprobe' does not exist")

    libc_name = ctypes.util.find_library('c')
    libc_path = "/lib64/" + libc_name
    if not os.path.exists(libc_path):
        pytest.skip(libc_path + " does not exist")

    e = 'uretprobe:' + libc_path + ':printf'
    #perf-prof trace -e uretprobe:/lib64/libc.so.6:printf -C 0
    prof = PerfProf(['trace', '-e', e, '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_wildcard_asterisk(runtime, memleak_check):
    #perf-prof trace -e 'sched:sched_wak*' -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wak*', '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_wildcard_question_mark(runtime, memleak_check):
    #perf-prof trace -e 'timer:timer_init' -C 0
    prof = PerfProf(['trace', '-e', 'timer:timer_ini?', '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_wildcard_with_filter(runtime, memleak_check):
    #perf-prof trace -e 'sched:sched_wak*/prio<10/' -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wak?[!_]*/prio<10/', '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_wildcard_multiple_events(runtime, memleak_check):
    #perf-prof trace -e 'sched:sched_wak*,sched:sched_switch' -C 0
    prof = PerfProf(['trace', '-e', 'sched:sched_wak*,sched:sched_switch', '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_batch_attr(runtime, memleak_check):
    #perf-prof trace -e 'sched:sched_switch//batch=1/' -C 0 -m 64 -N 10
    prof = PerfProf(['trace', '-e', 'sched:sched_switch//batch=1/', '-C', '0', '-m', '64', '-N', '10'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_batch_attr_mixed(runtime, memleak_check):
    #perf-prof trace -e 'sched:sched_wakeup,sched:sched_switch//batch=1/' -C 0 -m 64 --order -N 10
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup,sched:sched_switch//batch=1/', '-C', '0', '-m', '64', '--order', '-N', '10'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)
