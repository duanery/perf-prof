// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <perf/evsel.h>
#include <perf/cpumap.h>
#include <perf/threadmap.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <internal/evsel.h>
#include <internal/evlist.h>
#include <linux/zalloc.h>
#include <stdlib.h>
#include <string.h>
#include <internal/xyarray.h>
#include <internal/cpumap.h>
#include <internal/mmap.h>
#include <internal/threadmap.h>
#include <internal/lib.h>
#include <linux/string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <asm/bug.h>

void perf_evsel__init(struct perf_evsel *evsel, struct perf_event_attr *attr,
		      int idx)
{
	INIT_LIST_HEAD(&evsel->node);
	evsel->attr = *attr;
	evsel->idx  = idx;
	evsel->leader = evsel;
	evsel->init_enabled = !attr->disabled;
	evsel->enabled = !attr->disabled;
	evsel->bpf_prog_fd = -1;
}

struct perf_evsel *perf_evsel__new(struct perf_event_attr *attr)
{
	struct perf_evsel *evsel = zalloc(sizeof(*evsel));

	if (evsel != NULL)
		perf_evsel__init(evsel, attr, 0);

	return evsel;
}

void perf_evsel__delete(struct perf_evsel *evsel)
{
	if (!evsel)
		return;

	perf_cpu_map__put(evsel->own_cpus);
	perf_cpu_map__put(evsel->cpus);
	perf_thread_map__put(evsel->own_threads);
	perf_thread_map__put(evsel->threads);
	free(evsel->filter);
	free(evsel);
}

bool perf_evsel__oncpu(struct perf_evsel *evsel)
{
	return !perf_cpu_map__empty(evsel->cpus);
}

void perf_evsel__mmap_bind(struct perf_evsel *evsel, int cpu, int thread,
			   int *bind_cpu, pid_t *bind_tid)
{
	if (perf_evsel__oncpu(evsel)) {
		*bind_cpu = perf_cpu_map__cpu(evsel->cpus, cpu);
		*bind_tid = -1;
	} else {
		*bind_cpu = -1;
		*bind_tid = perf_thread_map__pid(evsel->threads, thread);
	}
}

#define FD(e, x, y) ((int *) xyarray__entry(e->fd, x, y))
#define MMAP(e, x, y) (e->mmap ? ((struct perf_mmap *) xyarray__entry(e->mmap, x, y)) : NULL)

int perf_evsel__alloc_fd(struct perf_evsel *evsel, int ncpus, int nthreads)
{
	evsel->fd = xyarray__new(ncpus, nthreads, sizeof(int));

	if (evsel->fd) {
		int cpu, thread;
		for (cpu = 0; cpu < ncpus; cpu++) {
			for (thread = 0; thread < nthreads; thread++) {
				int *fd = FD(evsel, cpu, thread);

				if (fd)
					*fd = -1;
			}
		}
	}

	return evsel->fd != NULL ? 0 : -ENOMEM;
}

static int perf_evsel__alloc_mmap(struct perf_evsel *evsel, int ncpus, int nthreads)
{
	evsel->mmap = xyarray__new(ncpus, nthreads, sizeof(struct perf_mmap));

	return evsel->mmap != NULL ? 0 : -ENOMEM;
}

