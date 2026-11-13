/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_EVLIST_H
#define __LIBPERF_INTERNAL_EVLIST_H

#include <linux/list.h>
#include <perf/evlist.h>
#include <internal/evsel.h>
#include <sys/epoll.h>

#define PERF_EVLIST__ID_HLIST_BITS 8
#define PERF_EVLIST__ID_HLIST_SIZE (1 << PERF_EVLIST__ID_HLIST_BITS)

#define PERF_EVLIST__MMAP_HLIST_BITS 6
#define PERF_EVLIST__MMAP_HLIST_SIZE (1 << PERF_EVLIST__MMAP_HLIST_BITS)

#define PERF_EVLIST__THREAD_HLIST_BITS 8
#define PERF_EVLIST__THREAD_HLIST_SIZE (1 << PERF_EVLIST__THREAD_HLIST_BITS)

#define PERF_EVLIST__POLL_HLIST_BITS 8
#define PERF_EVLIST__POLL_HLIST_SIZE (1 << PERF_EVLIST__POLL_HLIST_BITS)

struct perf_cpu_map;
struct perf_thread_map;
struct perf_mmap_param;

struct perf_mmap;
struct perf_evlist_thread;

/*
 * The writers of one ring buffer that go away together: all pid=-1 (and
 * cgroup) writers form the cpu domain, which never sees EPOLLHUP; the writers
 * bound to one task form a task domain, which sees EPOLLHUP on every one of
 * its fds once that task exits. Only a task domain can HUP.
 *
 * At most one fd per domain is registered with epoll, for one of two roles:
 * the data fd of the ring buffer (EPOLLIN; a write wakes up every event
 * attached to it) and the hup fd of the thread, task domains only (EPOLLHUP;
 * every event of an exited task enters EXIT state). See perf_evlist_poll__pick_data().
 *
 * A live domain holds a reference on its ring buffer. Once EPOLLHUP reaped
 * the task domain, perf_evlist__reap_done() marks it dead and drops the
 * reference; the fds stay open until perf_evlist__del_thread() or the evsel
 * closes them, and the domain is freed with the last one.
 *
 * @fds holds the live fds first, [0, @nr_live), then the ones stopped by a
 * removal in progress (perf_evsel_fd::stopped), [@nr_live, @nr_fds). A domain
 * with no live fd goes to the tail of the lists of its ring buffer and of its
 * thread, see perf_mmap_domain__order().
 */
struct perf_mmap_domain {
	struct perf_mmap	*map;          /* NULL once dead */
	pid_t			 tid;          /* -1: cpu domain */
	bool			 dead;
	int			 nr_fds;
	int			 nr_live;
	int			 nr_alloc;
	struct perf_evsel_fd	**fds;
	int			 poll_fd;      /* registered fd, -1 if none */
	unsigned		 poll_events;
	struct perf_evlist_thread *thread;     /* task domain only */
	struct list_head	 map_node;     /* perf_mmap::domain_list */
	struct list_head	 thread_node;  /* perf_evlist_thread::domain_list */
	struct hlist_node	 poll_node;    /* perf_evlist_poll::poll_fd_heads */
};

/*
 * Exit state of one thread, for the ring buffers it writes into: a task, never
 * a cgroup, since only a task can HUP. Created with its first task domain,
 * freed once its removal is done or by perf_evlist__munmap(); not the follow
 * set, see struct perf_evlist_member.
 *
 * EPOLLHUP (@hup) and perf_evlist__del_thread() (@del) may arrive in either
 * order. The first one reaps the ring buffers and sets its flag, the other one
 * is not set then: @del closes the fds on done, @hup keeps them open until
 * perf_evlist__del_thread() closes them.
 */
struct perf_evlist_thread {
	pid_t			 tid;
	bool			 hup;
	bool			 del;
	struct perf_mmap_domain	*hup_domain;   /* registered for its EPOLLHUP */
	struct list_head	 domain_list;  /* perf_mmap_domain::thread_node */
	struct hlist_node	 hnode;        /* perf_evlist::thread_heads */
};

/*
 * One entry of the thread follow set, see perf_evlist::member_list: a thread,
 * any thread (@tid -1), or a cgroup of the thread map
 * (perf_thread_map__pid_cgroup()), as the pid argument of perf_event_open()
 * takes any of them; the set may mix them. perf_evlist__add_thread()/
 * del_thread() add and remove threads only. Only what the evsels
 * following the evlist open on, from perf_evlist__set_maps() on, open or not:
 * no fd, no ring buffer, no exit state - those are in struct
 * perf_evlist_thread.
 */
struct perf_evlist_member {
	union {
		pid_t		 tid;          /* !@cgroup, -1: any thread */
		int		 cgroup_fd;    /* @cgroup */
	};
	bool			 cgroup;
	struct list_head	 node;         /* perf_evlist::member_list */
	struct hlist_node	 hnode;        /* perf_evlist::member_heads */
};

struct perf_evlist_poll {
	int epfd;
	int maxevents;
	struct epoll_event *events;
	void *external;
	perf_evlist_poll_add_t add;
	perf_evlist_poll_del_t del;
	int nr_live;	/* registered fds */
	/* Registered fd -> domain, see perf_evlist_poll__find(). */
	struct hlist_head poll_fd_heads[PERF_EVLIST__POLL_HLIST_SIZE];  /* perf_mmap_domain::poll_node */
};

