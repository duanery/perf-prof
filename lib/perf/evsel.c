// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
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
	int i;

	INIT_LIST_HEAD(&evsel->node);
	INIT_LIST_HEAD(&evsel->cpu_list);
	for (i = 0; i < PERF_EVSEL__CPU_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(&evsel->cpu_heads[i]);
	INIT_LIST_HEAD(&evsel->thread_list);
	for (i = 0; i < PERF_EVSEL__THREAD_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(&evsel->thread_heads[i]);
	for (i = 0; i < PERF_EVSEL__FD_HLIST_SIZE; i++)
		INIT_HLIST_HEAD(&evsel->fd_heads[i]);
	evsel->attr = *attr;
	evsel->idx  = idx;
	evsel->leader = evsel;
	evsel->nr_members = 1;
	evsel->init_enabled = !attr->disabled;
	evsel->enabled = !attr->disabled;
	evsel->bpf_prog_fd = -1;
	evsel->mmap_pages = -1;
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
	perf_thread_map__put(evsel->threads_snap);
	perf_cpu_map__put(evsel->cpus_snap);
	free(evsel->filter);
	if (evsel->bpf_prog_fd >= 0)
		close(evsel->bpf_prog_fd);
	perf_evsel__set_priv(evsel, NULL, NULL);
	free(evsel);
}

/*
 * The fds of an evsel.
 *
 * Each open fd is a struct perf_evsel_fd, hashed by the (cpu, tid) values it
 * was opened with and indexed along both dimensions: on the fd_list of its cpu
 * (struct perf_evsel_cpu) and on the fd_list of its thread (struct
 * perf_evsel_thread).
 */

static int perf_evsel__fd_hash(int cpu, pid_t tid)
{
	u32 key = ((u32)(cpu + 1) * 2654435761U) ^ (u32)(tid + 1);

	return hash_32(key, PERF_EVSEL__FD_HLIST_BITS);
}

struct perf_evsel_fd *__perf_evsel__fd_find(struct perf_evsel *evsel, int cpu,
					    pid_t tid, bool cgroup)
{
	struct perf_evsel_fd *entry;

	hlist_for_each_entry(entry, &evsel->fd_heads[perf_evsel__fd_hash(cpu, tid)], hnode) {
		if (entry->cpu == cpu && entry->tid == tid && entry->cgroup == cgroup)
			return entry;
	}
	return NULL;
}

struct perf_evsel_fd *perf_evsel__fd_find(struct perf_evsel *evsel, int cpu, pid_t tid)
{
	struct perf_evsel_fd *entry;

	hlist_for_each_entry(entry, &evsel->fd_heads[perf_evsel__fd_hash(cpu, tid)], hnode) {
		if (entry->cpu == cpu && entry->tid == tid)
			return entry;
	}
	return NULL;
}

int perf_evsel__fd(struct perf_evsel *evsel, int cpu, pid_t tid)
{
	struct perf_evsel_fd *entry = perf_evsel__fd_find(evsel, cpu, tid);

	return entry ? entry->fd : -1;
}

/* Cpu by cpu, in cpu map order; on a cpu in open order. */
struct perf_evsel_fd *perf_evsel__next_fd(struct perf_evsel *evsel,
					  struct perf_evsel_fd *prev)
{
	struct perf_evsel_cpu *c;
	struct list_head *next;

	if (prev) {
		c = prev->cpu_rec;
		next = prev->cpu_node.next;
	} else {
		if (list_empty(&evsel->cpu_list))
			return NULL;
		c = list_first_entry(&evsel->cpu_list, struct perf_evsel_cpu, node);
		next = c->fd_list.next;
	}
	while (next == &c->fd_list) {
		if (list_is_last(&c->node, &evsel->cpu_list))
			return NULL;
		c = list_next_entry(c, node);
		next = c->fd_list.next;
	}
	return list_entry(next, struct perf_evsel_fd, cpu_node);
}

int perf_evsel_fd__cpu(struct perf_evsel_fd *pos)
{
	return pos->cpu;
}

pid_t perf_evsel_fd__tid(struct perf_evsel_fd *pos)
{
	return pos->tid;
}

static int perf_evsel__thread_hash(pid_t tid)
{
	return hash_32((u32)(tid + 1), PERF_EVSEL__THREAD_HLIST_BITS);
}

struct perf_evsel_thread *perf_evsel__thread_find(struct perf_evsel *evsel,
						  pid_t tid, bool cgroup)
{
	struct perf_evsel_thread *th;

	hlist_for_each_entry(th, &evsel->thread_heads[perf_evsel__thread_hash(tid)], hnode) {
		if (th->tid == tid && th->cgroup == cgroup)
			return th;
	}
	return NULL;
}

/*
 * Find or create the record of a thread, with a reference: the evsel takes one
 * for each of its threads, see perf_evsel__get_threads().
 */
static struct perf_evsel_thread *
perf_evsel__thread_get(struct perf_evsel *evsel, pid_t tid, bool cgroup)
{
	struct perf_evsel_thread *th = perf_evsel__thread_find(evsel, tid, cgroup);

	if (th) {
		th->refcnt++;
		return th;
	}
	th = zalloc(sizeof(*th));
	if (!th)
		return NULL;
	th->tid = tid;
	th->cgroup = cgroup;
	th->refcnt = 1;
	INIT_LIST_HEAD(&th->fd_list);
	list_add_tail(&th->node, &evsel->thread_list);
	hlist_add_head(&th->hnode, &evsel->thread_heads[perf_evsel__thread_hash(tid)]);
	return th;
}

static void perf_evsel__thread_put(struct perf_evsel_thread *th)
{
	if (--th->refcnt)
		return;
	WARN_ON_ONCE(!list_empty(&th->fd_list));
	list_del(&th->node);
	hlist_del(&th->hnode);
	free(th);
}

static int perf_evsel__cpu_hash(int cpu)
{
	return hash_32((u32)(cpu + 1), PERF_EVSEL__CPU_HLIST_BITS);
}

struct perf_evsel_cpu *perf_evsel__cpu_find(struct perf_evsel *evsel, int cpu)
{
	struct perf_evsel_cpu *c;

	hlist_for_each_entry(c, &evsel->cpu_heads[perf_evsel__cpu_hash(cpu)], hnode) {
		if (c->cpu == cpu)
			return c;
	}
	return NULL;
}

/*
 * Find or create the record of a cpu, with a reference. The records are kept
 * in ascending cpu order, as in a cpu map, also for cpus added later.
 */
static struct perf_evsel_cpu *perf_evsel__cpu_get(struct perf_evsel *evsel, int cpu)
{
	struct perf_evsel_cpu *c = perf_evsel__cpu_find(evsel, cpu), *pos;

	if (c) {
		c->refcnt++;
		return c;
	}
	c = zalloc(sizeof(*c));
	if (!c)
		return NULL;
	c->cpu = cpu;
	c->refcnt = 1;
	INIT_LIST_HEAD(&c->fd_list);
	list_for_each_entry(pos, &evsel->cpu_list, node) {
		if (pos->cpu > cpu)
			break;
	}
	list_add_tail(&c->node, &pos->node);
	hlist_add_head(&c->hnode, &evsel->cpu_heads[perf_evsel__cpu_hash(cpu)]);
	return c;
}

static void perf_evsel__cpu_put(struct perf_evsel_cpu *c)
{
	if (--c->refcnt)
		return;
	WARN_ON_ONCE(!list_empty(&c->fd_list));
	list_del(&c->node);
	hlist_del(&c->hnode);
	free(c);
}

bool perf_evsel__has_thread(struct perf_evsel *evsel, pid_t pid)
{
	return perf_evsel__thread_find(evsel, pid, false) != NULL;
}

bool perf_evsel__has_cpu(struct perf_evsel *evsel, int cpu)
{
	return perf_evsel__cpu_find(evsel, cpu) != NULL;
}

/*
 * The evsel holds a reference on each of its cpus and threads, from
 * perf_evsel__open() or the add until the del or perf_evsel__close(): a cpu or
 * thread stays while it has no fd, the threads and cpus added later open on
 * it. Each fd holds a reference on its cpu and thread as well.
 */
static int perf_evsel__get_cpus(struct perf_evsel *evsel, struct perf_cpu_map *cpus)
{
	int cpu, idx;

	perf_cpu_map__for_each_cpu(cpu, idx, cpus) {
		if (!perf_evsel__cpu_find(evsel, cpu) &&
		    !perf_evsel__cpu_get(evsel, cpu))
			return -ENOMEM;
	}
	return 0;
}

static int perf_evsel__get_threads(struct perf_evsel *evsel, struct perf_thread_map *threads)
{
	int thread;

	for (thread = 0; thread < threads->nr; thread++) {
		pid_t tid = threads->map[thread].pid;
		bool cgroup = threads->map[thread].cgroup;

		if (!perf_evsel__thread_find(evsel, tid, cgroup) &&
		    !perf_evsel__thread_get(evsel, tid, cgroup))
			return -ENOMEM;
	}
	return 0;
}

/* Drop the evsel's references, after every fd is closed. */
static void perf_evsel__put_cpus(struct perf_evsel *evsel)
{
	struct perf_evsel_cpu *c, *tmp;

	list_for_each_entry_safe(c, tmp, &evsel->cpu_list, node)
		perf_evsel__cpu_put(c);
}

static void perf_evsel__put_threads(struct perf_evsel *evsel)
{
	struct perf_evsel_thread *th, *tmp;

	list_for_each_entry_safe(th, tmp, &evsel->thread_list, node)
		perf_evsel__thread_put(th);
}

/* The thread set no longer matches evsel->threads. */
static void perf_evsel__threads_changed(struct perf_evsel *evsel)
{
	evsel->threads_changed = true;
	perf_thread_map__put(evsel->threads_snap);
	evsel->threads_snap = NULL;
}

/* The cpu set no longer matches evsel->cpus. */
static void perf_evsel__cpus_changed(struct perf_evsel *evsel)
{
	evsel->cpus_changed = true;
	perf_cpu_map__put(evsel->cpus_snap);
	evsel->cpus_snap = NULL;
}

static int perf_evsel__mmap_one(struct perf_evsel *evsel, struct perf_evsel_fd *entry)
{
	struct perf_mmap_param mp = {
		.prot = PROT_READ | PROT_WRITE,
		.mask = (evsel->mmap_pages * page_size) - 1,
	};
	struct perf_mmap *map;
	int err;

	if (entry->mmap)
		return 0;
	map = perf_mmap__new(entry->cpu, entry->tid, false);
	if (!map)
		return -ENOMEM;
	err = perf_mmap__mmap(map, &mp, entry->fd);
	if (err) {
		perf_mmap__put(map);
		return err;
	}
	entry->mmap = map;
	return 0;
}

static int
sys_perf_event_open(struct perf_event_attr *attr,
		    pid_t pid, int cpu, int group_fd,
		    unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/*
 * Whether fds of @member may join groups led by fds of @leader.
 *
 * Kernel constraints, perf_event_open() fails with -EINVAL on a group_fd
 * otherwise:
 *  - only a group leader can be pinned or exclusive: @member must be neither,
 *    @leader may be either;
 *  - all events of a group use the same clock (use_clockid/clockid).
 *
 * keep_disable evsels stay out of groups: as a member it would run along its
 * leader, as a leader its members would never run.
 */
static bool perf_evsel__can_group(struct perf_evsel *leader, struct perf_evsel *member)
{
	if (leader->keep_disable || member->keep_disable)
		return false;
	if (member->attr.pinned || member->attr.exclusive)
		return false;
	if (leader->attr.use_clockid != member->attr.use_clockid)
		return false;
	return !leader->attr.use_clockid || leader->attr.clockid == member->attr.clockid;
}

/*
 * Fix the rank from the maps the evsel is opened with; threads added and
 * removed later leave it alone. A dummy map counts as one entry.
 */
void perf_evsel__set_rank(struct perf_evsel *evsel, struct perf_cpu_map *cpus,
			  struct perf_thread_map *threads)
{
	evsel->rank_cpus = perf_cpu_map__nr(cpus);
	evsel->rank_threads = perf_thread_map__nr(threads);
}

/*
 * Compare the ranks of @a and @b as values: > 0 when @a ranks higher, < 0 when
 * lower, 0 only for the same evsel.
 *
 * The wider an evsel the higher it ranks: more cpus first, then more threads,
 * see perf_evsel__set_rank(). Between equally wide evsels the one added to the
 * evlist earlier ranks higher.
 */
int perf_evsel__rank_cmp(struct perf_evsel *a, struct perf_evsel *b)
{
	if (a->rank_cpus != b->rank_cpus)
		return a->rank_cpus - b->rank_cpus;
	if (a->rank_threads != b->rank_threads)
		return a->rank_threads - b->rank_threads;
	return b->idx - a->idx;
}

/*
 * The fd a new fd at (@cpu, @tid) joins as group member, NULL to open alone
 * or as a leader.
 *
 * In a grouped evlist the group is decided per fd: the leader is the fd of the
 * highest ranked evsel that has one at the same (cpu, tid), see
 * perf_evlist__set_leader(). Otherwise evsel->leader leads; where it has no
 * fd, the member opens alone. An evsel joined to the open evlist, see
 * perf_evlist__add_evsel(), is grouped by its explicit leader only and leads
 * no fd of the others. Nothing joins an evsel on its way out.
 *
 * Kernel constraints on the group_fd of perf_event_open() (-EINVAL):
 *  - it is a group leader itself, not a member of another group: only fds
 *    with entry->leader == NULL are picked;
 *  - it is on the same cpu, and in the same context - the same task, or both
 *    per-cpu (cgroup events are per-cpu): only the fd at the same (cpu, tid,
 *    cgroup) is picked;
 *  - it is open before the member: the callers open in the rank order, see
 *    perf_evlist__open_order().
 * See also perf_evsel__can_group().
 */
static int perf_evsel__group_leader(struct perf_evsel *evsel, int cpu, pid_t tid,
				    bool cgroup, struct perf_evsel_fd **leader)
{
	struct perf_evlist *evlist = evsel->evlist;
	struct perf_evsel_fd *entry;
	struct perf_evsel *pos;

	*leader = NULL;
	if (evlist && evlist->grouped && !evsel->joined) {
		perf_evlist__for_each_entry(evlist, pos) {
			if (pos == evsel || pos->joined || pos->del ||
			    !perf_evsel__can_group(pos, evsel))
				continue;
			entry = __perf_evsel__fd_find(pos, cpu, tid, cgroup);
			if (!entry || entry->leader)
				continue;
			if (!*leader || perf_evsel__rank_cmp(pos, (*leader)->evsel) > 0)
				*leader = entry;
		}
		return 0;
	}

	if (evsel->leader == evsel || evsel->leader->del)
		return 0;
	/* Leader must be already processed/open, if not it's a bug. */
	if (!evsel->leader->opened)
		return -ENOTCONN;
	*leader = __perf_evsel__fd_find(evsel->leader, cpu, tid, cgroup);
	return 0;
}

/*
 * Open the fd of @evsel at (@c, @t), or find the one already open. A new fd
 * takes a reference on @c and @t; perf_evsel__close_one() drops them.
 *
 * Kernel constraints:
 *  - a group member is scheduled along its leader: in an evlist it opens
 *    enabled (attr.disabled = 0) and is switched by the leader. Outside an
 *    evlist the leader may run already, see perf_evsel__set_leader(): it
 *    opens as the evsel says. Fds added to an open evsel open disabled and
 *    are enabled afterwards, see perf_evsel__enable_new_thread();
 *  - PERF_FLAG_FD_CLOEXEC exists since v3.14; older kernels fail it with
 *    -EINVAL, retry without.
 */
static int perf_evsel__open_one(struct perf_evsel *evsel, struct perf_evsel_cpu *c,
				struct perf_evsel_thread *t, struct perf_evsel_fd **out)
{
	unsigned long flags = PERF_FLAG_FD_CLOEXEC;
	struct perf_event_attr attr = evsel->attr;
	struct perf_evsel_fd *entry, *leader;
	int cpu = c->cpu;
	pid_t tid = t->tid;
	bool cgroup = t->cgroup;
	int fd, group_fd, err;

	entry = __perf_evsel__fd_find(evsel, cpu, tid, cgroup);
	if (entry)
		goto out;

	err = perf_evsel__group_leader(evsel, cpu, tid, cgroup, &leader);
	if (err < 0)
		return err;
	group_fd = leader ? leader->fd : -1;

	if (cgroup)
		flags |= PERF_FLAG_PID_CGROUP;
	if (evsel->opened)
		attr.disabled = 1;
	else if (leader && evsel->evlist)
		attr.disabled = 0;

	fd = sys_perf_event_open(&attr, tid, cpu, group_fd, flags);
	if (fd < 0) {
		if (errno == EINVAL) {
			flags &= ~PERF_FLAG_FD_CLOEXEC;
			fd = sys_perf_event_open(&attr, tid, cpu, group_fd, flags);
		}
		if (fd < 0)
			return -errno;
	}

	entry = zalloc(sizeof(*entry));
	if (!entry) {
		close(fd);
		return -ENOMEM;
	}
	entry->evsel = evsel;
	entry->cpu = cpu;
	entry->tid = tid;
	entry->cgroup = cgroup;
	entry->fd = fd;
	entry->cpu_rec = c;
	entry->thread = t;
	entry->leader = leader;
	c->refcnt++;
	t->refcnt++;
	list_add_tail(&entry->cpu_node, &c->fd_list);
	list_add_tail(&entry->thread_node, &t->fd_list);
	hlist_add_head(&entry->hnode, &evsel->fd_heads[perf_evsel__fd_hash(cpu, tid)]);
	evsel->nr_fds++;
out:
	if (out)
		*out = entry;
	return 0;
}

int perf_evsel__open(struct perf_evsel *evsel, struct perf_cpu_map *cpus,
		     struct perf_thread_map *threads)
{
	struct perf_evsel_thread *t;
	struct perf_evsel_cpu *c;
	int err = 0;

	if (evsel->opened)
		return -EBUSY;

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
	 * The maps are the input: one fd per (cpu, thread) value of them. They
	 * do not change once the evsel is open; from here on the evsel works
	 * on its cpu and thread records.
	 */
	if (evsel->cpus != cpus) {
		perf_cpu_map__put(evsel->cpus);
		evsel->cpus = perf_cpu_map__get(cpus);
	}
	if (evsel->threads != threads) {
		perf_thread_map__put(evsel->threads);
		evsel->threads = perf_thread_map__get(threads);
	}

	if (evsel->leader != evsel && evsel->evlist && !evsel->evlist->grouped) {
		evsel->attr.disabled = evsel->leader->attr.disabled;
		evsel->init_enabled = 0;
	}
	evsel->enabled = !evsel->attr.disabled;
	evsel->any_thread = threads->nr == 1 && threads->map[0].pid == -1;
	evsel->any_cpu = cpus->nr == 1 && cpus->map[0] == -1;
	perf_evsel__set_rank(evsel, cpus, threads);
	err = perf_evsel__get_cpus(evsel, cpus);
	if (!err)
		err = perf_evsel__get_threads(evsel, threads);
	if (err)
		goto out_err;

	/* Thread by thread in thread map order: the records were created so. */
	list_for_each_entry(c, &evsel->cpu_list, node) {
		list_for_each_entry(t, &evsel->thread_list, node) {
			err = perf_evsel__open_one(evsel, c, t, NULL);
			if (err < 0)
				goto out_err;
		}
	}

	evsel->opened = true;
	return 0;

out_err:
	perf_evsel__close_fd(evsel);
	perf_evsel__put_cpus(evsel);
	perf_evsel__put_threads(evsel);
	return err;
}

/*
 * Close one fd, and drop its references on its cpu and thread records: they
 * go once the evsel let go of them too.
 *
 * Kernel constraints:
 *  - close() does not free an event whose file a VMA of its ring buffer still
 *    holds; it keeps writing until munmap unless disabled first;
 *  - freeing a group leader turns its members into events of their own
 *    (perf_group_detach()), each in the state it has itself: members open
 *    enabled and only follow the leader, so set their state to the one of
 *    their evsel. A leader held by a VMA keeps its members; they stay with a
 *    disabled leader.
 */
static void perf_evsel__close_one(struct perf_evsel *evsel, struct perf_evsel_fd *entry)
{
	struct perf_evlist *evlist = evsel->evlist;
	struct perf_evsel_fd *member;
	struct perf_evsel *pos;

	ioctl(entry->fd, PERF_EVENT_IOC_DISABLE, 0);
	if (evlist) {
		perf_evlist__mmap_detach_fd(evlist, entry);
		perf_evlist__for_each_entry(evlist, pos) {
			member = __perf_evsel__fd_find(pos, entry->cpu, entry->tid, entry->cgroup);
			if (!member || member->leader != entry)
				continue;
			member->leader = NULL;
			ioctl(member->fd, pos->enabled && !pos->keep_disable ?
			      PERF_EVENT_IOC_ENABLE : PERF_EVENT_IOC_DISABLE, 0);
		}
	}
	if (entry->mmap)
		perf_mmap__put(entry->mmap);
	close(entry->fd);
	list_del(&entry->cpu_node);
	list_del(&entry->thread_node);
	hlist_del(&entry->hnode);
	perf_evsel__cpu_put(entry->cpu_rec);
	perf_evsel__thread_put(entry->thread);
	evsel->nr_fds--;
	free(entry);
}

void perf_evsel__close_fd(struct perf_evsel *evsel)
{
	struct perf_evsel_fd *entry, *tmp;
	struct perf_evsel_cpu *c;

	list_for_each_entry_reverse(c, &evsel->cpu_list, node) {
		list_for_each_entry_safe_reverse(entry, tmp, &c->fd_list, cpu_node)
			perf_evsel__close_one(evsel, entry);
	}
}

void perf_evsel__close(struct perf_evsel *evsel)
{
	perf_evsel__close_fd(evsel);
	perf_evsel__put_cpus(evsel);
	perf_evsel__put_threads(evsel);
	evsel->mmap_pages = -1;
	evsel->opened = false;
	evsel->threads_changed = false;
	perf_thread_map__put(evsel->threads_snap);
	evsel->threads_snap = NULL;
	evsel->cpus_changed = false;
	perf_cpu_map__put(evsel->cpus_snap);
	evsel->cpus_snap = NULL;
}

/*
 * Close the fds of @cpu, last opened first. The evsel keeps its reference on
 * the cpu: threads added later still open on it. perf_evsel__del_cpu() drops
 * it.
 */
void perf_evsel__close_cpu(struct perf_evsel *evsel, int cpu)
{
	struct perf_evsel_cpu *c = perf_evsel__cpu_find(evsel, cpu);
	struct perf_evsel_fd *entry, *tmp;

	if (!c)
		return;
	list_for_each_entry_safe_reverse(entry, tmp, &c->fd_list, cpu_node)
		perf_evsel__close_one(evsel, entry);
}

/*
 * Close the fds of @pid, last opened first. The evsel keeps its reference on
 * the thread: cpus added later still open for it. perf_evsel__del_thread()
 * drops it.
 */
void perf_evsel__close_thread(struct perf_evsel *evsel, pid_t pid)
{
	struct perf_evsel_thread *th = perf_evsel__thread_find(evsel, pid, false);
	struct perf_evsel_fd *entry, *tmp;

	if (!th)
		return;
	list_for_each_entry_safe_reverse(entry, tmp, &th->fd_list, thread_node)
		perf_evsel__close_one(evsel, entry);
}

void perf_evsel__munmap(struct perf_evsel *evsel)
{
	struct perf_evsel_fd *entry;

	if (evsel->mmap_pages < 0)
		return;

	perf_evsel__for_each_fd(evsel, entry) {
		if (entry->mmap) {
			perf_mmap__put(entry->mmap);
			entry->mmap = NULL;
		}
	}
	evsel->mmap_pages = -1;
}

int perf_evsel__mmap(struct perf_evsel *evsel, int pages)
{
	struct perf_evsel_fd *entry;
	int ret;

	if (!evsel->nr_fds || evsel->mmap_pages >= 0)
		return -EINVAL;

	evsel->mmap_pages = pages;
	perf_evsel__for_each_fd(evsel, entry) {
		ret = perf_evsel__mmap_one(evsel, entry);
		if (ret) {
			perf_evsel__munmap(evsel);
			return ret;
		}
	}

	return 0;
}

void *perf_evsel__mmap_base(struct perf_evsel *evsel, int cpu, pid_t tid)
{
	struct perf_evsel_fd *entry = perf_evsel__fd_find(evsel, cpu, tid);

	if (!entry || !entry->mmap)
		return NULL;

	return entry->mmap->base;
}

/*
 * The size to read() a counter with. The kernel fails a read() shorter than
 * the event's read size with -ENOSPC; with PERF_FORMAT_GROUP that size counts
 * the members the group has on that (cpu, tid), which differ per fd in a
 * grouped evlist. So size for every evsel of the evlist being a member; the
 * kernel returns as much as there is.
 */
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
		nr = evsel->evlist && evsel->evlist->grouped ?
			evsel->evlist->nr_entries : evsel->nr_members;
		size += sizeof(u64);
	}

	size += entry * nr;
	return size;
}

