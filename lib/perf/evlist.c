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

	INIT_LIST_HEAD(&evlist->evsel_list);
	evlist->nr_entries = 0;
	INIT_LIST_HEAD(&evlist->mmap_list);
	INIT_LIST_HEAD(&evlist->mmap_ovw_list);
	for (i = 0; i < PERF_EVLIST__MMAP_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(&evlist->mmap_heads[i]);
	for (i = 0; i < PERF_EVLIST__THREAD_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(&evlist->thread_heads[i]);
	perf_evlist_poll__init(evlist);
	perf_evlist__reset_id_hash(evlist);
}

/*
 * The evlist maps are the default. An evsel that asked for its own cpu or
 * thread map keeps it; the two are independent, an evsel can bind its own cpus
 * and still follow the evlist threads.
 *
 * Once the evsel is open both are frozen: they are the input its fds were
 * opened from.
 */
static void __perf_evlist__propagate_maps(struct perf_evlist *evlist,
					  struct perf_evsel *evsel)
{
	if (!evsel->opened) {
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

	if (!evsel->opened) {
		struct perf_thread_map *threads;

		if (evsel->system_wide)
			threads = perf_thread_map__new_dummy();
		else if (evsel->own_threads)
			threads = perf_thread_map__get(evsel->own_threads);
		else
			threads = perf_thread_map__get(evlist->user_requested_threads);

		perf_thread_map__put(evsel->threads);
		evsel->threads = threads;
	}
}

static void perf_evlist__propagate_maps(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel, *n;

	evlist->needs_map_propagation = true;

	list_for_each_entry_safe(evsel, n, &evlist->evsel_list, node)
		__perf_evlist__propagate_maps(evlist, evsel);
}

void perf_evlist__add(struct perf_evlist *evlist,
		      struct perf_evsel *evsel)
{
	evsel->idx = evlist->nr_entries;
	evsel->evlist = evlist;
	list_add_tail(&evsel->node, &evlist->evsel_list);
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
		next = list_first_entry(&evlist->evsel_list,
					struct perf_evsel,
					node);
	} else {
		next = list_next_entry(prev, node);
	}

	/* Empty list is noticed here so don't need checking on entry. */
	if (&next->node == &evlist->evsel_list)
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
	perf_thread_map__put(evlist->user_requested_threads);
	evlist->user_requested_cpus = NULL;
	evlist->user_requested_threads = NULL;
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

int perf_evlist__set_maps(struct perf_evlist *evlist,
			  struct perf_cpu_map *cpus,
			  struct perf_thread_map *threads)
{
	/* The fds are laid out along them, see perf_evlist__open(). */
	if (evlist->opened)
		return -EBUSY;

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

	if (threads != evlist->user_requested_threads) {
		perf_thread_map__put(evlist->user_requested_threads);
		evlist->user_requested_threads = perf_thread_map__get(threads);
	}

	perf_evlist__propagate_maps(evlist);
	return 0;
}

/*
 * The order the evsels open their fds in, also for threads added later; close
 * in the reverse order.
 *
 * Kernel constraint: a group leader is open before its members, perf_event_open()
 * takes its fd as group_fd. In a grouped evlist that is the rank order, highest
 * first, see perf_evsel__rank_cmp(); otherwise the evlist order, explicit
 * leaders come first.
 */
static struct perf_evsel **perf_evlist__open_order(struct perf_evlist *evlist)
{
	struct perf_evsel **order = calloc(evlist->nr_entries ?: 1, sizeof(*order));
	struct perf_evsel *evsel;
	int i, nr = 0;

	if (!order)
		return NULL;
	perf_evlist__for_each_entry(evlist, evsel) {
		for (i = nr; i > 0 && evlist->grouped &&
			     perf_evsel__rank_cmp(evsel, order[i - 1]) > 0; i--)
			order[i] = order[i - 1];
		order[i] = evsel;
		nr++;
	}
	return order;
}

int perf_evlist__open(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel, **order;
	struct perf_evsel_fd *entry;
	int err, i;
	struct rlimit rl;

	perf_evlist__propagate_maps(evlist);
	evlist->opened = true;
	/* The open order needs the ranks before perf_evsel__open() fixes them. */
	perf_evlist__for_each_entry(evlist, evsel) {
		if (!evsel->opened)
			perf_evsel__set_rank(evsel, evsel->cpus, evsel->threads);
	}

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		evlist->rl_file = 0;
		perf_evlist__for_each_entry(evlist, evsel)
			evlist->rl_file += perf_cpu_map__nr(evsel->cpus) * perf_thread_map__nr(evsel->threads);
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

	order = perf_evlist__open_order(evlist);
	if (!order) {
		err = -ENOMEM;
		goto out_err;
	}
	for (i = 0; i < evlist->nr_entries; i++) {
		err = perf_evsel__open(order[i], order[i]->cpus, order[i]->threads);
		if (err < 0)
			break;
	}
	free(order);
	if (err < 0)
		goto out_err;

	/* Count-only users also need event ids without creating ring buffers. */
	perf_evlist__for_each_entry(evlist, evsel) {
		if (!(evsel->attr.read_format & PERF_FORMAT_ID))
			continue;
		perf_evsel__for_each_fd(evsel, entry) {
			err = perf_evlist__id_add_fd(evlist, entry);
			if (err)
				goto out_err;
		}
	}

	return 0;

out_err:
	perf_evlist__close(evlist);
	return err;
}

void perf_evlist__close(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel, **order;
	struct rlimit rl;
	int i;

	evlist->opened = false;
	if (evlist->rl_file > 0 && getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		rl.rlim_cur -= evlist->rl_file;
		evlist->rl_file = 0;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	order = perf_evlist__open_order(evlist);
	if (order) {
		for (i = evlist->nr_entries - 1; i >= 0; i--)
			perf_evsel__close(order[i]);
		free(order);
	} else {
		perf_evlist__for_each_entry_reverse(evlist, evsel)
			perf_evsel__close(evsel);
	}
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

void perf_evlist__reset_id_hash(struct perf_evlist *evlist)
{
	int i;

	for (i = 0; i < PERF_EVLIST__ID_HLIST_SIZE; ++i)
		INIT_HLIST_HEAD(&evlist->id_heads[i]);
}

/* The sample id of @entry, embedded in it; replaces an earlier one. */
void perf_evlist__id_add(struct perf_evlist *evlist,
			 struct perf_evsel_fd *entry, u64 id)
{
	struct perf_sample_id *sid = &entry->sid;

	perf_evlist__id_del(entry);
	sid->id = id;
	sid->evsel = entry->evsel;
	sid->cpu = entry->cpu;
	sid->tid = entry->tid;
	hlist_add_head(&sid->node, &evlist->id_heads[hash_64(id, PERF_EVLIST__ID_HLIST_BITS)]);
}

void perf_evlist__id_del(struct perf_evsel_fd *entry)
{
	struct perf_sample_id *sid = &entry->sid;

	if (!sid->evsel)
		return;
	hlist_del(&sid->node);
	memset(sid, 0, sizeof(*sid));
}

int perf_evlist__id_add_fd(struct perf_evlist *evlist,
			   struct perf_evsel_fd *entry)
{
	struct perf_evsel *evsel = entry->evsel;
	int fd = entry->fd;
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
	perf_evlist__id_add(evlist, entry, id);
	return 0;
}

struct perf_evsel *perf_evlist__id_to_evsel(struct perf_evlist *evlist,
                   uint64_t id, int *pcpu)
{
    int hash;
    struct perf_sample_id *sid;

	hash = hash_64(id, PERF_EVLIST__ID_HLIST_BITS);
    hlist_for_each_entry(sid, &evlist->id_heads[hash], node) {
        if (sid->id == id) {
            if (pcpu)
                *pcpu = sid->cpu;
            return sid->evsel;
        }
    }
    return NULL;
}

/*
 * Ring buffer management.
 *
 * The evlist owns the ring buffers (struct perf_mmap) of its evsels and keeps
 * them alive along the writers attached to them, grouped in domains (see
 * struct perf_mmap_domain): the cpu domain of the pid=-1 and cgroup writers,
 * which never sees EPOLLHUP, and the task domains of the writers bound to one
 * task, which see EPOLLHUP once the task exits. HUP is a property of the task
 * domain only.
 *
 *   ring buffer registry  - lookup by (cpu, tid, overwrite), list for iteration
 *   thread records        - exit state of each thread, see perf_evlist_thread
 *   poll registration     - one fd per domain, internal epoll or external loop
 *   domains               - the references the writers hold on a ring buffer
 *   writer fds            - attach/detach one fd of an evsel
 *   thread exit           - perf_evlist__hup() / perf_evlist__del_thread()
 *   mmap / munmap / poll  - the public entry points
 *
 * A ring buffer loses its last writer only through EPOLLHUP of a task domain,
 * or when its fds are closed outright (perf_evsel__del_thread(), munmap,
 * close).
 */

/* Ring buffer registry. */

static int perf_evlist__mmap_hash(int cpu, pid_t tid, bool overwrite)
{
	u32 key = ((u32)(cpu + 1) * 2654435761U) ^
		  ((u32)(tid + 1) * 2246822519U) ^ (u32)overwrite;

	return hash_32(key, PERF_EVLIST__MMAP_HLIST_BITS);
}

/*
 * Only ring buffers new writers may still join are found: a thread ring buffer
 * leaves the hash once its thread is deleted, the tid may be reused.
 */
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

static void perf_evlist__mmap_register(struct perf_evlist *evlist,
				       struct perf_mmap *map)
{
	int hash = perf_evlist__mmap_hash(map->cpu, map->tid, map->overwrite);

	list_add_tail(&map->node, map->overwrite ? &evlist->mmap_ovw_list
						 : &evlist->mmap_list);
	hlist_add_head(&map->hnode, &evlist->mmap_heads[hash]);
	map->registered = true;
	evlist->nr_mmaps++;
}

static void perf_evlist__mmap_unhash(struct perf_mmap *map)
{
	if (!hlist_unhashed(&map->hnode))
		hlist_del_init(&map->hnode);
}

static void perf_evlist__mmap_unregister(struct perf_evlist *evlist,
					 struct perf_mmap *map)
{
	perf_evlist__mmap_unhash(map);
	list_del_init(&map->node);
	map->registered = false;
	evlist->nr_mmaps--;
}

/*
 * Map the ring buffer of a binding from its first writer @fd and register it.
 *
 * Kernel constraint: @fd is not redirected yet, see perf_mmap__mmap().
 */
static struct perf_mmap *
perf_evlist__mmap_create(struct perf_evlist *evlist, int cpu, pid_t tid,
			 bool overwrite, int fd)
{
	struct perf_mmap_param mp = {
		.prot = overwrite ? PROT_READ : (PROT_READ | PROT_WRITE),
		.mask = evlist->mmap_len - page_size - 1,
	};
	struct perf_mmap *map = perf_mmap__new(cpu, tid, overwrite);

	if (!map)
		return NULL;
	if (perf_mmap__mmap(map, &mp, fd) < 0) {
		perf_mmap__put(map);
		return NULL;
	}
	map->evlist = evlist;
	perf_evlist__mmap_register(evlist, map);
	return map;
}

/* Thread records. */

static struct perf_evlist_thread *
perf_evlist__thread_find(struct perf_evlist *evlist, pid_t tid)
{
	int hash = hash_32((u32)tid, PERF_EVLIST__THREAD_HLIST_BITS);
	struct perf_evlist_thread *th;

	hlist_for_each_entry(th, &evlist->thread_heads[hash], hnode) {
		if (th->tid == tid)
			return th;
	}
	return NULL;
}

static struct perf_evlist_thread *
perf_evlist__thread_get(struct perf_evlist *evlist, pid_t tid)
{
	struct perf_evlist_thread *th = perf_evlist__thread_find(evlist, tid);

	if (th)
		return th;
	th = zalloc(sizeof(*th));
	if (!th)
		return NULL;
	th->tid = tid;
	INIT_LIST_HEAD(&th->domain_list);
	hlist_add_head(&th->hnode,
		&evlist->thread_heads[hash_32((u32)tid, PERF_EVLIST__THREAD_HLIST_BITS)]);
	return th;
}

static int perf_evlist_poll__sync(struct perf_evlist *evlist,
				  struct perf_mmap_domain *dom);

/*
 * Domains still attached - from evsels with their own thread map - lose the
 * hup role along with the thread.
 */
static void perf_evlist__thread_free(struct perf_evlist *evlist,
				     struct perf_evlist_thread *th)
{
	struct perf_mmap_domain *dom, *tmp;

	th->hup_domain = NULL;
	list_for_each_entry_safe(dom, tmp, &th->domain_list, thread_node) {
		list_del_init(&dom->thread_node);
		dom->thread = NULL;
		perf_evlist_poll__sync(evlist, dom);
	}
	hlist_del(&th->hnode);
	free(th);
}

/*
 * Poll registration.
 *
 * Without an external loop the fds go into the evlist's own epoll, keyed by
 * fd; epoll.poll_fd_heads[] maps a registered fd back to its domain either way.
 */

void perf_evlist_poll__init(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i;

	epoll->epfd = -1;
	epoll->maxevents = 0;
	epoll->events = NULL;
	epoll->external = NULL;
	epoll->add = NULL;
	epoll->del = NULL;
	epoll->nr_live = 0;
	for (i = 0; i < PERF_EVLIST__POLL_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(&epoll->poll_fd_heads[i]);
}

void perf_evlist_poll__external(struct perf_evlist *evlist, void *external,
				perf_evlist_poll_add_t add,
				perf_evlist_poll_del_t del)
{
	evlist->epoll.external = external;
	evlist->epoll.add = add;
	evlist->epoll.del = del;
}

void *perf_evlist_poll__get_external(struct perf_evlist *evlist, struct perf_mmap *map)
{
	if (!evlist && map)
		evlist = map->evlist;
	return evlist->epoll.external;
}

static int perf_evlist_poll__alloc(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	epoll->epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll->epfd < 0)
		return -errno;

	epoll->maxevents = 64;
	epoll->events = zalloc(epoll->maxevents * sizeof(*epoll->events));
	if (epoll->events == NULL) {
		close(epoll->epfd);
		epoll->epfd = -1;
		return -ENOMEM;
	}
	return 0;
}

/* Only the evlist's own epoll; the external loop and its ops stay set. */
static void perf_evlist_poll__free(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	zfree(&epoll->events);
	epoll->maxevents = 0;
	if (epoll->epfd >= 0)
		close(epoll->epfd);
	epoll->epfd = -1;
}

static int perf_evlist_poll__hash(int fd)
{
	return hash_32((u32)fd, PERF_EVLIST__POLL_HLIST_BITS);
}

static struct perf_mmap_domain *
perf_evlist_poll__find(struct perf_evlist *evlist, int fd)
{
	struct perf_mmap_domain *dom;

	hlist_for_each_entry(dom, &evlist->epoll.poll_fd_heads[perf_evlist_poll__hash(fd)], poll_node) {
		if (dom->poll_fd == fd)
			return dom;
	}
	return NULL;
}

static int perf_evlist_poll__add(struct perf_evlist *evlist,
				 struct perf_mmap_domain *dom, int fd, unsigned events)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	struct perf_mmap *map = dom->map;
	int err = 0;

	events |= EPOLLERR | EPOLLHUP | EPOLLET;
	if (epoll->external) {
		if (epoll->add)
			err = epoll->add(epoll->external, fd, events, map, !map->announced);
	} else {
		struct epoll_event event = { .events = events, .data.fd = fd };

		if (epoll->epfd < 0)
			err = perf_evlist_poll__alloc(evlist);
		if (!err && epoll_ctl(epoll->epfd, EPOLL_CTL_ADD, fd, &event) < 0)
			err = -errno;
	}
	if (err)
		return err;

	map->announced = true;
	dom->poll_fd = fd;
	dom->poll_events = events;
	hlist_add_head(&dom->poll_node, &epoll->poll_fd_heads[perf_evlist_poll__hash(fd)]);
	epoll->nr_live++;
	return 0;
}

/*
 * Kernel constraint: must run before the registered fd is closed. epoll drops
 * an fd only when its file is freed, and a VMA of the ring buffer may hold the
 * file after close(); the fd would stay in the epoll and, once its task exits,
 * keep reporting EPOLLHUP, which no event mask can turn off.
 */
static void perf_evlist_poll__del(struct perf_evlist *evlist,
				  struct perf_mmap_domain *dom)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int fd = dom->poll_fd;

	if (fd < 0)
		return;

	if (epoll->external) {
		if (epoll->del)
			epoll->del(epoll->external, fd, dom->map);
	} else
		epoll_ctl(epoll->epfd, EPOLL_CTL_DEL, fd, NULL);

	hlist_del_init(&dom->poll_node);
	dom->poll_fd = -1;
	epoll->nr_live--;
}

static int perf_mmap_domain__poll_fd(struct perf_mmap_domain *dom)
{
	int i;

	for (i = 0; i < dom->nr_fds; i++)
		if (dom->fds[i].poll)
			return dom->fds[i].fd;
	return -1;
}

static bool perf_mmap_domain__pollable(struct perf_mmap_domain *dom)
{
	return dom && !dom->dead && perf_mmap_domain__poll_fd(dom) >= 0;
}

/*
 * The registered fds serve two roles:
 *
 *   data: one fd per ring buffer, polled for EPOLLIN. The cpu domain's if
 *         there is one: it never goes away on its own.
 *   hup:  one fd per thread, from one of its task domains, polled for
 *         EPOLLHUP only. Prefer one that is already a data fd, then a
 *         single fd serves both. The cpu domain never takes this role.
 *
 * A domain is registered if it holds either role, with EPOLLIN only for the
 * data role. A thread whose EPOLLHUP is taken (th->hup) needs no hup fd.
 *
 * Kernel constraints that make one fd enough:
 *  - a write by any writer wakes up every event attached to the ring buffer
 *    (ring_buffer_wakeup()): since 10c6db110d0e ("perf: Fix loss of
 *    notification with multi-event"), in v3.2;
 *  - when a task exits, every event in its context enters
 *    PERF_EVENT_STATE_EXIT and perf_poll() reports EPOLLHUP, disabled events
 *    included: since 179033b3e064 ("perf: Add PERF_EVENT_STATE_EXIT state for
 *    events with exited task"), in v3.18.
 * And why only fds attached to a ring buffer are registered: perf_poll() of an
 * event without a ring buffer reports EPOLLHUP right away, since 61b67684c4a4
 * ("perf: Fix perf_poll to return proper POLLHUP value"), in v3.18.
 */
static struct perf_mmap_domain *
perf_evlist_poll__pick_data(struct perf_mmap *map)
{
	struct perf_mmap_domain *dom, *any = NULL;

	if (perf_mmap_domain__pollable(map->cpu_domain))
		return map->cpu_domain;
	if (perf_mmap_domain__pollable(map->poll_domain))
		return map->poll_domain;
	list_for_each_entry(dom, &map->domain_list, map_node) {
		if (!perf_mmap_domain__pollable(dom))
			continue;
		if (dom->thread && dom->thread->hup_domain == dom)
			return dom;
		if (!any)
			any = dom;
	}
	return any;
}

static struct perf_mmap_domain *
perf_evlist_poll__pick_hup(struct perf_evlist_thread *th)
{
	struct perf_mmap_domain *dom, *any = NULL;

	if (th->hup)
		return NULL;
	if (perf_mmap_domain__pollable(th->hup_domain))
		return th->hup_domain;
	list_for_each_entry(dom, &th->domain_list, thread_node) {
		if (!perf_mmap_domain__pollable(dom))
			continue;
		if (dom->map->poll_domain == dom)
			return dom;
		if (!any)
			any = dom;
	}
	return any;
}

/* Bring the registration of @dom in line with its roles. */
static int perf_evlist_poll__sync(struct perf_evlist *evlist,
				  struct perf_mmap_domain *dom)
{
	bool data, hup;
	unsigned events;
	int fd;

	/* Tearing down: registrations are only removed, see perf_evlist__munmap(). */
	if (!dom || evlist->munmapping)
		return 0;
	data = !dom->dead && dom->map->poll_domain == dom;
	hup = !dom->dead && dom->thread && dom->thread->hup_domain == dom;
	/* An overwrite ring buffer is read on demand, only its EPOLLHUP matters. */
	events = (data && !dom->map->overwrite ? EPOLLIN : 0) | EPOLLERR | EPOLLHUP | EPOLLET;

	if (dom->poll_fd >= 0 && (!(data || hup) || dom->poll_events != events))
		perf_evlist_poll__del(evlist, dom);
	if (!(data || hup) || dom->poll_fd >= 0)
		return 0;
	fd = perf_mmap_domain__poll_fd(dom);
	return fd < 0 ? 0 : perf_evlist_poll__add(evlist, dom, fd, events);
}

/*
 * After a domain of @map or of @th changed, re-pick the data domain of @map
 * and the hup domain of @th and sync the domains that gained or lost a role,
 * plus @dom itself. The current choices are kept while they stay valid.
 */
static int perf_evlist_poll__update(struct perf_evlist *evlist, struct perf_mmap *map,
				    struct perf_evlist_thread *th, struct perf_mmap_domain *dom)
{
	struct perf_mmap_domain *old_data = NULL, *old_hup = NULL;
	int err = 0, ret, i;

	if (map) {
		old_data = map->poll_domain;
		map->poll_domain = perf_evlist_poll__pick_data(map);
	}
	if (th) {
		old_hup = th->hup_domain;
		th->hup_domain = perf_evlist_poll__pick_hup(th);
	}
	{
		struct perf_mmap_domain *doms[] = {
			old_data, map ? map->poll_domain : NULL,
			old_hup, th ? th->hup_domain : NULL, dom,
		};

		for (i = 0; i < (int)ARRAY_SIZE(doms); i++) {
			ret = perf_evlist_poll__sync(evlist, doms[i]);
			if (ret && !err)
				err = ret;
		}
	}
	return err;
}

/* Domains. */

static int perf_mmap_domain__add_fd(struct perf_mmap_domain *dom, int fd, bool poll)
{
	if (dom->nr_fds == dom->nr_alloc) {
		int nr_alloc = dom->nr_alloc ? dom->nr_alloc * 2 : 4;
		void *fds = realloc(dom->fds, nr_alloc * sizeof(*dom->fds));

		if (!fds)
			return -ENOMEM;
		dom->fds = fds;
		dom->nr_alloc = nr_alloc;
	}
	dom->fds[dom->nr_fds].fd = fd;
	dom->fds[dom->nr_fds].poll = poll;
	dom->nr_fds++;
	return 0;
}

static void perf_mmap_domain__del_fd(struct perf_mmap_domain *dom, int fd)
{
	int i;

	for (i = 0; i < dom->nr_fds; i++) {
		if (dom->fds[i].fd == fd) {
			dom->fds[i] = dom->fds[--dom->nr_fds];
			return;
		}
	}
}

/* The live domain of @tid on @map, created on first use with a reference on @map. */
static struct perf_mmap_domain *
perf_evlist__mmap_domain_get(struct perf_evlist *evlist, struct perf_mmap *map, pid_t tid)
{
	struct perf_evlist_thread *th = NULL;
	struct perf_mmap_domain *dom;

	if (tid < 0) {
		if (map->cpu_domain)
			return map->cpu_domain;
	} else {
		th = perf_evlist__thread_get(evlist, tid);
		if (!th)
			return NULL;
		list_for_each_entry(dom, &th->domain_list, thread_node)
			if (dom->map == map)
				return dom;
	}

	dom = zalloc(sizeof(*dom));
	if (!dom)
		return NULL;
	dom->map = map;
	dom->tid = tid;
	dom->poll_fd = -1;
	INIT_HLIST_NODE(&dom->poll_node);
	INIT_LIST_HEAD(&dom->thread_node);
	list_add_tail(&dom->map_node, &map->domain_list);
	if (th) {
		dom->thread = th;
		list_add_tail(&dom->thread_node, &th->domain_list);
	} else
		map->cpu_domain = dom;
	perf_mmap__get(map);
	return dom;
}

/*
 * Mark @dom dead: unregister it, take it off its ring buffer and drop its
 * reference - into *@keep when given, the caller then owns it. Returns true
 * when the ring buffer has no writer left; it is then gone from the evlist.
 */
static bool perf_evlist__mmap_domain_put(struct perf_evlist *evlist,
					 struct perf_mmap_domain *dom,
					 struct perf_mmap **keep)
{
	struct perf_mmap *map = dom->map;
	bool lost;

	perf_evlist_poll__del(evlist, dom);
	dom->dead = true;
	list_del_init(&dom->map_node);
	if (map->cpu_domain == dom)
		map->cpu_domain = NULL;
	if (map->poll_domain == dom)
		map->poll_domain = NULL;

	lost = list_empty(&map->domain_list);
	if (lost)
		perf_evlist__mmap_unregister(evlist, map);
	else
		perf_evlist_poll__update(evlist, map, NULL, NULL);
	if (dom->thread && dom->thread->hup_domain == dom) {
		dom->thread->hup_domain = NULL;
		perf_evlist_poll__update(evlist, NULL, dom->thread, NULL);
	}
	dom->map = NULL;

	if (keep)
		*keep = map;
	else
		perf_mmap__put(map);
	return lost;
}

/* A dead domain goes with its last fd. */
static void perf_evlist__mmap_domain_free(struct perf_mmap_domain *dom)
{
	list_del(&dom->thread_node);
	free(dom->fds);
	free(dom);
}

/* Writer fds. */

/*
 * Any writer still attached can be the target of PERF_EVENT_IOC_SET_OUTPUT:
 * the kernel redirects into the ring buffer of the target, whichever event
 * mapped it. The fd that mapped it may be closed already: the VMA holds its
 * file, so the event and its ring buffer stay until munmap.
 */
static int perf_evlist__mmap_output_fd(struct perf_mmap *map)
{
	struct perf_mmap_domain *dom;

	list_for_each_entry(dom, &map->domain_list, map_node)
		if (dom->nr_fds)
			return dom->fds[0].fd;
	return -1;
}

/*
 * The first fd that needs a given (cpu, tid, overwrite) binding creates and
 * maps the ring buffer; every later fd - from this evsel or from any other
 * one - redirects its output into it. Either way the fd joins the domain of
 * its task, or the cpu domain for pid=-1 and cgroup fds.
 *
 * Kernel constraints of PERF_EVENT_IOC_SET_OUTPUT, perf_event_set_output()
 * fails with -EINVAL otherwise, and they make the ring buffer key (cpu, tid,
 * overwrite): the target is on the same cpu, and for cpu == -1 on the same
 * task; both use the same clock and the same write_backward; the redirected
 * fd has no mmap of its own. The target may be any event on the ring buffer,
 * see perf_evlist__mmap_output_fd().
 */
int perf_evlist__mmap_attach_fd(struct perf_evlist *evlist,
				struct perf_evsel_fd *entry)
{
	struct perf_evsel *evsel = entry->evsel;
	bool overwrite = evsel->attr.write_backward;
	bool has_id = evsel->attr.read_format & PERF_FORMAT_ID;
	struct perf_mmap_domain *dom;
	struct perf_mmap *map;
	pid_t bind_tid, tid;
	int bind_cpu, err;

	if (!evlist->mmaped || !evsel->mmaped)
		return has_id ? perf_evlist__id_add_fd(evlist, entry) : 0;
	if (entry->domain)
		return 0;

	/*
	 * A cpu-bound fd writes into the ring buffer of its cpu, whatever its
	 * task; a fd with cpu == -1 into the one of its thread.
	 */
	bind_cpu = entry->cpu;
	bind_tid = entry->cpu >= 0 ? -1 : entry->tid;
	tid = (entry->tid < 0 || entry->cgroup) ? -1 : entry->tid;

	map = perf_evlist__find_mmap(evlist, bind_cpu, bind_tid, overwrite);
	if (map) {
		if (ioctl(entry->fd, PERF_EVENT_IOC_SET_OUTPUT,
			  perf_evlist__mmap_output_fd(map)) != 0)
			return -errno;
		perf_mmap__get(map);
	} else {
		map = perf_evlist__mmap_create(evlist, bind_cpu, bind_tid, overwrite, entry->fd);
		if (!map)
			return -ENOMEM;
	}

	dom = perf_evlist__mmap_domain_get(evlist, map, tid);
	if (!dom || perf_mmap_domain__add_fd(dom, entry->fd, !evsel->system_wide)) {
		if (dom && !dom->nr_fds) {
			perf_evlist__mmap_domain_put(evlist, dom, NULL);
			perf_evlist__mmap_domain_free(dom);
		} else if (list_empty(&map->domain_list))
			perf_evlist__mmap_unregister(evlist, map);
		perf_mmap__put(map);
		return -ENOMEM;
	}
	entry->domain = dom;
	/* The domain holds its own reference now. */
	perf_mmap__put(map);

	err = perf_evlist_poll__update(evlist, map, dom->thread, dom);
	if (!err && has_id)
		err = perf_evlist__id_add_fd(evlist, entry);
	if (err)
		perf_evlist__mmap_detach_fd(evlist, entry);
	return err;
}

/*
 * The last fd of a live domain takes the domain - and possibly the ring
 * buffer - with it; the fds of a dead domain only free the domain.
 */
void perf_evlist__mmap_detach_fd(struct perf_evlist *evlist,
				 struct perf_evsel_fd *entry)
{
	struct perf_mmap_domain *dom;

	perf_evlist__id_del(entry);

	dom = entry->domain;
	if (!dom)
		return;
	entry->domain = NULL;

	if (dom->poll_fd == entry->fd)
		perf_evlist_poll__del(evlist, dom);
	if (!dom->dead && dom->nr_fds == 1)
		perf_evlist__mmap_domain_put(evlist, dom, NULL);
	if (dom->dead) {
		perf_mmap_domain__del_fd(dom, entry->fd);
		if (!dom->nr_fds)
			perf_evlist__mmap_domain_free(dom);
		return;
	}

	/* Another fd of the domain takes over its roles. */
	perf_mmap_domain__del_fd(dom, entry->fd);
	perf_evlist_poll__update(evlist, dom->map, dom->thread, dom);
}

/*
 * Thread exit.
 *
 * Only evsels following the evlist thread map are driven from here, as in
 * perf_evlist__add_thread().
 */

static bool perf_evlist__thread_evsel(struct perf_evsel *evsel)
{
	return !evsel->own_threads && !evsel->system_wide;
}

/* Close the fds of @tid, which also frees its dead domains, and forget it. */
static void perf_evlist__thread_close(struct perf_evlist *evlist, pid_t tid)
{
	struct perf_evsel *evsel, **order = perf_evlist__open_order(evlist);
	struct perf_evlist_thread *th;
	int i;

	if (order) {
		for (i = evlist->nr_entries - 1; i >= 0; i--) {
			if (perf_evlist__thread_evsel(order[i]))
				perf_evsel__del_thread(order[i], tid);
		}
		free(order);
	} else {
		perf_evlist__for_each_entry_reverse(evlist, evsel) {
			if (perf_evlist__thread_evsel(evsel))
				perf_evsel__del_thread(evsel, tid);
		}
	}
	th = perf_evlist__thread_find(evlist, tid);
	if (th)
		perf_evlist__thread_free(evlist, th);
}

/*
 * Order matters, a group member needs the leader's fd for the new thread: open
 * in perf_evlist__open_order().
 */
int perf_evlist__add_thread(struct perf_evlist *evlist, pid_t pid)
{
	struct perf_evlist_thread *th = perf_evlist__thread_find(evlist, pid);
	struct perf_evsel_thread *th_fds;
	struct perf_evsel_fd *entry;
	struct perf_evsel *evsel;
	struct perf_evsel **added;
	int err, nr = 0, pass, i;

	/*
	 * A previous thread with this tid that is still on its way out - tid
	 * reuse, or a thread attached again. Drop it outright; a ring buffer
	 * it alone wrote into is released without reporting it.
	 */
	if (th && (th->hup || th->del))
		perf_evlist__thread_close(evlist, pid);

	/* In open order; the evsels taking the thread are compacted to the front. */
	added = perf_evlist__open_order(evlist);
	if (!added)
		return -ENOMEM;
	evlist->adding_thread = true;

	for (i = 0; i < evlist->nr_entries; i++) {
		evsel = added[i];
		if (!perf_evlist__thread_evsel(evsel))
			continue;
		if (!evsel->opened || evsel->any_thread)
			continue;
		if (perf_evsel__has_thread(evsel, pid))
			continue;

		err = perf_evsel__add_thread(evsel, pid);
		if (err < 0)
			goto out_del;
		added[nr++] = evsel;
	}
	/* Watermark evsels create the ring buffers, see perf_evlist__mmap(). */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < nr; i++) {
			evsel = added[i];
			if (!!evsel->attr.watermark != !pass)
				continue;
			th_fds = perf_evsel__thread_find(evsel, pid, false);
			if (!th_fds)
				continue;
			list_for_each_entry(entry, &th_fds->fd_list, thread_node) {
				err = perf_evlist__mmap_attach_fd(evlist, entry);
				if (err)
					goto out_del;
			}
		}
	}
	/* Open group members before enabling any of the new leaders. */
	for (pass = 0; pass < nr; pass++) {
		err = perf_evsel__enable_thread(added[pass], pid, true);
		if (err)
			goto out_del;
	}
	evlist->adding_thread = false;
	free(added);

	return 0;

out_del:
	evlist->adding_thread = false;
	while (nr)
		perf_evsel__del_thread(added[--nr], pid);
	th = perf_evlist__thread_find(evlist, pid);
	if (th && list_empty(&th->domain_list))
		perf_evlist__thread_free(evlist, th);
	free(added);
	return err;
}

