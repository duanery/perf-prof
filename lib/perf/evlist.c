// SPDX-License-Identifier: GPL-2.0
#include <perf/evlist.h>
#include <perf/evsel.h>
#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <internal/evlist.h>
#include <internal/evsel.h>
#include <internal/xyarray.h>
#include <internal/mmap.h>
#include <internal/cpumap.h>
#include <internal/threadmap.h>
#include <internal/lib.h>
#include <linux/zalloc.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <perf/cpumap.h>
#include <perf/threadmap.h>

void perf_evlist__init(struct perf_evlist *evlist)
{
	int i;

	INIT_LIST_HEAD(&evlist->entries);
	evlist->nr_entries = 0;
	INIT_LIST_HEAD(&evlist->mmap_list);
	INIT_LIST_HEAD(&evlist->mmap_ovw_list);
	for (i = 0; i < PERF_EVLIST__MMAP_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(&evlist->mmap_heads[i]);
	perf_evlist_poll__init(evlist);
	perf_evlist__reset_id_hash(evlist);
}

/*
 * The evlist maps are the default. An evsel that asked for its own cpu or
 * thread map keeps it; the two are independent, an evsel can bind its own cpus
 * and still follow the evlist threads.
 *
 * Once the evsel is open its cpu binding is frozen: fds and ring buffers are
 * already laid out along it.
 */
static void __perf_evlist__propagate_maps(struct perf_evlist *evlist,
					  struct perf_evsel *evsel)
{
	if (!evsel->cpus_bound) {
		struct perf_cpu_map *cpus;

		if (evsel->system_wide)
			cpus = perf_cpu_map__new(NULL);
		else if (evsel->own_cpus)
			cpus = perf_cpu_map__get(evsel->own_cpus);
		else
			cpus = perf_cpu_map__get(evlist->user_requested_cpus);

		perf_cpu_map__put(evsel->cpus);
		evsel->cpus = cpus;
	}

	/* A map the evsel owns is never overwritten, it may have grown. */
	if (!evsel->own_threads || evsel->threads != evsel->own_threads) {
		struct perf_thread_map *threads;

		if (evsel->system_wide)
			threads = perf_thread_map__new_dummy();
		else if (evsel->own_threads)
			threads = perf_thread_map__get(evsel->own_threads);
		else
			threads = perf_thread_map__get(evlist->threads);

		perf_thread_map__put(evsel->threads);
		evsel->threads = threads;
	}

	evlist->all_cpus = perf_cpu_map__merge(evlist->all_cpus, evsel->cpus);
}

static void perf_evlist__propagate_maps(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel, *n;

	evlist->needs_map_propagation = true;

	/* Clear the all_cpus set which will be merged into during propagation. */
	perf_cpu_map__put(evlist->all_cpus);
	evlist->all_cpus = NULL;

	list_for_each_entry_safe(evsel, n, &evlist->entries, node)
		__perf_evlist__propagate_maps(evlist, evsel);
}

void perf_evlist__add(struct perf_evlist *evlist,
		      struct perf_evsel *evsel)
{
	evsel->idx = evlist->nr_entries;
	evsel->evlist = evlist;
	list_add_tail(&evsel->node, &evlist->entries);
	evlist->nr_entries += 1;

	if (evlist->needs_map_propagation)
		__perf_evlist__propagate_maps(evlist, evsel);
}

void perf_evlist__remove(struct perf_evlist *evlist,
			 struct perf_evsel *evsel)
{
	list_del_init(&evsel->node);
	evsel->evlist = NULL;
	evlist->nr_entries -= 1;
}

struct perf_evlist *perf_evlist__new(void)
{
	struct perf_evlist *evlist = zalloc(sizeof(*evlist));

	if (evlist != NULL)
		perf_evlist__init(evlist);

	return evlist;
}

struct perf_evsel *
perf_evlist__next(struct perf_evlist *evlist, struct perf_evsel *prev)
{
	struct perf_evsel *next;

	if (!prev) {
		next = list_first_entry(&evlist->entries,
					struct perf_evsel,
					node);
	} else {
		next = list_next_entry(prev, node);
	}

	/* Empty list is noticed here so don't need checking on entry. */
	if (&next->node == &evlist->entries)
		return NULL;

	return next;
}

static void perf_evlist__purge(struct perf_evlist *evlist)
{
	struct perf_evsel *pos, *n;

	perf_evlist__for_each_entry_safe(evlist, n, pos) {
		list_del_init(&pos->node);
		perf_evsel__delete(pos);
	}

	evlist->nr_entries = 0;
}

void perf_evlist__exit(struct perf_evlist *evlist)
{
	perf_cpu_map__put(evlist->user_requested_cpus);
	perf_cpu_map__put(evlist->all_cpus);
	perf_thread_map__put(evlist->threads);
	evlist->user_requested_cpus = NULL;
	evlist->all_cpus = NULL;
	evlist->threads = NULL;
}

void perf_evlist__delete(struct perf_evlist *evlist)
{
	if (evlist == NULL)
		return;

	perf_evlist__munmap(evlist);
	perf_evlist__close(evlist);
	perf_evlist__purge(evlist);
	perf_evlist__exit(evlist);
	free(evlist);
}

void perf_evlist__set_maps(struct perf_evlist *evlist,
			   struct perf_cpu_map *cpus,
			   struct perf_thread_map *threads)
{
	/*
	 * Allow for the possibility that one or another of the maps isn't being
	 * changed i.e. don't put it.  Note we are assuming the maps that are
	 * being applied are brand new and evlist is taking ownership of the
	 * original reference count of 1.  If that is not the case it is up to
	 * the caller to increase the reference count.
	 */
	if (cpus != evlist->user_requested_cpus) {
		perf_cpu_map__put(evlist->user_requested_cpus);
		evlist->user_requested_cpus = perf_cpu_map__get(cpus);
	}

	if (threads != evlist->threads) {
		perf_thread_map__put(evlist->threads);
		evlist->threads = perf_thread_map__get(threads);
	}

	perf_evlist__propagate_maps(evlist);
}

int perf_evlist__open(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;
	int err;
	struct rlimit rl;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		evlist->rl_file = evlist->nr_entries * perf_cpu_map__nr(evlist->user_requested_cpus) *
					perf_thread_map__nr(evlist->threads);
		rl.rlim_cur += evlist->rl_file;
		if (rl.rlim_cur > rl.rlim_max) {
			rl.rlim_cur -= evlist->rl_file;
			evlist->rl_file = rl.rlim_max - rl.rlim_cur;
			rl.rlim_cur = rl.rlim_max;
		}
		if (evlist->rl_file > 0 && setrlimit(RLIMIT_NOFILE, &rl) < 0) {
			evlist->rl_file = 0;
			return -errno;
		}
	}

	perf_evlist__for_each_entry(evlist, evsel) {
		err = perf_evsel__open(evsel, evsel->cpus, evsel->threads);
		if (err < 0)
			goto out_err;
	}

	return 0;

out_err:
	perf_evlist__close(evlist);
	return err;
}

