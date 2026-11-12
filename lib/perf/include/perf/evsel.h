/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_EVSEL_H
#define __LIBPERF_EVSEL_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <perf/core.h>

struct perf_evsel;
struct perf_evsel_fd;
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
LIBPERF_API void perf_evsel__close_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API int perf_evsel__mmap(struct perf_evsel *evsel, int pages);
LIBPERF_API void perf_evsel__munmap(struct perf_evsel *evsel);
LIBPERF_API void *perf_evsel__mmap_base(struct perf_evsel *evsel, int cpu, pid_t tid);
LIBPERF_API int perf_evsel__read_size(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__read(struct perf_evsel *evsel, int cpu, pid_t tid,
				 struct perf_counts_values *count);
LIBPERF_API int perf_evsel__enable(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__enable_cpu(struct perf_evsel *evsel, int cpu);
LIBPERF_API int perf_evsel__enable_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API int perf_evsel__enable_group(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__disable(struct perf_evsel *evsel);
LIBPERF_API int perf_evsel__disable_cpu(struct perf_evsel *evsel, int cpu);
LIBPERF_API int perf_evsel__disable_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API int perf_evsel__disable_group(struct perf_evsel *evsel);
LIBPERF_API void perf_evsel__keep_disable(struct perf_evsel *evsel, bool keep_disable);
LIBPERF_API int perf_evsel__apply_filter(struct perf_evsel *evsel, const char *filter);
LIBPERF_API int perf_evsel__apply_filter_cpu(struct perf_evsel *evsel, const char *filter, int cpu);
LIBPERF_API int perf_evsel__apply_filter_thread(struct perf_evsel *evsel, const char *filter, pid_t pid);
LIBPERF_API int perf_evsel__set_bpf(struct perf_evsel *evsel, unsigned int prog_fd);
LIBPERF_API void perf_evsel__set_own_cpus(struct perf_evsel *evsel, struct perf_cpu_map *own_cpus);
LIBPERF_API void perf_evsel__set_own_threads(struct perf_evsel *evsel, struct perf_thread_map *own_threads);
/*
 * Make @leader the group leader of @evsel; @evsel itself or NULL ends the
 * membership. Before perf_evsel__open(): the kernel takes the group leader at
 * perf_event_open() time, -EBUSY once @evsel is open.
 *
 * @leader must be open already and lead a group of its own: it is no member
 * on any fd, nor on its way out of its evlist; and the two must be able to
 * share a group (clock, pinned or exclusive member). @evsel must lead no
 * members itself. -EINVAL otherwise.
 *
 * On each (cpu, tid) @evsel joins the fd of @leader there, and opens alone
 * where @leader has none. Outside an evlist the member fds open with the
 * attr.disabled of @evsel, not enabled to follow @leader: a leader in an open
 * evlist runs already. Open disabled, perf_evlist__add_evsel() enables.
 */
LIBPERF_API int perf_evsel__set_leader(struct perf_evsel *evsel, struct perf_evsel *leader);
/*
 * Private data of the caller, for it to find its own state from an evsel.
 * perf_evsel__delete() releases it with @destructor, unless NULL; so does
 * setting another one.
 */
LIBPERF_API void perf_evsel__set_priv(struct perf_evsel *evsel, void *priv,
				      void (*destructor)(void *priv));
LIBPERF_API void *perf_evsel__priv(struct perf_evsel *evsel);
/*
 * Add/remove a thread or a cpu on an already open evsel. The fds are
 * opened/closed and join/leave the ring buffers of their bindings accordingly.
 * The maps the evsel was opened with do not change: perf_evsel__cpus() and
 * perf_evsel__threads() follow the adds and removes.
 *
 * A thread opens on every cpu of the evsel, a cpu for every thread. Neither
 * can be added to an evsel opened for any thread (pid == -1), resp. on any cpu
 * (cpu == -1): -EINVAL.
 *
 * perf_evsel__del_thread()/del_cpu() close the fds right away: a ring buffer
 * left without any writer is released at once. The caller must have read it
 * before. For a thread that exits, use perf_evlist__del_thread(); to remove a
 * cpu from an evlist, perf_evlist__del_cpu().
 *
 * The fd a ring buffer was mapped from stays alive, disabled, as long as the
 * mapping: when it is a group leader, its members stay in its group.
 */
LIBPERF_API int perf_evsel__add_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API int perf_evsel__del_thread(struct perf_evsel *evsel, pid_t pid);
LIBPERF_API int perf_evsel__add_cpu(struct perf_evsel *evsel, int cpu);
LIBPERF_API int perf_evsel__del_cpu(struct perf_evsel *evsel, int cpu);
LIBPERF_API bool perf_evsel__is_group_leader(struct perf_evsel *evsel, int cpu, pid_t tid);
/*
 * The cpus the evsel has fds for. Until a cpu is added or removed this is the
 * cpu map it was opened with; after that a snapshot in ascending order, valid
 * until the next change.
 */
LIBPERF_API struct perf_cpu_map *perf_evsel__cpus(struct perf_evsel *evsel);
/*
 * The threads the evsel has fds for. Until a thread is added or removed this
 * is the thread map it was opened with; after that a snapshot in open order,
 * valid until the next change.
 */
LIBPERF_API struct perf_thread_map *perf_evsel__threads(struct perf_evsel *evsel);
LIBPERF_API struct perf_event_attr *perf_evsel__attr(struct perf_evsel *evsel);
LIBPERF_API uint64_t perf_evsel__get_id(struct perf_evsel *evsel, int cpu, pid_t tid);

/*
 * The fds of an evsel, addressed by the cpu and pid values they were opened
 * with (-1 included; for a cgroup the pid is the cgroup fd). Iterate them with
 * perf_evsel__for_each_fd(); perf_evsel__fd() returns -1 when there is none.
 */
LIBPERF_API int perf_evsel__fd(struct perf_evsel *evsel, int cpu, pid_t tid);
LIBPERF_API struct perf_evsel_fd *perf_evsel__next_fd(struct perf_evsel *evsel,
						      struct perf_evsel_fd *prev);
LIBPERF_API int perf_evsel_fd__cpu(struct perf_evsel_fd *pos);
LIBPERF_API pid_t perf_evsel_fd__tid(struct perf_evsel_fd *pos);

#define perf_evsel__for_each_fd(evsel, pos)			\
	for ((pos) = perf_evsel__next_fd((evsel), NULL);	\
	     (pos) != NULL;					\
	     (pos) = perf_evsel__next_fd((evsel), (pos)))

#endif /* __LIBPERF_EVSEL_H */
