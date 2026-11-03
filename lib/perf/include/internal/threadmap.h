/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_THREADMAP_H
#define __LIBPERF_INTERNAL_THREADMAP_H

#include <linux/refcount.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * A removed thread leaves a hole behind instead of shifting the map down:
 * evsel->fd / evsel->sample_id are indexed by thread slot, and the
 * struct perf_sample_id entries are linked into evlist->heads[]. Moving them
 * would corrupt those lists. Holes are reused by the next added thread.
 *
 * PID_ANY (-1) is a real value: the dummy thread map means "any thread".
 */
#define PERF_THREAD_MAP_HOLE	((pid_t)-2)

struct thread_map_data {
	pid_t	 pid;
	int	 cgroup;  // PERF_FLAG_PID_CGROUP
	char	*comm;
};

struct perf_thread_map {
	refcount_t	refcnt;
	int		nr;
	int		err_thread;
	struct thread_map_data map[];
};

struct perf_thread_map *perf_thread_map__realloc(struct perf_thread_map *map, int nr);
struct perf_thread_map *perf_thread_map__dup(struct perf_thread_map *orig);

#endif /* __LIBPERF_INTERNAL_THREADMAP_H */