static int
sys_perf_event_open(struct perf_event_attr *attr,
		    pid_t pid, int cpu, int group_fd,
		    unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int get_group_fd(struct perf_evsel *evsel, int cpu, int thread, int *group_fd)
{
	struct perf_evsel *leader = evsel->leader;
	int *fd;

	if (evsel == leader) {
		*group_fd = -1;
		return 0;
	}

	/*
	 * Leader must be already processed/open,
	 * if not it's a bug.
	 */
	if (!leader->fd)
		return -ENOTCONN;

	/*
	 * A group can only be formed out of events on the same (cpu, thread).
	 * The leader may have been bound to a different map though - see
	 * perf_evsel__set_own_cpus()/perf_evsel__set_own_threads() - so convert
	 * both indices into the leader's maps.
	 */
	if (evsel->cpus != leader->cpus) {
		int map_cpu = perf_cpu_map__cpu(evsel->cpus, cpu);
		cpu = perf_cpu_map__idx(leader->cpus, map_cpu);
		if (cpu < 0)
			return -EBADF;
	}

	if (evsel->threads != leader->threads) {
		pid_t pid = perf_thread_map__pid(evsel->threads, thread);
		thread = perf_thread_map__idx(leader->threads, pid);
		if (thread < 0)
			return -EBADF;
	}

	fd = FD(leader, cpu, thread);
	if (fd == NULL || *fd == -1)
		return -EBADF;

	*group_fd = *fd;

	return 0;
}

int perf_evsel__open_one(struct perf_evsel *evsel, int cpu, int thread)
{
	unsigned long flags = PERF_FLAG_FD_CLOEXEC;
	struct perf_thread_map *threads = evsel->threads;
	struct perf_cpu_map *cpus = evsel->cpus;
	int fd, group_fd, *evsel_fd, err;

	evsel_fd = FD(evsel, cpu, thread);
	if (evsel_fd == NULL)
		return -EINVAL;
	if (*evsel_fd >= 0)
		return 0;
	if (!perf_thread_map__valid(threads, thread))
		return 0;

	err = get_group_fd(evsel, cpu, thread, &group_fd);
	if (err < 0)
		return err;

	if (threads->map[thread].cgroup)
		flags |= PERF_FLAG_PID_CGROUP;

	fd = sys_perf_event_open(&evsel->attr, threads->map[thread].pid,
				 cpus->map[cpu], group_fd, flags);
	if (fd < 0) {
		if (errno == EINVAL) {
			flags &= ~PERF_FLAG_FD_CLOEXEC;
			fd = sys_perf_event_open(&evsel->attr,
						 threads->map[thread].pid,
						 cpus->map[cpu], group_fd, flags);
		}
		if (fd < 0)
			return -errno;
	}

	*evsel_fd = fd;
	return 0;
}

int perf_evsel__open(struct perf_evsel *evsel, struct perf_cpu_map *cpus,
		     struct perf_thread_map *threads)
{
	int cpu, thread, err = 0;

	if (cpus == NULL) {
		static struct perf_cpu_map *empty_cpu_map;

		if (empty_cpu_map == NULL) {
			empty_cpu_map = perf_cpu_map__dummy_new();
			if (empty_cpu_map == NULL)
				return -ENOMEM;
		}

		cpus = empty_cpu_map;
	}

	if (threads == NULL) {
		static struct perf_thread_map *empty_thread_map;

		if (empty_thread_map == NULL) {
			empty_thread_map = perf_thread_map__new_dummy();
			if (empty_thread_map == NULL)
				return -ENOMEM;
		}

		threads = empty_thread_map;
	}

	/*
	 * Freeze the binding on the evsel: everything below - fds, ring
	 * buffers, dynamically added threads - is laid out along these maps.
	 */
	if (evsel->cpus != cpus) {
		perf_cpu_map__put(evsel->cpus);
		evsel->cpus = perf_cpu_map__get(cpus);
	}
	if (evsel->threads != threads) {
		perf_thread_map__put(evsel->threads);
		evsel->threads = perf_thread_map__get(threads);
	}
	evsel->cpus_bound = true;

	if (evsel->fd == NULL &&
	    perf_evsel__alloc_fd(evsel, cpus->nr, threads->nr) < 0)
		return -ENOMEM;

	if (evsel->leader != evsel) {
		evsel->attr.disabled = evsel->leader->attr.disabled;
		evsel->init_enabled = 0;
	}
	evsel->enabled = !evsel->attr.disabled;

	for (cpu = 0; cpu < cpus->nr; cpu++) {
		for (thread = 0; thread < threads->nr; thread++) {
			err = perf_evsel__open_one(evsel, cpu, thread);
			if (err < 0)
				return err;
		}
	}

	return 0;
}

static void perf_evsel__close_fd_cpu(struct perf_evsel *evsel, int cpu)
{
	int thread;

	for (thread = 0; thread < xyarray__max_y(evsel->fd); ++thread) {
		int *fd = FD(evsel, cpu, thread);

		if (fd && *fd >= 0) {
			close(*fd);
			*fd = -1;
		}
	}
}

void perf_evsel__close_fd(struct perf_evsel *evsel)
{
	int cpu;

	for (cpu = 0; cpu < xyarray__max_x(evsel->fd); cpu++)
		perf_evsel__close_fd_cpu(evsel, cpu);
}

void perf_evsel__free_fd(struct perf_evsel *evsel)
{
	xyarray__delete(evsel->fd);
	evsel->fd = NULL;
}

void perf_evsel__close(struct perf_evsel *evsel)
{
	if (evsel->fd == NULL)
		return;

	perf_evsel__close_fd(evsel);
	perf_evsel__free_fd(evsel);
}

void perf_evsel__close_cpu(struct perf_evsel *evsel, int cpu)
{
	if (evsel->fd == NULL)
		return;

	perf_evsel__close_fd_cpu(evsel, cpu);
}

void perf_evsel__munmap(struct perf_evsel *evsel)
{
	int cpu, thread;

	if (evsel->fd == NULL || evsel->mmap == NULL)
		return;

	for (cpu = 0; cpu < xyarray__max_x(evsel->fd); cpu++) {
		for (thread = 0; thread < xyarray__max_y(evsel->fd); thread++) {
			int *fd = FD(evsel, cpu, thread);

			if (fd == NULL || *fd < 0)
				continue;

			perf_mmap__munmap(MMAP(evsel, cpu, thread));
		}
	}

	xyarray__delete(evsel->mmap);
	evsel->mmap = NULL;
}

int perf_evsel__mmap(struct perf_evsel *evsel, int pages)
{
	int ret, cpu, thread;
	struct perf_mmap_param mp = {
		.prot = PROT_READ | PROT_WRITE,
		.mask = (pages * page_size) - 1,
	};

	if (evsel->fd == NULL || evsel->mmap)
		return -EINVAL;

	if (perf_evsel__alloc_mmap(evsel, xyarray__max_x(evsel->fd), xyarray__max_y(evsel->fd)) < 0)
		return -ENOMEM;

	for (cpu = 0; cpu < xyarray__max_x(evsel->fd); cpu++) {
		for (thread = 0; thread < xyarray__max_y(evsel->fd); thread++) {
			int *fd = FD(evsel, cpu, thread);
			struct perf_mmap *map;

			if (fd == NULL || *fd < 0)
				continue;

			map = MMAP(evsel, cpu, thread);
			perf_mmap__init(map, NULL, false, NULL);

			ret = perf_mmap__mmap(map, &mp, *fd, cpu);
			if (ret) {
				perf_evsel__munmap(evsel);
				return ret;
			}
		}
	}

	return 0;
}

void *perf_evsel__mmap_base(struct perf_evsel *evsel, int cpu, int thread)
{
	int *fd = FD(evsel, cpu, thread);

	if (fd == NULL || *fd < 0 || MMAP(evsel, cpu, thread) == NULL)
		return NULL;

	return MMAP(evsel, cpu, thread)->base;
}

int perf_evsel__read_size(struct perf_evsel *evsel)
{
	u64 read_format = evsel->attr.read_format;
	int entry = sizeof(u64); /* value */
	int size = 0;
	int nr = 1;

	if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
		size += sizeof(u64);

	if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
		size += sizeof(u64);

	if (read_format & PERF_FORMAT_ID)
		entry += sizeof(u64);

	if (read_format & PERF_FORMAT_LOST)
		entry += sizeof(u64);

	if (read_format & PERF_FORMAT_GROUP) {
		nr = evsel->nr_members;
		size += sizeof(u64);
	}

	size += entry * nr;
	return size;
}

int perf_evsel__read(struct perf_evsel *evsel, int cpu, int thread,
		     struct perf_counts_values *count)
{
	size_t size = perf_evsel__read_size(evsel);
	u64 read_format = evsel->attr.read_format;
	int *fd = FD(evsel, cpu, thread);

	memset(count, 0, sizeof(*count));

	if (fd == NULL || *fd < 0)
		return -EINVAL;

	if (MMAP(evsel, cpu, thread) &&
	    !(read_format & (PERF_FORMAT_ID | PERF_FORMAT_LOST)) &&
	    !perf_mmap__read_self(MMAP(evsel, cpu, thread), count))
		return 0;

	if (readn(*fd, count->values, size) <= 0)
		return -errno;

	return 0;
}

static int perf_evsel__run_ioctl(struct perf_evsel *evsel,
				 int ioc,  void *arg,
				 int cpu)
{
	int thread;

	for (thread = 0; thread < xyarray__max_y(evsel->fd); thread++) {
		int err;
		int *fd = FD(evsel, cpu, thread);

		/* Slots left behind by perf_evsel__del_thread() have no fd. */
		if (fd == NULL || *fd < 0)
			continue;

		err = ioctl(*fd, ioc, arg);

		if (err)
			return err;
	}

	return 0;
}

int perf_evsel__enable_cpu(struct perf_evsel *evsel, int cpu)
{
	return perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_ENABLE, NULL, cpu);
}

