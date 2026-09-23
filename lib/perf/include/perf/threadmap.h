/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_THREADMAP_H
#define __LIBPERF_THREADMAP_H

#include <perf/core.h>
#include <sys/types.h>
#include <stdbool.h>

struct perf_thread_map;

LIBPERF_API struct perf_thread_map *perf_thread_map__new_dummy(void);

LIBPERF_API void perf_thread_map__set_pid(struct perf_thread_map *map, int thread, pid_t pid);
LIBPERF_API void perf_thread_map__pid_cgroup(struct perf_thread_map *map, int thread);
LIBPERF_API char *perf_thread_map__comm(struct perf_thread_map *map, int thread);
LIBPERF_API int perf_thread_map__nr(struct perf_thread_map *threads);
LIBPERF_API pid_t perf_thread_map__pid(struct perf_thread_map *map, int thread);
LIBPERF_API int perf_thread_map__idx(struct perf_thread_map *map, int pid);
/* False for slots left behind by perf_evsel__del_thread(). */
LIBPERF_API bool perf_thread_map__valid(struct perf_thread_map *map, int thread);

LIBPERF_API struct perf_thread_map *perf_thread_map__get(struct perf_thread_map *map);
LIBPERF_API void perf_thread_map__put(struct perf_thread_map *map);

#define perf_thread_map__for_each_thread(thread, idx, threads)		\
	for ((idx) = 0, (thread) = perf_thread_map__pid(threads, idx);	\
	     (idx) < perf_thread_map__nr(threads);			\
	     (idx)++, (thread) = perf_thread_map__pid(threads, idx))


#endif /* __LIBPERF_THREADMAP_H */
