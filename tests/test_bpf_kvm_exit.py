#!/usr/bin/env python3

import platform

from PerfProf import PerfProf
from conftest import result_check

def kernel_release():
    # (major, minor) of the running kernel, for gating on instructions that
    # only exist from a given version.
    major, minor = platform.release().split('.')[:2]
    return int(major), int(minor)

def test_bpf(runtime, memleak_check):
    #perf-prof bpf:kvm_exit --order -i 5000 --perins --detail
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins", "--detail", "--filter", "exit_latency > 2000000"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_bpf_order(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins", "--detail", "--order"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)\

def test_bpf_1(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins", "--detail", "-q"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_bpf_2(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins", "--output2", "/dev/null"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_bpf_3(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins", "-p", "1"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_bpf_4(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins", "--detail", "--than", "10ms"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


#
# --filter: a C expression compiled to BPF and evaluated in the kernel.
#
def bpf_filter(expr, runtime, memleak_check, args=None):
    cmdline = ["bpf:kvm_exit", "-i", "1000", "--filter", expr]
    if args:
        cmdline.extend(args)
    prof = PerfProf(cmdline)
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_filter_eq(runtime, memleak_check):
    bpf_filter('exit_reason == 12', runtime, memleak_check)
def test_filter_ne(runtime, memleak_check):
    bpf_filter('exit_reason != 12', runtime, memleak_check)
def test_filter_unsigned_cmp(runtime, memleak_check):
    bpf_filter('exit_reason > 1 && exit_reason < 100', runtime, memleak_check)
def test_filter_signed_cmp(runtime, memleak_check):
    bpf_filter('exit_latency > 1000000', runtime, memleak_check)
def test_filter_narrow_field(runtime, memleak_check):
    bpf_filter('isa == 1 || switches > 0', runtime, memleak_check)
def test_filter_logical_and(runtime, memleak_check):
    bpf_filter('exit_reason == 12 && exit_latency > 1000000', runtime, memleak_check)
def test_filter_logical_or(runtime, memleak_check):
    bpf_filter('exit_reason == 12 || switches > 2', runtime, memleak_check)
def test_filter_ternary(runtime, memleak_check):
    bpf_filter('exit_reason == 12 ? exit_latency > 100 : 0', runtime, memleak_check)
def test_filter_bitops(runtime, memleak_check):
    bpf_filter('(1 << isa) & 6', runtime, memleak_check)
def test_filter_arith(runtime, memleak_check):
    bpf_filter('exit_latency - runq_delay > 1000', runtime, memleak_check)
def test_filter_unsigned_div(runtime, memleak_check):
    bpf_filter('(unsigned long)exit_latency / 1000 > 5', runtime, memleak_check)
def test_filter_field_vs_field(runtime, memleak_check):
    bpf_filter('offcpu_wait > runq_delay', runtime, memleak_check)
def test_filter_shift(runtime, memleak_check):
    bpf_filter('exit_latency >> 10 > 0', runtime, memleak_check)
def test_filter_not(runtime, memleak_check):
    bpf_filter('!switches', runtime, memleak_check)

# A narrow signed load has to be sign-extended to match the userspace VM, both
# when the field access folds into one load off the event pointer and when it
# does not -- the second form computes the address first, so the load reads
# through the accumulator instead.
def test_filter_signed_narrow_load(runtime, memleak_check):
    bpf_filter('*(char *)&exit_latency < 0', runtime, memleak_check)
def test_filter_signed_narrow_load_unfolded(runtime, memleak_check):
    bpf_filter('*(char *)((char *)&exit_latency + 1) < 0', runtime, memleak_check)
def test_filter_signed_short_load(runtime, memleak_check):
    bpf_filter('*(short *)&exit_latency < 0', runtime, memleak_check)

# A constant large enough to look like a heap address must still be treated as
# a constant: the backend tells fields, string literals and plain immediates
# apart by address range, and those ranges have to be bounded on both sides.
def test_filter_large_immediate(runtime, memleak_check):
    bpf_filter('pid > 1000000000', runtime, memleak_check)
def test_filter_large_immediate_signed(runtime, memleak_check):
    bpf_filter('exit_latency > 2000000000', runtime, memleak_check)

# Past 32 bits the constant needs ld_imm64, which occupies two instruction
# slots; the second must not be miscounted when jump offsets are resolved.
def test_filter_imm64(runtime, memleak_check):
    bpf_filter('exit_latency > 10000000000', runtime, memleak_check)
def test_filter_imm64_before_branch(runtime, memleak_check):
    bpf_filter('exit_latency > 10000000000 && exit_reason == 12', runtime, memleak_check)

# Assignment: rewrites the event, and doubles as the only way to hold a
# temporary since the expression language has no variables of its own.
def test_filter_assign(runtime, memleak_check):
    bpf_filter('exit_reason = 12, exit_reason == 12', runtime, memleak_check)
def test_filter_assign_temp(runtime, memleak_check):
    bpf_filter('offcpu_wait = exit_latency, offcpu_wait > 1000', runtime, memleak_check)

# runq_delay/offcpu_wait are resolved into their final values just before the
# filter runs, so referring to them must work and must not see an intermediate.
def test_filter_runq_delay(runtime, memleak_check):
    bpf_filter('runq_delay > 1000', runtime, memleak_check)
def test_filter_offcpu_wait(runtime, memleak_check):
    bpf_filter('offcpu_wait > 0 && switches > 0', runtime, memleak_check)
# Both live past the truncation boundary of the -p path, at offsets 24 and 32.
def test_filter_offcpu_wait_and_runq_delay(runtime, memleak_check):
    bpf_filter('offcpu_wait + runq_delay > 1000', runtime, memleak_check)

# The reported breakdown must add up: runq_delay is clamped into [0, off-CPU
# time] because sched_info.run_delay is accounted on rq_clock() rather than the
# bpf_ktime_get_ns() clock exit_latency uses, and an unclamped delta can exceed
# exit_latency once the vcpu migrates. This filter keeps only events that
# violate the invariant, so on a host running VMs it must stay silent -- any
# output is a real regression. On a host without /dev/kvm no events are
# produced and it merely exercises the compile-and-load path.
def test_breakdown_invariant(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--than", "0", "--filter",
                     'runq_delay < 0 || offcpu_wait < 0 || '
                     'runq_delay + offcpu_wait > exit_latency'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)
        assert 'bpf:kvm_exit:' not in line, f"breakdown invariant violated: {line}"
# In -p mode switches/runq_delay/offcpu_wait are never resolved and stay 0, but
# naming them is still a valid filter rather than an error.
def test_filter_unresolved_fields_pid_mode(runtime, memleak_check):
    bpf_filter('offcpu_wait > 0 || switches > 0', runtime, memleak_check, ["-p", "1"])

def test_filter_with_perins(runtime, memleak_check):
    bpf_filter('exit_latency > 1000000', runtime, memleak_check, ["--perins", "--detail"])
def test_filter_with_order(runtime, memleak_check):
    bpf_filter('exit_reason == 12', runtime, memleak_check, ["--order"])
def test_filter_pid_mode(runtime, memleak_check):
    # kvm_entry_pid() takes a different path than the oncpu kvm_entry().
    bpf_filter('exit_reason == 12', runtime, memleak_check, ["-p", "1"])
def test_filter_verbose(runtime, memleak_check):
    # -v dumps the VM and BPF instructions; exercises expr_bpf_dump(). Not run
    # under the leak checker: the first printf() from the dump makes glibc
    # allocate stdio's 4096-byte buffer, which it never frees, and that shows
    # up as a leak that has nothing to do with the filter.
    if memleak_check:
        return
    bpf_filter('exit_reason == 12', runtime, memleak_check, ["-v"])


#
# Constructs the BPF backend cannot express must be rejected at startup, with a
# diagnostic, rather than producing code the verifier rejects later.
#
def bpf_filter_reject(expr, expected, stderr=True):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--filter", expr])
    want = PerfProf.STDERR if stderr else PerfProf.STDOUT
    found = False
    for std, line in prof.run(3, False):
        if std == want and expected in line:
            found = True
    assert found, f"expected {expected!r} for filter {expr!r}"

# Signed / and % need the 6.6 div/mod encoding, so which behaviour is correct
# depends on the kernel under test: accepted on 6.6+, refused with a diagnostic
# below that.
def test_filter_signed_div(runtime, memleak_check):
    if kernel_release() >= (6, 6):
        bpf_filter('exit_latency / 1000 > 5', runtime, memleak_check)
    else:
        bpf_filter_reject('exit_latency / 1000 > 5', 'signed division')
def test_filter_signed_mod(runtime, memleak_check):
    if kernel_release() >= (6, 6):
        bpf_filter('exit_latency % 7 == 0', runtime, memleak_check)
    else:
        bpf_filter_reject('exit_latency % 7 == 0', 'signed division')
def test_filter_reject_ksymbol():
    bpf_filter_reject('ksymbol(exit_latency)', 'ksymbol()')
def test_filter_reject_comm_get():
    bpf_filter_reject('comm_get(pid) == "x"', 'comm_get()')
def test_filter_reject_string_literal():
    bpf_filter_reject('printf("%d", pid)', 'string literals')
def test_filter_reject_undefined_field():
    # The expression front end reports syntax errors on stdout, along with the
    # list of fields that are available.
    bpf_filter_reject('nosuchfield == 1', 'undefined variable', stderr=False)

# The field table is read from BTF, so it tracks struct kvm_vcpu_event: the
# names the event used to expose are gone rather than silently still working.
def test_filter_reject_old_field_names():
    bpf_filter_reject('sched_latency > 0', 'undefined variable', stderr=False)
    bpf_filter_reject('latency > 0', 'undefined variable', stderr=False)
    bpf_filter_reject('run_delay > 0', 'undefined variable', stderr=False)

def test_filter_reject_cpu_global():
    # _cpu and _pid are perf sample-header globals with no meaning in the kernel;
    # they must not compile silently (they would emit a dereference of the
    # userspace prog->glo address). EXPR_F_NO_SAMPLE_GLO withholds them so that
    # an `undefined variable' error is raised instead.
    bpf_filter_reject('_cpu == 0', 'undefined variable', stderr=False)

def test_filter_reject_pid_global():
    bpf_filter_reject('_pid == 0', 'undefined variable', stderr=False)


#
# The kvm_vcpu map is reclaimed by the BPF sched_process_free program, so the
# global_comm service is only needed to turn a pid into a name, and only in
# oncpu mode. It is a system-wide service (task_newtask, task_rename,
# sched_process_free on every CPU), so it must stay off unless an option that
# prints a name is given.
#
def test_no_comm_service_without_name_options(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_comm_service_with_than(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--than", "10ms"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_comm_service_with_perins_detail(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins", "--detail"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

# --perins without --detail aggregates per process (tgid) and prints no name,
# so it must not pull the service in either.
def test_no_comm_service_with_perins_only(runtime, memleak_check):
    prof = PerfProf(["bpf:kvm_exit", "-i", "1000", "--perins"])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)