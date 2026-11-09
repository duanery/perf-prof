// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <time.h>
#include <linux/perf_event.h>
#include <perf/core.h>
#include <perf/cpumap.h>
#include <perf/threadmap.h>
#include <perf/evlist.h>
#include <perf/evsel.h>
#include <perf/mmap.h>
#include <perf/event.h>
#include <internal/evlist.h>
#include <internal/evsel.h>
#include <internal/mmap.h>
#include <internal/threadmap.h>
#include <internal/tests.h>
#include "tests.h"

static int libperf_print(enum libperf_print_level level,
			 const char *fmt, va_list ap)
{
	return tests_verbose ? vfprintf(stderr, fmt, ap) : 0;
}

static int tp_id(const char *sys, const char *name)
{
	char path[PATH_MAX];
	int id = -1;
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/kernel/debug/tracing/events/%s/%s/id", sys, name);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", &id) != 1)
		id = -1;
	fclose(f);
	return id;
}

static int nr_mmaps(struct perf_evlist *evlist)
{
	struct perf_mmap *map;
	int nr = 0;

	perf_evlist__for_each_mmap(evlist, map, false)
		nr++;
	return nr;
}

static int nr_ids(struct perf_evlist *evlist)
{
	struct perf_sample_id *sid;
	int i, nr = 0;

	for (i = 0; i < PERF_EVLIST__ID_HLIST_SIZE; i++)
		hlist_for_each_entry(sid, &evlist->id_heads[i], node)
			nr++;
	return nr;
}

/* A child that runs prctl() once for every poke on its pipe. */
struct child {
	pid_t pid;
	int go[2];
	int done[2];
};

static int child_start(struct child *c)
{
	char buf;

	if (pipe(c->go) || pipe(c->done))
		return -1;

	c->pid = fork();
	if (c->pid < 0)
		return -1;

	if (c->pid == 0) {
		close(c->go[1]);
		close(c->done[0]);
		while (read(c->go[0], &buf, 1) == 1) {
			if (buf == 'q')
				break;
			prctl(0, 0, 0, 0, 0);
			if (write(c->done[1], "d", 1) != 1)
				break;
		}
		_exit(0);
	}

	close(c->go[0]);
	close(c->done[1]);
	return 0;
}

static void child_poke(struct child *c)
{
	char buf;

	if (write(c->go[1], "p", 1) == 1)
		read(c->done[0], &buf, 1);
}

static void child_stop(struct child *c)
{
	write(c->go[1], "q", 1);
	close(c->go[1]);
	close(c->done[0]);
	waitpid(c->pid, NULL, 0);
}

/* Drain @tid's ring buffer, or every ring buffer when @tid is -1. */
static int drain(struct perf_evlist *evlist, pid_t tid)
{
	union perf_event *event;
	struct perf_mmap *map;
	int nr_samples = 0;
	bool writable;

	perf_evlist__for_each_mmap(evlist, map, false) {
		if (tid != -1 && perf_mmap__tid(map) != tid)
			continue;
		if (perf_mmap__read_init(map) < 0)
			continue;
		while ((event = perf_mmap__read_event(map, &writable)) != NULL) {
			if (event->header.type == PERF_RECORD_SAMPLE)
				nr_samples++;
			perf_mmap__consume(map);
		}
		perf_mmap__read_done(map);
	}
	return nr_samples;
}

/*
 * Evsels sharing a binding share a ring buffer; an evsel with its own cpus
 * still lands in the ring buffer of the cpu it fires on, and the sample id
 * tells the events apart.
 */
static int test_mmap_sharing(int id)
{
	struct perf_event_attr attr = {
		.type		= PERF_TYPE_TRACEPOINT,
		.config		= id,
		.size		= sizeof(attr),
		.sample_period	= 1,
		.sample_type	= PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
				  PERF_SAMPLE_CPU,
		.disabled	= 1,
		.wakeup_events	= 1,
		.read_format	= PERF_FORMAT_ID,
	};
	struct perf_cpu_map *cpus, *cpu0;
	struct perf_evsel *e1, *e2, *e3;
	struct perf_evlist *evlist;
	int err, ncpus;

	cpus = perf_cpu_map__new("0-1");
	cpu0 = perf_cpu_map__new("0");
	__T("failed to create cpu maps", cpus && cpu0);
	ncpus = perf_cpu_map__nr(cpus);

	evlist = perf_evlist__new();
	__T("failed to create evlist", evlist);

	e1 = perf_evsel__new(&attr);
	__T("failed to create evsel1", e1);
	perf_evlist__add(evlist, e1);

	e2 = perf_evsel__new(&attr);
	__T("failed to create evsel2", e2);
	perf_evlist__add(evlist, e2);

	/* e3 narrows the binding down to a single cpu of its own. */
	e3 = perf_evsel__new(&attr);
	__T("failed to create evsel3", e3);
	perf_evsel__set_own_cpus(e3, cpu0);
	perf_evlist__add(evlist, e3);

	perf_evlist__set_maps(evlist, cpus, NULL);

	err = perf_evlist__open(evlist);
	__T("failed to open evlist", !err);
	err = perf_evlist__mmap(evlist, 4);
	__T("failed to mmap evlist", !err);

	__T("evsel lost its own cpu binding",
	    perf_cpu_map__nr(perf_evsel__cpus(e3)) == 1);
	__T("expected one ring buffer per cpu", nr_mmaps(evlist) == ncpus);
	__T("no ring buffer for cpu0",
	    perf_evlist__find_mmap(evlist, 0, -1, false));
	__T("no ring buffer for cpu1",
	    perf_evlist__find_mmap(evlist, 1, -1, false));
	__T("cpu binding should not carry a tid",
	    perf_mmap__tid(perf_evlist__find_mmap(evlist, 0, -1, false)) == -1);
	__T("cpu binding should report oncpu",
	    perf_mmap__oncpu(perf_evlist__find_mmap(evlist, 0, -1, false)));

	__T("evsels sharing a ring buffer need distinct ids",
	    perf_evsel__get_id(e1, 0, -1) != perf_evsel__get_id(e2, 0, -1));
	__T("id does not map back to its evsel",
	    perf_evlist__id_to_evsel(evlist, perf_evsel__get_id(e2, 0, -1),
				     NULL) == e2);
	__T("id does not map back to the own-cpu evsel",
	    perf_evlist__id_to_evsel(evlist, perf_evsel__get_id(e3, 0, -1),
				     NULL) == e3);

	perf_evlist__delete(evlist);
	perf_cpu_map__put(cpus);
	perf_cpu_map__put(cpu0);
	return 0;
}

