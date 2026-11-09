/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_EVSEL_H
#define __LIBPERF_INTERNAL_EVSEL_H

#include <linux/types.h>
#include <linux/perf_event.h>
#include <stdbool.h>
#include <sys/types.h>

struct perf_cpu_map;
struct perf_thread_map;
struct perf_mmap;
struct perf_mmap_domain;

/*
 * Per fd, to map back from PERF_SAMPLE_ID to evsel, only used when there are
 * more than one entry in the evlist.
 */
struct perf_sample_id {
	struct hlist_node	 node;   /* perf_evlist::id_heads */
	u64			 id;
	struct perf_evsel	*evsel;
	int			 cpu;
	pid_t			 tid;

	/* Holds total ID period value for PERF_SAMPLE_READ processing. */
	u64			 period;
};

/*
 * One cpu of an evsel, -1 for a dummy cpu map. Referenced by each of its fds,
 * and by the evsel from perf_evsel__open() or perf_evsel__add_cpu() until
 * perf_evsel__del_cpu() or perf_evsel__close(): the cpu stays while it has no
 * fd, threads added later open on every cpu. Freed with the last reference.
 *
 * @del: perf_evlist__del_cpu() stopped its fds, perf_evlist__reap_done()
 * closes them.
 */
struct perf_evsel_cpu {
	int			 cpu;
	int			 refcnt;
	bool			 del;
	struct list_head	 node;     /* perf_evsel::cpu_list */
	struct hlist_node	 hnode;    /* perf_evsel::cpu_heads */
	struct list_head	 fd_list;  /* perf_evsel_fd::cpu_node */
};

/*
 * One thread of an evsel: @tid is the pid argument of perf_event_open(), -1
 * for any thread; for a cgroup the cgroup fd, with @cgroup set. Referenced by
 * each of its fds, and by the evsel from perf_evsel__open() or
 * perf_evsel__add_thread() until perf_evsel__del_thread() or
 * perf_evsel__close(): the thread stays while it has no fd, cpus added later
 * open for every thread. Freed with the last reference.
 */
struct perf_evsel_thread {
	pid_t			 tid;
	bool			 cgroup;
	int			 refcnt;
	struct list_head	 node;     /* perf_evsel::thread_list */
	struct hlist_node	 hnode;    /* perf_evsel::thread_heads */
	struct list_head	 fd_list;  /* perf_evsel_fd::thread_node */
};

#define PERF_EVSEL__CPU_HLIST_BITS 6
#define PERF_EVSEL__CPU_HLIST_SIZE (1 << PERF_EVSEL__CPU_HLIST_BITS)
#define PERF_EVSEL__FD_HLIST_BITS 8
#define PERF_EVSEL__FD_HLIST_SIZE (1 << PERF_EVSEL__FD_HLIST_BITS)
#define PERF_EVSEL__THREAD_HLIST_BITS 6
#define PERF_EVSEL__THREAD_HLIST_SIZE (1 << PERF_EVSEL__THREAD_HLIST_BITS)

/*
 * One open fd of an evsel, keyed by the values it was opened with: @cpu and
 * @tid are the cpu and pid arguments of perf_event_open(), -1 included; for a
 * cgroup fd @tid is the cgroup fd and @cgroup is set. The object never moves
 * while the fd is open, so other structures may point at it.
 */
struct perf_evsel_fd {
	struct perf_evsel	*evsel;
	int			 cpu;
	pid_t			 tid;
	bool			 cgroup;
	int			 fd;
	struct perf_evsel_cpu	*cpu_rec;
	struct perf_evsel_thread *thread;
	/* The fd of the group leader, NULL when this fd leads its group or is alone. */
	struct perf_evsel_fd	*leader;
	/* The domain this fd writes into; its ring buffer is domain->map. */
	struct perf_mmap_domain	*domain;
	/* The ring buffer of perf_evsel__mmap(). */
	struct perf_mmap	*mmap;
	struct perf_sample_id	 sid;
	struct list_head	 cpu_node;     /* perf_evsel_cpu::fd_list */
	struct list_head	 thread_node;  /* perf_evsel_thread::fd_list */
	struct hlist_node	 hnode;        /* perf_evsel::fd_heads */
};

