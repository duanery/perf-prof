/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_EVSEL_H
#define __LIBPERF_INTERNAL_EVSEL_H

#include <linux/types.h>
#include <linux/perf_event.h>
#include <stdbool.h>
#include <sys/types.h>

struct perf_cpu_map;
struct perf_thread_map;
struct xyarray;
struct perf_mmap;

struct perf_evsel_fd {
	int fd;
	int group_fd;
	struct perf_mmap *mmap;
};

/*
 * Per fd, to map back from PERF_SAMPLE_ID to evsel, only used when there are
 * more than one entry in the evlist.
 */
struct perf_sample_id {
	struct hlist_node	 node;
	u64			 id;
	struct perf_evsel	*evsel;
       /*
	* 'idx' will be used for AUX area sampling. A sample will have AUX area
	* data that will be queued for decoding, where there are separate
	* queues for each CPU (per-cpu tracing) or task (per-thread tracing).
	* The sample ID can be used to lookup 'idx' which is effectively the
	* queue number.
	*/
	int			 idx;
	int			 cpu;
	pid_t			 tid;

	/* Holds total ID period value for PERF_SAMPLE_READ processing. */
	u64			 period;
};

struct perf_evsel {
	struct list_head	 node;
	struct perf_evlist	*evlist;
	struct perf_event_attr	 attr;
	/*
	 * @cpus/@threads are the bindings in effect. @own_cpus/@own_threads are
	 * the bindings this evsel asked for; when NULL the evlist default is
	 * used. See __perf_evlist__propagate_maps().
	 */
	struct perf_cpu_map	*cpus;
	struct perf_cpu_map	*own_cpus;
	struct perf_thread_map	*threads;
	struct perf_thread_map	*own_threads;
	struct xyarray		*fd;
	struct xyarray		*mmap;
	struct xyarray		*sample_id;
	u64			*id;
	u32			 ids;
	struct perf_evsel	*leader;
	void			*external;
	bool			 keep_disable;
	bool			 init_enabled;
	/* Set once the evsel is open: the cpu binding no longer changes. */
	bool			 cpus_bound;
	/* Set once perf_evlist__mmap() has run over this evsel. */
	bool			 mmaped;
	bool			 enabled;
	/* The effective thread map has diverged from the initial binding. */
	bool			 threads_private;
	/*
	 * Remembered so that threads added after perf_evlist__open() get the
	 * same filter and bpf program as the fds opened up front.
	 */
	char			*filter;
	int			 bpf_prog_fd;
	int			 mmap_pages;

	/* parse modifier helper */
	int			 nr_members;
	/*
	 * system_wide is for events that need to be on every CPU, irrespective
	 * of user requested CPUs or threads. Tha main example of this is the
	 * dummy event. Map propagation will set cpus for this event to all CPUs
	 * as software PMU events like dummy, have a CPU map that is empty.
	 */
	bool			 system_wide;
	/*
	 * Some events, for example uncore events, require a CPU.
	 * i.e. it cannot be the 'any CPU' value of -1.
	 */
	bool			 requires_cpu;
	int			 idx;
};

void perf_evsel__init(struct perf_evsel *evsel, struct perf_event_attr *attr,
		      int idx);
int perf_evsel__alloc_fd(struct perf_evsel *evsel, int ncpus, int nthreads);
void perf_evsel__close_fd(struct perf_evsel *evsel);
void perf_evsel__free_fd(struct perf_evsel *evsel);
int perf_evsel__alloc_id(struct perf_evsel *evsel, int ncpus, int nthreads);
void perf_evsel__free_id(struct perf_evsel *evsel);

/*
 * The binding dimension of an evsel. A non-empty cpu map means one ring buffer
 * per cpu (all of the evsel's threads write into the ring buffer of the cpu
 * they run on); an empty (dummy) cpu map means one ring buffer per thread.
 */
bool perf_evsel__oncpu(struct perf_evsel *evsel);
/* Resolve the (cpu, tid) ring buffer key for one fd of the evsel. */
void perf_evsel__mmap_bind(struct perf_evsel *evsel, int cpu, int thread,
			   int *bind_cpu, pid_t *bind_tid);
int perf_evsel__open_one(struct perf_evsel *evsel, int cpu, int thread);
int perf_evsel__make_threads_private(struct perf_evsel *evsel);
struct perf_evsel_fd *perf_evsel__fd_entry(struct perf_evsel *evsel, int cpu, int thread);
int perf_evsel__enable_thread(struct perf_evsel *evsel, pid_t pid, bool group);

#endif /* __LIBPERF_INTERNAL_EVSEL_H */