void perf_evlist__close(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;
	struct rlimit rl;

	if (evlist->rl_file > 0 && getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		rl.rlim_cur -= evlist->rl_file;
		evlist->rl_file = 0;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	perf_evlist__for_each_entry_reverse(evlist, evsel)
		perf_evsel__close(evsel);
}

/*
 * Evsels pinned to their own thread map with perf_evsel__set_own_threads() are
 * left alone: the caller manages those explicitly. Order matters, a group
 * member needs the leader's fd for the new thread, and the leader comes first
 * in the list.
 */
int perf_evlist__add_thread(struct perf_evlist *evlist, pid_t pid)
{
	struct perf_evsel *evsel;
	int err;

	perf_evlist__for_each_entry(evlist, evsel) {
		if (evsel->own_threads && !evsel->threads_cow)
			continue;
		if (evsel->system_wide)
			continue;

		err = perf_evsel__add_thread(evsel, pid);
		if (err < 0)
			goto out_del;
	}

	return 0;

out_del:
	perf_evlist__del_thread(evlist, pid);
	return err;
}

int perf_evlist__del_thread(struct perf_evlist *evlist, pid_t pid)
{
	struct perf_evsel *evsel;
	int nr = 0;

	perf_evlist__for_each_entry_reverse(evlist, evsel) {
		if (evsel->own_threads && !evsel->threads_cow)
			continue;
		if (perf_evsel__del_thread(evsel, pid) == 0)
			nr++;
	}

	return nr ? 0 : -ENOENT;
}

void perf_evlist__enable(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	perf_evlist__for_each_entry(evlist, evsel)
		perf_evsel__enable_group(evsel);
}

void perf_evlist__disable(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	perf_evlist__for_each_entry(evlist, evsel)
		perf_evsel__disable_group(evsel);
}

u64 perf_evlist__read_format(struct perf_evlist *evlist)
{
	struct perf_evsel *first = perf_evlist__first(evlist);

	return first->attr.read_format;
}

#define SID(e, x, y) xyarray__entry(e->sample_id, x, y)

static void perf_evlist__id_hash(struct perf_evlist *evlist,
				 struct perf_evsel *evsel,
				 int cpu, int thread, u64 id)
{
	int hash;
	struct perf_sample_id *sid = SID(evsel, cpu, thread);

	sid->id = id;
	sid->evsel = evsel;
	hash = hash_64(sid->id, PERF_EVLIST__HLIST_BITS);
	hlist_add_head(&sid->node, &evlist->heads[hash]);
}

void perf_evlist__reset_id_hash(struct perf_evlist *evlist)
{
	int i;

	for (i = 0; i < PERF_EVLIST__HLIST_SIZE; ++i)
		INIT_HLIST_HEAD(&evlist->heads[i]);
}

static unsigned int perf_evsel__id_max(struct perf_evsel *evsel)
{
	if (!evsel->sample_id)
		return 0;
	return (unsigned int)xyarray__max_x(evsel->sample_id) *
	       (unsigned int)xyarray__max_y(evsel->sample_id);
}

/*
 * evsel->id[] is a bag of live ids, sized to the sample_id xyarray.
 * del_thread must take the id out or the next hole-reuse id_add()
 * writes past the allocation and smashes the heap.
 */
static void perf_evsel__id_remove(struct perf_evsel *evsel, u64 id)
{
	u32 i;

	if (!evsel->id || !evsel->ids)
		return;

	for (i = 0; i < evsel->ids; i++) {
		if (evsel->id[i] != id)
			continue;
		evsel->id[i] = evsel->id[--evsel->ids];
		return;
	}
}

void perf_evlist__id_add(struct perf_evlist *evlist,
			 struct perf_evsel *evsel,
			 int cpu, int thread, u64 id)
{
	unsigned int max = perf_evsel__id_max(evsel);

	perf_evlist__id_hash(evlist, evsel, cpu, thread, id);
	if (!evsel->id || evsel->ids >= max)
		return;
	evsel->id[evsel->ids++] = id;
}

int perf_evlist__id_add_fd(struct perf_evlist *evlist,
			   struct perf_evsel *evsel,
			   int cpu, int thread, int fd)
{
	u64 read_data[4] = { 0, };
	int id_idx = 1; /* The first entry is the counter value */
	u64 id;
	int ret;

	ret = ioctl(fd, PERF_EVENT_IOC_ID, &id);
	if (!ret)
		goto add;

	if (errno != ENOTTY)
		return -1;

	/* Legacy way to get event id.. All hail to old kernels! */

	/*
	 * This way does not work with group format read, so bail
	 * out in that case.
	 */
	if (perf_evlist__read_format(evlist) & PERF_FORMAT_GROUP)
		return -1;

	if (!(evsel->attr.read_format & PERF_FORMAT_ID) ||
	    read(fd, &read_data, sizeof(read_data)) == -1)
		return -1;

	if (evsel->attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
		++id_idx;
	if (evsel->attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
		++id_idx;

	id = read_data[id_idx];

add:
	perf_evlist__id_add(evlist, evsel, cpu, thread, id);
	return 0;
}

struct perf_evsel *perf_evlist__id_to_evsel(struct perf_evlist *evlist,
                   uint64_t id, int *pcpu)
{
    int hash;
    struct perf_sample_id *sid;

	hash = hash_64(id, PERF_EVLIST__HLIST_BITS);
    hlist_for_each_entry(sid, &evlist->heads[hash], node) {
        if (sid->id == id) {
            if (pcpu)
                *pcpu = sid->cpu;
            return sid->evsel;
        }
    }
    return NULL;
}

void perf_evlist_poll__init(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	epoll->epfd = -1;
	epoll->maxevents = 0;
	epoll->events = NULL;
	epoll->external = false;
	epoll->add = NULL;
	epoll->del = NULL;
	epoll->nr = 0;
	epoll->nr_live = 0;
	epoll->nr_alloc = 0;
	epoll->data = NULL;
}

void perf_evlist_poll__external(struct perf_evlist *evlist, void *external)
{
	evlist->epoll.external = external;
}

void *perf_evlist_poll__get_external(struct perf_evlist *evlist, struct perf_mmap *map)
{
	if (!evlist && map)
		evlist = map->evlist;
	return evlist->epoll.external;
}

int perf_evlist_poll__alloc(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	struct perf_cpu_map *cpus = evlist->all_cpus;
	struct perf_thread_map *threads = evlist->threads;

	if (!epoll->external) {
		epoll->epfd = epoll_create1(EPOLL_CLOEXEC);
		if (epoll->epfd < 0)
			return -errno;

		epoll->maxevents = 64;
		epoll->events = zalloc(epoll->maxevents * sizeof(*epoll->events));
		if (epoll->events == NULL)
			return -ENOMEM;
	}
	epoll->nr = 0;
	epoll->nr_live = 0;
	epoll->nr_alloc = evlist->nr_entries * perf_cpu_map__nr(cpus) * perf_thread_map__nr(threads);
	epoll->data = zalloc(epoll->nr_alloc * sizeof(*epoll->data));
	if (epoll->data == NULL)
		return -ENOMEM;

	return 0;
}

/*
 * @nr is the high water mark of used slots, @nr_live the number of live ones.
 * Slots freed by perf_evlist_poll__del() are reused, so that adding a thread
 * after some other thread was removed does not overwrite a live entry. Slot
 * indices are handed to epoll as event.data.u32 and must stay stable.
 */
static int perf_evlist_poll__slot(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i;

	if (epoll->nr_live < epoll->nr) {
		for (i = 0; i < epoll->nr; i++)
			if (epoll->data[i].mmap == NULL)
				return i;
	}

	if (epoll->nr == epoll->nr_alloc) {
		int nr_alloc = epoll->nr_alloc ? epoll->nr_alloc * 2 : 16;
		void *data = realloc(epoll->data, nr_alloc * sizeof(*epoll->data));

		if (!data)
			return -ENOMEM;
		memset((char *)data + epoll->nr_alloc * sizeof(*epoll->data), 0,
		       (nr_alloc - epoll->nr_alloc) * sizeof(*epoll->data));
		epoll->data = data;
		epoll->nr_alloc = nr_alloc;
	}

	return epoll->nr++;
}

void perf_evlist_poll__free(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	if (epoll->events)
		free(epoll->events);
	if (epoll->epfd >= 0)
		close(epoll->epfd);
	if (epoll->data)
		free(epoll->data);
	perf_evlist_poll__init(evlist);
}

int perf_evlist_poll__add(struct perf_evlist *evlist, int fd,
				struct perf_mmap *mmap, unsigned revent)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	struct epoll_event event;
	int n = perf_evlist_poll__slot(evlist);

	if (n < 0)
		return n;

	epoll->data[n].fd = fd;
	epoll->data[n].events = revent | EPOLLERR | EPOLLHUP | EPOLLET;
	epoll->data[n].mmap = mmap;

	if (!epoll->external) {
		event.events = epoll->data[n].events;
		event.data.u32 = n;
		if (epoll_ctl(epoll->epfd, EPOLL_CTL_ADD, fd, &event) < 0) {
			epoll->data[n].mmap = NULL;
			return -errno;
		}
	} else if (epoll->add) {
		int err = epoll->add(epoll->external, fd, epoll->data[n].events, mmap);

		if (err) {
			epoll->data[n].mmap = NULL;
			return err;
		}
	}
	epoll->nr_live ++;
	return 0;
}

int perf_evlist_poll__del(struct perf_evlist *evlist, int n)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	if (epoll->data[n].mmap) {
		struct perf_mmap *map = epoll->data[n].mmap;
		int fd = epoll->data[n].fd;

		epoll->data[n].mmap = NULL;
		epoll->nr_live --;
		if (!epoll->external) {
			if (epoll_ctl(epoll->epfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
				perf_mmap__put(map);
				return -errno;
			}
		} else if (epoll->del)
			epoll->del(epoll->external, fd, map);
		perf_mmap__put(map);
	}
	return 0;
}

int perf_evlist_poll__del_fd(struct perf_evlist *evlist, int fd)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i;

	for (i = 0; i < epoll->nr; i++) {
		if (epoll->data[i].mmap && epoll->data[i].fd == fd)
			return perf_evlist_poll__del(evlist, i);
	}
	return -ENOENT;
}

void perf_evlist_poll__set_ops(struct perf_evlist *evlist,
			       perf_evlist_poll_add_t add,
			       perf_evlist_poll_del_t del)
{
	evlist->epoll.add = add;
	evlist->epoll.del = del;
}

int perf_evlist_poll__foreach_fd(struct perf_evlist *evlist, foreach_fd fn)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i, err;

	if (!epoll->external)
		return -EINVAL;

	for (i = 0; i < epoll->nr; i++) {
		if (!epoll->data[i].mmap)
			continue;
		err = fn(epoll->data[i].fd, epoll->data[i].events, epoll->data[i].mmap);
		if (err)
			return err;
	}
	return 0;
}