/*
 * Grouping is per fd: on each cpu the evsel covering the most cpus leads,
 * whatever the evlist order, and the groups differ in size per cpu.
 */
static int test_group_by_rank(int id)
{
	struct perf_event_attr attr = {
		.type		= PERF_TYPE_TRACEPOINT,
		.config		= id,
		.size		= sizeof(attr),
		.disabled	= 1,
		.read_format	= PERF_FORMAT_GROUP | PERF_FORMAT_ID,
	};
	struct perf_cpu_map *cpus, *cpu0;
	struct perf_evsel *e1, *e2, *e3;
	struct perf_evlist *evlist;
	u64 counts[7];

	cpus = perf_cpu_map__new("0-1");
	cpu0 = perf_cpu_map__new("0");
	__T("failed to create cpu maps", cpus && cpu0);

	evlist = perf_evlist__new();
	__T("failed to create evlist", evlist);

	/* e3 comes first but covers a single cpu. */
	e3 = perf_evsel__new(&attr);
	__T("failed to create evsel3", e3);
	perf_evsel__set_own_cpus(e3, cpu0);
	perf_evlist__add(evlist, e3);

	e1 = perf_evsel__new(&attr);
	__T("failed to create evsel1", e1);
	perf_evlist__add(evlist, e1);

	e2 = perf_evsel__new(&attr);
	__T("failed to create evsel2", e2);
	perf_evlist__add(evlist, e2);

	perf_evlist__set_leader(evlist);
	perf_evlist__set_maps(evlist, cpus, NULL);
	__T("failed to open evlist", !perf_evlist__open(evlist));

	__T("evsel1 should lead cpu0", perf_evsel__is_group_leader(e1, 0, -1));
	__T("evsel1 should lead cpu1", perf_evsel__is_group_leader(e1, 1, -1));
	__T("evsel2 should be a member on cpu0", !perf_evsel__is_group_leader(e2, 0, -1));
	__T("evsel3 should be a member on cpu0", !perf_evsel__is_group_leader(e3, 0, -1));
	__T("evsel3 should join evsel1",
	    perf_evsel__fd_find(e3, 0, -1)->leader == perf_evsel__fd_find(e1, 0, -1));
	__T("expected a single leading evsel", perf_evlist__nr_groups(evlist) == 1);
	__T("read cpu0 group", !perf_evsel__read(e1, 0, -1, (void *)counts));
	__T("cpu0 has three events", counts[0] == 3);
	__T("read cpu1 group", !perf_evsel__read(e1, 1, -1, (void *)counts));
	__T("cpu1 has two events", counts[0] == 2);

	perf_evlist__delete(evlist);
	perf_cpu_map__put(cpus);
	perf_cpu_map__put(cpu0);
	return 0;
}

/* One ring buffer per thread, created and destroyed as threads come and go. */
static int test_dynamic_threads(int id)
{
	struct perf_event_attr attr = {
		.type		= PERF_TYPE_TRACEPOINT,
		.config		= id,
		.size		= sizeof(attr),
		.sample_period	= 1,
		.sample_type	= PERF_SAMPLE_TID | PERF_SAMPLE_TIME,
		.disabled	= 1,
		.wakeup_events	= 1,
		.read_format	= PERF_FORMAT_ID,
	};
	struct child a = {}, b = {}, c = {};
	struct perf_thread_map *threads;
	struct perf_cpu_map *cpus;
	struct perf_evlist *evlist;
	struct perf_evsel *evsel;
	struct perf_mmap *map;
	int err, i;

	__T("failed to start child a", !child_start(&a));
	__T("failed to start child b", !child_start(&b));
	__T("failed to start child c", !child_start(&c));

	cpus = perf_cpu_map__dummy_new();
	threads = perf_thread_map__new_dummy();
	__T("failed to create maps", cpus && threads);
	perf_thread_map__set_pid(threads, 0, a.pid);

	evlist = perf_evlist__new();
	__T("failed to create evlist", evlist);
	evsel = perf_evsel__new(&attr);
	__T("failed to create evsel", evsel);
	perf_evlist__add(evlist, evsel);
	perf_evlist__set_maps(evlist, cpus, threads);

	err = perf_evlist__open(evlist);
	__T("failed to open evlist", !err);
	err = perf_evlist__mmap(evlist, 4);
	__T("failed to mmap evlist", !err);

	__T("expected one ring buffer for one thread", nr_mmaps(evlist) == 1);
	map = perf_evlist__find_mmap(evlist, -1, a.pid, false);
	__T("no ring buffer bound to thread a", map);
	__T("thread binding should not carry a cpu", perf_mmap__cpu(map) == -1);
	__T("thread binding reports the wrong tid",
	    perf_mmap__tid(map) == a.pid);
	__T("thread binding should not report oncpu", !perf_mmap__oncpu(map));

	perf_evlist__enable(evlist);

	/* Add a thread while the evlist is open and mapped. */
	err = perf_evlist__add_thread(evlist, b.pid);
	__T("failed to add thread b", !err);
	__T("expected a ring buffer per thread", nr_mmaps(evlist) == 2);
	__T("no ring buffer bound to thread b",
	    perf_evlist__find_mmap(evlist, -1, b.pid, false));
	__T("thread b missing from the evsel threads",
	    perf_thread_map__idx(perf_evsel__threads(evsel), b.pid) >= 0);
	__T("the input thread map should stay untouched",
	    perf_thread_map__nr(threads) == 1 && threads == evsel->threads);

	child_poke(&a);
	child_poke(&b);
	usleep(50000);

	__T("no sample from thread a", drain(evlist, a.pid) == 1);
	__T("no sample from thread b", drain(evlist, b.pid) == 1);

	/* Deleting a thread that still runs only stops it: no EPOLLHUP yet. */
	err = perf_evlist__del_thread(evlist, b.pid);
	__T("deleting a running thread should wait for its EPOLLHUP", err == 0);
	__T("the ring buffer of thread b should not be found any more",
	    !perf_evlist__find_mmap(evlist, -1, b.pid, false));
	__T("the ring buffer of thread b should still be readable", nr_mmaps(evlist) == 2);
	__T("thread b keeps its fds until they are closed",
	    perf_evsel__has_thread(evsel, b.pid));
	child_poke(&b);
	usleep(50000);
	__T("a deleted thread should not be sampled", drain(evlist, b.pid) == 0);

	/* The exit of thread b completes the delete. */
	child_stop(&b);
	for (i = 0; i < 10 && nr_mmaps(evlist) == 2; i++)
		perf_evlist__poll(evlist, 100);
	__T("EPOLLHUP should release the ring buffer of thread b", nr_mmaps(evlist) == 1);
	__T("thread b should be gone from the evsel",
	    !perf_evsel__has_thread(evsel, b.pid) &&
	    perf_thread_map__idx(perf_evsel__threads(evsel), b.pid) < 0);

	/* The remaining thread keeps being sampled. */
	child_poke(&a);
	usleep(50000);
	__T("no sample from thread a after the delete",
	    drain(evlist, a.pid) == 1);

	err = perf_evlist__add_thread(evlist, c.pid);
	__T("failed to add thread c", !err);
	__T("the threads should be a and c, in open order",
	    perf_thread_map__nr(perf_evsel__threads(evsel)) == 2 &&
	    perf_thread_map__pid(perf_evsel__threads(evsel), 0) == a.pid &&
	    perf_thread_map__pid(perf_evsel__threads(evsel), 1) == c.pid);
	child_poke(&c);
	usleep(50000);
	__T("no sample from the added thread", drain(evlist, c.pid) == 1);

	/*
	 * Adding a tid whose delete is still pending replaces the old thread
	 * outright, its sample ids included.
	 */
	for (i = 0; i < 32; i++) {
		err = perf_evlist__del_thread(evlist, c.pid);
		__T("failed to delete thread c in reuse loop", !err);
		err = perf_evlist__add_thread(evlist, c.pid);
		__T("failed to re-add thread c in reuse loop", !err);
		__T("only the ids of a and c remain", nr_ids(evlist) == 2);
	}
	__T("a replaced thread should not leave its ring buffer behind", nr_mmaps(evlist) == 2);
	child_poke(&c);
	usleep(50000);
	__T("no sample from thread c after reuse loop", drain(evlist, c.pid) == 1);

	perf_evlist__delete(evlist);
	perf_thread_map__put(threads);
	perf_cpu_map__put(cpus);

	child_stop(&a);
	child_stop(&c);
	return 0;
}

