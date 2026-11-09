/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_THREAD_MAP_H
#define __PERF_THREAD_MAP_H

#include <sys/types.h>
#include <stdio.h>
#include <linux/refcount.h>
#include <perf/threadmap.h>
#include <perf/event.h>
#include <linux/rblist.h>


struct perf_record_thread_map;

struct perf_thread_map *thread_map__new_dummy(void);
struct perf_thread_map *thread_map__new_by_pid(pid_t pid);
struct perf_thread_map *thread_map__new_by_tid(pid_t tid);
struct perf_thread_map *thread_map__new_by_uid(uid_t uid);
struct perf_thread_map *thread_map__new_all_cpus(void);
struct perf_thread_map *thread_map__new(pid_t pid, pid_t tid, uid_t uid);
struct perf_thread_map *thread_map__new_event(struct perf_record_thread_map *event);

struct perf_thread_map *thread_map__new_str(const char *pid,
		const char *tid, uid_t uid, bool all_threads);

struct perf_thread_map *thread_map__new_by_tid_str(const char *tid_str);

size_t thread_map__fprintf(struct perf_thread_map *threads, FILE *fp);

void thread_map__read_comms(struct perf_thread_map *threads);
bool thread_map__has(struct perf_thread_map *threads, pid_t pid);
int thread_map__remove(struct perf_thread_map *threads, int idx);

/*
 * The threads a profiler follows while they come and go (ptrace). Lookups are
 * by value; a thread map is built only where one is needed, e.g. to generate a
 * pid filter: tid_set__map() returns a new map, sorted by pid.
 */
struct tid_set {
	struct rblist tids;
};

int tid_set__init(struct tid_set *set, struct perf_thread_map *threads);
void tid_set__exit(struct tid_set *set);
int tid_set__add(struct tid_set *set, pid_t pid);
bool tid_set__del(struct tid_set *set, pid_t pid);
bool tid_set__has(struct tid_set *set, pid_t pid);
struct perf_thread_map *tid_set__map(struct tid_set *set);
#endif	/* __PERF_THREAD_MAP_H */
