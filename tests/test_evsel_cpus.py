#!/usr/bin/env python3
'''
Per-evsel cpu binding tests: different events (evsels) bound to different cpus.

Event syntax: `sched:sched_switch//cpus=0-3/` pins one event (evsel) to its own
cpu map, other events still follow the global `-C` cpus. libperf side is
perf_evsel__set_own_cpus() + __perf_evlist__propagate_maps(); ring buffers are
created/shared per (cpu, tid) binding by perf_evlist__mmap().

Verification strategy: dump events into a sqlite database (--output2), then
assert the set of _cpu values recorded for each event table.
'''

from PerfProf import PerfProf
from conftest import result_check
import pytest
import os
import sqlite3

NR_CPUS = os.cpu_count()

def db_columns(db_file, table):
    conn = sqlite3.connect(db_file)
    try:
        return [row[1] for row in conn.execute("PRAGMA table_info(%s)" % table)]
    finally:
        conn.close()

def db_query(db_file, sql):
    conn = sqlite3.connect(db_file)
    try:
        return conn.execute(sql).fetchall()
    finally:
        conn.close()

def run_sql_to_db(prof_args, db_file, runtime, memleak_check):
    '''Run `perf-prof sql ... --output2 db_file` for @runtime seconds.'''
    if os.path.exists(db_file):
        os.remove(db_file)
    # perf-prof sql <prof_args> --output2 db_file -i 1000 -m 64
    prof = PerfProf(['sql'] + prof_args + ['--output2', db_file, '-i', '1000', '-m', '64'])
    try:
        for std, line in prof.run(runtime, memleak_check):
            result_check(std, line, runtime, memleak_check)
        assert os.path.exists(db_file), "Database file was not created"
    finally:
        pass
    yield db_file
    if os.path.exists(db_file):
        os.remove(db_file)

def check_cpus_only(db_file, table, cpus, must_have_events=True):
    '''Assert all rows of @table were recorded on the cpus in @cpus (a set).'''
    assert table in [r[0] for r in db_query(db_file, "SELECT name FROM sqlite_master WHERE type='table'")], \
           "Table %s not found in database" % table
    assert '_cpu' in db_columns(db_file, table), "No _cpu column in table %s" % table
    rows = db_query(db_file, "SELECT DISTINCT _cpu FROM %s" % table)
    got = set([row[0] for row in rows])
    if must_have_events:
        count = db_query(db_file, "SELECT COUNT(*) FROM %s" % table)[0][0]
        assert count > 0, "No events recorded in table %s" % table
        assert got.issubset(cpus), "%s: events on cpus %s, expected subset of %s" % (table, sorted(got), sorted(cpus))
    return got

def test_evsel_cpus_same_event_two_bindings(runtime, memleak_check):
    '''One event split into two evsels, each pinned to a different cpu.
    Same ring-buffer layout rules apply per evsel; events of each evsel must
    appear only on its own cpus.
    '''
    # perf-prof sql -e 'sched:sched_switch//cpus=0/alias=sw_cpu0/,sched:sched_switch//cpus=1/alias=sw_cpu1/' \
    #               --output2 test_evsel_cpus_split.db -i 1000 -m 64
    for db_file in run_sql_to_db(['-e', 'sched:sched_switch//cpus=0/alias=sw_cpu0/,sched:sched_switch//cpus=1/alias=sw_cpu1/'],
                                 'test_evsel_cpus_split.db', runtime, memleak_check):
        cpu0 = check_cpus_only(db_file, 'sw_cpu0', {0})
        cpu1 = check_cpus_only(db_file, 'sw_cpu1', {1})
        assert 0 in cpu0, "cpu0-pinned evsel must record events on cpu 0"
        assert 1 in cpu1, "cpu1-pinned evsel must record events on cpu 1"

def test_evsel_cpus_distinct_events(runtime, memleak_check):
    '''Different events pinned to different cpus: sched_wakeup on cpu 0,
    sched_switch on cpu 1.
    '''
    # perf-prof sql -e 'sched:sched_wakeup//cpus=0/alias=wu0/,sched:sched_switch//cpus=1/alias=sw1/' \
    #               --output2 test_evsel_cpus_distinct.db -i 1000 -m 64
    for db_file in run_sql_to_db(['-e', 'sched:sched_wakeup//cpus=0/alias=wu0/,sched:sched_switch//cpus=1/alias=sw1/'],
                                 'test_evsel_cpus_distinct.db', runtime, memleak_check):
        check_cpus_only(db_file, 'wu0', {0})
        check_cpus_only(db_file, 'sw1', {1})

def test_evsel_cpus_shared_mmap_same_cpu(runtime, memleak_check):
    '''Different events pinned to the SAME cpu: the first fd creates the ring
    buffer, the later one joins it via PERF_EVENT_IOC_SET_OUTPUT and takes a
    reference. The two events sharing one mmap must still be told apart by
    their event ids.
    '''
    # perf-prof sql -C 0-3 -e 'sched:sched_switch//cpus=0/alias=sw0/,sched:sched_wakeup//cpus=0/alias=wu0/' \
    #               --output2 test_evsel_cpus_shared.db -i 1000 -m 64
    for db_file in run_sql_to_db(['-C', '0-3',
                                  '-e', 'sched:sched_switch//cpus=0/alias=sw0/,sched:sched_wakeup//cpus=0/alias=wu0/'],
                                 'test_evsel_cpus_shared.db', runtime, memleak_check):
        check_cpus_only(db_file, 'sw0', {0})
        check_cpus_only(db_file, 'wu0', {0})

