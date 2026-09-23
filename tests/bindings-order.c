// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <monitor.h>
#include <internal/lib.h>

/* Keep the test independent of profiler registration and perf permissions. */
#undef __ctor
#define __ctor __attribute__((unused))
#include "../src/order.c"

int kprobe_type, uprobe_type;
int perf_sample_time_init(struct prof_dev *dev) { return 0; }
int tep__event_size(int id) { return 0; }

struct prof_dev *prof_dev_get(struct prof_dev *dev)
{
    dev->refcount++;
    return dev;
}

bool prof_dev_put(struct prof_dev *dev)
{
    assert(dev->refcount > 0);
    dev->refcount--;
    return false;
}

int perf_event_process_record(struct prof_dev *dev, union perf_event *event,
                             int cpu, int tid, bool writable, bool converted)
{
    assert(cpu == -1 && tid == 42);
    dev->sampled_events++;
    return 0;
}

evclock_t perfclock_to_evclock(struct prof_dev *dev, perfclock_t time)
{
    return (evclock_t) { .clock = time };
}

perfclock_t evclock_to_perfclock(struct prof_dev *dev, evclock_t time)
{
    return time.clock;
}

unsigned long long get_ktime_ns(void)
{
    return 1;
}

static union perf_event *empty_stream(void *stream, bool init, int *cpu,
                                     int *tid, bool *writable, bool *converted)
{
    return NULL;
}

static void dev_init(struct prof_dev *dev, struct env *env, struct monitor *prof)
{
    memset(dev, 0, sizeof(*dev));
    dev->env = env;
    dev->prof = prof;
    dev->refcount = 1;
    dev->order.enabled = true;
    INIT_LIST_HEAD(&dev->order.heap_event_list);
    INIT_LIST_HEAD(&dev->links.child_list);
    INIT_LIST_HEAD(&dev->links.link_to_parent);
    INIT_LIST_HEAD(&dev->dead_threads);
}

static void map_init(struct perf_mmap *map)
{
    memset(map, 0, sizeof(*map));
    perf_mmap__init(map, NULL, false, NULL);
    perf_mmap__set_bind(map, -1, 42);
    map->mask = page_size - 1;
    map->base = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(map->base != MAP_FAILED);
    refcount_set(&map->refcnt, 1);
}

static void test_order_lifecycle(void)
{
    struct env env = {};
    struct monitor prof = { .name = "test" };
    struct prof_dev parent, child;
    struct perf_mmap map;
    struct perf_event_mmap_page *metadata;
    struct {
        struct perf_event_header header;
        u64 time;
    } sample = { .header = { .type = PERF_RECORD_SAMPLE, .size = sizeof(sample) }, .time = 123 };
    int i;

    dev_init(&parent, &env, &prof);
    dev_init(&child, &env, &prof);
    child.links.parent = &parent;
    list_add_tail(&child.links.link_to_parent, &parent.links.child_list);
    map_init(&map);
    metadata = map.base;
    memcpy((char *)map.base + page_size, &sample, sizeof(sample));
    metadata->data_head = sizeof(sample);
    assert(order_mmap_add(&child, &map) == 0);
    assert(order_together(&parent, &child) == 0);
    assert(order_register(&parent, empty_stream, &parent) == 0);

    assert(!order_drain(&child, &map, sizeof(sample)));
    assert(child.sampled_events == 0 && metadata->data_tail == 0);
    assert(parent.order.break_reason == ORDER_BREAK_STREAM_STOP);
    order_unregister(&parent, &parent);
    assert(order_drain(&child, &map, sizeof(sample)));
    assert(child.sampled_events == 1 && metadata->data_tail == sizeof(sample));
    assert(child.refcount == 1 && parent.refcount == 1);

    order_mmap_del(&child, &map);
    assert(list_empty(&child.order.heap_event_list));
    assert(child.order.nr_mmaps == 0 && refcount_read(&map.refcnt) == 1);
    for (i = 0; i < 10000; i++) {
        assert(order_mmap_add(&child, &map) == 0);
        order_mmap_del(&child, &map);
    }
    assert(parent.order.heap_size <= 16 && child.order.nr_mmaps == 0);
    order_deinit(&child);
    order_deinit(&parent);
    perf_mmap__put(&map);
}

struct state {
    struct prof_binding binding;
    struct list_head events;
};

static void state_init(void *ptr)
{
    struct state *state = ptr;
    INIT_LIST_HEAD(&state->events);
}

static void test_binding_lifecycle(void)
{
    struct prof_bindings bindings = { .size = sizeof(struct state), .init = state_init };
    struct state *first = prof_binding_get(&bindings, -1, 42), *state;
    struct list_head event;
    int i;

    assert(first);
    list_add_tail(&event, &first->events);
    for (i = 100; i < 10100; i++) {
        state = prof_binding_get(&bindings, -1, i);
        assert(state && list_empty(&state->events));
    }
    assert(first == prof_binding_find(&bindings, -1, 42));
    assert(event.prev == &first->events && event.next == &first->events);
    list_del(&event);
    assert(list_empty(&first->events));
    prof_bindings_exit(&bindings);
    assert(bindings.nr == 0);
    for (i = 0; i < 10000; i++) {
        assert(prof_binding_get(&bindings, -1, 42));
        prof_bindings_remove_thread(&bindings, 42);
        assert(bindings.nr == 0 && RB_EMPTY_ROOT(&bindings.root));
    }
}

int main(void)
{
    page_size = sysconf(_SC_PAGESIZE);
    test_binding_lifecycle();
    test_order_lifecycle();
    puts("bindings and order lifecycle: OK");
    return 0;
}