/*
 * Read the counter at (@cpu, @tid). With PERF_FORMAT_GROUP the kernel reads
 * the whole group of the fd's leader, also when reading a member; read the
 * leaders only, see perf_evsel__is_group_leader(). The read is shorter than
 * perf_evsel__read_size() when the group has fewer members there.
 */
int perf_evsel__read(struct perf_evsel *evsel, int cpu, pid_t tid,
		     struct perf_counts_values *count)
{
	size_t size = perf_evsel__read_size(evsel);
	u64 read_format = evsel->attr.read_format;
	struct perf_evsel_fd *entry = perf_evsel__fd_find(evsel, cpu, tid);
	ssize_t ret;

	memset(count, 0, sizeof(*count));

	if (!entry)
		return -EINVAL;

	if (entry->mmap &&
	    !(read_format & (PERF_FORMAT_ID | PERF_FORMAT_LOST)) &&
	    !perf_mmap__read_self(entry->mmap, count))
		return 0;

	do {
		ret = read(entry->fd, count->values, size);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0)
		return ret < 0 ? -errno : -EIO;

	return 0;
}

/*
 * Run @ioc on one fd.
 *
 * Kernel constraint: with PERF_IOC_FLAG_GROUP, ENABLE/DISABLE apply to the
 * group leader of the fd and all its members, whichever fd of the group gets
 * it. So members are skipped: their leader takes them along.
 */