static int test_independent_threads(int id)
{
	struct child a = {}, b = {};
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.sample_period = 1, .sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_ID,
		.read_format = PERF_FORMAT_ID, .disabled = 1, .wakeup_events = 1,
	};
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *first = perf_evsel__new(&attr), *second = perf_evsel__new(&attr);
	struct perf_cpu_map *cpus = perf_cpu_map__dummy_new();
	struct perf_thread_map *threads = perf_thread_map__new_dummy();
	struct perf_mmap *map;
	u64 first_id, second_id;
	int i;

	__T("start first child", !child_start(&a));
	__T("start second child", !child_start(&b));
	perf_thread_map__set_pid(threads, 0, a.pid);
	perf_evsel__set_own_threads(first, threads);
	perf_evsel__set_own_threads(second, threads);
	perf_evlist__add(list, first);
	perf_evlist__add(list, second);
	perf_evlist__set_maps(list, cpus, threads);
	__T("open independent evsels", !perf_evlist__open(list));
	__T("map independent evsels", !perf_evlist__mmap(list, 4));
	perf_evlist__enable(list);

	__T("add only to first evsel", !perf_evsel__add_thread(first, b.pid));
	__T("caller map remains valid", perf_thread_map__nr(threads) == 1);
	__T("second evsel remains independent", !perf_evsel__has_thread(second, b.pid));
	__T("evlist does not drive explicit bindings", !perf_evlist__add_thread(list, b.pid));
	__T("explicit binding unchanged", !perf_evsel__has_thread(second, b.pid));
	child_poke(&b);
	__T("first evsel samples added thread", drain(list, b.pid) == 1);
	__T("delete added thread", !perf_evsel__del_thread(first, b.pid));
	__T("last writer destroys map", !perf_evlist__find_mmap(list, -1, b.pid, false));

	/* The ring buffer outlives the fd it was mapped from. */
	map = perf_evlist__find_mmap(list, -1, a.pid, false);
	first_id = perf_evsel__get_id(first, -1, a.pid);
	second_id = perf_evsel__get_id(second, -1, a.pid);
	__T("delete shared map creator", !perf_evsel__del_thread(first, a.pid));
	__T("ring buffer survives its creator", perf_evlist__find_mmap(list, -1, a.pid, false) == map);
	__T("deleted id removed", !perf_evlist__id_to_evsel(list, first_id, NULL));
	__T("surviving id retained", perf_evlist__id_to_evsel(list, second_id, NULL) == second);
	child_poke(&a);
	__T("surviving writer still samples", drain(list, a.pid) == 1);
	__T("rejoin after creator removal", !perf_evsel__add_thread(first, a.pid));
	__T("rejoined writer is in the ring buffer domain",
	    perf_evsel__fd_find(first, -1, a.pid)->domain == perf_evsel__fd_find(second, -1, a.pid)->domain);
	child_poke(&a);
	__T("both writers still sample", drain(list, a.pid) == 2);

	for (i = 0; i < 256; i++) {
		__T("add churn thread", !perf_evsel__add_thread(first, b.pid));
		__T("delete churn thread", !perf_evsel__del_thread(first, b.pid));
	}
	__T("fds follow the live threads", first->nr_fds == 1);
	__T("only live map remains", nr_mmaps(list) == 1);
	__T("only live ids remain", nr_ids(list) == 2);
	__T("one poll fd for the two writers of a thread", list->epoll.nr_live == 1);
	perf_evlist__delete(list);
	perf_cpu_map__put(cpus);
	perf_thread_map__put(threads);
	child_stop(&a);
	child_stop(&b);
	return 0;
}