int perf_evsel__enable(struct perf_evsel *evsel)
{
	int i;
	int err = 0;

	if (evsel->keep_disable) return 0;

	evsel->enabled = 1;
	if (evsel->init_enabled) {
		evsel->init_enabled = 0;
		return 0;
	}

	for (i = 0; i < xyarray__max_x(evsel->fd) && !err; i++)
		err = perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_ENABLE, NULL, i);
	return err;
}

int perf_evsel__enable_group(struct perf_evsel *evsel)
{
	int i;
	int err = 0;

	if (evsel->keep_disable) return 0;

	/* Members are enabled by PERF_IOC_FLAG_GROUP on the leader. */
	evsel->enabled = 1;
	if (evsel->leader != evsel) return 0;

	if (evsel->init_enabled) {
		evsel->init_enabled = 0;
		return 0;
	}

	for (i = 0; i < xyarray__max_x(evsel->fd) && !err; i++)
		err = perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_ENABLE, (void *)PERF_IOC_FLAG_GROUP, i);
	return err;
}


int perf_evsel__disable_cpu(struct perf_evsel *evsel, int cpu)
{
	return perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_DISABLE, NULL, cpu);
}

int perf_evsel__disable(struct perf_evsel *evsel)
{
	int i;
	int err = 0;

	if (evsel->keep_disable) return 0;

	evsel->init_enabled = 0;
	evsel->enabled = 0;
	for (i = 0; i < xyarray__max_x(evsel->fd) && !err; i++)
		err = perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_DISABLE, NULL, i);
	return err;
}