/*
 * Kernel constraints:
 *  - the events of a task are all written by the time ptrace reports its
 *    exit: do_exit() runs perf_event_exit_task() before exit_notify();
 *  - the EPOLLHUP comes on every fd of the task once it exits, not before.
 *    Until then a running thread keeps writing: disable its fds.
 */
int perf_evlist__del_thread(struct perf_evlist *evlist, pid_t pid)
{
	struct perf_evlist_thread *th = perf_evlist__thread_find(evlist, pid);
	struct perf_mmap_domain *dom;
	struct perf_evsel *evsel;
	bool live = false;

	if (th && th->del)
		return 0;
	if (th && !th->hup)
		list_for_each_entry(dom, &th->domain_list, thread_node)
			live |= !dom->dead;

	/* Stop the fds and wait for the EPOLLHUP to give up the ring buffers. */
	if (live) {
		th->del = true;
		perf_evlist__for_each_entry(evlist, evsel) {
			if (perf_evlist__thread_evsel(evsel))
				perf_evsel__disable_thread(evsel, pid);
		}
		list_for_each_entry(dom, &th->domain_list, thread_node) {
			if (dom->map && dom->map->tid == pid)
				perf_evlist__mmap_unhash(dom->map);
		}
		return 0;
	}

	perf_evlist__thread_close(evlist, pid);
	return 1;
}