/* A stand-in for the external poll loop of perf-prof. */
struct ext_poll {
	int epfd;
	int nr_add;
	int nr_new;
	int nr_del;
};

static int ext_add(void *external, int fd, unsigned events, struct perf_mmap *map, bool new_map)
{
	struct ext_poll *ext = external;
	struct epoll_event event = { .events = events, .data.fd = fd };

	if (epoll_ctl(ext->epfd, EPOLL_CTL_ADD, fd, &event) < 0)
		return -errno;
	ext->nr_add++;
	ext->nr_new += new_map;
	return 0;
}

static void ext_del(void *external, int fd, struct perf_mmap *map)
{
	struct ext_poll *ext = external;

	epoll_ctl(ext->epfd, EPOLL_CTL_DEL, fd, NULL);
	ext->nr_del++;
}

/* The fd of the next EPOLLHUP, or -1. */
static int ext_wait_hup(struct ext_poll *ext)
{
	struct epoll_event events[16];
	int i, n, tries;

	for (tries = 0; tries < 10; tries++) {
		n = epoll_wait(ext->epfd, events, 16, 100);
		for (i = 0; i < n; i++)
			if (events[i].events & EPOLLHUP)
				return events[i].data.fd;
	}
	return -1;
}

/* The tid of the domain behind the next EPOLLIN, or -2. */
static int ext_wait_in(struct perf_evlist *evlist, struct ext_poll *ext)
{
	struct epoll_event events[16];
	struct perf_mmap_domain *dom;
	int i, n, tries, h;

	for (tries = 0; tries < 10; tries++) {
		n = epoll_wait(ext->epfd, events, 16, 100);
		for (i = 0; i < n; i++) {
			if (!(events[i].events & EPOLLIN))
				continue;
			for (h = 0; h < PERF_EVLIST__POLL_HLIST_SIZE; h++)
				hlist_for_each_entry(dom, &evlist->epoll.poll_fd_heads[h], poll_node)
					if (dom->poll_fd == events[i].data.fd)
						return dom->tid;
		}
	}
	return -2;
}

static int poll_hup(struct perf_evlist *evlist, struct ext_poll *ext, struct perf_evlist_hup *hup)
{
	int fd = ext_wait_hup(ext);

	if (fd < 0 || perf_evlist__hup(evlist, fd, hup) < 0)
		return -1;
	return fd;
}

/*
 * A ring buffer is polled for data through one fd, a thread for its EPOLLHUP
 * through one fd: the cpu domain takes the data role when there is one, the
 * task domains then only keep one fd per thread for the EPOLLHUP.
 */