int perf_evsel__disable_group(struct perf_evsel *evsel)
{
	int i;
	int err = 0;

	if (evsel->keep_disable) return 0;

	evsel->init_enabled = 0;
	evsel->enabled = 0;
	if (evsel->leader != evsel) return 0;

	for (i = 0; i < xyarray__max_x(evsel->fd) && !err; i++)
		err = perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_DISABLE, (void *)PERF_IOC_FLAG_GROUP, i);
	return err;
}

void perf_evsel__keep_disable(struct perf_evsel *evsel, bool keep_disable)
{
	if (keep_disable && !evsel->attr.disabled) {
		evsel->attr.disabled = 1;
		evsel->init_enabled = 0;
	}
	evsel->keep_disable = keep_disable;
}

int perf_evsel__apply_filter_cpu(struct perf_evsel *evsel, const char *filter, int cpu)
{
	return perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_SET_FILTER, (void *)filter, cpu);
}

int perf_evsel__apply_filter(struct perf_evsel *evsel, const char *filter)
{
	int err = 0, i;

	for (i = 0; i < evsel->cpus->nr && !err; i++)
		err = perf_evsel__run_ioctl(evsel,
				     PERF_EVENT_IOC_SET_FILTER,
				     (void *)filter, i);
	if (!err) {
		/* Remembered so perf_evsel__add_thread() can repeat it. */
		char *dup = filter ? strdup(filter) : NULL;

		if (!filter || dup) {
			free(evsel->filter);
			evsel->filter = dup;
		}
	}
	return err;
}