/*
 * Kernel constraint: when a task exits, every event in its context enters
 * PERF_EVENT_STATE_EXIT, not only the one of @fd, so the EPOLLHUP of one fd
 * ends every task domain of the thread. pid=-1 and cgroup events are not in a
 * task context: the cpu domain never sees EPOLLHUP.
 */
int perf_evlist__hup(struct perf_evlist *evlist, int fd, struct perf_evlist_hup *hup)
{
	struct perf_mmap_domain *dom = perf_evlist_poll__find(evlist, fd), *tmp;
	struct perf_evlist_thread *th;
	int nr = 0;

	memset(hup, 0, sizeof(*hup));
	hup->tid = -1;

	if (!dom || !dom->thread || dom->thread->hup)
		return 0;
	th = dom->thread;

	list_for_each_entry(dom, &th->domain_list, thread_node)
		if (!dom->dead)
			nr++;
	hup->maps = calloc(nr ?: 1, sizeof(*hup->maps));
	if (!hup->maps)
		return -ENOMEM;

	/* Set first: a dying thread needs no replacement hup fd. */
	th->hup = true;
	list_for_each_entry_safe(dom, tmp, &th->domain_list, thread_node) {
		struct perf_evlist_hup_map *m;

		if (dom->dead)
			continue;
		m = &hup->maps[hup->nr_maps++];
		m->retired = perf_evlist__mmap_domain_put(evlist, dom, &m->map);
	}
	hup->tid = th->tid;
	hup->complete = th->del;
	return 0;
}