static int test_poll_domains(int id)
{
	struct child a = {}, b = {}, c = {};
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.sample_period = 1, .sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_ID,
		.read_format = PERF_FORMAT_ID, .disabled = 1, .wakeup_events = 1,
	};
	struct ext_poll ext = { .epfd = epoll_create1(EPOLL_CLOEXEC) };
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *tasks = perf_evsel__new(&attr), *watermark, *cpuwide;
	struct perf_cpu_map *all = perf_cpu_map__new(NULL);
	struct perf_thread_map *threads = perf_thread_map__new_dummy(), *any = perf_thread_map__new_dummy();
	struct perf_evlist_hup hup;
	struct perf_evsel_fd *entry;
	struct perf_mmap *map;
	int ncpus = perf_cpu_map__nr(all), i, retired;

	__T("start first child", !child_start(&a));
	__T("start second child", !child_start(&b));
	attr.watermark = 1;
	attr.wakeup_watermark = 1;
	watermark = perf_evsel__new(&attr);
	perf_thread_map__set_pid(threads, 0, a.pid);
	perf_evlist__add(list, tasks);
	perf_evlist__add(list, watermark);
	perf_evlist__set_maps(list, all, threads);
	perf_evlist_poll__external(list, &ext, ext_add, ext_del);
	__T("open task-bound cpu writers", !perf_evlist__open(list));
	__T("map task-bound cpu writers", !perf_evlist__mmap(list, 4));
	__T("one cpu ring buffer per cpu", nr_mmaps(list) == ncpus);
	__T("each ring buffer reported once", ext.nr_new == ncpus);
	__T("one data fd per ring buffer, also the thread's hup fd", list->epoll.nr_live == ncpus);
	perf_evlist__enable(list);

	__T("add a second thread", !perf_evlist__add_thread(list, b.pid));
	__T("no new ring buffer for a cpu-bound thread", nr_mmaps(list) == ncpus && ext.nr_new == ncpus);
	__T("the second thread only adds its hup fd", list->epoll.nr_live == ncpus + 1);
	child_poke(&a);
	child_poke(&b);
	__T("both threads sample through the cpu ring buffers", drain(list, -1) == 4);

	/* The first thread to exit hands the data role to the other one. */
	child_stop(&a);
	__T("EPOLLHUP of the first thread", poll_hup(list, &ext, &hup) >= 0);
	__T("EPOLLHUP names the thread", hup.tid == a.pid && !hup.complete);
	__T("every cpu ring buffer of the thread is reported", hup.nr_maps == ncpus);
	for (i = 0; i < hup.nr_maps; i++)
		__T("ring buffers still have a writer", !hup.maps[i].retired);
	perf_evlist__hup_done(list, &hup);
	__T("the other thread's fds take over", list->epoll.nr_live == ncpus);
	__T("the ring buffers stay", nr_mmaps(list) == ncpus);
	__T("delete after EPOLLHUP closes the thread", perf_evlist__del_thread(list, a.pid) == 1);
	__T("the closed thread is gone", !perf_evsel__has_thread(tasks, a.pid));

	/*
	 * The fds the ring buffers were mapped from are closed now; the VMAs
	 * keep them alive. The data role moved to the other thread's fds.
	 */
	perf_evlist__for_each_mmap(list, map, false)
		__T("the other thread polls the data", map->poll_domain &&
		    map->poll_domain->tid == b.pid && (map->poll_domain->poll_events & EPOLLIN));
	child_poke(&b);
	__T("EPOLLIN arrives through the other thread's fd", ext_wait_in(list, &ext) == b.pid);
	__T("the other thread still samples", drain(list, -1) == 2);

	/* A new writer joins a ring buffer whose creator fd is closed. */
	__T("start third child", !child_start(&c));
	__T("add a third thread", !perf_evlist__add_thread(list, c.pid));
	__T("no new ring buffer for the third thread", nr_mmaps(list) == ncpus);
	child_poke(&c);
	__T("the third thread samples", drain(list, -1) == 2);
	child_stop(&c);
	__T("EPOLLHUP of the third thread", poll_hup(list, &ext, &hup) >= 0 && hup.tid == c.pid);
	perf_evlist__hup_done(list, &hup);
	__T("the third thread is closed", perf_evlist__del_thread(list, c.pid) == 1);

	/* A pid=-1 writer takes the data role; the thread keeps one hup fd. */
	cpuwide = perf_evsel__new(&attr);
	perf_evsel__set_own_threads(cpuwide, any);
	perf_evlist__add(list, cpuwide);
	__T("open a pid=-1 writer", !perf_evsel__open(cpuwide, all, any));
	cpuwide->mmaped = true;
	perf_evsel__for_each_fd(cpuwide, entry)
		__T("attach the pid=-1 writer", !perf_evlist__mmap_attach_fd(list, entry));
	__T("pid=-1 data fds plus one hup fd", list->epoll.nr_live == ncpus + 1);
	perf_evlist__for_each_mmap(list, map, false)
		__T("the cpu domain polls the data", map->poll_domain == map->cpu_domain);

	/* The thread's EPOLLHUP still arrives through its hup fd. */
	child_stop(&b);
	__T("EPOLLHUP next to a pid=-1 writer", poll_hup(list, &ext, &hup) >= 0);
	__T("EPOLLHUP names the last thread", hup.tid == b.pid && !hup.complete);
	__T("every cpu ring buffer is reported", hup.nr_maps == ncpus);
	for (i = 0; i < hup.nr_maps; i++)
		__T("the pid=-1 writers keep the ring buffers", !hup.maps[i].retired);
	perf_evlist__hup_done(list, &hup);
	__T("only the pid=-1 data fds are left", list->epoll.nr_live == ncpus);
	__T("delete after EPOLLHUP completes", perf_evlist__del_thread(list, b.pid) == 1);
	__T("the closed thread is gone", !perf_evsel__has_thread(tasks, b.pid));

	/* The last writer takes the ring buffers along. */
	perf_evsel__for_each_fd(cpuwide, entry)
		perf_evlist__mmap_detach_fd(list, entry);
	retired = nr_mmaps(list);
	__T("ring buffers without writers are gone", retired == 0);
	__T("nothing left to poll", list->epoll.nr_live == 0 && ext.nr_add == ext.nr_del);
	perf_evsel__close(cpuwide);

	perf_evlist__remove(list, cpuwide);
	perf_evsel__delete(cpuwide);
	perf_evlist__delete(list);
	perf_cpu_map__put(all);
	perf_thread_map__put(threads);
	perf_thread_map__put(any);
	close(ext.epfd);
	return 0;
}

/*
 * munmap only unregisters: with task-bound and pid=-1 writers on the cpu ring
 * buffers, detaching the fds must not hand their roles over to the next fd,
 * in either order of the evsels.
 */
static int test_munmap_poll(int id, bool cpuwide_first)
{
	struct child a = {};
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.sample_period = 1, .sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_ID,
		.read_format = PERF_FORMAT_ID, .disabled = 1, .wakeup_events = 1,
	};
	struct ext_poll ext = { .epfd = epoll_create1(EPOLL_CLOEXEC) };
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *tasks = perf_evsel__new(&attr), *tasks2 = perf_evsel__new(&attr);
	struct perf_evsel *cpuwide = perf_evsel__new(&attr);
	struct perf_cpu_map *all = perf_cpu_map__new(NULL);
	struct perf_thread_map *threads = perf_thread_map__new_dummy(), *any = perf_thread_map__new_dummy();
	int ncpus = perf_cpu_map__nr(all), nr_add;

	__T("start child", !child_start(&a));
	perf_thread_map__set_pid(threads, 0, a.pid);
	perf_evsel__set_own_threads(cpuwide, any);
	if (cpuwide_first)
		perf_evlist__add(list, cpuwide);
	perf_evlist__add(list, tasks);
	perf_evlist__add(list, tasks2);
	if (!cpuwide_first)
		perf_evlist__add(list, cpuwide);
	perf_evlist__set_maps(list, all, threads);
	perf_evlist_poll__external(list, &ext, ext_add, ext_del);
	__T("open mixed cpu writers", !perf_evlist__open(list));
	__T("map mixed cpu writers", !perf_evlist__mmap(list, 4));
	__T("pid=-1 data fds plus one hup fd", list->epoll.nr_live == ncpus + 1);
	nr_add = ext.nr_add;
	perf_evlist__munmap(list);
	__T("munmap registers no fd", ext.nr_add == nr_add);
	__T("munmap unregisters every fd", ext.nr_del == ext.nr_add && list->epoll.nr_live == 0);
	__T("munmap releases every ring buffer", nr_mmaps(list) == 0);

	perf_evlist__delete(list);
	perf_cpu_map__put(all);
	perf_thread_map__put(threads);
	perf_thread_map__put(any);
	close(ext.epfd);
	child_stop(&a);
	return 0;
}

/*
 * The two steps of a thread exit, in both orders: the fds are closed by the
 * second one, and the events written before the exit stay readable until then.
 */