int perf_evlist__poll_mmap(struct perf_evlist *evlist, int timeout, handle_mmap handle)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i, cnt;

	if (epoll->external)
		return -EINVAL;

	if (epoll->epfd < 0) {
		return usleep(timeout * 1000);
	}

	cnt = epoll_wait(epoll->epfd, epoll->events, epoll->maxevents, timeout);
	if (cnt < 0)
		return -errno;

	for (i = 0; i < cnt; i++) {
		unsigned int revents = epoll->events[i].events;
		int n = epoll->events[i].data.u32;
		if (!epoll->data[n].mmap)
			continue;
		if (handle)
			handle(epoll->data[n].mmap);
		if (revents & EPOLLHUP)
			perf_evlist_poll__del(evlist, n);
	}
	return epoll->nr_live ? cnt : -ENOENT;
}

int perf_evlist__poll(struct perf_evlist *evlist, int timeout)
{
	return perf_evlist__poll_mmap(evlist, timeout, NULL);
}

#define FD(e, x, y) (*(int *) xyarray__entry(e->fd, x, y))

static int perf_evlist__mmap_hash(int cpu, pid_t tid, bool overwrite)
{
	u32 key = ((u32)(cpu + 1) * 2654435761U) ^
		  ((u32)(tid + 1) * 2246822519U) ^ (u32)overwrite;

	return hash_32(key, PERF_EVLIST__MMAP_HLIST_BITS);
}

