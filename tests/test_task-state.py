#!/usr/bin/env python3

from PerfProf import PerfProf
from conftest import result_check
import re
import subprocess

def test_task_state_mode0(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode0_perfeval_cpus(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '--perfeval-cpus', '0'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode0_perfeval_limit(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '--perfeval-cpus', '0', '--sampling-limit', '1000'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode1(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 --filter 'python,sh,bash,ls,ps,awk,grep'
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '--filter', 'python,sh,bash,ls,ps,awk,grep'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode2(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 -SD
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '-SD'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode3(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 -SD --filter 'python,sh,bash,ls,ps,awk,grep'
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '-SD', '--filter', 'python,sh,bash,ls,ps,awk,grep'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode0_NOS(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 --no-interruptible
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '--no-interruptible'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode1_NOS(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 --filter 'python,sh,bash,ls,ps,awk,grep' --no-interruptible
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '--filter', 'python,sh,bash,ls,ps,awk,grep', '--no-interruptible'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode1_than(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 --filter 'python,sh,bash,ls,ps,awk,grep' --than 1s
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '--filter', 'python,sh,bash,ls,ps,awk,grep', '--than', '1s'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode1_than_perins(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 --filter 'python,sh,bash,ls,ps,awk,grep' --than 1s --perins
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '--filter', 'python,sh,bash,ls,ps,awk,grep', '--than', '1s', '--perins'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode3_than(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 -SD --filter 'python,sh,bash,ls,ps,awk,grep' --than 1s
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '-SD', '--filter', 'python,sh,bash,ls,ps,awk,grep', '--than', '1s'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_mode3_than_perins(runtime, memleak_check):
    # perf-prof task-state -i 1000 -m 256 -SD --filter 'python,sh,bash,ls,ps,awk,grep' --than 1s --perins
    task_state = PerfProf(['task-state', '-i', '1000', '-m', '256', '-SD', '--filter', 'python,sh,bash,ls,ps,awk,grep', '--than', '1s', '--perins'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_filter(runtime, memleak_check):
    # perf-prof task-state --filter 'java,python*' -S --than 100ms -g
    task_state = PerfProf(['task-state', '--filter', 'java,python*', '-S', '--than', '100ms', '-g'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_ip_link_show(runtime, memleak_check):
    # perf-prof task-state -- ip link show
    task_state = PerfProf(['task-state', '--', 'ip', 'link', 'show'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_pthread1(runtime, memleak_check):
    # perf-prof task-state -i 1000 --perins -m 64 -- ./pthread --loop 10 --depth 3
    task_state = PerfProf(['task-state', '-i', '1000', '--perins', '-m', '64', '--', './pthread', '--loop', '10', '--depth', '3'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_pthread2(runtime, memleak_check):
    # perf-prof task-state -i 1000 --perins -m 64 -- ./pthread --depth 2
    task_state = PerfProf(['task-state', '-i', '1000', '--perins', '-m', '64', '--', './pthread', '--depth', '2'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

def test_task_state_pthread3(runtime, memleak_check):
    # perf-prof task-state -i 1000 --perins -m 64 -- ./pthread --daemonize --depth 5
    task_state = PerfProf(['task-state', '-i', '1000', '--perins', '-m', '64', '--', './pthread', '--daemonize', '--depth', '5'])
    for std, line in task_state.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)

FILTER = re.compile(r'sched:(\w+) filter "(.*)"')
SUMMARY = re.compile(r'task-state: replaced (\d+) replace_failed (\d+) dropped (\d+) twins (\d+)')

def _tail(filter):
    # The open tail: a bare "field>=pid" term, not a range.
    return [int(m.group(1)) for m in re.finditer(r'(?:^|\|\||\()(?:prev_|next_)?pid>=(\d+)(?=\|\||\)|$)', filter)]

def _fork_load(n=4):
    return [subprocess.Popen(['sh', '-c', 'while :; do /bin/true; done']) for i in range(n)]

def _stop(procs):
    for p in procs:
        p.kill()
        p.wait()

def test_task_state_ptrace_open_tail(runtime, memleak_check):
    # perf-prof task-state --ptrace -v -- ./binding-workload 16
    task_state = PerfProf(['task-state', '--ptrace', '-v', '--', './binding-workload', '16'])
    initial, replaced, workers, summary = [], [], 0, None
    for std, line in task_state.run(max(runtime, 10), memleak_check):
        result_check(std, line, runtime, memleak_check)
        m = FILTER.search(line)
        if m:
            (replaced if 'replace' in line else initial).append(m.group(2))
        if line.startswith('WORKER '):
            workers += 1
        m = SUMMARY.search(line)
        if m:
            summary = list(map(int, m.groups()))
    assert workers == 16
    # The known threads only at the start; the first worker opens the tail,
    # once per evsel; the later workers are let in by it.
    assert len(initial) == 3 and not any(_tail(f) for f in initial)
    assert len(replaced) == 3 and all(len(_tail(f)) == 1 for f in replaced)
    assert summary and summary[0] == 3 and summary[1] == 0

def test_task_state_pid_gaps(runtime, memleak_check):
    # ./task-threads gaps 1000 N; perf-prof task-state -p <pid> -v -i 1000
    workload = subprocess.Popen(['./task-threads', 'gaps', '1000', str(max(runtime, 10) + 10)],
                                stdout=subprocess.PIPE, universal_newlines=True)
    try:
        pid = int(workload.stdout.readline().split()[1])
        task_state = PerfProf(['task-state', '-p', str(pid), '-v', '-i', '1000'])
        filters, summary = [], None
        for std, line in task_state.run(runtime, memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = FILTER.search(line)
            if m:
                filters.append(m.group(2))
            assert 'replace sched' not in line
            m = SUMMARY.search(line)
            if m:
                summary = list(map(int, m.groups()))
    finally:
        _stop([workload])
    # 500 tids one apart are over a page as single pids: the gaps are merged.
    assert len(filters) == 3
    for f in filters:
        assert len(f) < 4096 and '&&' in f and not _tail(f)
    assert summary and summary[0] == 0

def test_task_state_ptrace_refilter(runtime, memleak_check):
    # perf-prof task-state --ptrace -v -i 1000 -m 256 -- ./task-threads pingpong 6, with forks around
    load = _fork_load()
    try:
        task_state = PerfProf(['task-state', '--ptrace', '-v', '-i', '1000', '-m', '256', '--', './task-threads', 'pingpong', '6'])
        tails, summary = [], None
        for std, line in task_state.run(max(runtime, 10), memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = FILTER.search(line)
            if m and 'replace' in line:
                tails.extend(_tail(m.group(2)))
            m = SUMMARY.search(line)
            if m:
                summary = list(map(int, m.groups()))
    finally:
        _stop(load)
    # The forks let in by the open tail are dropped: the filters are
    # replaced again, the tail moves on.
    assert summary and summary[0] > 3 and summary[1] == 0
    assert max(tails) > min(tails)

def test_task_state_forward_no_twins(runtime, memleak_check):
    # perf-prof trace -e task-state/-S -v -m 256/ --ptrace -- ./task-threads pingpong 6, with forks around
    load = _fork_load()
    last, twins, summary = {}, [], None
    try:
        prof = PerfProf(['trace', '-e', 'task-state/-S -v -m 256/', '--ptrace', '--', './task-threads', 'pingpong', '6'])
        for std, line in prof.run(max(runtime, 10), memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = re.search(r'\[(\d+)\]\s+(\d+\.\d+):\s+(sched:\w+):\s+(.*)$', line)
            if m:
                cpu, time, key = m.group(1), float(m.group(2)), (m.group(3), m.group(4))
                # Both evsels of a role write the records meanwhile: one is dropped.
                if cpu in last and last[cpu][0] == key and time - last[cpu][1] < 0.0000015:
                    twins.append(line)
                last[cpu] = (key, time)
            m = SUMMARY.search(line)
            if m:
                summary = list(map(int, m.groups()))
    finally:
        _stop(load)
    assert summary and summary[0] > 2
    assert not twins, twins[:4]

def test_task_state_ptrace_tail_close(runtime, memleak_check):
    # perf-prof task-state --ptrace -v -i 1000 -m 256 -- ./task-threads pingpong 8 2, with forks around
    load = _fork_load()
    try:
        task_state = PerfProf(['task-state', '--ptrace', '-v', '-i', '1000', '-m', '256', '--', './task-threads', 'pingpong', '8', '2'])
        last, opened, summary = {}, set(), None
        for std, line in task_state.run(max(runtime, 12), memleak_check):
            result_check(std, line, runtime, memleak_check)
            m = FILTER.search(line)
            if m and 'replace' in line:
                field = re.match(r'\(?(\w+)', m.group(2)).group(1)
                last[field] = m.group(2)
                if _tail(m.group(2)):
                    opened.add(field)
            m = SUMMARY.search(line)
            if m:
                summary = list(map(int, m.groups()))
    finally:
        _stop(load)
    # The threads come for 2s, then the forks let in by the tail close it:
    # the filters end with the followed threads only, no more replacements.
    assert summary and summary[1] == 0
    assert opened == set(last) and len(last) == 3
    assert not any(_tail(f) for f in last.values()), last