static int test_thread_exit(int id)
{
	struct child a = {}, b = {}, c = {};
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.sample_period = 1, .sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_ID,
		.read_format = PERF_FORMAT_ID, .disabled = 1, .wakeup_events = 1,
	};
	struct ext_poll ext = { .epfd = epoll_create1(EPOLL_CLOEXEC) };
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *evsel = perf_evsel__new(&attr);
	struct perf_cpu_map *cpus = perf_cpu_map__dummy_new();
	struct perf_thread_map *threads = perf_thread_map__new_dummy();
	struct perf_evlist_hup hup;
	struct perf_mmap *map;
	int fd;

	__T("start first child", !child_start(&a));
	__T("start second child", !child_start(&b));
	__T("start third child", !child_start(&c));
	perf_thread_map__set_pid(threads, 0, a.pid);
	perf_evlist__add(list, evsel);
	perf_evlist__set_maps(list, cpus, threads);
	perf_evlist_poll__external(list, &ext, ext_add, ext_del);
	__T("open evlist", !perf_evlist__open(list));
	__T("map evlist", !perf_evlist__mmap(list, 4));
	perf_evlist__enable(list);
	__T("add thread b", !perf_evlist__add_thread(list, b.pid));
	__T("add thread c", !perf_evlist__add_thread(list, c.pid));
	__T("one ring buffer and poll fd per thread", nr_mmaps(list) == 3 && list->epoll.nr_live == 3);

	/* ptrace first: the delete waits, the EPOLLHUP completes it. */
	child_poke(&b);
	map = perf_evlist__find_mmap(list, -1, b.pid, false);
	child_stop(&b);
	__T("delete before EPOLLHUP waits", perf_evlist__del_thread(list, b.pid) == 0);
	__T("a second delete is a no-op", perf_evlist__del_thread(list, b.pid) == 0);
	fd = poll_hup(list, &ext, &hup);
	__T("EPOLLHUP after delete", fd >= 0);
	__T("EPOLLHUP completes the delete", hup.tid == b.pid && hup.complete);
	__T("the thread ring buffer is retired", hup.nr_maps == 1 && hup.maps[0].map == map && hup.maps[0].retired);
	__T("the retired ring buffer is gone from the evlist", nr_mmaps(list) == 2);
	__T("events before the exit are still readable", perf_mmap__read_init(map) == 0 &&
	    perf_mmap__read_event(map, &(bool){false}) != NULL);
	perf_mmap__read_done(map);
	__T("fds stay open until the EPOLLHUP is done", perf_evsel__has_thread(evsel, b.pid));
	perf_evlist__hup_done(list, &hup);
	__T("done closes the thread", !perf_evsel__has_thread(evsel, b.pid));
	__T("a second EPOLLHUP is ignored", !perf_evlist__hup(list, fd, &hup) && hup.tid == -1);
	perf_evlist__hup_done(list, &hup);

	/* EPOLLHUP first: the delete completes at once. */
	child_stop(&c);
	__T("EPOLLHUP before delete", poll_hup(list, &ext, &hup) >= 0);
	__T("EPOLLHUP alone does not complete", hup.tid == c.pid && !hup.complete);
	perf_evlist__hup_done(list, &hup);
	__T("fds stay open until the delete", perf_evsel__has_thread(evsel, c.pid));
	__T("delete after EPOLLHUP completes", perf_evlist__del_thread(list, c.pid) == 1);
	__T("delete closes the thread", !perf_evsel__has_thread(evsel, c.pid));
	__T("only the first thread is left", nr_mmaps(list) == 1 && list->epoll.nr_live == 1);
	__T("every registered fd was unregistered", ext.nr_add - ext.nr_del == 1);

	/* A thread without a ring buffer has nothing to wait for. */
	__T("unknown thread is gone at once", perf_evlist__del_thread(list, c.pid) == 1);

	perf_evlist__delete(list);
	__T("munmap unregisters the rest", ext.nr_add == ext.nr_del);
	perf_cpu_map__put(cpus);
	perf_thread_map__put(threads);
	close(ext.epfd);
	child_stop(&a);
	return 0;
}

static int test_mixed_groups(int id)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID, .disabled = 1,
	};
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *a = perf_evsel__new(&attr), *b = perf_evsel__new(&attr);
	struct perf_cpu_map *cpu0 = perf_cpu_map__new("0"), *both = perf_cpu_map__new("0-1");
	struct perf_thread_map *threads = perf_thread_map__new_dummy();
	u64 counts[5];

	perf_evsel__set_own_cpus(a, cpu0);
	perf_evsel__set_own_cpus(b, both);
	perf_evlist__add(list, a);
	perf_evlist__add(list, b);
	/* Match profiler init: grouping requested before default maps propagate. */
	perf_evlist__set_leader(list);
	perf_evlist__set_maps(list, both, threads);
	__T("open partially overlapping groups", !perf_evlist__open(list));
	__T("the evsel on more cpus leads cpu0", perf_evsel__is_group_leader(b, 0, -1));
	__T("the evsel on fewer cpus joins it", !perf_evsel__is_group_leader(a, 0, -1));
	__T("the evsel on more cpus leads cpu1", perf_evsel__is_group_leader(b, 1, -1));
	__T("group member uses correct leader",
	    perf_evsel__fd_find(a, 0, -1)->leader == perf_evsel__fd_find(b, 0, -1));
	__T("read cpu0 group", !perf_evsel__read(b, 0, -1, (void *)counts));
	__T("cpu0 has both events", counts[0] == 2);
	__T("read cpu1 group", !perf_evsel__read(b, 1, -1, (void *)counts));
	__T("cpu1 has one event", counts[0] == 1);
	__T("count-only id lookup", perf_evlist__id_to_evsel(list, counts[2], NULL) == b);
	perf_evsel__set_own_cpus(a, both);
	__T("open cpu binding is immutable", perf_cpu_map__nr(a->cpus) == 1);
	perf_evlist__delete(list);
	perf_cpu_map__put(cpu0);
	perf_cpu_map__put(both);
	perf_thread_map__put(threads);
	return 0;
}

/*
 * Only a group leader can be pinned or exclusive, and a group has one clock:
 * a pinned evsel may lead but not join, an evsel with another clock opens
 * alone.
 */
