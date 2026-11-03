/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_EVLIST_H
#define __LIBPERF_INTERNAL_EVLIST_H

#include <linux/list.h>
#include <internal/evsel.h>
#include <sys/epoll.h>

#define PERF_EVLIST__HLIST_BITS 8
#define PERF_EVLIST__HLIST_SIZE (1 << PERF_EVLIST__HLIST_BITS)

#define PERF_EVLIST__MMAP_HLIST_BITS 6
#define PERF_EVLIST__MMAP_HLIST_SIZE (1 << PERF_EVLIST__MMAP_HLIST_BITS)

struct perf_cpu_map;
struct perf_thread_map;
struct perf_mmap_param;

struct perf_mmap;

/*
 * @add/@del let the owner of an external poll loop track fds that appear or go
 * away after perf_evlist__mmap(), i.e. when threads are added or removed.
 */
typedef int (*perf_evlist_poll_add_t)(void *external, int fd, unsigned events,
				      struct perf_mmap *map);
typedef void (*perf_evlist_poll_del_t)(void *external, int fd,
				       struct perf_mmap *map);

struct perf_evlist_poll {
	int epfd;
	int maxevents;
	struct epoll_event *events;
	void *external;
	perf_evlist_poll_add_t add;
	perf_evlist_poll_del_t del;
	int nr;		/* high water mark of used slots */
	int nr_live;	/* slots currently holding an fd */
	int nr_alloc;
	struct perf_evlist_poll_data {
		int fd;
		unsigned events;
		struct perf_mmap *mmap;
	} *data;
};

struct perf_evlist {
	struct list_head	 entries;
	int			 nr_entries;
	bool			 has_user_cpus;
	bool			 needs_map_propagation;
	/**
	 * The cpus passed from the command line or all online CPUs by
	 * default.
	 */
	struct perf_cpu_map	*user_requested_cpus;
	/** The union of all evsel cpu maps. */
	struct perf_cpu_map	*all_cpus;
	struct perf_thread_map	*threads;
	int			 rl_file;
	int			 nr_mmaps;
	size_t			 mmap_len;
	struct perf_evlist_poll epoll;
	struct hlist_head	 heads[PERF_EVLIST__HLIST_SIZE];
	/*
	 * Ring buffers are allocated on demand and shared by every evsel with
	 * the same (cpu, tid, overwrite) binding. @mmap_list/@mmap_ovw_list keep
	 * them in creation order for perf_evlist__for_each_mmap(), @mmap_heads
	 * indexes them by binding for perf_evlist__find_mmap().
	 */
	struct list_head	 mmap_list;
	struct list_head	 mmap_ovw_list;
	struct hlist_head	 mmap_heads[PERF_EVLIST__MMAP_HLIST_SIZE];
	int			 next_mmap_idx;
	bool			 mmaped;
	bool			 grouped;
	bool			 adding_thread;
};

void perf_evlist_poll__init(struct perf_evlist *evlist);
int perf_evlist_poll__alloc(struct perf_evlist *evlist);
void perf_evlist_poll__free(struct perf_evlist *evlist);
int perf_evlist_poll__add(struct perf_evlist *evlist, int fd,
			  struct perf_mmap *mmap, unsigned revent);
int perf_evlist_poll__del(struct perf_evlist *evlist, int n);
int perf_evlist_poll__del_fd(struct perf_evlist *evlist, int fd);
void perf_evlist_poll__set_ops(struct perf_evlist *evlist,
			       perf_evlist_poll_add_t add,
			       perf_evlist_poll_del_t del);


void perf_evlist__init(struct perf_evlist *evlist);
void perf_evlist__exit(struct perf_evlist *evlist);

/*
 * Attach one already open fd of @evsel to the ring buffer of its binding,
 * creating the ring buffer if this is the first fd to use it. Used both by
 * perf_evlist__mmap() and by the dynamic thread path.
 */
int perf_evlist__mmap_evsel_fd(struct perf_evlist *evlist,
			       struct perf_evsel *evsel,
			       int cpu, int thread);
void perf_evlist__unmap_evsel_fd(struct perf_evlist *evlist,
				 struct perf_evsel *evsel,
				 int cpu, int thread);
int perf_evlist__prepare_remove_fd(struct perf_evlist *evlist, struct perf_evsel_fd *entry);

/**
 * __perf_evlist__for_each_entry - iterate thru all the evsels
 * @list: list_head instance to iterate
 * @evsel: struct perf_evsel iterator
 */
#define __perf_evlist__for_each_entry(list, evsel) \
	list_for_each_entry(evsel, list, node)

/**
 * evlist__for_each_entry - iterate thru all the evsels
 * @evlist: perf_evlist instance to iterate
 * @evsel: struct perf_evsel iterator
 */
#define perf_evlist__for_each_entry(evlist, evsel) \
	__perf_evlist__for_each_entry(&(evlist)->entries, evsel)

/**
 * __perf_evlist__for_each_entry_reverse - iterate thru all the evsels in reverse order
 * @list: list_head instance to iterate
 * @evsel: struct evsel iterator
 */
#define __perf_evlist__for_each_entry_reverse(list, evsel) \
	list_for_each_entry_reverse(evsel, list, node)

/**
 * perf_evlist__for_each_entry_reverse - iterate thru all the evsels in reverse order
 * @evlist: evlist instance to iterate
 * @evsel: struct evsel iterator
 */
#define perf_evlist__for_each_entry_reverse(evlist, evsel) \
	__perf_evlist__for_each_entry_reverse(&(evlist)->entries, evsel)

/**
 * __perf_evlist__for_each_entry_safe - safely iterate thru all the evsels
 * @list: list_head instance to iterate
 * @tmp: struct evsel temp iterator
 * @evsel: struct evsel iterator
 */
#define __perf_evlist__for_each_entry_safe(list, tmp, evsel) \
	list_for_each_entry_safe(evsel, tmp, list, node)

/**
 * perf_evlist__for_each_entry_safe - safely iterate thru all the evsels
 * @evlist: evlist instance to iterate
 * @evsel: struct evsel iterator
 * @tmp: struct evsel temp iterator
 */
#define perf_evlist__for_each_entry_safe(evlist, tmp, evsel) \
	__perf_evlist__for_each_entry_safe(&(evlist)->entries, tmp, evsel)

static inline struct perf_evsel *perf_evlist__first(struct perf_evlist *evlist)
{
	return list_entry(evlist->entries.next, struct perf_evsel, node);
}

static inline struct perf_evsel *perf_evlist__last(struct perf_evlist *evlist)
{
	return list_entry(evlist->entries.prev, struct perf_evsel, node);
}

u64 perf_evlist__read_format(struct perf_evlist *evlist);

void perf_evlist__id_add(struct perf_evlist *evlist,
			 struct perf_evsel *evsel,
			 int cpu, int thread, u64 id);

int perf_evlist__id_add_fd(struct perf_evlist *evlist,
			   struct perf_evsel *evsel,
			   int cpu, int thread, int fd);

void perf_evlist__reset_id_hash(struct perf_evlist *evlist);

void __perf_evlist__set_leader(struct list_head *list, struct perf_evsel *leader);

void perf_evlist__go_system_wide(struct perf_evlist *evlist, struct perf_evsel *evsel);
#endif /* __LIBPERF_INTERNAL_EVLIST_H */