struct perf_evlist {
	struct list_head	 evsel_list;  /* perf_evsel::node */
	int			 nr_entries;
	bool			 needs_map_propagation;
	/*
	 * The maps set by perf_evlist__set_maps(), only before
	 * perf_evlist__open(), see @opened.
	 *
	 * @user_requested_cpus: the cpus passed from the command line or all
	 * online CPUs by default.
	 * @user_requested_threads: the threads passed from the command line,
	 * or the dummy map for any thread.
	 */
	struct perf_cpu_map	*user_requested_cpus;
	struct perf_thread_map	*user_requested_threads;
	/*
	 * The follow set: the cpus and threads the evsels without their own
	 * maps open on, see perf_evlist__cpus()/threads(). It starts as the
	 * maps above, perf_evlist__add_cpu()/del_cpu()/add_thread()/
	 * del_thread() change it at any stage, EPOLLHUP takes an exited
	 * thread out. It is what is asked for, not what is open: the evsels
	 * hold that in their records.
	 *
	 * Cpus in a bitmap, grown as needed: small and dense, kept in
	 * ascending order for free. Any cpu (-1) is no member: an empty set
	 * stands for it.
	 *
	 * Threads, cgroups and any thread (-1) in a list in the order they
	 * came, the map's first, hashed for the lookup: sparse up to pid_max.
	 * Any thread and cgroups may go together, any thread and threads not:
	 * @nr_tids counts the threads (tid >= 0, not the cgroups), any thread
	 * leaves when it goes above 0. It comes back when it drops to 0 if
	 * @map_any_thread, the map of perf_evlist__set_maps() had any thread
	 * (NULL and an empty map do); and when the last member goes, the set
	 * never empties. See perf_evlist__member_del().
	 *
	 * Whether an open evsel takes a cpu or thread added later is up to
	 * it, see perf_evsel::any_cpu/any_thread.
	 *
	 * @cpus_changed/@threads_changed: a set no longer matches its map;
	 * @cpus_snap/@threads_snap are built from it on demand, dropped with
	 * the next change.
	 */
	unsigned long		*cpu_bits;
	int			 nr_cpu_bits;
	bool			 cpus_changed;
	struct perf_cpu_map	*cpus_snap;
	struct list_head	 member_list;  /* perf_evlist_member::node */
	struct hlist_head	 member_heads[PERF_EVLIST__THREAD_HLIST_SIZE];  /* perf_evlist_member::hnode */
	int			 nr_members;
	int			 nr_tids;
	bool			 map_any_thread;
	bool			 threads_changed;
	struct perf_thread_map	*threads_snap;
	/* perf_evlist__open() until perf_evlist__close(): the maps are fixed. */
	bool			 opened;
	int			 rl_file;
	int			 nr_mmaps;
	size_t			 mmap_len;
	struct perf_evlist_poll epoll;
	/* Sample id -> evsel, see perf_evlist__id_to_evsel(). */
	struct hlist_head	 id_heads[PERF_EVLIST__ID_HLIST_SIZE];  /* perf_sample_id::node */
	/*
	 * Ring buffers are allocated on demand and shared by every evsel with
	 * the same (cpu, tid, overwrite) binding. The lists are for
	 * perf_evlist__for_each_mmap(); @mmap_heads holds the ones new writers
	 * may join, for perf_evlist__find_mmap().
	 */
	struct list_head	 mmap_list;      /* perf_mmap::node */
	struct list_head	 mmap_ovw_list;  /* perf_mmap::node */
	struct hlist_head	 mmap_heads[PERF_EVLIST__MMAP_HLIST_SIZE];      /* perf_mmap::hnode */
	struct hlist_head	 thread_heads[PERF_EVLIST__THREAD_HLIST_SIZE];  /* perf_evlist_thread::hnode */
	bool			 mmaped;
	/* perf_evlist__munmap() in progress: poll fds are not registered again. */
	bool			 munmapping;
	bool			 grouped;
	/* perf_evlist__add_thread()/add_cpu(): it attaches and enables the new fds. */
	bool			 adding;
};

void perf_evlist_poll__init(struct perf_evlist *evlist);
void perf_evlist__init(struct perf_evlist *evlist);
void perf_evlist__exit(struct perf_evlist *evlist);

/*
 * Attach one open fd of @evsel to the ring buffer of its binding, creating the
 * ring buffer if this is the first fd to use it; detach it again before the fd
 * is closed. Used by perf_evlist__mmap() and by the dynamic thread path.
 */
int perf_evlist__mmap_attach_fd(struct perf_evlist *evlist,
				struct perf_evsel_fd *entry);
void perf_evlist__mmap_detach_fd(struct perf_evlist *evlist,
				 struct perf_evsel_fd *entry);

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
	__perf_evlist__for_each_entry(&(evlist)->evsel_list, evsel)

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
	__perf_evlist__for_each_entry_reverse(&(evlist)->evsel_list, evsel)

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
	__perf_evlist__for_each_entry_safe(&(evlist)->evsel_list, tmp, evsel)

static inline struct perf_evsel *perf_evlist__first(struct perf_evlist *evlist)
{
	return list_entry(evlist->evsel_list.next, struct perf_evsel, node);
}

static inline struct perf_evsel *perf_evlist__last(struct perf_evlist *evlist)
{
	return list_entry(evlist->evsel_list.prev, struct perf_evsel, node);
}

u64 perf_evlist__read_format(struct perf_evlist *evlist);

void perf_evlist__id_add(struct perf_evlist *evlist,
			 struct perf_evsel_fd *entry, u64 id);
void perf_evlist__id_del(struct perf_evsel_fd *entry);

int perf_evlist__id_add_fd(struct perf_evlist *evlist,
			   struct perf_evsel_fd *entry);

void perf_evlist__reset_id_hash(struct perf_evlist *evlist);

void perf_evlist__go_system_wide(struct perf_evlist *evlist, struct perf_evsel *evsel);
#endif /* __LIBPERF_INTERNAL_EVLIST_H */
