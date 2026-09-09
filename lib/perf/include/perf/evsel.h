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
 * the ring buffer of the thread is created/dropped accordingly.
 *
 * The caller must drain the ring buffer of @pid before removing it: the last
 * reference going away destroys the mapping immediately.
 */
LIBPERF_API int perf_evsel__add_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API int perf_evsel__del_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API struct perf_cpu_map *perf_evsel__cpus(struct perf_evsel *evsel);
LIBPERF_API struct perf_thread_map *perf_evsel__threads(struct perf_evsel *evsel);
LIBPERF_API struct perf_event_attr *perf_evsel__attr(struct perf_evsel *evsel);
LIBPERF_API uint64_t perf_evsel__get_id(struct perf_evsel *evsel, int cpu, int thread);

#endif /* __LIBPERF_EVSEL_H */
