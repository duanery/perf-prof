/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_EVLIST_H
#define __LIBPERF_EVLIST_H

#include <perf/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct perf_evlist;
struct perf_evsel;
struct perf_cpu_map;
struct perf_thread_map;
struct perf_mmap;

LIBPERF_API void perf_evlist__add(struct perf_evlist *evlist,
				  struct perf_evsel *evsel);
LIBPERF_API void perf_evlist__remove(struct perf_evlist *evlist,
				     struct perf_evsel *evsel);
LIBPERF_API struct perf_evlist *perf_evlist__new(void);
LIBPERF_API void perf_evlist__delete(struct perf_evlist *evlist);
LIBPERF_API struct perf_evsel* perf_evlist__next(struct perf_evlist *evlist,
						 struct perf_evsel *evsel);
LIBPERF_API int perf_evlist__open(struct perf_evlist *evlist);
LIBPERF_API void perf_evlist__close(struct perf_evlist *evlist);
LIBPERF_API void perf_evlist__enable(struct perf_evlist *evlist);
LIBPERF_API void perf_evlist__disable(struct perf_evlist *evlist);

#define perf_evlist__for_each_evsel(evlist, pos)	\
	for ((pos) = perf_evlist__next((evlist), NULL);	\
	     (pos) != NULL;				\
	     (pos) = perf_evlist__next((evlist), (pos)))

LIBPERF_API void perf_evlist__set_maps(struct perf_evlist *evlist,
				       struct perf_cpu_map *cpus,
				       struct perf_thread_map *threads);
LIBPERF_API void perf_evlist_poll__external(struct perf_evlist *evlist, void *external);
LIBPERF_API void *perf_evlist_poll__get_external(struct perf_evlist *evlist, struct perf_mmap *map);
typedef int (*foreach_fd)(int fd, unsigned events, struct perf_mmap *mmap);
LIBPERF_API int perf_evlist_poll__foreach_fd(struct perf_evlist *evlist, foreach_fd fn);
typedef void (*handle_mmap)(struct perf_mmap *map);
LIBPERF_API int perf_evlist__poll_mmap(struct perf_evlist *evlist, int timeout, handle_mmap handle);
LIBPERF_API int perf_evlist__poll(struct perf_evlist *evlist, int timeout);
LIBPERF_API struct perf_evsel *perf_evlist__id_to_evsel(struct perf_evlist *evlist,
                               uint64_t id, int *pcpu);
LIBPERF_API int perf_evlist__mmap(struct perf_evlist *evlist, int pages);
LIBPERF_API void perf_evlist__munmap(struct perf_evlist *evlist);

/*
 * Add/remove a thread on every evsel that follows the evlist thread map.
 * Evsels bound to their own thread map (perf_evsel__set_own_threads()) are
 * left alone; drive those with perf_evsel__add_thread() directly.
 */
LIBPERF_API int perf_evlist__add_thread(struct perf_evlist *evlist, pid_t pid);
LIBPERF_API int perf_evlist__del_thread(struct perf_evlist *evlist, pid_t pid);

/* Look up the ring buffer of a binding; see perf_mmap__cpu()/perf_mmap__tid(). */
LIBPERF_API struct perf_mmap *perf_evlist__find_mmap(struct perf_evlist *evlist,
						     int cpu, pid_t tid,
						     bool overwrite);
LIBPERF_API struct perf_mmap *perf_evlist__next_mmap(struct perf_evlist *evlist,
						     struct perf_mmap *map,
						     bool overwrite);
#define perf_evlist__for_each_mmap(evlist, pos, overwrite)		\
	for ((pos) = perf_evlist__next_mmap((evlist), NULL, overwrite);	\
	     (pos) != NULL;						\
	     (pos) = perf_evlist__next_mmap((evlist), (pos), overwrite))

LIBPERF_API void perf_evlist__set_leader(struct perf_evlist *evlist);
LIBPERF_API int perf_evlist__nr_groups(struct perf_evlist *evlist);
LIBPERF_API int perf_evlist__max_read_size(struct perf_evlist *evlist);
#endif /* __LIBPERF_EVLIST_H */
