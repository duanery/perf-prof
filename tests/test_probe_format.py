#!/usr/bin/env python3

from PerfProf import PerfProf
from conftest import result_check
import ctypes.util
import os
import pytest


def _libc_path():
    libc_name = ctypes.util.find_library('c')
    if not libc_name:
        return None
    for d in ('/lib64/', '/usr/lib64/', '/lib/x86_64-linux-gnu/'):
        p = d + libc_name
        if os.path.exists(p):
            return p
    return None


# --- Positive: kprobe / kretprobe --------------------------------------------

def test_kprobe_new_bare(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e kprobe:try_to_wake_up
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_kprobe_new_with_offset(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e kprobe:try_to_wake_up+0
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up+0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_kretprobe_new(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e kretprobe:try_to_wake_up
    prof = PerfProf(['trace', '-e', 'kretprobe:try_to_wake_up', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_kprobe_still_works_after_refactor(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")

    #perf-prof trace -e 'kprobe:try_to_wake_up/common_pid+1>10/'
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up/common_pid+1>10/', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# --- Positive: uprobe / uretprobe --------------------------------------------

def test_uprobe_new_absolute_path(runtime, memleak_check):
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")

    e = 'uprobe:' + libc + ':printf'
    #perf-prof trace -e uprobe:/lib64/libc.so.6:printf -C 0
    prof = PerfProf(['trace', '-e', e, '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_uprobe_new_with_filter(runtime, memleak_check):
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")

    e = 'uprobe:' + libc + ':printf/cpu==0/'
    #perf-prof trace -e 'uprobe:/lib64/libc.so.6:printf/cpu==0/'
    prof = PerfProf(['trace', '-e', e, '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_uretprobe_new(runtime, memleak_check):
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")

    e = 'uretprobe:' + libc + ':printf'
    #perf-prof trace -e uretprobe:/lib64/libc.so.6:printf
    prof = PerfProf(['trace', '-e', e, '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_uretprobe_new_bare_offset(runtime, memleak_check):
    # uretprobe with a bare file offset — used to disambiguate duplicate
    # function names inside the binary. Discover printf's file offset with
    # nm(1) so we exercise a real, valid offset.
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")

    import subprocess
    try:
        out = subprocess.Popen(['nm', '-D', libc],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True).communicate()[0]
    except FileNotFoundError:
        pytest.skip("nm(1) not available")
    off_hex = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == 'printf' and parts[-2] in ('T', 'W', 'i'):
            off_hex = '0x' + (parts[0].lstrip('0') or '0')
            break
    if off_hex is None:
        pytest.skip("printf offset not found in libc")

    e = 'uretprobe:' + libc + ':' + off_hex
    #perf-prof trace -e uretprobe:/lib64/libc.so.6:0x<offset>
    prof = PerfProf(['trace', '-e', e, '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# --- Negative: parser must reject --------------------------------------------
#
# Negative tests use PerfProf with a short runtime and scan stderr for a
# parser rejection message. If the parser accepts the malformed event, the
# tool starts running and stderr stays quiet — we detect that as a failure.

def _expect_parse_reject(prof, keywords):
    """Iterate prof output; assert stderr contains any of `keywords`."""
    seen_reject = False
    for std, line in prof.run(3, 0):
        if std == PerfProf.STDERR:
            low = line.lower()
            if any(k in low for k in keywords):
                seen_reject = True
    assert seen_reject, \
        "expected parser to reject the event, but no matching stderr message"

def test_uretprobe_rejects_func_plus_offset():
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")

    e = 'uretprobe:' + libc + ':printf+0'
    prof = PerfProf(['trace', '-e', e, '-m', '64', '-N', '1'])
    _expect_parse_reject(prof, ['uretprobe', 'offset', 'not found', 'malformed'])

def test_old_uprobe_at_syntax_rejected():
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")

    e = 'uprobe:printf@"' + libc + '"'
    prof = PerfProf(['trace', '-e', e, '-m', '64', '-N', '1'])
    _expect_parse_reject(prof, ['uprobe', 'not found', 'malformed', 'not accessible'])


# --- "bpf:" prefix regression --------------------------------------------------

def test_bpf_kvm_exit_is_event_source():
    # bpf:kvm_exit is both the profiler "perf-prof bpf:kvm_exit" and a BPF event
    # source for -e. For -e the parser must classify it as a BPF event, not
    # TRACEPOINT / kprobe / uprobe, so the /EXPR/ part is a filter expression
    # on struct kvm_vcpu_event and not the profiler's option string.
    prof = PerfProf(['trace', '-e', 'bpf:kvm_exit/exit_reason==12/', '-N', '1'])
    for std, line in prof.run(3, 0):
        if std == PerfProf.STDERR:
            low = line.lower()
            assert 'bpf:kvm_exit not found' not in low, \
                "bpf:kvm_exit misclassified as tracepoint: " + line
            assert 'profiler bpf not found' not in low, \
                "bpf:kvm_exit misclassified via 'bpf' prefix: " + line