static int test_group_constraints(int id)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID, .disabled = 1,
	};
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *pinned, *member, *narrow, *clock;
	struct perf_cpu_map *cpu0 = perf_cpu_map__new("0"), *both = perf_cpu_map__new("0-1");
	struct perf_thread_map *threads = perf_thread_map__new_dummy();
	u64 counts[9];

	attr.pinned = 1;
	pinned = perf_evsel__new(&attr);
	narrow = perf_evsel__new(&attr);
	attr.pinned = 0;
	member = perf_evsel__new(&attr);
	attr.use_clockid = 1;
	attr.clockid = CLOCK_MONOTONIC;
	clock = perf_evsel__new(&attr);
	perf_evsel__set_own_cpus(narrow, cpu0);
	perf_evsel__set_own_cpus(clock, cpu0);
	perf_evlist__add(list, pinned);
	perf_evlist__add(list, member);
	perf_evlist__add(list, narrow);
	perf_evlist__add(list, clock);
	perf_evlist__set_leader(list);
	perf_evlist__set_maps(list, both, threads);
	__T("open groups with pinned evsels", !perf_evlist__open(list));
	__T("the pinned evsel leads cpu0", perf_evsel__is_group_leader(pinned, 0, -1));
	__T("the pinned evsel leads cpu1", perf_evsel__is_group_leader(pinned, 1, -1));
	__T("a non-pinned evsel joins it",
	    perf_evsel__fd_find(member, 0, -1)->leader == perf_evsel__fd_find(pinned, 0, -1));
	__T("a pinned evsel does not join", perf_evsel__is_group_leader(narrow, 0, -1));
	__T("an evsel with another clock does not join", perf_evsel__is_group_leader(clock, 0, -1));
	__T("read the pinned group", !perf_evsel__read(pinned, 0, -1, (void *)counts));
	__T("the pinned group has two events", counts[0] == 2);
	perf_evlist__delete(list);
	perf_cpu_map__put(cpu0);
	perf_cpu_map__put(both);
	perf_thread_map__put(threads);
	return 0;
}

static int nr_list(struct list_head *head)
{
	struct list_head *pos;
	int nr = 0;

	list_for_each(pos, head)
		nr++;
	return nr;
}

/*
 * Every fd is on the fd_list of its cpu and of its thread. A thread record
 * goes with its last fd, a cpu record stays for the threads added later. The
 * evlist maps are fixed while the evlist is open.
 */
static int test_fd_index(int id)
{
	struct child a = {}, b = {};
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.sample_period = 1, .sample_type = PERF_SAMPLE_TIME,
		.disabled = 1, .wakeup_events = 1,
	};
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *evsel = perf_evsel__new(&attr);
	struct perf_cpu_map *cpus = perf_cpu_map__new("0-1");
	struct perf_thread_map *threads = perf_thread_map__new_dummy();
	struct perf_evsel_thread *th;
	struct perf_evsel_cpu *c;

	__T("start first child", !child_start(&a));
	__T("start second child", !child_start(&b));
	perf_thread_map__set_pid(threads, 0, a.pid);
	perf_evlist__add(list, evsel);
	__T("set maps before open", !perf_evlist__set_maps(list, cpus, threads));
	__T("open the indexed evsel", !perf_evlist__open(list));
	__T("maps are fixed while open", perf_evlist__set_maps(list, NULL, NULL) == -EBUSY);
	__T("the evsel keeps its input maps",
	    perf_evsel__cpus(evsel) == cpus && perf_evsel__threads(evsel) == threads);

	__T("add a thread", !perf_evsel__add_thread(evsel, b.pid));
	__T("two cpu records", nr_list(&evsel->cpu_list) == 2);
	list_for_each_entry(c, &evsel->cpu_list, node)
		__T("one fd per thread on every cpu", nr_list(&c->fd_list) == 2);
	th = perf_evsel__thread_find(evsel, b.pid, false);
	__T("the added thread has a record", th && nr_list(&th->fd_list) == 2);
	__T("each fd holds the thread record", th->refcnt == 2);
	__T("two thread records", nr_list(&evsel->thread_list) == 2);

	__T("delete the first thread", !perf_evsel__del_thread(evsel, a.pid));
	__T("its record goes with its fds", !perf_evsel__thread_find(evsel, a.pid, false));
	list_for_each_entry(c, &evsel->cpu_list, node)
		__T("the cpu keeps the other thread", nr_list(&c->fd_list) == 1);
	__T("delete the second thread", !perf_evsel__del_thread(evsel, b.pid));
	__T("no thread record left", list_empty(&evsel->thread_list) && !evsel->nr_fds);
	__T("the cpu records stay", nr_list(&evsel->cpu_list) == 2);
	list_for_each_entry(c, &evsel->cpu_list, node)
		__T("only the evsel holds a cpu without fds", c->refcnt == 1);
	__T("add a thread to an evsel without fds", !perf_evsel__add_thread(evsel, a.pid));
	__T("it opens on every cpu", evsel->nr_fds == 2);

	perf_evlist__close(list);
	__T("maps can be set again after close", !perf_evlist__set_maps(list, NULL, NULL));
	perf_evlist__delete(list);
	perf_cpu_map__put(cpus);
	perf_thread_map__put(threads);
	child_stop(&a);
	child_stop(&b);
	return 0;
}

/* An explicit group member opens alone where its leader has no fd. */
static int test_explicit_group(int id)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID, .disabled = 1,
	};
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *a = perf_evsel__new(&attr), *b = perf_evsel__new(&attr);
	struct perf_cpu_map *cpu0 = perf_cpu_map__new("0"), *both = perf_cpu_map__new("0-1");
	struct perf_thread_map *threads = perf_thread_map__new_dummy();
	u64 counts[5];

	perf_evsel__set_own_cpus(a, cpu0);
	perf_evsel__set_own_cpus(b, both);
	perf_evlist__add(list, a);
	perf_evlist__add(list, b);
	b->leader = a;
	a->nr_members = 2;
	perf_evlist__set_maps(list, both, threads);
	__T("open an explicit group wider than its leader", !perf_evlist__open(list));
	__T("the member joins the leader on cpu0",
	    perf_evsel__fd_find(b, 0, -1)->leader == perf_evsel__fd_find(a, 0, -1));
	__T("the member is alone on cpu1", perf_evsel__is_group_leader(b, 1, -1));
	__T("read cpu0 group", !perf_evsel__read(a, 0, -1, (void *)counts));
	__T("cpu0 has both events", counts[0] == 2);
	__T("read cpu1 alone", !perf_evsel__read(b, 1, -1, (void *)counts));
	__T("cpu1 has the member only", counts[0] == 1);
	__T("enable the leader's groups", !perf_evsel__enable_group(a));
	__T("enable the member where it is alone", !perf_evsel__enable_group(b));
	perf_evlist__delete(list);
	perf_cpu_map__put(cpu0);
	perf_cpu_map__put(both);
	perf_thread_map__put(threads);
	return 0;
}

