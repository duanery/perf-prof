#!/usr/bin/env python3
"""
Test DWARF unwind callchain support for profilers that support --user-callchain.

DWARF unwind enables user-space stack unwinding using DWARF debug info,
which is more reliable than frame pointer (FP) based unwinding.

Profilers tested: profile, trace, multi-trace, task-state, num-dist,
    kmemleak, kmemprof, syscalls, rundelay, nested-trace, hrtimer,
    page-faults, breakpoint, split-lock, python.
"""

from PerfProf import PerfProf
from conftest import result_check
import pytest
import os
import tempfile


# =============================================================================
# profile
# =============================================================================

def test_profile_dwarf(runtime, memleak_check):
    #perf-prof profile -F 997 -C 0 -m 128 -g --user-callchain=dwarf
    prof = PerfProf(["profile", '-F', '997', '-C', '0', '-m', '128', '-g', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_profile_dwarf_flame_graph(runtime, memleak_check):
    #perf-prof profile -F 997 -C 0 -m 128 -g --user-callchain=dwarf,8192 --flame-graph profile_dwarf
    prof = PerfProf(["profile", '-F', '997', '-C', '0', '-m', '128', '-g', '--user-callchain=dwarf,8192', '--flame-graph', 'profile_dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# trace
# =============================================================================

def test_trace_dwarf(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0 -m 128 -g --user-callchain=dwarf
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '128', '-g', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_dwarf_flame_graph(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup -C 0 -m 128 -g --user-callchain=dwarf,8192 --flame-graph trace_dwarf
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup', '-C', '0', '-m', '128', '-g', '--user-callchain=dwarf,8192', '--flame-graph', 'trace_dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_trace_dwarf_multi_event(runtime, memleak_check):
    #perf-prof trace -e sched:sched_wakeup,sched:sched_switch//stack/ -C 0 -m 128 --user-callchain=dwarf
    prof = PerfProf(['trace', '-e', 'sched:sched_wakeup,sched:sched_switch//stack/', '-C', '0', '-m', '128', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# multi-trace
# =============================================================================

def test_multi_trace_dwarf(runtime, memleak_check):
    #perf-prof multi-trace -e 'irq:softirq_entry/vec==1/stack/' -e 'irq:softirq_exit/vec==1/stack/' -i 1000 --user-callchain=dwarf
    prof = PerfProf(["multi-trace",
                     '-e', 'irq:softirq_entry/vec==1/stack/',
                     '-e', 'irq:softirq_exit/vec==1/stack/',
                     '-i', '1000', '--user-callchain=dwarf', '-m', '512',
                     '--than', '10us'])
    for std, line in prof.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)

def test_multi_trace_dwarf_detail(runtime, memleak_check):
    #perf-prof multi-trace -e 'irq:softirq_entry/vec==1/stack/' -e 'irq:softirq_exit/vec==1/stack/' -i 1000 --user-callchain=dwarf,8192 --than 100us --order --detail=-1ms
    prof = PerfProf(["multi-trace",
                     '-e', 'irq:softirq_entry/vec==1/stack/',
                     '-e', 'irq:softirq_exit/vec==1/stack/',
                     '-i', '1000', '--user-callchain=dwarf,8192', '-m', '512',
                     '--than', '10us', '--order', '--detail=-1ms'])
    for std, line in prof.run(runtime, memleak_check, util_interval=5):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# task-state
# =============================================================================

def test_task_state_dwarf(runtime, memleak_check):
    #perf-prof task-state -i 1000 -m 256 -g --user-callchain=dwarf
    prof = PerfProf(['task-state', '-i', '1000', '-m', '256', '-g', '--than', '1s', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_dwarf_flame_graph(runtime, memleak_check):
    #perf-prof task-state -i 1000 -m 512 -g --user-callchain=dwarf,8192 --flame-graph task_state_dwarf
    prof = PerfProf(['task-state', '-i', '1000', '-m', '512', '-g', '--than', '1s', '--user-callchain=dwarf,8192', '--flame-graph', 'task_state_dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_dwarf_D(runtime, memleak_check):
    #perf-prof task-state -i 1000 -m 256 -SD -g --user-callchain=dwarf
    prof = PerfProf(['task-state', '-i', '1000', '-m', '256', '-D', '-g', '--than', '5ms', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# num-dist
# =============================================================================

@pytest.fixture(autouse=False)
def sysctl_kernel_sched_schedstats():
    old = PerfProf.sysctl('kernel.sched_schedstats', '1')
    yield
    PerfProf.sysctl('kernel.sched_schedstats', old)

def test_num_dist_dwarf(runtime, memleak_check, sysctl_kernel_sched_schedstats):
    #perf-prof num-dist -e sched:sched_stat_runtime//num=runtime/ -i 1000 -g --user-callchain=dwarf -m 128
    prof = PerfProf(['num-dist', '-e', 'sched:sched_stat_runtime//num=runtime/', '-i', '1000', '-g', '--user-callchain=dwarf', '-m', '512', '--than', '1ms'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# kmemleak
# =============================================================================

def test_kmemleak_dwarf(runtime, memleak_check):
    #perf-prof kmemleak --alloc 'kmem:kmalloc//ptr=ptr/size=bytes_req/stack/' --free 'kmem:kfree//ptr=ptr/' -m 256 --order -g --user-callchain=dwarf
    prof = PerfProf(['kmemleak',
                     '--alloc', 'kmem:kmalloc//ptr=ptr/size=bytes_req/,kmem:kmalloc_node//ptr=ptr/size=bytes_req/',
                     '--free', 'kmem:kfree//ptr=ptr/',
                     '-m', '256', '--order', '-g', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# kmemprof
# =============================================================================

def test_kmemprof_dwarf(runtime, memleak_check):
    #perf-prof kmemprof -e 'kmem:kmalloc//ptr=ptr/size=bytes_req/stack/' -e 'kmem:kfree//ptr=ptr/stack/' -m 512 --order -i 1000 -k ptr --user-callchain=dwarf
    prof = PerfProf(['kmemprof',
                     '-e', 'kmem:kmalloc//key=ptr/size=bytes_req/stack/,kmem:kmalloc_node//key=ptr/size=bytes_req/stack/',
                     '-e', 'kmem:kfree//key=ptr/stack/',
                     '-m', '512', '--order', '-i', '1000', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# syscalls
# =============================================================================

def test_syscalls_dwarf(runtime, memleak_check):
    #perf-prof syscalls -e 'raw_syscalls:sys_enter//stack/' -e raw_syscalls:sys_exit -i 1000 -m 128 -p 1 --user-callchain=dwarf
    prof = PerfProf(["syscalls", '-e', 'raw_syscalls:sys_enter//stack/', '-e', 'raw_syscalls:sys_exit',
                     '-i', '1000', '-m', '128', '-p', '1', '--than', '10us', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_syscalls_dwarf_perins(runtime, memleak_check):
    #perf-prof syscalls -e 'raw_syscalls:sys_enter//stack/' -e raw_syscalls:sys_exit -i 1000 -m 128 -p 1 --user-callchain=dwarf,8192 --perins
    prof = PerfProf(["syscalls", '-e', 'raw_syscalls:sys_enter//stack/', '-e', 'raw_syscalls:sys_exit',
                     '-i', '1000', '-m', '128', '-p', '1', '--than', '10us', '--user-callchain=dwarf,8192', '--perins'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# rundelay
# =============================================================================

def test_rundelay_dwarf(runtime, memleak_check):
    #perf-prof rundelay -e 'sched:sched_wakeup*//stack/,sched:sched_switch//stack/' -e 'sched:sched_switch//stack/' -i 1000 -m 256 --perins --user-callchain=dwarf
    prof = PerfProf(["rundelay", '-e', 'sched:sched_wakeup*//stack/,sched:sched_switch//stack/',
                     '-e', 'sched:sched_switch//stack/', '-i', '1000', '-m', '256',
                     '--than', '1ms', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# nested-trace
# =============================================================================

def test_nested_trace_dwarf(runtime, memleak_check):
    #perf-prof nested-trace -e 'irq:softirq_entry/vec==1/stack/,irq:softirq_exit/vec==1/stack/' -i 1000 --impl call-delay --user-callchain=dwarf -m 128
    prof = PerfProf(["nested-trace", '-e', 'irq:softirq_entry/vec==1/stack/,irq:softirq_exit/vec==1/stack/',
                     '-i', '1000', '--impl', 'call-delay', '--than', '10us', '--user-callchain=dwarf', '-m', '128'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# hrtimer
# =============================================================================

def test_hrtimer_dwarf(runtime, memleak_check):
    #perf-prof hrtimer -e sched:sched_switch -C 0 --period 10ms -g --user-callchain=dwarf -m 128 'sched_switch==0'
    prof = PerfProf(["hrtimer", '-e', 'sched:sched_switch', '-C', '0', '--period', '10ms',
                     '-g', '--user-callchain=dwarf', '-m', '128', 'sched_switch==0'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# page-faults
# =============================================================================

def test_page_faults_dwarf(runtime, memleak_check):
    #perf-prof page-faults -C 0 -g --user-callchain=dwarf -N 100
    prof = PerfProf(["page-faults", '-C', '0', '-g', '--user-callchain=dwarf', '-N', '100'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_page_faults_dwarf_record_size(runtime, memleak_check):
    #perf-prof page-faults -C 0 -g --user-callchain=dwarf,8192 -N 100
    prof = PerfProf(["page-faults", '-C', '0', '-g', '--user-callchain=dwarf,8192', '-N', '100'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# breakpoint
# =============================================================================

def test_breakpoint_dwarf(runtime, memleak_check):
    sym = PerfProf.kallsyms_lookup_name("try_to_wake_up")
    #perf-prof breakpoint <addr>:x -g --user-callchain=dwarf -N 2
    prof = PerfProf(["breakpoint", hex(sym) + ":x", '-g', '--user-callchain=dwarf', '-N', '2'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# split-lock
# =============================================================================

def test_split_lock_dwarf(runtime, memleak_check):
    #perf-prof split-lock -i 1000 --test -g --user-callchain=dwarf
    prof = PerfProf(["split-lock", '-i', '1000', '--test', '-g', '--user-callchain=dwarf'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# =============================================================================
# python
# =============================================================================

# Python script that accesses callchain with DWARF unwind
DWARF_CALLCHAIN_SCRIPT = '''
from collections import Counter

stacks = Counter()

def sched__sched_wakeup(event):
    callchain = event.get('_callchain', [])
    if callchain:
        stack = []
        for frame in callchain[:5]:
            stack.append(frame['symbol'])
        if stack:
            stacks[' <- '.join(stack)] += 1

def __interval__():
    print(f"Unique stacks: {len(stacks)}")
    for stack, count in stacks.most_common(3):
        print(f"  {count}: {stack[:60]}...")
'''

# Python script that tests event.print() with DWARF callchain
DWARF_PRINT_SCRIPT = '''
count = 0

def __sample__(event):
    global count
    count += 1
    if count <= 5:
        event.print(callchain=True)

def __interval__():
    print("dwarf print ok")
'''

def _write_script(content):
    fd, path = tempfile.mkstemp(suffix='.py', prefix='perf_prof_dwarf_test_')
    with os.fdopen(fd, 'w') as f:
        f.write(content)
    return path

def test_python_dwarf_callchain(runtime, memleak_check):
    """Test python profiler accessing callchain with DWARF unwind"""
    script_path = _write_script(DWARF_CALLCHAIN_SCRIPT)
    try:
        prof = PerfProf(['python', '-e', 'sched:sched_wakeup', '-C', '0', '-g', '--user-callchain=dwarf',
                         '-i', '1000', '-m', '256', script_path])
        for std, line in prof.run(runtime, memleak_check):
            result_check(std, line, runtime, memleak_check)
    finally:
        os.unlink(script_path)

def test_python_dwarf_print(runtime, memleak_check):
    """Test python profiler event.print() with DWARF callchain"""
    script_path = _write_script(DWARF_PRINT_SCRIPT)
    try:
        prof = PerfProf(['python', '-e', 'sched:sched_wakeup', '-C', '0', '-g', '--user-callchain=dwarf,8192',
                         '-i', '1000', '-m', '256', script_path])
        for std, line in prof.run(runtime, memleak_check):
            result_check(std, line, runtime, memleak_check)
    finally:
        os.unlink(script_path)

def test_python_dwarf_no_kernel_callchain(runtime, memleak_check):
    """Test python profiler with DWARF unwind and --no-kernel-callchain"""
    script_path = _write_script(DWARF_CALLCHAIN_SCRIPT)
    try:
        prof = PerfProf(['python', '-e', 'sched:sched_wakeup', '-C', '0', '-g',
                         '--user-callchain=dwarf', '--no-kernel-callchain',
                         '-i', '1000', '-m', '256', script_path])
        for std, line in prof.run(runtime, memleak_check):
            result_check(std, line, runtime, memleak_check)
    finally:
        os.unlink(script_path)