static int perf_evsel__ioctl_one(struct perf_evsel_fd *entry, int ioc, void *arg)
{
	if (arg == (void *)PERF_IOC_FLAG_GROUP &&
	    (ioc == PERF_EVENT_IOC_ENABLE || ioc == PERF_EVENT_IOC_DISABLE) &&
	    entry->leader)
		return 0;

	return ioctl(entry->fd, ioc, arg);
}

/* Run @ioc on every fd of the evsel. */
static int perf_evsel__run_ioctl(struct perf_evsel *evsel, int ioc, void *arg)
{
	struct perf_evsel_fd *entry;
	struct perf_evsel_cpu *c;
	int err;

	list_for_each_entry(c, &evsel->cpu_list, node) {
		list_for_each_entry(entry, &c->fd_list, cpu_node) {
			err = perf_evsel__ioctl_one(entry, ioc, arg);
			if (err)
				return err;
		}
	}
	return 0;
}

/* Run @ioc on the fds of @cpu only; nothing to do if the evsel has none there. */
static int perf_evsel__run_ioctl_cpu(struct perf_evsel *evsel, int cpu, int ioc, void *arg)
{
	struct perf_evsel_cpu *c = perf_evsel__cpu_find(evsel, cpu);
	struct perf_evsel_fd *entry;
	int err;

	if (!c)
		return 0;
	list_for_each_entry(entry, &c->fd_list, cpu_node) {
		err = perf_evsel__ioctl_one(entry, ioc, arg);
		if (err)
			return err;
	}
	return 0;
}

