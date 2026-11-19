#!/usr/bin/env python3

from PerfProf import PerfProf
from conftest import result_check
import re
import subprocess

def test_rundelay(runtime, memleak_check):
    #perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch -e sched:sched_switch -i 1000 -m 256 --perins
    prof = PerfProf(["rundelay", '-e', 'sched:sched_wakeup*,sched:sched_switch',
                     '-e', 'sched:sched_switch', '-i', '1000', '-m', '256', '--perins'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_rundelay_p1(runtime, memleak_check):
    #perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch -e sched:sched_switch -p 1 -i 1000 --than 4ms --detail=samekey
    prof = PerfProf(["rundelay", '-e', 'sched:sched_wakeup*,sched:sched_switch',
                     '-e', 'sched:sched_switch', '-p', '1', '-i', '1000', '--than', '4ms', '--detail=samekey'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_rundelay_filter_python(runtime, memleak_check):
    #perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch -e sched:sched_switch --filter python -i 1000 --than 4ms --detail=samekey
    prof = PerfProf(["rundelay", '-e', 'sched:sched_wakeup*,sched:sched_switch',
                     '-e', 'sched:sched_switch', '--filter', 'python', '-i', '1000', '--than', '4ms', '--detail=samekey'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_rundelay_filter_python_perins(runtime, memleak_check):
    #perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch -e sched:sched_switch --filter python -i 1000 --than 4ms --detail=samekey --perins
    prof = PerfProf(["rundelay", '-e', 'sched:sched_wakeup*,sched:sched_switch',
                     '-e', 'sched:sched_switch', '--filter', 'python', '-i', '1000', '--than', '4ms', '--detail=samekey', '--perins'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_rundelay_filter_python_profile(runtime, memleak_check):
    #perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch -e 'sched:sched_switch,profile/-F 500 --watermark 50 -m 16 -g/untraced/'
    #--filter python -i 1000 --than 4ms --detail=samecpu
    prof = PerfProf(["rundelay", '-e', 'sched:sched_wakeup*,sched:sched_switch',
                     '-e', 'sched:sched_switch,profile/-F 500 --watermark 50 -m 16 -g/untraced/', '--filter', 'python', '-i', '1000', '--than', '4ms', '--detail=samecpu'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

RUNDELAY = ['rundelay', '-e', 'sched:sched_wakeup*,sched:sched_switch', '-e', 'sched:sched_switch']
FILTER = re.compile(r'sched:(\w+) filter "(.*)"')
SUMMARY = re.compile(r'rundelay: replaced (\d+) replace_failed (\d+) dropped (\d+) twins (\d+)')
LATENCY = re.compile(r'(sched_wakeup|sched_wakeup_new|sched_switch) => sched_switch\s+(\d+)')

def _tail(filter):
    # The open tail: a bare "field>=pid" term, not a range.
    return [int(m.group(1)) for m in re.finditer(r'(?:^|\|\||\()(?:prev_|next_)?pid>=(\d+)(?=\|\||\)|$)', filter)]

def _fork_load(n=4):
    return [subprocess.Popen(['sh', '-c', 'while :; do /bin/true; done']) for i in range(n)]

def _stop(procs):
    for p in procs:
        p.kill()
        p.wait()

def test_rundelay_pid_exact(runtime, memleak_check):
    # ./task-threads gaps 8 N; perf-prof rundelay ... -p <pid> -v -i 1000
    workload = subprocess.Popen(['./task-threads', 'gaps', '8', str(max(runtime, 10) + 10)],
                                stdout=subprocess.PIPE, universal_newlines=True)
    try:
        pid = int(workload.stdout.readline().split()[1])
        prof = PerfProf(RUNDELAY + ['-p', str(pid), '-v', '-i', '1000'])
        filters, ftrace_filter = [], []
        for std, line in prof.run(runtime, memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = FILTER.search(line)
            if m:
                filters.append(m.group(2))
            if 'rundelay: ftrace_filter' in line:
                ftrace_filter.append(line.split()[-1])
            assert 'replace sched' not in line
    finally:
        _stop([workload])
    # The filters fit, no gap merged: the kernel filters are exact, ftrace_filter off.
    assert len(filters) == 4 and not any(_tail(f) for f in filters)
    assert filters[2].startswith('(prev_state==')
    assert ftrace_filter == ['off']

def test_rundelay_pid_gaps(runtime, memleak_check):
    # ./task-threads gaps 1000 N; perf-prof rundelay ... -p <pid> -v -i 1000
    workload = subprocess.Popen(['./task-threads', 'gaps', '1000', str(max(runtime, 10) + 10)],
                                stdout=subprocess.PIPE, universal_newlines=True)
    try:
        pid = int(workload.stdout.readline().split()[1])
        prof = PerfProf(RUNDELAY + ['-p', str(pid), '-v', '-i', '1000'])
        filters, summary = [], None
        for std, line in prof.run(runtime, memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = FILTER.search(line)
            if m:
                filters.append(m.group(2))
            assert 'replace sched' not in line
            # The merged gaps let in others: ftrace_filter stays.
            assert 'ftrace_filter off' not in line
            m = SUMMARY.search(line)
            if m:
                summary = list(map(int, m.groups()))
    finally:
        _stop([workload])
    # 500 tids one apart are over a page as single pids: the gaps are merged.
    assert len(filters) == 4
    for f in filters:
        assert len(f) < 4096 and '&&' in f and not _tail(f)
    assert summary and summary[0] == 0

def test_rundelay_ptrace_tail_close(runtime, memleak_check):
    # perf-prof rundelay ... --ptrace -v -i 1000 -m 256 -- ./task-threads pingpong 8 2, with forks around
    load = _fork_load()
    try:
        prof = PerfProf(RUNDELAY + ['--ptrace', '-v', '-i', '1000', '-m', '256',
                                    '--', './task-threads', 'pingpong', '8', '2'])
        last, opened, summary, calls = {}, set(), None, 0
        for std, line in prof.run(max(runtime, 12), memleak_check):
            result_check(std, line, runtime, memleak_check)
            # The framework drops the twins of a replacement, the new evsel writes first.
            assert 'wrote first' not in line
            m = FILTER.search(line)
            if m and 'replace' in line:
                field = re.search(r'(prev_pid|next_pid|pid)', m.group(2)).group(1)
                last[m.group(1) + ':' + field] = m.group(2)
                if _tail(m.group(2)):
                    opened.add(m.group(1) + ':' + field)
            m = LATENCY.search(line)
            if m:
                calls += int(m.group(2))
            m = SUMMARY.search(line)
            if m:
                summary = list(map(int, m.groups()))
    finally:
        _stop(load)
    # The threads come for 2s, then the forks let in by the tail close it:
    # the filters end with the followed threads only.
    assert summary and summary[0] > 0 and summary[1] == 0
    assert opened == set(last) and len(last) == 4, last
    assert not any(_tail(f) for f in last.values()), last
    assert calls > 0

def test_rundelay_ptrace_exits(runtime, memleak_check):
    # ./task-threads exits 4 30; perf-prof rundelay ... -p <pid> --ptrace -v -i 1000
    workload = subprocess.Popen(['./task-threads', 'exits', '4', '30'],
                                stdout=subprocess.PIPE, universal_newlines=True)
    try:
        pid = int(workload.stdout.readline().split()[1])
        prof = PerfProf(RUNDELAY + ['-p', str(pid), '--ptrace', '-v', '-i', '1000'])
        events = []
        for std, line in prof.run(max(runtime, 16), memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = re.search(r'(\d+):(\d+):(\d+\.\d+) rundelay: (del thread|replace|ftrace_filter \w+)', line)
            if m:
                events.append((int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3)), m.group(4)))
    finally:
        _stop([workload])
    # Exact at first. An exit turns ftrace_filter on at once; the rebuild
    # waits for the exits to settle, then it is off again.
    kinds = [k for t, k in events]
    assert kinds[0] == 'ftrace_filter off' and kinds[1] == 'del thread' and kinds[2] == 'ftrace_filter on', kinds
    dels = [t for t, k in events if k == 'del thread']
    replaces = [t for t, k in events if k == 'replace']
    assert len(dels) == 4 and len(replaces) == 4, kinds
    assert min(replaces) - max(dels) > 5, events
    assert kinds[-1] == 'ftrace_filter off', kinds

PERINS = re.compile(r'^(\d+)\s+(\S+)\s')

def test_rundelay_ptrace_perins(runtime, memleak_check):
    # perf-prof rundelay ... --ptrace --perins -i 1000 -m 256 -- ./task-threads pingpong 6 2, with forks around
    load = _fork_load()
    try:
        prof = PerfProf(RUNDELAY + ['--ptrace', '--perins', '-i', '1000', '-m', '256',
                                    '--', './task-threads', 'pingpong', '6', '2'])
        threads, seen, wakeup_new = set(), {}, set()
        for std, line in prof.run(max(runtime, 10), memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = re.match(r'(WORKLOAD|WORKER) (\d+)', line)
            if m:
                threads.add(int(m.group(2)))
            m = PERINS.match(line)
            if m:
                seen[int(m.group(1))] = m.group(2)
                if 'sched_wakeup_new =>' in line:
                    wakeup_new.add(int(m.group(1)))
    finally:
        _stop(load)
    # The threads created under ptrace are counted, from sched_wakeup_new on,
    # the forks around are not.
    assert len(threads) > 10, threads
    assert set(seen) - threads == set(), seen
    assert threads - set(seen) == set(), threads - set(seen)
    assert set(seen.values()) == {'task-threads'}, seen
    assert len(wakeup_new) > 5, wakeup_new