int perf_evsel__set_bpf(struct perf_evsel *evsel, unsigned int prog_fd)
{
	int err = 0, i;

	for (i = 0; i < evsel->cpus->nr && !err; i++)
		err = perf_evsel__run_ioctl(evsel,
				     PERF_EVENT_IOC_SET_BPF,
				     (void *)(unsigned long)prog_fd, i);
	if (!err)
		evsel->bpf_prog_fd = prog_fd;
	return err;
}

void perf_evsel__set_own_cpus(struct perf_evsel *evsel, struct perf_cpu_map *own_cpus)
{
	perf_cpu_map__put(evsel->own_cpus);
	evsel->own_cpus = perf_cpu_map__get(own_cpus);
}

void perf_evsel__set_own_threads(struct perf_evsel *evsel, struct perf_thread_map *own_threads)
{
	perf_thread_map__put(evsel->own_threads);
	evsel->own_threads = perf_thread_map__get(own_threads);
	evsel->threads_cow = false;

	if (own_threads) {
		perf_thread_map__put(evsel->threads);
		evsel->threads = perf_thread_map__get(own_threads);
	}
}

/*
 * Adding or removing a thread only affects this evsel, so it needs a thread map
 * of its own before the first change. Copying instead of growing the shared map
 * in place also keeps every other holder of it - including prof_dev->threads on
 * the perf-prof side - pointing at valid memory.
 */
int perf_evsel__make_threads_private(struct perf_evsel *evsel)
{
	struct perf_thread_map *threads;

	if (evsel->own_threads && evsel->threads == evsel->own_threads)
		return 0;

	threads = perf_thread_map__dup(evsel->threads);
	if (!threads)
		return -ENOMEM;

	perf_thread_map__put(evsel->own_threads);
	perf_thread_map__put(evsel->threads);
	evsel->own_threads = threads;
	evsel->threads = perf_thread_map__get(threads);
	evsel->threads_cow = true;

	return 0;
}

#define SAMPLE_ID(e, x, y) \
	((struct perf_sample_id *)xyarray__entry(e->sample_id, x, y))

static void perf_evsel__relink_ids(struct perf_evsel *evsel, bool link)
{
	struct perf_evlist *evlist = evsel->evlist;
	int cpu, thread;

	if (!evsel->sample_id || !evlist)
		return;

	for (cpu = 0; cpu < xyarray__max_x(evsel->sample_id); cpu++) {
		for (thread = 0; thread < xyarray__max_y(evsel->sample_id); thread++) {
			struct perf_sample_id *sid = SAMPLE_ID(evsel, cpu, thread);

			if (!sid || !sid->evsel)
				continue;
			if (link)
				hlist_add_head(&sid->node,
					&evlist->heads[hash_64(sid->id, PERF_EVLIST__HLIST_BITS)]);
			else
				hlist_del(&sid->node);
		}
	}
}

/*
 * Grow the thread dimension of the per-fd arrays. struct perf_sample_id is
 * linked into evlist->heads[], so unlink before the realloc moves it and link
 * it back afterwards.
 */