/* Run @ioc on the fds of @pid only; nothing to do if the evsel has none of it. */
static int perf_evsel__run_ioctl_thread(struct perf_evsel *evsel, pid_t pid, int ioc, void *arg)
{
	struct perf_evsel_thread *th = perf_evsel__thread_find(evsel, pid, false);
	struct perf_evsel_fd *entry;
	int err;

	if (!th)
		return 0;
	list_for_each_entry(entry, &th->fd_list, thread_node) {
		err = perf_evsel__ioctl_one(entry, ioc, arg);
		if (err)
			return err;
	}
	return 0;
}

int perf_evsel__enable_cpu(struct perf_evsel *evsel, int cpu)
{
	return perf_evsel__run_ioctl_cpu(evsel, cpu, PERF_EVENT_IOC_ENABLE, NULL);
}

int perf_evsel__enable_thread(struct perf_evsel *evsel, pid_t pid)
{
	return perf_evsel__run_ioctl_thread(evsel, pid, PERF_EVENT_IOC_ENABLE, NULL);
}

int perf_evsel__enable(struct perf_evsel *evsel)
{
	if (evsel->keep_disable) return 0;

	evsel->enabled = 1;
	if (evsel->init_enabled) {
		evsel->init_enabled = 0;
		return 0;
	}

	return perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_ENABLE, NULL);
}