static int test_cpu_and_thread_mmaps(int id)
{
	struct child a = {}, b = {};
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.sample_period = 1, .sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_ID,
		.read_format = PERF_FORMAT_ID, .disabled = 1, .wakeup_events = 1,
	};
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *thread_event = perf_evsel__new(&attr), *bpf_event;
	struct perf_cpu_map *dummy = perf_cpu_map__dummy_new(), *cpu0 = perf_cpu_map__new("0");
	struct perf_thread_map *threads = perf_thread_map__new_dummy(), *any = perf_thread_map__new_dummy();

	__T("start mixed binding child", !child_start(&a));
	__T("start added mixed binding child", !child_start(&b));
	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_BPF_OUTPUT;
	bpf_event = perf_evsel__new(&attr);
	perf_thread_map__set_pid(threads, 0, a.pid);
	perf_evsel__set_own_cpus(bpf_event, cpu0);
	perf_evsel__set_own_threads(bpf_event, any);
	perf_evlist__add(list, thread_event);
	perf_evlist__add(list, bpf_event);
	perf_evlist__set_maps(list, dummy, threads);
	__T("open mixed BPF/perf bindings", !perf_evlist__open(list));
	__T("mmap mixed BPF/perf bindings", !perf_evlist__mmap(list, 4));
	__T("thread ring exists", perf_evlist__find_mmap(list, -1, a.pid, false));
	__T("BPF cpu ring exists", perf_evlist__find_mmap(list, 0, -1, false));
	__T("two distinct binding rings", nr_mmaps(list) == 2);
	__T("add default thread in mixed list", !perf_evlist__add_thread(list, b.pid));
	__T("BPF retains any-thread binding", perf_thread_map__pid(bpf_event->threads, 0) == -1);
	__T("delete default thread", !perf_evlist__del_thread(list, b.pid));
	__T("BPF ring survives deletion", perf_evlist__find_mmap(list, 0, -1, false));
	perf_evlist__delete(list);
	perf_cpu_map__put(dummy);
	perf_cpu_map__put(cpu0);
	perf_thread_map__put(threads);
	perf_thread_map__put(any);
	child_stop(&a);
	child_stop(&b);
	return 0;
}

static int test_dynamic_group(int id)
{
	struct child a = {}, b = {};
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT, .config = id, .size = sizeof(attr),
		.sample_period = 1, .sample_type = PERF_SAMPLE_TIME | PERF_SAMPLE_ID,
		.read_format = PERF_FORMAT_ID, .disabled = 1, .wakeup_events = 1,
	};
	struct perf_evlist *list = perf_evlist__new();
	struct perf_evsel *first = perf_evsel__new(&attr), *second = perf_evsel__new(&attr);
	struct perf_cpu_map *cpus = perf_cpu_map__dummy_new();
	struct perf_thread_map *threads = perf_thread_map__new_dummy();

	__T("start group child", !child_start(&a));
	__T("start added group child", !child_start(&b));
	perf_thread_map__set_pid(threads, 0, a.pid);
	perf_evlist__add(list, first);
	perf_evlist__add(list, second);
	perf_evlist__set_leader(list);
	perf_evlist__set_maps(list, cpus, threads);
	__T("open dynamic group", !perf_evlist__open(list));
	__T("map dynamic group", !perf_evlist__mmap(list, 4));
	perf_evlist__enable(list);
	__T("add enabled group", !perf_evlist__add_thread(list, b.pid));
	child_poke(&b);
	__T("both new group members enabled", drain(list, b.pid) == 2);
	perf_evlist__disable(list);
	child_poke(&b);
	__T("group disable stops both members", drain(list, b.pid) == 0);
	__T("delete disabled group thread", !perf_evlist__del_thread(list, b.pid));
	__T("add disabled group thread", !perf_evlist__add_thread(list, b.pid));
	child_poke(&b);
	__T("new group stays disabled", drain(list, b.pid) == 0);
	perf_evlist__enable(list);
	child_poke(&b);
	__T("group re-enable enables both", drain(list, b.pid) == 2);
	/*
	 * The leader created the ring buffer: its VMA keeps the leader event
	 * alive after close, so its members would stay grouped under it. Take
	 * the member out instead.
	 */
	__T("delete only group member", !perf_evsel__del_thread(second, b.pid));
	child_poke(&b);
	__T("leader survives member deletion", drain(list, b.pid) == 1);
	perf_evlist__delete(list);
	perf_cpu_map__put(cpus);
	perf_thread_map__put(threads);
	child_stop(&a);
	child_stop(&b);
	return 0;
}

int test_bind(int argc, char **argv)
{
	int id;

	__T_START;

	libperf_init(libperf_print);

	id = tp_id("syscalls", "sys_enter_prctl");
	if (id < 0) {
		fprintf(stdout, " no sys_enter_prctl tracepoint, skipped...");
		__T_END;
		return 0;
	}

	test_mmap_sharing(id);
	test_group_by_rank(id);
	test_dynamic_threads(id);
	test_independent_threads(id);
	test_poll_domains(id);
	test_munmap_poll(id, false);
	test_munmap_poll(id, true);
	test_thread_exit(id);
	test_mixed_groups(id);
	test_explicit_group(id);
	test_group_constraints(id);
	test_fd_index(id);
	test_cpu_and_thread_mmaps(id);
	test_dynamic_group(id);

	__T_END;
	return tests_failed == 0 ? 0 : -1;
}