static int perf_evsel__grow_threads(struct perf_evsel *evsel, int nthreads)
{
	int old_y, ncpus, cpu, thread;
	struct xyarray *xy;
	size_t new_n;
	u64 *id;

	if (evsel->fd && nthreads > xyarray__max_y(evsel->fd)) {
		old_y = xyarray__max_y(evsel->fd);

		xy = xyarray__grow_y(evsel->fd, nthreads);
		if (!xy)
			return -ENOMEM;
		evsel->fd = xy;

		for (cpu = 0; cpu < xyarray__max_x(xy); cpu++)
			for (thread = old_y; thread < nthreads; thread++)
				*(int *)xyarray__entry(xy, cpu, thread) = -1;
	}

	if (evsel->mmap && nthreads > xyarray__max_y(evsel->mmap)) {
		xy = xyarray__grow_y(evsel->mmap, nthreads);
		if (!xy)
			return -ENOMEM;
		evsel->mmap = xy;
	}

	if (evsel->sample_id && nthreads > xyarray__max_y(evsel->sample_id)) {
		perf_evsel__relink_ids(evsel, false);

		xy = xyarray__grow_y(evsel->sample_id, nthreads);
		if (!xy) {
			perf_evsel__relink_ids(evsel, true);
			return -ENOMEM;
		}
		evsel->sample_id = xy;

		perf_evsel__relink_ids(evsel, true);

		ncpus = xyarray__max_x(xy);
		if (evsel->id) {
			new_n = (size_t)ncpus * nthreads;
			id = realloc(evsel->id, new_n * sizeof(u64));
			if (!id)
				return -ENOMEM;
			if (new_n > evsel->ids)
				memset(id + evsel->ids, 0,
				       (new_n - evsel->ids) * sizeof(u64));
			evsel->id = id;
		}
	}

	return 0;
}

static int perf_evsel__thread_slot(struct perf_evsel *evsel)
{
	struct perf_thread_map *threads = evsel->threads;
	int thread, nr;

	for (thread = 0; thread < threads->nr; thread++)
		if (threads->map[thread].pid == PERF_THREAD_MAP_HOLE)
			return thread;

	nr = threads->nr + 1;
	threads = perf_thread_map__realloc(threads, nr);
	if (!threads)
		return -ENOMEM;

	threads->nr = nr;
	evsel->own_threads = evsel->threads = threads;
	/* perf_thread_map__realloc() zeroes the new slot; pid 0 means "self". */
	threads->map[nr - 1].pid = PERF_THREAD_MAP_HOLE;

	if (perf_evsel__grow_threads(evsel, nr) < 0)
		return -ENOMEM;

	return nr - 1;
}

static void perf_evsel__close_thread(struct perf_evsel *evsel, int thread)
{
	int cpu;

	for (cpu = 0; cpu < xyarray__max_x(evsel->fd); cpu++) {
		int *fd = FD(evsel, cpu, thread);

		if (evsel->evlist)
			perf_evlist__unmap_evsel_fd(evsel->evlist, evsel, cpu, thread);

		if (fd && *fd >= 0) {
			close(*fd);
			*fd = -1;
		}
	}
}