/*
 * PERF_IOC_FLAG_GROUP goes to the fds that lead their group or are alone, the
 * members follow. An evsel may lead on some (cpu, tid) and be a member on
 * others, and a member of an explicit group opens alone where its leader has
 * no fd.
 */
int perf_evsel__enable_group(struct perf_evsel *evsel)
{
	if (evsel->keep_disable) return 0;

	evsel->enabled = 1;
	if (evsel->init_enabled) {
		evsel->init_enabled = 0;
		return 0;
	}

	return perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_ENABLE, (void *)PERF_IOC_FLAG_GROUP);
}


int perf_evsel__disable_cpu(struct perf_evsel *evsel, int cpu)
{
	return perf_evsel__run_ioctl_cpu(evsel, cpu, PERF_EVENT_IOC_DISABLE, NULL);
}

int perf_evsel__disable_thread(struct perf_evsel *evsel, pid_t pid)
{
	return perf_evsel__run_ioctl_thread(evsel, pid, PERF_EVENT_IOC_DISABLE, NULL);
}

int perf_evsel__disable(struct perf_evsel *evsel)
{
	if (evsel->keep_disable) return 0;

	evsel->init_enabled = 0;
	evsel->enabled = 0;
	return perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_DISABLE, NULL);
}

int perf_evsel__disable_group(struct perf_evsel *evsel)
{
	if (evsel->keep_disable) return 0;

	evsel->init_enabled = 0;
	evsel->enabled = 0;
	return perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_DISABLE, (void *)PERF_IOC_FLAG_GROUP);
}