struct perf_mmap *perf_evlist__find_mmap(struct perf_evlist *evlist,
					 int cpu, pid_t tid, bool overwrite)
{
	int hash = perf_evlist__mmap_hash(cpu, tid, overwrite);
	struct perf_mmap *map;

	hlist_for_each_entry(map, &evlist->mmap_heads[hash], hnode) {
		if (map->cpu == cpu && map->tid == tid &&
		    map->overwrite == overwrite)
			return map;
	}
	return NULL;
}

/*
 * Keep the old meaning of perf_mmap__idx(): the index of the cpu (or of the
 * thread, when binding per thread) in the evlist maps. Ring buffers created for
 * a thread added after perf_evlist__mmap() get an index past the end.
 */
static int perf_evlist__mmap_idx(struct perf_evlist *evlist, int cpu, pid_t tid)
{
	int idx;

	if (cpu != -1)
		idx = perf_cpu_map__idx(evlist->all_cpus, cpu);
	else
		idx = perf_thread_map__idx(evlist->threads, tid);

	if (idx < 0)
		idx = evlist->next_mmap_idx++;

	return idx;
}

static void perf_evlist__mmap_unmap_cb(struct perf_mmap *map)
{
	struct perf_evlist *evlist = map->evlist;

	if (!map->dynamic)
		return;

	map->dynamic = false;
	list_del(&map->list);
	hlist_del(&map->hnode);
	evlist->nr_mmaps--;
	free(map);
}

