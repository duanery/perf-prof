// SPDX-License-Identifier: GPL-2.0
#include <perf/threadmap.h>
#include <stdlib.h>
#include <linux/refcount.h>
#include <internal/threadmap.h>
#include <string.h>
#include <asm/bug.h>
#include <stdio.h>

static void perf_thread_map__reset(struct perf_thread_map *map, int start, int nr)
{
	size_t size = (nr - start) * sizeof(map->map[0]);

	memset(&map->map[start], 0, size);
	map->err_thread = -1;
}

struct perf_thread_map *perf_thread_map__realloc(struct perf_thread_map *map, int nr)
{
	size_t size = sizeof(*map) + sizeof(map->map[0]) * nr;
	int start = map ? map->nr : 0;

	map = realloc(map, size);
	/*
	 * We only realloc to add more items, let's reset new items.
	 */
	if (map)
		perf_thread_map__reset(map, start, nr);

	return map;
}

#define thread_map__alloc(__nr) perf_thread_map__realloc(NULL, __nr)

struct perf_thread_map *perf_thread_map__dup(struct perf_thread_map *orig)
{
	struct perf_thread_map *map;
	int i;

	if (!orig)
		return NULL;

	map = thread_map__alloc(orig->nr);
	if (!map)
		return NULL;

	map->nr = orig->nr;
	map->err_thread = orig->err_thread;
	refcount_set(&map->refcnt, 1);

	for (i = 0; i < orig->nr; i++) {
		map->map[i].pid = orig->map[i].pid;
		map->map[i].cgroup = orig->map[i].cgroup;
		if (orig->map[i].comm) {
			map->map[i].comm = strdup(orig->map[i].comm);
			if (!map->map[i].comm) {
				perf_thread_map__put(map);
				return NULL;
			}
		}
	}

	return map;
}

void perf_thread_map__set_pid(struct perf_thread_map *map, int thread, pid_t pid)
{
	map->map[thread].pid = pid;
}

void perf_thread_map__pid_cgroup(struct perf_thread_map *map, int thread)
{
	map->map[thread].cgroup = 1;
}

char *perf_thread_map__comm(struct perf_thread_map *map, int thread)
{
	return map->map[thread].comm;
}

struct perf_thread_map *perf_thread_map__new_dummy(void)
{
	struct perf_thread_map *threads = thread_map__alloc(1);

	if (threads != NULL) {
		perf_thread_map__set_pid(threads, 0, -1);
		threads->nr = 1;
		refcount_set(&threads->refcnt, 1);
	}
	return threads;
}

static void perf_thread_map__delete(struct perf_thread_map *threads)
{
	if (threads) {
		int i;

		WARN_ONCE(refcount_read(&threads->refcnt) != 0,
			  "thread map refcnt unbalanced\n");
		for (i = 0; i < threads->nr; i++)
			free(perf_thread_map__comm(threads, i));
		free(threads);
	}
}

struct perf_thread_map *perf_thread_map__get(struct perf_thread_map *map)
{
	if (map)
		refcount_inc(&map->refcnt);
	return map;
}

void perf_thread_map__put(struct perf_thread_map *map)
{
	if (map && refcount_dec_and_test(&map->refcnt))
		perf_thread_map__delete(map);
}

int perf_thread_map__nr(struct perf_thread_map *threads)
{
	return threads ? threads->nr : 1;
}

pid_t perf_thread_map__pid(struct perf_thread_map *map, int thread)
{
    if (map && thread < map->nr)
	    return map->map[thread].pid;

    return -1;
}

int perf_thread_map__idx(struct perf_thread_map *map, int pid)
{
    int i;

    if (!map || pid == PERF_THREAD_MAP_HOLE)
        return -1;

    for (i = 0; i < map->nr; i++) {
        if (map->map[i].pid == pid)
            return i;
    }
	return -1;
}

bool perf_thread_map__valid(struct perf_thread_map *map, int thread)
{
	if (!map)
		return thread == 0;

	return thread < map->nr && map->map[thread].pid != PERF_THREAD_MAP_HOLE;
}