void perf_evsel__keep_disable(struct perf_evsel *evsel, bool keep_disable)
{
	if (keep_disable && !evsel->attr.disabled) {
		evsel->attr.disabled = 1;
		evsel->init_enabled = 0;
	}
	evsel->keep_disable = keep_disable;
}

/*
 * Kernel constraint: a trace event filter can be set once per fd, a second
 * PERF_EVENT_IOC_SET_FILTER fails with -EEXIST (ftrace_profile_set_filter()).
 */
int perf_evsel__apply_filter_cpu(struct perf_evsel *evsel, const char *filter, int cpu)
{
	return perf_evsel__run_ioctl_cpu(evsel, cpu, PERF_EVENT_IOC_SET_FILTER, (void *)filter);
}

int perf_evsel__apply_filter_thread(struct perf_evsel *evsel, const char *filter, pid_t pid)
{
	return perf_evsel__run_ioctl_thread(evsel, pid, PERF_EVENT_IOC_SET_FILTER, (void *)filter);
}

/*
 * Kernel constraint: a trace event filter can be set once per fd, a second
 * PERF_EVENT_IOC_SET_FILTER fails with -EEXIST. It is remembered for the fds
 * of threads added later, which need it set on their own, see
 * perf_evsel__add_thread().
 */
int perf_evsel__apply_filter(struct perf_evsel *evsel, const char *filter)
{
	int err;
	char *dup = filter ? strdup(filter) : NULL;

	if (filter && !dup)
		return -ENOMEM;

	err = perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_SET_FILTER,
				    (void *)filter);
	if (!err) {
		free(evsel->filter);
		evsel->filter = dup;
	} else
		free(dup);
	return err;
}

/*
 * Kernel constraint: a BPF program can be attached once per fd, a second
 * PERF_EVENT_IOC_SET_BPF fails with -EEXIST. A dup of @prog_fd is kept for the
 * fds of threads added later.
 */
int perf_evsel__set_bpf(struct perf_evsel *evsel, unsigned int prog_fd)
{
	int err;
	int held_fd = fcntl(prog_fd, F_DUPFD_CLOEXEC, 0);

	if (held_fd < 0)
		return -errno;

	err = perf_evsel__run_ioctl(evsel, PERF_EVENT_IOC_SET_BPF,
				    (void *)(unsigned long)prog_fd);
	if (!err) {
		if (evsel->bpf_prog_fd >= 0)
			close(evsel->bpf_prog_fd);
		evsel->bpf_prog_fd = held_fd;
	} else
		close(held_fd);
	return err;
}

