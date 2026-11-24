#!/usr/bin/env python3

import platform
import time
import pytest

from PerfProf import PerfProf, DeadLoop
from conftest import result_check

# BPF event sources: bpf:xxx built from src/bpf-skel/xxx.bpf.c, used by -e like
# a tracepoint. The struct comes from BTF, so all of them need it.
pytestmark = pytest.mark.skipif(not PerfProf.btf_exists(), reason="no /sys/kernel/btf/vmlinux")

def kernel_release():
    major, minor = platform.release().split('.')[:2]
    return int(major), int(minor)

def run_ok(args, runtime, memleak_check):
    prof = PerfProf(args)
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def expect_error(args, expected, stderr=True):
    prof = PerfProf(args)
    want = PerfProf.STDERR if stderr else PerfProf.STDOUT
    found = False
    for std, line in prof.run(3, False):
        if std == want and expected in line:
            found = True
    assert found, f"expected {expected!r} from {args!r}"


#
# The registry: list, help, completion and wildcards see the sources.
#
def test_list():
    found = set()
    for std, line in PerfProf(['list']).run(3, False):
        if std == PerfProf.STDOUT:
            found.add(line.strip())
    assert 'bpf:kvm_exit' in found
    assert 'bpf:sched_rundelay' in found

def test_help():
    # help prints the description and the struct read from BTF.
    found = []
    for std, line in PerfProf(['trace', '-e', 'bpf:sched_rundelay', 'help']).run(3, False):
        if std == PerfProf.STDOUT:
            found.append(line)
    out = ''.join(found)
    assert 'desc: Scheduling delay' in out
    assert 'struct sched_rundelay_event' in out
    assert 'field:uint64_t rundelay;' in out