static struct perf_mmap *
perf_evlist__alloc_mmap(struct perf_evlist *evlist, int cpu, pid_t tid,
			bool overwrite)
{
	struct perf_mmap *map = zalloc(sizeof(*map));

	if (!map)
		return NULL;

	perf_mmap__init(map, NULL, overwrite, perf_evlist__mmap_unmap_cb);
	perf_mmap__set_bind(map, cpu, tid);
	map->idx = perf_evlist__mmap_idx(evlist, cpu, tid);
	map->evlist = evlist;

	return map;
}

static void perf_evlist__link_mmap(struct perf_evlist *evlist,
				   struct perf_mmap *map)
{
	int hash = perf_evlist__mmap_hash(map->cpu, map->tid, map->overwrite);

	map->dynamic = true;
	list_add_tail(&map->list, map->overwrite ? &evlist->mmap_ovw_list
						 : &evlist->mmap_list);
	hlist_add_head(&map->hnode, &evlist->mmap_heads[hash]);
	evlist->nr_mmaps++;
}

static void perf_evsel__set_sid_idx(struct perf_evsel *evsel, int idx, int cpu, int thread)
{
	struct perf_sample_id *sid = SID(evsel, cpu, thread);

	sid->idx = idx;
	sid->cpu = perf_cpu_map__cpu(evsel->cpus, cpu);
	sid->tid = perf_thread_map__pid(evsel->threads, thread);
}