void perf_evsel__set_own_cpus(struct perf_evsel *evsel, struct perf_cpu_map *own_cpus)
{
	if (evsel->opened || evsel->own_cpus == own_cpus)
		return;
	perf_cpu_map__get(own_cpus);
	perf_cpu_map__put(evsel->own_cpus);
	evsel->own_cpus = own_cpus;
	perf_cpu_map__put(evsel->cpus);
	evsel->cpus = perf_cpu_map__get(own_cpus ?: (evsel->evlist ? evsel->evlist->user_requested_cpus : NULL));
}

void perf_evsel__set_own_threads(struct perf_evsel *evsel, struct perf_thread_map *own_threads)
{
	if (evsel->opened)
		return;
	perf_thread_map__get(own_threads);
	perf_thread_map__put(evsel->own_threads);
	evsel->own_threads = own_threads;
	perf_thread_map__put(evsel->threads);
	evsel->threads = perf_thread_map__get(own_threads ?: (evsel->evlist ? evsel->evlist->user_requested_threads : NULL));
}

void perf_evsel__set_priv(struct perf_evsel *evsel, void *priv,
			  void (*destructor)(void *priv))
{
	if (evsel->priv_destructor && evsel->priv != priv)
		evsel->priv_destructor(evsel->priv);
	evsel->priv = priv;
	evsel->priv_destructor = destructor;
}

void *perf_evsel__priv(struct perf_evsel *evsel)
{
	return evsel->priv;
}

int perf_evsel__set_leader(struct perf_evsel *evsel, struct perf_evsel *leader)
{
	struct perf_evsel_fd *entry;

	if (!leader)
		leader = evsel;
	if (evsel->opened)
		return -EBUSY;
	if (leader == evsel->leader)
		return 0;

	if (leader != evsel) {
		if (!leader->opened || leader->del || leader->leader != leader)
			return -EINVAL;
		/* Kernel constraint: the group_fd leads its group, see perf_evsel__group_leader(). */
		perf_evsel__for_each_fd(leader, entry) {
			if (entry->leader)
				return -EINVAL;
		}
		if (evsel->nr_members > 1 || !perf_evsel__can_group(leader, evsel))
			return -EINVAL;
	}

	if (evsel->leader != evsel)
		evsel->leader->nr_members--;
	if (leader != evsel)
		leader->nr_members++;
	evsel->leader = leader;
	return 0;
}

bool perf_evsel__is_group_leader(struct perf_evsel *evsel, int cpu, pid_t tid)
{
	struct perf_evsel_fd *entry = perf_evsel__fd_find(evsel, cpu, tid);

	return entry && !entry->leader;
}

/*
 * Enable a new fd. With @group, as in perf_evsel__run_ioctl(): leaders get
 * PERF_IOC_FLAG_GROUP and take their members along.
 */
static int perf_evsel__enable_new(struct perf_evsel_fd *entry, bool group)
{
	if (ioctl(entry->fd, PERF_EVENT_IOC_ENABLE,
		  group && !entry->leader ? PERF_IOC_FLAG_GROUP : 0) < 0)
		return -errno;
	return 0;
}

/* Enable the fds of a cpu added later, see perf_evsel__enable_new(). */
int perf_evsel__enable_new_cpu(struct perf_evsel *evsel, int cpu, bool group)
{
	struct perf_evsel_cpu *c = perf_evsel__cpu_find(evsel, cpu);
	struct perf_evsel_fd *entry;
	int err;

	if (!c || !evsel->enabled || evsel->keep_disable)
		return 0;
	list_for_each_entry(entry, &c->fd_list, cpu_node) {
		err = perf_evsel__enable_new(entry, group);
		if (err)
			return err;
	}
	return 0;
}