def test_evsel_cpus_own_plus_default(runtime, memleak_check):
    '''Mixed binding: one evsel has its own cpus (cpu 0), another follows the
    evlist default (-C 0-3).
    '''
    # perf-prof sql -C 0-3 -e 'sched:sched_switch//cpus=0/alias=sw0/,sched:sched_wakeup/alias=wu_default/' \
    #               --output2 test_evsel_cpus_mixed.db -i 1000 -m 64
    for db_file in run_sql_to_db(['-C', '0-3',
                                  '-e', 'sched:sched_switch//cpus=0/alias=sw0/,sched:sched_wakeup//alias=wu_default/'],
                                 'test_evsel_cpus_mixed.db', runtime, memleak_check):
        check_cpus_only(db_file, 'sw0', {0})
        check_cpus_only(db_file, 'wu_default', {0, 1, 2, 3})

def test_evsel_cpus_intersect_with_global_cpus(runtime, memleak_check):
    '''cpus= intersects with global -C: cpus=0-1 ∩ -C 1-2 => {1}.'''
    # perf-prof sql -C 1-2 -e 'sched:sched_switch//cpus=0-1/alias=sw/' \
    #               --output2 test_evsel_cpus_intersect.db -i 1000 -m 64
    for db_file in run_sql_to_db(['-C', '1-2', '-e', 'sched:sched_switch//cpus=0-1/alias=sw/'],
                                 'test_evsel_cpus_intersect.db', runtime, memleak_check):
        check_cpus_only(db_file, 'sw', {1})

def test_evsel_cpus_empty_intersect_fallback(runtime, memleak_check):
    '''Empty intersection falls back to the evlist default binding:
    cpus=99 ∩ -C 0-3 = {} => the evsel follows -C 0-3.
    '''
    # perf-prof sql -C 0-3 -e 'sched:sched_switch//cpus=99/alias=sw/' \
    #               --output2 test_evsel_cpus_fallback.db -i 1000 -m 64
    for db_file in run_sql_to_db(['-C', '0-3', '-e', 'sched:sched_switch//cpus=99/alias=sw/'],
                                 'test_evsel_cpus_fallback.db', runtime, memleak_check):
        got = check_cpus_only(db_file, 'sw', {0, 1, 2, 3})
        assert 99 not in got, "cpu 99 is not in -C 0-3, no event may be recorded on it"

def test_evsel_cpus_global_cpus(runtime, memleak_check):
    '''All evsels uniformly bound to their own cpus (uniformly bound evlist
    forms a single group led by the first evsel).'''
    # perf-prof sql -e 'sched:sched_wakeup//cpus=0-1/alias=wu01/,sched:sched_switch//cpus=0-1/alias=sw01/' \
    #               --output2 test_evsel_cpus_uniform.db -i 1000 -m 64
    for db_file in run_sql_to_db(['-e', 'sched:sched_wakeup//cpus=0-1/alias=wu01/,sched:sched_switch//cpus=0-1/alias=sw01/'],
                                 'test_evsel_cpus_uniform.db', runtime, memleak_check):
        got = check_cpus_only(db_file, 'wu01', {0, 1})
        got = check_cpus_only(db_file, 'sw01', {0, 1})
        assert got == {0, 1}, "sched events must appear on both cpu 0 and cpu 1, got %s" % sorted(got)

def test_evsel_cpus_with_order(runtime, memleak_check):
    '''Mixed own-cpu/default bindings under --order: per-mmap ordered queues
    are merged into one global min-heap, and the output must be printed in
    ascending time order.
    '''
    # perf-prof trace --order -C 0-3 -e 'sched:sched_wakeup//cpus=0/,sched:sched_switch//cpus=1/,sched:sched_wakeup/target_cpu<4/' \
    #                 -m 128 -i 1000 -N 100000
    prof = PerfProf(['trace', '--order', '-C', '0-3',
                     '-e', 'sched:sched_wakeup//cpus=0/,sched:sched_switch//cpus=1/,sched:sched_wakeup/target_cpu<4/',
                     '-m', '128', '-i', '1000', '-N', '100000'])
    import datetime
    prev_time = None
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)
        if std == PerfProf.STDOUT:
            # Data lines look like: "2026-09-14 10:41:47.151842  comm  tid ..."
            fields = line.split()
            if len(fields) < 2:
                continue
            try:
                ts = datetime.datetime.strptime(fields[0] + ' ' + fields[1],
                                                '%Y-%m-%d %H:%M:%S.%f').timestamp()
            except ValueError:
                continue
            if prev_time is not None:
                assert ts >= prev_time, "--order output must be in ascending time order: %f > %f" % (prev_time, ts)
            prev_time = ts
