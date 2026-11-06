#!/usr/bin/env python3
from PerfProf import PerfProf
from conftest import result_check
import os
import re


def test_ptrace_shared_ring_ids(runtime, memleak_check):
    prof = PerfProf([
        'trace', '-e', 'syscalls:sys_enter_prctl,syscalls:sys_exit_prctl',
        '--ptrace', '--order', '-m', '64', '--', './binding-workload', '24',
    ])
    workers = set()
    counts = {}
    for std, line in prof.run(max(runtime, 10), memleak_check):
        result_check(std, line, runtime, memleak_check)
        if line.startswith('WORKER '):
            workers.add(int(line.split()[1]))
        match = re.search(r'\s(\d+)\s+.*\[\d+\].*syscalls:(sys_enter_prctl|sys_exit_prctl):', line)
        if match:
            key = (int(match.group(1)), match.group(2))
            counts[key] = counts.get(key, 0) + 1
    assert len(workers) == 24
    for tid in workers:
        assert counts.get((tid, 'sys_enter_prctl')) == 4
        assert counts.get((tid, 'sys_exit_prctl')) == 4


def test_multi_trace_dynamic_bindings(runtime, memleak_check):
    prof = PerfProf([
        'multi-trace', '-e', 'syscalls:sys_enter_prctl',
        '-e', 'syscalls:sys_exit_prctl', '--ptrace', '--order',
        '-i', '100', '--perins', '-m', '64', '--', './binding-workload', '64',
    ])
    output = []
    for std, line in prof.run(max(runtime, 10), memleak_check):
        result_check(std, line, runtime, memleak_check)
        output.append(line)
    assert sum(line.startswith('WORKER ') for line in output) == 64
    assert any('sys_enter_prctl' in line and 'sys_exit_prctl' in line for line in output)


def test_task_state_forward_filter(runtime, memleak_check):
    prof = PerfProf([
        'trace', '-e', 'task-state/-m 256/,syscalls:sys_enter_prctl',
        '--ptrace', '--order', '-m', '64', '--', './binding-workload', '16',
    ])
    tids = set()
    switches = []
    wakeups = []
    for std, line in prof.run(max(runtime, 10), memleak_check):
        result_check(std, line, runtime, memleak_check)
        if line.startswith(('WORKER ', 'WORKLOAD ')):
            tids.add(int(line.split()[1]))
        if 'sched:sched_switch:' in line:
            match = re.search(r':(\d+)\s+\[.*==>.*:(\d+)\s+\[', line)
            assert match, line
            switches.append(tuple(map(int, match.groups())))
        elif 'sched:sched_wakeup' in line:
            match = re.search(r'sched:sched_wakeup(?:_new)?:.*:(\d+)\s+\[', line)
            assert match, line
            wakeups.append(int(match.group(1)))
    assert len(tids) == 17
    assert switches
    assert all(prev in tids or next_tid in tids for prev, next_tid in switches)
    assert all(tid in tids for tid in wakeups)


def _count_prctl(args, runtime, memleak_check, nr_workers):
    workers = set()
    counts = {}
    for std, line in PerfProf(args).run(max(runtime, 10), memleak_check):
        result_check(std, line, runtime, memleak_check)
        if line.startswith('WORKER '):
            workers.add(int(line.split()[1]))
        match = re.search(r'\s(\d+)\s+.*\[\d+\].*syscalls:sys_enter_prctl:', line)
        if match:
            tid = int(match.group(1))
            counts[tid] = counts.get(tid, 0) + 1
    assert len(workers) == nr_workers
    for tid in workers:
        assert counts.get(tid) == 4


def test_ptrace_cpu_ring_task_writers(runtime, memleak_check):
    # cpus= binds the event to cpus while --ptrace binds it to each thread:
    # every cpu ring has only task-bound writers, one HUP domain per thread.
    # Each exiting thread must hand the rings over and the last one end them.
    _count_prctl(['trace', '-e', 'syscalls:sys_enter_prctl//cpus=0-%d/' % (os.cpu_count() - 1),
                  '--ptrace', '--order', '-m', '64', '--', './binding-workload', '24'],
                 runtime, memleak_check, 24)

