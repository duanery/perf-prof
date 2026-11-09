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
struct perf_evlist_reap;

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

/*
 * The maps every evsel without its own maps opens on. They are fixed from
 * perf_evlist__open() to perf_evlist__close(): -EBUSY in between. Threads
 * come and go one by one with perf_evlist__add_thread()/del_thread(), cpus
 * with perf_evlist__add_cpu()/del_cpu().
 */
LIBPERF_API int perf_evlist__set_maps(struct perf_evlist *evlist,
				      struct perf_cpu_map *cpus,
				      struct perf_thread_map *threads);
/*
 * Hand the poll fds over to an external poll loop. libperf decides which fds
 * to poll (one per domain of a ring buffer) and calls @add / @del as they
 * appear and go away, from perf_evlist__mmap() on. @new_map is true the first
 * time @map is reported. On EPOLLHUP the loop calls perf_evlist__hup().
 */
typedef int (*perf_evlist_poll_add_t)(void *external, int fd, unsigned events,
				      struct perf_mmap *map, bool new_map);
typedef void (*perf_evlist_poll_del_t)(void *external, int fd,
				       struct perf_mmap *map);
LIBPERF_API void perf_evlist_poll__external(struct perf_evlist *evlist, void *external,
					    perf_evlist_poll_add_t add,
					    perf_evlist_poll_del_t del);
LIBPERF_API void *perf_evlist_poll__get_external(struct perf_evlist *evlist, struct perf_mmap *map);
typedef void (*handle_mmap)(struct perf_mmap *map);
LIBPERF_API int perf_evlist__poll_mmap(struct perf_evlist *evlist, int timeout, handle_mmap handle);
LIBPERF_API int perf_evlist__poll(struct perf_evlist *evlist, int timeout);
LIBPERF_API struct perf_evsel *perf_evlist__id_to_evsel(struct perf_evlist *evlist,
                               uint64_t id, int *pcpu);
LIBPERF_API int perf_evlist__mmap(struct perf_evlist *evlist, int pages);
LIBPERF_API void perf_evlist__munmap(struct perf_evlist *evlist);

/*
 * Removing writers from the evlist - a thread, a cpu, and later an evsel -
 * goes through three steps:
 *
 *  stop - the writers produce no more events: DISABLE, or the kernel stopped
 *         them (an exited task);
 *  reap - libperf hands over every ring buffer they wrote into, see struct
 *         perf_evlist_reap. Their fds are still open, so the caller can read
 *         the ring buffers to the end and the sample ids still resolve;
 *  done - perf_evlist__reap_done(): libperf drops the ring buffer references
 *         of the reap and releases what is left, e.g. closes the fds.
 *
 * A thread is stopped by perf_evlist__del_thread() and reaped on EPOLLHUP by
 * perf_evlist__hup(), in either order; a cpu is stopped and reaped in one go
 * by perf_evlist__del_cpu().
 */

/*
 * Add/remove a thread on every evsel that follows the evlist thread map.
 * Evsels bound to their own thread map (perf_evsel__set_own_threads()) are
 * left alone; drive those with perf_evsel__add_thread() directly.
 *
 * A thread goes away in two signals that may come in either order: the caller
 * (ptrace) reports its exit with perf_evlist__del_thread(), which stops its
 * fds (DISABLE), and the kernel reports EPOLLHUP on its fds, which reaps its
 * ring buffers, see perf_evlist__hup(). The second one closes the fds. A
 * thread with no ring buffer to reap is closed right away.
 *
 * perf_evlist__del_thread() returns 1 when the thread is gone: the caller may
 * now drop its own state of the thread. It returns 0 while waiting for the
 * EPOLLHUP.
 */
LIBPERF_API int perf_evlist__add_thread(struct perf_evlist *evlist, pid_t pid);
LIBPERF_API int perf_evlist__del_thread(struct perf_evlist *evlist, pid_t pid);

/*
 * Add/remove a cpu on every evsel that follows the evlist cpu map. Evsels
 * bound to their own cpu map (perf_evsel__set_own_cpus()) and system wide
 * evsels are left alone; drive those with perf_evsel__add_cpu() directly.
 * Evsels opened on any cpu (cpu == -1) take no cpu.
 *
 * perf_evlist__del_cpu() stops the fds of @cpu (DISABLE) and reaps the ring
 * buffers they wrote into, the caller reads them to the end; then
 * perf_evlist__reap_done() closes the fds. Until then the cpu cannot be added
 * or removed again: -EBUSY. -ENOENT: no evsel has @cpu.
 */
LIBPERF_API int perf_evlist__add_cpu(struct perf_evlist *evlist, int cpu);
LIBPERF_API int perf_evlist__del_cpu(struct perf_evlist *evlist, int cpu,
				     struct perf_evlist_reap *reap);

enum perf_evlist_reap_type {
	PERF_EVLIST_REAP__NONE,		/* nothing reaped */
	PERF_EVLIST_REAP__THREAD,	/* @tid exited, perf_evlist__hup() */
	PERF_EVLIST_REAP__CPU,		/* @cpu removed, perf_evlist__del_cpu() */
};

/*
 * The result of the reap step. @maps holds every ring buffer the removed
 * writers wrote into, each with a reference: the caller reads them to the
 * end, so that no event is left behind. @retired marks the ones left without
 * any writer: they are already gone from the evlist and the caller drops its
 * own references, e.g. the order heap. @complete: the removal is complete
 * once perf_evlist__reap_done() returns - always for a cpu, for a thread when
 * perf_evlist__del_thread() came before the EPOLLHUP; the caller may drop its
 * own state of @tid or @cpu.
 *
 * Always pass the result to perf_evlist__reap_done(), even a
 * PERF_EVLIST_REAP__NONE one.
 */
struct perf_evlist_reap {
	enum perf_evlist_reap_type type;
	pid_t tid;		/* PERF_EVLIST_REAP__THREAD, -1 otherwise */
	int cpu;		/* PERF_EVLIST_REAP__CPU, -1 otherwise */
	bool complete;
	int nr_maps;
	struct perf_evlist_reap_map {
		struct perf_mmap *map;
		bool retired;
	} *maps;
};
/* EPOLLHUP on @fd: reap its exited thread. PERF_EVLIST_REAP__NONE if @fd
 * belongs to no exiting thread, e.g. a second EPOLLHUP. */
LIBPERF_API int perf_evlist__hup(struct perf_evlist *evlist, int fd,
				 struct perf_evlist_reap *reap);
LIBPERF_API void perf_evlist__reap_done(struct perf_evlist *evlist,
					struct perf_evlist_reap *reap);

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