static int
perf_evlist__mmap_cb_mmap(struct perf_mmap *map, struct perf_mmap_param *mp,
			  int output, int cpu)
{
	return perf_mmap__mmap(map, mp, output, cpu);
}

/*
 * Attach one open fd of @evsel to the ring buffer of its binding. The first fd
 * that needs a given (cpu, tid, overwrite) binding creates and maps the ring
 * buffer; every later fd - from this evsel or from any other one - redirects
 * its output into it and takes a reference.
 */
static int
__perf_evlist__mmap_fd(struct perf_evlist *evlist,
		       struct perf_evlist_mmap_ops *ops,
		       struct perf_evsel *evsel, struct perf_mmap_param *mp,
		       int cpu, int thread)
{
	bool overwrite = evsel->attr.write_backward;
	struct perf_mmap *map;
	pid_t bind_tid;
	int bind_cpu, fd;
	int *pfd;

	pfd = xyarray__entry(evsel->fd, cpu, thread);
	if (pfd == NULL || *pfd < 0)
		return 0;
	fd = *pfd;

	perf_evsel__mmap_bind(evsel, cpu, thread, &bind_cpu, &bind_tid);

	mp->prot = overwrite ? PROT_READ : (PROT_READ | PROT_WRITE);
	mp->mask = evlist->mmap_len - page_size - 1;

	map = perf_evlist__find_mmap(evlist, bind_cpu, bind_tid, overwrite);
	if (!map) {
		map = perf_evlist__alloc_mmap(evlist, bind_cpu, bind_tid, overwrite);
		if (!map)
			return -ENOMEM;

		/*
		 * One reference for the fd below - its poll entry drops it -
		 * plus one extra so that perf_mmap__consume() can still drain
		 * the last events after every real reference is gone. I.e. we
		 * can get the POLLHUP meaning that the fd doesn't exist
		 * anymore, but the last events for it are still in the ring
		 * buffer, waiting to be consumed.
		 */
		refcount_set(&map->refcnt, 2);
		map->extra_ref = true;

		if (ops->mmap(map, mp, fd, bind_cpu) < 0) {
			free(map);
			return -1;
		}

		perf_evlist__link_mmap(evlist, map);
	} else {
		if (ioctl(fd, PERF_EVENT_IOC_SET_OUTPUT, map->fd) != 0)
			return -1;

		perf_mmap__get(map);
	}

	map->nr_fds++;

	if (!evsel->system_wide &&
	    perf_evlist_poll__add(evlist, fd, map, overwrite ? 0 : EPOLLIN) < 0) {
		map->nr_fds--;
		perf_mmap__put(map);
		return -1;
	}

	if (evsel->attr.read_format & PERF_FORMAT_ID) {
		if (perf_evlist__id_add_fd(evlist, evsel, cpu, thread, fd) < 0)
			return -1;
		perf_evsel__set_sid_idx(evsel, map->idx, cpu, thread);
	}

	return 0;
}

int perf_evlist__mmap_evsel_fd(struct perf_evlist *evlist,
			       struct perf_evsel *evsel, int cpu, int thread)
{
	struct perf_evlist_mmap_ops ops = { .mmap = perf_evlist__mmap_cb_mmap };
	struct perf_mmap_param mp = {};

	if (!evlist->mmaped || !evsel->mmaped)
		return 0;

	if (evlist->epoll.epfd == -1 && !evlist->epoll.external &&
	    perf_evlist_poll__alloc(evlist) < 0)
		return -ENOMEM;

	return __perf_evlist__mmap_fd(evlist, &ops, evsel, &mp, cpu, thread);
}

