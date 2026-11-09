/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_INTERNAL_MMAP_H
#define __LIBPERF_INTERNAL_MMAP_H

#include <linux/compiler.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/list.h>
#include <sys/types.h>
#include <stdbool.h>
#include <perf/mmap.h>

/* perf sample has 16 bits size limit */
#define PERF_SAMPLE_MAX_SIZE (1 << 16)

struct perf_mmap;
struct perf_mmap_domain;
struct perf_counts_values;

/**
 * struct perf_mmap - perf's ring buffer mmap details
 *
 * @refcnt - every holder of the ring buffer: each live domain of the
 *   evlist (see struct perf_mmap_domain), the order heap of perf-prof, and
 *   the reaps (struct perf_evlist_reap). The last perf_mmap__put() unmaps and
 *   frees it.
 *
 * @cpu, @tid - the binding of this ring buffer. Exactly one of them is set:
 *   cpu-bound    : cpu >= 0, tid == -1. Every thread of a cpu-bound evsel
 *                  writes into the ring buffer of the cpu it runs on.
 *   thread-bound : cpu == -1, tid >= 0.
 * Together with @overwrite they form the key under which evsels look up an
 * existing ring buffer to share, see perf_evlist__find_mmap().
 *
 * The VMA keeps the event it was mapped from alive, so no fd has to stay open
 * for the mapping: any writer still attached serves as SET_OUTPUT target.
 */
struct perf_mmap {
	void			*base;
	int			 mask;
	int			 cpu;
	pid_t			 tid;
	refcount_t		 refcnt;
	u64			 prev;
	u64			 start;
	u64			 end;
	bool			 overwrite;
	u64			 flush;
	struct perf_evlist	*evlist;
	void			*event_copy;
	size_t			 event_copy_sz;
	/*
	 * Managed by the evlist. @cpu_domain is the cpu domain of the pid=-1
	 * writers, if any. @poll_domain holds the fd polled for the data of
	 * the ring buffer.
	 */
	struct list_head	 node;         /* perf_evlist::mmap_list / mmap_ovw_list */
	struct hlist_node	 hnode;        /* perf_evlist::mmap_heads */
	struct list_head	 domain_list;  /* perf_mmap_domain::map_node */
	struct perf_mmap_domain	*cpu_domain;
	struct perf_mmap_domain	*poll_domain;  /* registered for EPOLLIN */
	bool			 registered;
	/* The external poll loop has been told about this ring buffer. */
	bool			 announced;
};

struct perf_mmap_param {
	int	prot;
	int	mask;
};

struct perf_mmap *perf_mmap__new(int cpu, pid_t tid, bool overwrite);
int perf_mmap__mmap(struct perf_mmap *map, struct perf_mmap_param *mp, int fd);
size_t perf_mmap__mmap_len(struct perf_mmap *map);

u64 perf_mmap__read_head(struct perf_mmap *map);
bool perf_mmap__empty(struct perf_mmap *map);

int perf_mmap__read_self(struct perf_mmap *map, struct perf_counts_values *count);

#endif /* __LIBPERF_INTERNAL_MMAP_H */