struct perf_evsel {
	struct list_head	 node;  /* perf_evlist::evsel_list */
	struct perf_evlist	*evlist;
	struct perf_event_attr	 attr;
	/*
	 * @cpus/@threads are the bindings in effect. @own_cpus/@own_threads are
	 * the bindings this evsel asked for; when NULL the evlist default is
	 * used. See __perf_evlist__propagate_maps().
	 *
	 * They are input only: perf_evsel__open() opens one fd per (cpu,
	 * thread) value of them, and neither changes once the evsel is open;
	 * they are handed back by perf_evsel__cpus()/perf_evsel__threads().
	 * After the open the evsel only works on its own records, which also
	 * hold the cpus and threads added and removed one by one later.
	 */
	struct perf_cpu_map	*cpus;
	struct perf_cpu_map	*own_cpus;
	struct perf_thread_map	*threads;
	struct perf_thread_map	*own_threads;
	struct list_head	 cpu_list;     /* perf_evsel_cpu::node */
	struct hlist_head	 cpu_heads[PERF_EVSEL__CPU_HLIST_SIZE];        /* perf_evsel_cpu::hnode */
	struct list_head	 thread_list;  /* perf_evsel_thread::node */
	struct hlist_head	 thread_heads[PERF_EVSEL__THREAD_HLIST_SIZE];  /* perf_evsel_thread::hnode */
	struct hlist_head	 fd_heads[PERF_EVSEL__FD_HLIST_SIZE];          /* perf_evsel_fd::hnode */
	int			 nr_fds;
	/* Opened for any thread (pid == -1): no thread can be added. */
	bool			 any_thread;
	/* Opened on any cpu (cpu == -1): no cpu can be added. */
	bool			 any_cpu;
	/* The rank for grouping, see perf_evsel__rank_cmp(). */
	int			 rank_cpus;
	int			 rank_threads;
	/* Threads were added or removed: @threads no longer lists the fds. */
	bool			 threads_changed;
	struct perf_thread_map	*threads_snap;
	/* Cpus were added or removed: @cpus no longer lists the fds. */
	bool			 cpus_changed;
	struct perf_cpu_map	*cpus_snap;
	struct perf_evsel	*leader;
	void			*external;
	bool			 keep_disable;
	bool			 init_enabled;
	/*
	 * Set by perf_evsel__open() until perf_evsel__close(): the fds are
	 * open, the input maps no longer change.
	 */
	bool			 opened;
	/* Set once perf_evlist__mmap() has run over this evsel. */
	bool			 mmaped;
	bool			 enabled;
	/*
	 * Remembered so that threads added after perf_evlist__open() get the
	 * same filter and bpf program as the fds opened up front.
	 */
	char			*filter;
	int			 bpf_prog_fd;
	/* perf_evsel__mmap() pages, -1 when not mapped. */
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
void perf_evsel__close_fd(struct perf_evsel *evsel);

struct perf_evsel_fd *__perf_evsel__fd_find(struct perf_evsel *evsel, int cpu,
					    pid_t tid, bool cgroup);
struct perf_evsel_thread *perf_evsel__thread_find(struct perf_evsel *evsel,
						  pid_t tid, bool cgroup);
/* Any fd at (@cpu, @tid), the cgroup flag aside. */
struct perf_evsel_fd *perf_evsel__fd_find(struct perf_evsel *evsel, int cpu, pid_t tid);
bool perf_evsel__has_thread(struct perf_evsel *evsel, pid_t pid);
bool perf_evsel__has_cpu(struct perf_evsel *evsel, int cpu);
void perf_evsel__set_rank(struct perf_evsel *evsel, struct perf_cpu_map *cpus,
			  struct perf_thread_map *threads);
int perf_evsel__rank_cmp(struct perf_evsel *a, struct perf_evsel *b);
int perf_evsel__enable_thread(struct perf_evsel *evsel, pid_t pid, bool group);
void perf_evsel__disable_thread(struct perf_evsel *evsel, pid_t pid);
int perf_evsel__enable_new_cpu(struct perf_evsel *evsel, int cpu, bool group);
struct perf_evsel_cpu *perf_evsel__cpu_find(struct perf_evsel *evsel, int cpu);

#endif /* __LIBPERF_INTERNAL_EVSEL_H */