void perf_evlist__unmap_evsel_fd(struct perf_evlist *evlist,
				 struct perf_evsel *evsel, int cpu, int thread)
{
	int *pfd = xyarray__entry(evsel->fd, cpu, thread);
	struct perf_mmap *map;
	pid_t bind_tid;
	int bind_cpu;

	if (pfd == NULL || *pfd < 0)
		return;

	perf_evsel__mmap_bind(evsel, cpu, thread, &bind_cpu, &bind_tid);
	map = perf_evlist__find_mmap(evlist, bind_cpu, bind_tid,
				     evsel->attr.write_backward);

	/* Hold the ring buffer across the put below so we can inspect it. */
	if (map)
		perf_mmap__get(map);

	/* Drops the reference this fd holds on the ring buffer. */
	perf_evlist_poll__del_fd(evlist, *pfd);

	if (map) {
		if (map->nr_fds > 0)
			map->nr_fds--;
		/*
		 * No fd writes into it any more, so nothing can arrive after
		 * what the caller already drained: drop the drain reference and
		 * let the ring buffer go.
		 */
		if (map->nr_fds == 0 && map->extra_ref) {
			map->extra_ref = false;
			perf_mmap__put(map);
		}
		perf_mmap__put(map);
	}

	if (evsel->sample_id) {
		struct perf_sample_id *sid = SID(evsel, cpu, thread);

		if (sid && sid->evsel) {
			perf_evsel__id_remove(evsel, sid->id);
			hlist_del(&sid->node);
			memset(sid, 0, sizeof(*sid));
		}
	}
}

static int
mmap_evsel(struct perf_evlist *evlist, struct perf_evlist_mmap_ops *ops,
	   struct perf_evsel *evsel, struct perf_mmap_param *mp)
{
	int nr_cpus = perf_cpu_map__nr(evsel->cpus);
	int nr_threads = perf_thread_map__nr(evsel->threads);
	int cpu, thread;

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		for (thread = 0; thread < nr_threads; thread++) {
			if (evsel->system_wide && thread)
				continue;
			if (!perf_thread_map__valid(evsel->threads, thread))
				continue;
			if (__perf_evlist__mmap_fd(evlist, ops, evsel, mp, cpu, thread) < 0)
				return -1;
		}
	}

	evsel->mmaped = true;
	return 0;
}

int perf_evlist__mmap_ops(struct perf_evlist *evlist,
			  struct perf_evlist_mmap_ops *ops,
			  struct perf_mmap_param *mp)
{
	struct perf_evsel *evsel;
	int pass;

	if (!ops || !ops->mmap)
		return -EINVAL;

	mp->mask = evlist->mmap_len - page_size - 1;

	evlist->next_mmap_idx = perf_cpu_map__empty(evlist->all_cpus) ?
				perf_thread_map__nr(evlist->threads) :
				perf_cpu_map__nr(evlist->all_cpus);

	perf_evlist__for_each_entry(evlist, evsel) {
		if ((evsel->attr.read_format & PERF_FORMAT_ID) &&
		    evsel->sample_id == NULL &&
		    perf_evsel__alloc_id(evsel, evsel->fd->max_x, evsel->fd->max_y) < 0)
			return -ENOMEM;
	}

	if (evlist->epoll.epfd == -1 && perf_evlist_poll__alloc(evlist) < 0)
		return -ENOMEM;

	evlist->mmaped = true;

	/*
	 * rb->watermark is a property of the ring buffer, fixed by the fd that
	 * mmap()s it; wakeup_events is a property of each event. So let the
	 * watermark evsels create the ring buffers first (pass 0) and have the
	 * wakeup_events evsels join them with SET_OUTPUT (pass 1). Within a
	 * pass, evsels are processed in the order they were added.
	 */
	for (pass = 0; pass < 2; pass++) {
		perf_evlist__for_each_entry(evlist, evsel) {
			if (!evsel->attr.watermark != !pass)
				continue;
			if (mmap_evsel(evlist, ops, evsel, mp) < 0)
				goto out_unmap;
		}
	}

	return 0;

out_unmap:
	perf_evlist__munmap(evlist);
	return -1;
}

int perf_evlist__mmap(struct perf_evlist *evlist, int pages)
{
	struct perf_mmap_param mp;
	struct perf_evlist_mmap_ops ops = {
		.mmap = perf_evlist__mmap_cb_mmap,
	};

	evlist->mmap_len = (pages + 1) * page_size;

	return perf_evlist__mmap_ops(evlist, &ops, &mp);
}