/* Enable the fds of a thread added later, see perf_evsel__enable_new(). */
int perf_evsel__enable_new_thread(struct perf_evsel *evsel, pid_t pid, bool group)
{
	struct perf_evsel_thread *th = perf_evsel__thread_find(evsel, pid, false);
	struct perf_evsel_fd *entry;
	int err;

	if (!th || !evsel->enabled || evsel->keep_disable)
		return 0;
	list_for_each_entry(entry, &th->fd_list, thread_node) {
		err = perf_evsel__enable_new(entry, group);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Open the fd at (@c, @t) on an open evsel, for a thread or a cpu added later.
 * It opens disabled, see perf_evsel__open_one(); the caller enables it.
 *
 * Kernel constraint: filters and BPF programs are per fd, the new fd gets the
 * ones set on the evsel.
 */
static int perf_evsel__open_new(struct perf_evsel *evsel, struct perf_evsel_cpu *c,
				struct perf_evsel_thread *t)
{
	struct perf_evlist *evlist = evsel->evlist;
	struct perf_evsel_fd *entry;
	int err;

	err = perf_evsel__open_one(evsel, c, t, &entry);
	if (err < 0)
		return err;

	if (evsel->filter &&
	    ioctl(entry->fd, PERF_EVENT_IOC_SET_FILTER, evsel->filter) < 0)
		return -errno;
	if (evsel->bpf_prog_fd >= 0 &&
	    ioctl(entry->fd, PERF_EVENT_IOC_SET_BPF, evsel->bpf_prog_fd) < 0)
		return -errno;
	if (evsel->mmap_pages >= 0) {
		err = perf_evsel__mmap_one(evsel, entry);
		if (err)
			return err;
	}
	/* perf_evlist__add_thread()/add_cpu() attach them themselves, see there. */
	if (evlist && !evlist->adding)
		return perf_evlist__mmap_attach_fd(evlist, entry);
	return 0;
}

/* Open the fds of @pid on every cpu of the evsel. */
int perf_evsel__add_thread(struct perf_evsel *evsel, pid_t pid)
{
	struct perf_evlist *evlist = evsel->evlist;
	struct perf_evsel_thread *t;
	struct perf_evsel_cpu *c;
	int err = 0;

	if (pid < 0 || !evsel->opened)
		return -EINVAL;

	/* Opened for any thread already; naming one would double count. */
	if (evsel->any_thread)
		return -EINVAL;

	if (perf_evsel__has_thread(evsel, pid))
		return 0;

	perf_evsel__threads_changed(evsel);
	t = perf_evsel__thread_get(evsel, pid, false);
	if (!t)
		return -ENOMEM;
	list_for_each_entry(c, &evsel->cpu_list, node) {
		/* On its way out, see perf_evlist__del_cpu(). */
		if (c->del)
			continue;
		err = perf_evsel__open_new(evsel, c, t);
		if (err < 0)
			goto out_del;
	}

	if (!(evlist && evlist->adding)) {
		err = perf_evsel__enable_new_thread(evsel, pid, false);
		if (err)
			goto out_del;
	}

	return 0;

out_del:
	perf_evsel__del_thread(evsel, pid);
	return err;
}

int perf_evsel__del_thread(struct perf_evsel *evsel, pid_t pid)
{
	struct perf_evsel_thread *th;

	if (pid < 0 || !evsel->opened)
		return -EINVAL;

	th = perf_evsel__thread_find(evsel, pid, false);
	if (!th)
		return -ENOENT;

	perf_evsel__threads_changed(evsel);
	perf_evsel__close_thread(evsel, pid);
	perf_evsel__thread_put(th);
	return 0;
}

/*
 * Open the fds of @cpu for every thread of the evsel.
 *
 * Kernel constraint: perf_event_open() fails with -ESRCH for a task that is
 * exiting or gone. Such a thread still has its record until its exit is
 * reported, see perf_evlist__del_thread(); it gets no fd on the new cpu.
 */
int perf_evsel__add_cpu(struct perf_evsel *evsel, int cpu)
{
	struct perf_evlist *evlist = evsel->evlist;
	struct perf_evsel_thread *t;
	struct perf_evsel_cpu *c;
	int err = 0;

	if (cpu < 0 || !evsel->opened)
		return -EINVAL;

	/* Opened on any cpu (cpu == -1) already; naming one would double count. */
	if (evsel->any_cpu)
		return -EINVAL;

	if (perf_evsel__has_cpu(evsel, cpu))
		return 0;

	perf_evsel__cpus_changed(evsel);
	c = perf_evsel__cpu_get(evsel, cpu);
	if (!c)
		return -ENOMEM;
	list_for_each_entry(t, &evsel->thread_list, node) {
		err = perf_evsel__open_new(evsel, c, t);
		if (err == -ESRCH && t->tid >= 0 && !t->cgroup &&
		    !perf_evsel__fd_find(evsel, cpu, t->tid))
			continue;
		if (err < 0)
			goto out_del;
	}

	if (!(evlist && evlist->adding)) {
		err = perf_evsel__enable_new_cpu(evsel, cpu, false);
		if (err)
			goto out_del;
	}

	return 0;

out_del:
	perf_evsel__del_cpu(evsel, cpu);
	return err;
}

int perf_evsel__del_cpu(struct perf_evsel *evsel, int cpu)
{
	struct perf_evsel_cpu *c;

	if (cpu < 0 || !evsel->opened)
		return -EINVAL;

	c = perf_evsel__cpu_find(evsel, cpu);
	if (!c)
		return -ENOENT;

	perf_evsel__cpus_changed(evsel);
	perf_evsel__close_cpu(evsel, cpu);
	perf_evsel__cpu_put(c);
	return 0;
}

struct perf_cpu_map *perf_evsel__cpus(struct perf_evsel *evsel)
{
	struct perf_evsel_cpu *c;
	struct perf_cpu_map *map;
	int nr = 0;

	if (!evsel->cpus_changed)
		return evsel->cpus;
	if (evsel->cpus_snap)
		return evsel->cpus_snap;

	list_for_each_entry(c, &evsel->cpu_list, node)
		nr++;
	map = malloc(sizeof(*map) + sizeof(int) * (nr ?: 1));
	if (!map)
		return NULL;
	refcount_set(&map->refcnt, 1);

	nr = 0;
	list_for_each_entry(c, &evsel->cpu_list, node)
		map->map[nr++] = c->cpu;
	map->nr = nr;
	evsel->cpus_snap = map;
	return map;
}

struct perf_thread_map *perf_evsel__threads(struct perf_evsel *evsel)
{
	struct perf_evsel_thread *th;
	struct perf_thread_map *map;
	int nr = 0;

	if (!evsel->threads_changed)
		return evsel->threads;
	if (evsel->threads_snap)
		return evsel->threads_snap;

	list_for_each_entry(th, &evsel->thread_list, node)
		nr++;
	map = perf_thread_map__realloc(NULL, nr ?: 1);
	if (!map)
		return NULL;
	refcount_set(&map->refcnt, 1);

	nr = 0;
	list_for_each_entry(th, &evsel->thread_list, node) {
		map->map[nr].pid = th->tid;
		map->map[nr].cgroup = th->cgroup;
		nr++;
	}
	map->nr = nr;
	evsel->threads_snap = map;
	return map;
}

struct perf_event_attr *perf_evsel__attr(struct perf_evsel *evsel)
{
	return &evsel->attr;
}

uint64_t perf_evsel__get_id(struct perf_evsel *evsel, int cpu, pid_t tid)
{
	struct perf_evsel_fd *entry;

	if (evsel->system_wide)
		tid = -1;

	entry = perf_evsel__fd_find(evsel, cpu, tid);
	return entry ? entry->sid.id : 0;
}