void perf_evlist__hup_done(struct perf_evlist *evlist, struct perf_evlist_hup *hup)
{
	struct perf_evlist_thread *th;
	int i;

	for (i = 0; i < hup->nr_maps; i++)
		perf_mmap__put(hup->maps[i].map);
	zfree(&hup->maps);
	hup->nr_maps = 0;

	if (hup->complete) {
		th = perf_evlist__thread_find(evlist, hup->tid);
		if (th && th->hup && th->del)
			perf_evlist__thread_close(evlist, hup->tid);
	}
}

/* mmap / munmap / poll. */

static int
perf_evlist__mmap_evsel(struct perf_evlist *evlist, struct perf_evsel *evsel)
{
	struct perf_evsel_fd *entry;
	int err;

	evsel->mmaped = true;
	perf_evsel__for_each_fd(evsel, entry) {
		err = perf_evlist__mmap_attach_fd(evlist, entry);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Kernel constraint: rb->watermark is a property of the ring buffer, fixed by
 * the fd that mmap()s it; wakeup_events is a property of each event. So let
 * the watermark evsels create the ring buffers first (pass 0) and have the
 * wakeup_events evsels join them with SET_OUTPUT (pass 1). Within a pass,
 * evsels are processed in the order they were added.
 */
int perf_evlist__mmap(struct perf_evlist *evlist, int pages)
{
	struct perf_evsel *evsel;
	int pass, err;

	evlist->mmap_len = (pages + 1) * page_size;
	evlist->mmaped = true;

	for (pass = 0; pass < 2; pass++) {
		perf_evlist__for_each_entry(evlist, evsel) {
			if (!!evsel->attr.watermark != !pass)
				continue;
			err = perf_evlist__mmap_evsel(evlist, evsel);
			if (err) {
				perf_evlist__munmap(evlist);
				return err;
			}
		}
	}

	return 0;
}

/*
 * Detaching every fd releases every domain, and with them the ring buffers.
 *
 * Kernel constraint: when the last VMA of a ring buffer goes, perf_mmap_close()
 * detaches every writer redirected into it, they drop their events from then
 * on. So a ring buffer is unmapped only once it has no writer left.
 *
 * Unregister all poll fds up front and keep them unregistered: detaching fds
 * one by one would otherwise hand the roles of each detached fd over to the
 * next one, whatever the order of the evsels.
 */
void perf_evlist__munmap(struct perf_evlist *evlist)
{
	struct perf_evlist_thread *th;
	struct perf_mmap_domain *dom;
	struct perf_evsel_fd *entry;
	struct hlist_node *tmp;
	struct perf_evsel *evsel;
	int i;

	evlist->munmapping = true;
	for (i = 0; i < PERF_EVLIST__POLL_HLIST_SIZE; i++)
		hlist_for_each_entry_safe(dom, tmp, &evlist->epoll.poll_fd_heads[i], poll_node)
			perf_evlist_poll__del(evlist, dom);

	/* Also drops the sample ids. */
	perf_evlist__for_each_entry(evlist, evsel) {
		perf_evsel__for_each_fd(evsel, entry)
			perf_evlist__mmap_detach_fd(evlist, entry);
	}
	for (i = 0; i < PERF_EVLIST__THREAD_HLIST_SIZE; i++)
		hlist_for_each_entry_safe(th, tmp, &evlist->thread_heads[i], hnode)
			perf_evlist__thread_free(evlist, th);
	perf_evlist_poll__free(evlist);

	perf_evlist__for_each_entry(evlist, evsel)
		evsel->mmaped = false;

	evlist->mmaped = false;
	evlist->munmapping = false;
}

struct perf_mmap*
perf_evlist__next_mmap(struct perf_evlist *evlist, struct perf_mmap *map,
		       bool overwrite)
{
	struct list_head *head = overwrite ? &evlist->mmap_ovw_list
					   : &evlist->mmap_list;
	struct list_head *next = map ? map->node.next : head->next;

	return next == head ? NULL : list_entry(next, struct perf_mmap, node);
}

/*
 * Without an external loop the evlist polls its fds itself and handles
 * EPOLLHUP: @handle reads the ring buffer of every event, and on EPOLLHUP every
 * ring buffer of the exited thread before it lets go of them.
 */
int perf_evlist__poll_mmap(struct perf_evlist *evlist, int timeout, handle_mmap handle)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i, j, cnt;

	if (epoll->external)
		return -EINVAL;

	if (epoll->epfd < 0)
		return usleep(timeout * 1000);

	cnt = epoll_wait(epoll->epfd, epoll->events, epoll->maxevents, timeout);
	if (cnt < 0)
		return -errno;

	for (i = 0; i < cnt; i++) {
		int fd = epoll->events[i].data.fd;
		struct perf_mmap_domain *dom = perf_evlist_poll__find(evlist, fd);
		struct perf_evlist_hup hup;

		/* Gone with an earlier EPOLLHUP of this batch. */
		if (!dom)
			continue;
		if (handle)
			handle(dom->map);
		if (!(epoll->events[i].events & EPOLLHUP) ||
		    perf_evlist__hup(evlist, fd, &hup))
			continue;
		for (j = 0; handle && j < hup.nr_maps; j++)
			handle(hup.maps[j].map);
		perf_evlist__hup_done(evlist, &hup);
	}
	return epoll->nr_live ? cnt : -ENOENT;
}

int perf_evlist__poll(struct perf_evlist *evlist, int timeout)
{
	return perf_evlist__poll_mmap(evlist, timeout, NULL);
}

/*
 * Group every evsel of the evlist, as far as the kernel allows: a group lives
 * in one (cpu, task or cgroup) context, so the group is decided per fd rather
 * than per evsel, and the maps of the evsels need not match.
 *
 * The evsels are ranked by how much they cover: more cpus in their cpu map
 * first, then more threads in their thread map, then the evlist order, see
 * perf_evsel__rank_cmp(). On each (cpu, tid) the fd of the highest ranked
 * evsel that has one there leads, the others there are its members, as far
 * as perf_evsel__can_group() allows. E.g. with
 * event1 on cpu 0-7 and event2 on cpu 0-3, event1 leads everywhere; the group
 * on cpu 0 has two events, the one on cpu 7 only event1.
 *
 * The rank follows the maps the evsels are opened with; threads added later
 * do not change it. PERF_FORMAT_GROUP reads return as many members as the
 * group has on that (cpu, tid).
 */
void perf_evlist__set_leader(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	evlist->grouped = true;

	perf_evlist__for_each_entry(evlist, evsel) {
		evsel->leader = evsel;
		evsel->nr_members = 1;
	}
}

/* Whether some fd of @evsel leads a group with members. */
static bool perf_evsel__leads_members(struct perf_evlist *evlist, struct perf_evsel *evsel)
{
	struct perf_evsel_fd *entry, *member;
	struct perf_evsel *pos;

	perf_evsel__for_each_fd(evsel, entry) {
		if (entry->leader)
			continue;
		perf_evlist__for_each_entry(evlist, pos) {
			member = __perf_evsel__fd_find(pos, entry->cpu, entry->tid, entry->cgroup);
			if (member && member->leader == entry)
				return true;
		}
	}
	return false;
}

int perf_evlist__nr_groups(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;
	int nr_groups = 0;

	perf_evlist__for_each_evsel(evlist, evsel) {
		/* Grouped per fd: count the evsels leading somewhere, once open. */
		if (evlist->grouped) {
			if (perf_evsel__leads_members(evlist, evsel))
				nr_groups++;
			continue;
		}
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