void perf_evlist__munmap(struct perf_evlist *evlist)
{
	struct perf_mmap *map, *tmp;
	struct perf_evsel *evsel;
	int i;

	if (evlist->epoll.nr) {
		for (i = 0; i < evlist->epoll.nr_alloc; i++)
			perf_evlist_poll__del(evlist, i);
	}
	perf_evlist_poll__free(evlist);

	list_for_each_entry_safe(map, tmp, &evlist->mmap_list, list)
		perf_mmap__munmap(map);

	list_for_each_entry_safe(map, tmp, &evlist->mmap_ovw_list, list)
		perf_mmap__munmap(map);

	perf_evlist__reset_id_hash(evlist);

	perf_evlist__for_each_entry(evlist, evsel) {
		if (evsel->attr.read_format & PERF_FORMAT_ID)
			perf_evsel__free_id(evsel);
		evsel->mmaped = false;
	}

	evlist->mmaped = false;
}

struct perf_mmap*
perf_evlist__next_mmap(struct perf_evlist *evlist, struct perf_mmap *map,
		       bool overwrite)
{
	struct list_head *head = overwrite ? &evlist->mmap_ovw_list
					   : &evlist->mmap_list;
	struct list_head *next = map ? map->list.next : head->next;

	return next == head ? NULL : list_entry(next, struct perf_mmap, list);
}

void __perf_evlist__set_leader(struct list_head *list, struct perf_evsel *leader)
{
	struct perf_evsel *evsel;
	int n = 0;

	__perf_evlist__for_each_entry(list, evsel) {
		evsel->leader = leader;
		n++;
	}
	leader->nr_members = n;
}

static bool perf_cpu_map__equal(struct perf_cpu_map *a, struct perf_cpu_map *b)
{
	if (a == b)
		return true;
	if (!a || !b || a->nr != b->nr)
		return false;
	return memcmp(a->map, b->map, a->nr * sizeof(a->map[0])) == 0;
}

static bool perf_thread_map__equal(struct perf_thread_map *a,
				   struct perf_thread_map *b)
{
	int i;

	if (a == b)
		return true;
	if (!a || !b || a->nr != b->nr)
		return false;
	for (i = 0; i < a->nr; i++)
		if (a->map[i].pid != b->map[i].pid)
			return false;
	return true;
}

/*
 * The kernel only groups events that live in the same (cpu, pid) context, so
 * evsels are grouped by binding: each evsel joins the first earlier evsel with
 * the same cpu and thread map, and starts a group of its own otherwise. With a
 * uniformly bound evlist this is a single group led by the first evsel.
 */
void perf_evlist__set_leader(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel, *pos;

	perf_evlist__for_each_entry(evlist, evsel) {
		struct perf_evsel *leader = evsel;

		perf_evlist__for_each_entry(evlist, pos) {
			if (pos == evsel)
				break;
			if (pos->leader == pos &&
			    perf_cpu_map__equal(pos->cpus, evsel->cpus) &&
			    perf_thread_map__equal(pos->threads, evsel->threads)) {
				leader = pos;
				break;
			}
		}

		evsel->leader = leader;
		if (leader == evsel)
			evsel->nr_members = 1;
		else
			leader->nr_members++;
	}
}

int perf_evlist__nr_groups(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;
	int nr_groups = 0;

	perf_evlist__for_each_evsel(evlist, evsel) {
		/*
		 * evsels by default have a nr_members of 1, and they are their
		 * own leader. If the nr_members is >1 then this is an
		 * indication of a group.
		 */
		if (evsel->leader == evsel && evsel->nr_members > 1)
			nr_groups++;
	}
	return nr_groups;
}

void perf_evlist__go_system_wide(struct perf_evlist *evlist, struct perf_evsel *evsel)
{
	if (!evsel->system_wide) {
		evsel->system_wide = true;
		if (evlist->needs_map_propagation)
			__perf_evlist__propagate_maps(evlist, evsel);
	}
}

int perf_evlist__max_read_size(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;
	int max_size = sizeof(struct perf_counts_values);
	int read_size;

	perf_evlist__for_each_evsel(evlist, evsel) {
		read_size = perf_evsel__read_size(evsel);
		if (read_size > max_size)
			max_size = read_size;
	}
	return max_size;
}
