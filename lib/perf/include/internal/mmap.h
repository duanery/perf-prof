/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_MMAP_H
#define __LIBPERF_INTERNAL_MMAP_H

#include <linux/compiler.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/list.h>
#include <sys/types.h>
#include <stdbool.h>

/* perf sample has 16 bits size limit */
#define PERF_SAMPLE_MAX_SIZE (1 << 16)

struct perf_mmap;
struct perf_counts_values;

typedef void (*libperf_unmap_cb_t)(struct perf_mmap *map);

/**
 * struct perf_mmap - perf's ring buffer mmap details
 *
 * @refcnt - e.g. code using PERF_EVENT_IOC_SET_OUTPUT to share this
 *
 * @cpu, @tid - the binding of this ring buffer. Exactly one of them is set:
 *   cpu-bound    : cpu >= 0, tid == -1. Every thread of a cpu-bound evsel
 *                  writes into the ring buffer of the cpu it runs on.
 *   thread-bound : cpu == -1, tid >= 0.
 * Together with @overwrite they form the key under which evsels look up an
 * existing ring buffer to share, see perf_evlist__find_mmap().
 */
struct perf_mmap {
	void			*base;
	int			 mask;
	int			 fd;
	int			 cpu;
	pid_t			 tid;
	int			 idx;
	refcount_t		 refcnt;
	u64			 prev;
	u64			 start;
	u64			 end;
	bool			 overwrite;
	u64			 flush;
	libperf_unmap_cb_t	 unmap_cb;
	struct perf_evlist	*evlist;
	void			*event_copy;
	size_t			 event_copy_sz;
	struct perf_mmap	*next;
	/* Linked into evlist->mmap_list / mmap_ovw_list, allocated on demand. */
	struct list_head	 list;
	struct hlist_node	 hnode;
	bool			 dynamic;
	/*
	 * @nr_fds counts the fds writing into this ring buffer. @extra_ref is
	 * the reference held on top of them so that perf_mmap__consume() can
	 * drain the last events once they are all gone; it is dropped by
	 * whoever notices first, the drain or the removal of the last fd.
	 */
	int			 nr_fds;
	bool			 extra_ref;
};

struct perf_mmap_param {
	int	prot;
	int	mask;
};

size_t perf_mmap__mmap_len(struct perf_mmap *map);

void perf_mmap__init(struct perf_mmap *map, struct perf_mmap *prev,
		     bool overwrite, libperf_unmap_cb_t unmap_cb);
void perf_mmap__set_bind(struct perf_mmap *map, int cpu, pid_t tid);
int perf_mmap__mmap(struct perf_mmap *map, struct perf_mmap_param *mp,
		    int fd, int cpu);
void perf_mmap__munmap(struct perf_mmap *map);
void perf_mmap__get(struct perf_mmap *map);
void perf_mmap__put(struct perf_mmap *map);

u64 perf_mmap__read_head(struct perf_mmap *map);
bool perf_mmap__empty(struct perf_mmap *map);

int perf_mmap__read_self(struct perf_mmap *map, struct perf_counts_values *count);

#endif /* __LIBPERF_INTERNAL_MMAP_H */
