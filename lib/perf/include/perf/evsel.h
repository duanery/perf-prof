/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_EVSEL_H
#define __LIBPERF_EVSEL_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <perf/core.h>

struct perf_evsel;
struct perf_event_attr;
struct perf_cpu_map;
struct perf_thread_map;

struct perf_counts_values {
	union {
		struct {
			uint64_t val;
			uint64_t ena;
			uint64_t run;
		};
		uint64_t values[3];
	};
};

LIBPERF_API struct perf_evsel *perf_evsel__new(struct perf_event_attr *attr);
LIBPERF_API void perf_evsel__delete(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__open(struct perf_evsel *evsel, struct perf_cpu_map *cpus,
				 struct perf_thread_map *threads);
LIBPERF_API void perf_evsel__close(struct perf_evsel *evsel);
LIBPERF_API void perf_evsel__close_cpu(struct perf_evsel *evsel, int cpu);
LIBPERF_API int perf_evsel__mmap(struct perf_evsel *evsel, int pages);
LIBPERF_API void perf_evsel__munmap(struct perf_evsel *evsel);
LIBPERF_API void *perf_evsel__mmap_base(struct perf_evsel *evsel, int cpu, int thread);
LIBPERF_API int perf_evsel__read_size(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__read(struct perf_evsel *evsel, int cpu, int thread,
				 struct perf_counts_values *count);
LIBPERF_API int perf_evsel__enable(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__enable_cpu(struct perf_evsel *evsel, int cpu);
LIBPERF_API int perf_evsel__enable_group(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__disable(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__disable_cpu(struct perf_evsel *evsel, int cpu);
LIBPERF_API int perf_evsel__disable_group(struct perf_evsel *evsel);
LIBPERF_API void perf_evsel__keep_disable(struct perf_evsel *evsel, bool keep_disable);
LIBPERF_API int perf_evsel__apply_filter(struct perf_evsel *evsel, const char *filter);
LIBPERF_API int perf_evsel__apply_filter_cpu(struct perf_evsel *evsel, const char *filter, int cpu);
LIBPERF_API int perf_evsel__set_bpf(struct perf_evsel *evsel, unsigned int prog_fd);
LIBPERF_API void perf_evsel__set_own_cpus(struct perf_evsel *evsel, struct perf_cpu_map *own_cpus);
LIBPERF_API void perf_evsel__set_own_threads(struct perf_evsel *evsel, struct perf_thread_map *own_threads);
/*
 * Add/remove a thread on an already open evsel. The fds are opened/closed and
 * join/leave the ring buffers of their bindings accordingly.
 *
 * perf_evsel__del_thread() closes the fds right away: a ring buffer left
 * without any writer is released at once. The caller must have read it
 * before. For a thread that exits, use perf_evlist__del_thread() instead.
 *
 * The fd a ring buffer was mapped from stays alive, disabled, as long as the
 * mapping: when it is a group leader, its members stay in its group.
 */
LIBPERF_API int perf_evsel__add_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API int perf_evsel__del_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API bool perf_evsel__is_group_leader(struct perf_evsel *evsel, int cpu, int thread);
LIBPERF_API struct perf_cpu_map *perf_evsel__cpus(struct perf_evsel *evsel);
LIBPERF_API struct perf_thread_map *perf_evsel__threads(struct perf_evsel *evsel);
LIBPERF_API struct perf_event_attr *perf_evsel__attr(struct perf_evsel *evsel);
LIBPERF_API uint64_t perf_evsel__get_id(struct perf_evsel *evsel, int cpu, int thread);

#endif /* __LIBPERF_EVSEL_H */