def test_wildcard(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:sched*', '-C', '0', '-m', '64'], runtime, memleak_check)

def test_not_found():
    expect_error(['trace', '-e', 'bpf:nosuch', '-N', '1'], 'bpf:nosuch not found')


#
# bpf:sched_rundelay
#
def test_sched_rundelay(runtime, memleak_check):
    #perf-prof trace -e bpf:sched_rundelay -C 0
    run_ok(['trace', '-e', 'bpf:sched_rundelay', '-C', '0', '-m', '64'], runtime, memleak_check)

def test_sched_rundelay_all_cpus(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:sched_rundelay', '-m', '64'], runtime, memleak_check)

def test_sched_rundelay_stack(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:sched_rundelay//stack/', '-C', '0', '-m', '64'], runtime, memleak_check)

def test_sched_rundelay_pid(runtime, memleak_check):
    # Process scope: the tgid goes to the BPF program through ops->prepare.
    loop = DeadLoop(lambda: time.sleep(0.001))
    try:
        run_ok(['trace', '-e', 'bpf:sched_rundelay', '-p', str(loop.tid), '-m', '64'], runtime, memleak_check)
    finally:
        loop.stop()

def test_sched_rundelay_tid(runtime, memleak_check):
    loop = DeadLoop(lambda: time.sleep(0.001))
    try:
        run_ok(['trace', '-e', 'bpf:sched_rundelay', '-t', str(loop.tid), '-m', '64'], runtime, memleak_check)
    finally:
        loop.stop()

def test_sched_rundelay_order(runtime, memleak_check):
    # Mixed with a tracepoint, ordered on one timeline.
    run_ok(['trace', '-e', 'bpf:sched_rundelay,sched:sched_switch', '-C', '0', '-m', '64', '--order'],
           runtime, memleak_check)

def test_sched_rundelay_order_pid(runtime, memleak_check):
    loop = DeadLoop(lambda: time.sleep(0.001))
    try:
        run_ok(['trace', '-e', 'bpf:sched_rundelay,sched:sched_switch', '-p', str(loop.tid),
                '-m', '64', '--order'], runtime, memleak_check)
    finally:
        loop.stop()

def test_sched_rundelay_top(runtime, memleak_check):
    run_ok(['top', '-e', 'bpf:sched_rundelay//key=pid/comm=comm/top-by=rundelay/', '-i', '1000', '-m', '64'],
           runtime, memleak_check)

def test_sched_rundelay_num_dist(runtime, memleak_check):
    run_ok(['num-dist', '-e', 'bpf:sched_rundelay//num=rundelay/', '-i', '1000', '-m', '64'],
           runtime, memleak_check)

def test_sched_rundelay_multi_trace(runtime, memleak_check):
    run_ok(['multi-trace', '-e', 'sched:sched_wakeup', '-e', 'bpf:sched_rundelay//key=pid/', '-k', 'pid', '--order',
            '-i', '1000', '-m', '64'], runtime, memleak_check)


#
# /EXPR/: compiled to BPF and run in the kernel, or in userspace when the BPF
# backend cannot express it.
#
def test_filter(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:sched_rundelay/rundelay>10000/', '-m', '64'], runtime, memleak_check)

def test_filter_fields(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:sched_rundelay/reason==0&&runnable_cpu!=_cpu||prio<100/', '-m', '64'],
           runtime, memleak_check)

def test_filter_comm(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:sched_rundelay/comm[0]==\'p\'/', '-m', '64'], runtime, memleak_check)

def test_filter_userspace():
    # Signed division needs a 6.6+ kernel; below that the event is restarted
    # without the BPF filter and the same expression runs in userspace.
    if kernel_release() >= (6, 6):
        pytest.skip("signed division compiles to BPF on 6.6+")
    expect_error(['trace', '-e', 'bpf:sched_rundelay/(prio/2)>0/', '-N', '1'],
                 "filters '(prio/2)>0' in userspace", stderr=False)

def test_filter_undefined():
    expect_error(['trace', '-e', 'bpf:sched_rundelay/nosuch>1/', '-N', '1'], 'undefined variable', stderr=False)

def test_filter_sample_glo():
    # _cpu and _pid only exist in userspace; the kernel filter must not see them.
    expect_error(['trace', '-e', 'bpf:sched_rundelay/_cpu==0/', '-N', '1'], 'undefined variable', stderr=False)


#
# bpf:kvm_exit as an event source: starts without VMs, no events.
#
def test_kvm_exit(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:kvm_exit', '-m', '64'], runtime, memleak_check)

def test_kvm_exit_filter(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:kvm_exit/exit_reason==12/', '-C', '0', '-m', '64'], runtime, memleak_check)

def test_kvm_exit_pid(runtime, memleak_check):
    run_ok(['trace', '-e', 'bpf:kvm_exit', '-p', '1', '-m', '64'], runtime, memleak_check)


#
# What BPF events do not support is rejected at startup.
#
def test_reject_push():
    expect_error(['trace', '-e', 'bpf:sched_rundelay//push=1/', '-N', '1'], 'push= is not supported')

def test_reject_cgroups():
    expect_error(['trace', '-e', 'bpf:sched_rundelay', '--cgroups', '.', '-N', '1'], '--cgroups is not supported')

def test_reject_counting():
    expect_error(['stat', '-e', 'bpf:sched_rundelay', '-N', '1'], 'can only be sampled')

def test_reject_processes():
    # Both sources take one process at most.
    expect_error(['trace', '-e', 'bpf:sched_rundelay', '-p', '1,2', '-N', '1'], 'cannot trace these processes')
    expect_error(['trace', '-e', 'bpf:kvm_exit', '-p', '1,2', '-N', '1'], 'cannot trace these processes')

def test_reject_sql():
    expect_error(['sql', '-e', 'bpf:sched_rundelay', '-i', '1000', '--query', 'select 1'],
                 'BPF events are not supported')

def test_reject_python():
    if 'python' not in PerfProf.profilers():
        pytest.skip("perf-prof is built without python")
    expect_error(['python', '-e', 'bpf:sched_rundelay', 'x.py'], 'BPF events are not supported')