int perf_evsel__add_thread(struct perf_evsel *evsel, pid_t pid)
{
	struct perf_thread_map *threads;
	int thread, cpu, nr_cpus, err;

	if (pid < 0 || !evsel->fd)
		return -EINVAL;

	err = perf_evsel__make_threads_private(evsel);
	if (err)
		return err;

	threads = evsel->threads;

	/* The dummy map means "any thread"; naming one would double count. */
	if (threads->nr == 1 && threads->map[0].pid == -1)
		return -EINVAL;

	if (perf_thread_map__idx(threads, pid) >= 0)
		return 0;

	thread = perf_evsel__thread_slot(evsel);
	if (thread < 0)
		return thread;

	/* perf_evsel__thread_slot() may have reallocated the map. */
	threads = evsel->threads;
	threads->map[thread].pid = pid;
	threads->map[thread].cgroup = 0;

	nr_cpus = perf_cpu_map__nr(evsel->cpus);
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		err = perf_evsel__open_one(evsel, cpu, thread);
		if (err < 0)
			goto out_close;
	}

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		int *fd = FD(evsel, cpu, thread);

		if (!fd || *fd < 0)
			continue;

		if (evsel->filter &&
		    ioctl(*fd, PERF_EVENT_IOC_SET_FILTER, evsel->filter) < 0) {
			err = -errno;
			goto out_close;
		}
		if (evsel->bpf_prog_fd >= 0 &&
		    ioctl(*fd, PERF_EVENT_IOC_SET_BPF, evsel->bpf_prog_fd) < 0) {
			err = -errno;
			goto out_close;
		}
	}

	if (evsel->evlist) {
		for (cpu = 0; cpu < nr_cpus; cpu++) {
			err = perf_evlist__mmap_evsel_fd(evsel->evlist, evsel, cpu, thread);
			if (err < 0)
				goto out_close;
		}
	}

	if (evsel->enabled && !evsel->keep_disable) {
		for (cpu = 0; cpu < nr_cpus; cpu++) {
			int *fd = FD(evsel, cpu, thread);

			if (fd && *fd >= 0)
				ioctl(*fd, PERF_EVENT_IOC_ENABLE, 0);
		}
	}

	return 0;

out_close:
	perf_evsel__close_thread(evsel, thread);
	threads->map[thread].pid = PERF_THREAD_MAP_HOLE;
	return err;
}

int perf_evsel__del_thread(struct perf_evsel *evsel, pid_t pid)
{
	struct perf_thread_map *threads;
	int thread, err;

	if (pid < 0 || !evsel->fd)
		return -EINVAL;

	if (perf_thread_map__idx(evsel->threads, pid) < 0)
		return -ENOENT;

	err = perf_evsel__make_threads_private(evsel);
	if (err)
		return err;

	threads = evsel->threads;
	thread = perf_thread_map__idx(threads, pid);
	if (thread < 0)
		return -ENOENT;

	perf_evsel__close_thread(evsel, thread);

	zfree(&threads->map[thread].comm);
	threads->map[thread].cgroup = 0;
	threads->map[thread].pid = PERF_THREAD_MAP_HOLE;

	return 0;
}

struct perf_cpu_map *perf_evsel__cpus(struct perf_evsel *evsel)
{
	return evsel->cpus;
}

struct perf_thread_map *perf_evsel__threads(struct perf_evsel *evsel)
{
	return evsel->threads;
}

struct perf_event_attr *perf_evsel__attr(struct perf_evsel *evsel)
{
	return &evsel->attr;
}

int perf_evsel__alloc_id(struct perf_evsel *evsel, int ncpus, int nthreads)
{
	if (ncpus == 0 || nthreads == 0)
		return 0;

	if (evsel->system_wide)
		nthreads = 1;

	evsel->sample_id = xyarray__new(ncpus, nthreads, sizeof(struct perf_sample_id));
	if (evsel->sample_id == NULL)
		return -ENOMEM;

	evsel->id = zalloc(ncpus * nthreads * sizeof(u64));
	if (evsel->id == NULL) {
		xyarray__delete(evsel->sample_id);
		evsel->sample_id = NULL;
		return -ENOMEM;
	}

	return 0;
}

#define SID(e, x, y) (e->sample_id ? xyarray__entry(e->sample_id, x, y) : NULL)
uint64_t perf_evsel__get_id(struct perf_evsel *evsel, int cpu, int thread)
{
	struct perf_sample_id *sid;

	if (evsel->system_wide)
		thread = 0;

	sid = SID(evsel, cpu, thread);
	return sid ? sid->id : 0;
}

void perf_evsel__free_id(struct perf_evsel *evsel)
{
	xyarray__delete(evsel->sample_id);
	evsel->sample_id = NULL;
	zfree(&evsel->id);
	evsel->ids = 0;
}
