#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/kernel.h>
#include <linux/circ_buf.h>
#include <linux/bitops.h>
#include <monitor.h>
#include <api/fs/fs.h>
#include <internal/mmap.h>
#include <trace_helpers.h>
#include "perf_regs_mask.h"

int perf_event_max_stack = PERF_MAX_STACK_DEPTH;
int perf_event_max_contexts_per_stack = 0;
static __ctor void init(void)
{
    char path[PATH_MAX];
    const char *procfs = procfs__mountpoint();

    if (!procfs)
        return;

    snprintf(path, sizeof(path), "%s/sys/kernel/perf_event_max_stack", procfs);
    filename__read_int(path, &perf_event_max_stack);
    snprintf(path, sizeof(path), "%s/sys/kernel/perf_event_max_contexts_per_stack", procfs);
    filename__read_int(path, &perf_event_max_contexts_per_stack);
}

#define PERF_MMAP_EVENT 0
#define STREAM_EVENT 1

#define ALIGN_SIZE 64
// heap sort element: represents an ordered event queue.
struct heap_event {
    struct list_head link;
    struct prof_dev *dev;
    union perf_event *event;
    heapclock_t time;
    int ins;
    bool writable;
    bool converted;
    bool unconsumed; // event is unconsumed.
    char type; // 0: perf_mmap_event; 1: stream_event;
};

// perf ringbuffer event: ordered by default.
struct perf_mmap_event {
    struct heap_event base;
    struct perf_mmap *map;
    /*
     * A monotonically increasing timestamp for
     * each perf_map event.
     */
    heapclock_t event_mono_time;
    /*
     * There may be a lost event at the `map->end'
     * position, but it is not seen.
     */
    u64 maybe_lost_end;
    u64 pause_start_time; // ns
    bool lost_pause;
} __attribute__((aligned(ALIGN_SIZE)));

/*
 * stream event: read from tcp, character devices, files, etc.
 * (event-spread: Ensure that broadcast events are time-ordered.)
 *
 * Use read_event() to read each ordered event, and each read will
 * consume the previous event.
 */
struct stream_event {
    struct heap_event base;
    read_event *read_event;
    void *stream;
    u64 pause_start_time; // ns
    bool empty_pause;
} __attribute__((aligned(ALIGN_SIZE)));

static __always_inline bool less_than(const void *lhs, const void *rhs, void __maybe_unused *args)
{
    struct heap_event *a = *(struct heap_event **)lhs;
    struct heap_event *b = *(struct heap_event **)rhs;
    return a->time < b->time;
}
static __always_inline void swap_ptr(void *lhs, void *rhs, void __maybe_unused *args)
{
    struct heap_event *a = *(struct heap_event **)lhs;
    *(struct heap_event **)lhs = *(struct heap_event **)rhs;
    *(struct heap_event **)rhs = a;
}

static int perf_sample_max_size(struct perf_evsel *evsel)
{
    static u64 known_sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_IP | PERF_SAMPLE_TID |
        PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID |
        PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_READ | PERF_SAMPLE_CALLCHAIN |
        PERF_SAMPLE_RAW | PERF_SAMPLE_REGS_USER | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_DATA_SRC |
        PERF_SAMPLE_TRANSACTION | PERF_SAMPLE_REGS_INTR | PERF_SAMPLE_PHYS_ADDR;
    struct perf_event_attr *attr = perf_evsel__attr(evsel);
    u32 type = attr->type;
    u64 sample_type = attr->sample_type;
    int size = 0;

    if (sample_type & ~known_sample_type)
        fprintf(stderr, "%s: Unknown sample_type %lu\n", __func__,
                        sample_type & ~known_sample_type);

    if (sample_type & PERF_SAMPLE_IDENTIFIER)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_IP)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_TID)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_TIME)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_ADDR)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_ID)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_STREAM_ID)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_CPU)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_PERIOD)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_READ)
        size += perf_evsel__read_size(evsel);
    if (sample_type & PERF_SAMPLE_CALLCHAIN) {
        size += sizeof(u64);
        if (attr->sample_max_stack)
            size += sizeof(u64) * attr->sample_max_stack;
        else
            size += sizeof(u64) * (perf_event_max_stack +
                                   perf_event_max_contexts_per_stack);
    }
    if (sample_type & PERF_SAMPLE_RAW) {
        int raw_size = -1;
        if (type == PERF_TYPE_TRACEPOINT)
            raw_size = tep__event_size(attr->config);
        else if (type == kprobe_type || type == uprobe_type) {
            if (type == kprobe_type)
                type = (attr->config & 1/*PERF_PROBE_CONFIG_IS_RETPROBE*/) ? KRETPROBE : KPROBE;
            if (type == uprobe_type)
                type = (attr->config & 1/*PERF_PROBE_CONFIG_IS_RETPROBE*/) ? URETPROBE : UPROBE;
            raw_size = tep__event_size(type);
        }
        if (raw_size < 0) {
            fprintf(stderr, "%s: Unknown raw_size(type %d)\n", __func__, type);
            size += sizeof(u64);
        } else
            size += round_up(raw_size + sizeof(u32), sizeof(u64));
    }
    if (sample_type & PERF_SAMPLE_REGS_USER) {
        u64 mask = attr->sample_regs_user;
        size += sizeof(u64) + hweight64(mask) * sizeof(u64);
    }
    if (sample_type & PERF_SAMPLE_WEIGHT)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_DATA_SRC)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_TRANSACTION)
        size += sizeof(u64);
    if (sample_type & PERF_SAMPLE_REGS_INTR) {
        u64 mask = attr->sample_regs_intr;
        size += sizeof(u64) + hweight64(mask) * sizeof(u64);
    }
    if (sample_type & PERF_SAMPLE_PHYS_ADDR)
        size += sizeof(u64);

    return size;
}

static u64 perf_sample_watermark(struct prof_dev *dev)
{
    struct perf_evsel *evsel;
    struct perf_event_attr *attr;
    u64 watermark;
    u64 min_watermark = -1UL;

    perf_evlist__for_each_evsel(dev->evlist, evsel) {
        attr = perf_evsel__attr(evsel);

        // non-sample event
        if (attr->sample_period == 0)
            continue;

        if (attr->watermark)
            watermark = attr->wakeup_watermark;
        else
            watermark = attr->wakeup_events * perf_sample_max_size(evsel);

        if (watermark < min_watermark)
            min_watermark = watermark;
    }
    return min_watermark;
}

int order_init(struct prof_dev *dev)
{
    struct prof_dev *source, *tmp;
    struct perf_mmap *map;
    int nr_mmaps = 0, heap_size = 0;
    struct perf_mmap_event *mmap_event;
    int ret;

    if (dev->order.enabled)
        return 0;

    if (perf_sample_time_init(dev) < 0)
        return -1;
    if (!(dev->pos.sample_type & PERF_SAMPLE_TIME)) {
        fprintf(stderr, "%s: --order cannot be enabled\n", dev->prof->name);
        return -1;
    }

    for_each_source_dev_get(source, tmp, dev) {
        order_init(source);
        heap_size += source->order.heap_size;
    }

    perf_evlist__for_each_mmap(dev->evlist, map, dev->env->overwrite)
        nr_mmaps++;
    heap_size += nr_mmaps;
    heap_size += dev->order.nr_streams;

    dev->order.heap_size = heap_size;
    dev->order.data = calloc(heap_size, sizeof(*dev->order.data));
    if (!dev->order.data)
        return -1;
    min_heap_init(&dev->order.heapsort, dev->order.data, heap_size);

    dev->order.nr_mmaps = nr_mmaps;
    ret = posix_memalign(&dev->order.permap_event, ALIGN_SIZE, nr_mmaps * sizeof(struct perf_mmap_event));
    dev->order.heap_popped_time = 0;
    dev->order.wakeup_watermark = perf_sample_watermark(dev);

    if (ret != 0 || !dev->order.permap_event)
        goto failed;

    memset(dev->order.permap_event, 0, nr_mmaps * sizeof(struct perf_mmap_event));
    perf_evlist__for_each_mmap(dev->evlist, map, dev->env->overwrite) {
        int idx = perf_mmap__idx(map);
        if (idx == 0)
            perf_event_convert_read_tsc_conversion(dev, map);

        mmap_event = (struct perf_mmap_event *)dev->order.permap_event + idx;
        mmap_event->base.dev = dev;
        mmap_event->base.ins = idx;
        mmap_event->base.converted = false;
        mmap_event->base.type = PERF_MMAP_EVENT;
        mmap_event->map = map;

        list_add_tail(&mmap_event->base.link, &dev->order.heap_event_list);
    }
    for_each_source_dev_get(source, tmp, dev) {
        list_splice_tail_init(&source->order.heap_event_list, &dev->order.heap_event_list);
    }

    dev->order.enabled = 1;
    return 0;

failed:
    order_deinit(dev);
    return -1;
}

void order_deinit(struct prof_dev *dev)
{
    struct heap_event *heap_event, *tmp;
    struct perf_mmap_event *mmap_event;
    int i;

    for (i = 0; i < dev->order.nr_mmaps; i++) {
        mmap_event = (struct perf_mmap_event *)dev->order.permap_event + i;
        list_del(&mmap_event->base.link);
    }
    list_for_each_entry_safe(heap_event, tmp, &dev->order.heap_event_list, link) {
        list_del_init(&heap_event->link);
        if (heap_event->type == STREAM_EVENT)
            free(heap_event);
    }

    if (dev->order.data)
        free(dev->order.data);
    if (dev->order.permap_event)
        free(dev->order.permap_event);
}

int order_together(struct prof_dev *main_dev, struct prof_dev *dev)
{
    int heap_size;
    void **data;

    // main_dev may be a forwarding source, and heap-sorting must
    // be enabled for the top-level device. See order_main_dev().
    main_dev = order_main_dev(main_dev);

    if (order_init(main_dev) < 0)
        return -1;
    if (order_init(dev) < 0)
        return -1;

    heap_size = main_dev->order.heap_size + dev->order.heap_size;
    data = calloc(heap_size, sizeof(*main_dev->order.data));
    if (!data)
        return -1;

    free(main_dev->order.data);
    main_dev->order.heap_size = heap_size;
    main_dev->order.data = data;
    min_heap_init(&main_dev->order.heapsort, data, heap_size);

    list_splice_tail_init(&dev->order.heap_event_list, &main_dev->order.heap_event_list);
    return 0;
}

int order_register(struct prof_dev *dev, read_event *read_event, void *stream)
{
    struct prof_dev *main_dev = order_main_dev(dev);
    struct stream_event *stream_event = NULL;
    int ret;

    ret = posix_memalign((void **)&stream_event, ALIGN_SIZE, sizeof(*stream_event));
    if (ret != 0 && !stream_event)
        return -1;

    memset(stream_event, 0, sizeof(*stream_event));
    stream_event->base.dev = dev;
    stream_event->base.type = STREAM_EVENT;
    stream_event->read_event = read_event;
    stream_event->stream = stream;

    if (main_dev->order.enabled) {
        int heap_size = main_dev->order.heap_size + 1;
        void **data = calloc(heap_size, sizeof(*dev->order.data));
        if (!data)
            goto failed;

        free(main_dev->order.data);
        main_dev->order.heap_size = heap_size;
        main_dev->order.data = data;
        min_heap_init(&main_dev->order.heapsort, data, heap_size);
    }
    main_dev->order.nr_streams++;
    list_add(&stream_event->base.link, &main_dev->order.heap_event_list);
    return 0;

failed:
    free(stream_event);
    return -1;
}

void order_unregister(struct prof_dev *dev, void *stream)
{
    struct prof_dev *main_dev = order_main_dev(dev);
    struct heap_event *heap_event, *tmp;
    struct stream_event *stream_event;

    list_for_each_entry_safe(heap_event, tmp, &main_dev->order.heap_event_list, link) {
        stream_event = (struct stream_event *)heap_event;
        if (heap_event->type == STREAM_EVENT && stream_event->stream == stream) {
            list_del(&heap_event->link);
            main_dev->order.nr_streams--;
            free(stream_event);
            return;
        }
    }
}

static __always_inline heapclock_t heapclock(struct prof_dev *main_dev, perfclock_t time)
{
    /*
     * Only for perf_mmap events.
     * stream_event  need_conv  Which clock does heap sort use?
     *           NO         NO  perfclock_t
     *           NO        YES  perfclock_t
     *          YES         NO  perfclock_t  # Unable to convert, out of order.
     *          YES        YES  evclock_t    # Convert, not out of order.
     */
    if (unlikely(main_dev->order.nr_streams && main_dev->convert.need_conv))
        return perfclock_to_evclock(main_dev, time).clock;
    else
        return time;
}

static __always_inline u64 heapclock_to_evclock(struct prof_dev *main_dev, heapclock_t time)
{
    /*
     * Only for perf_mmap events.
     * stream_event  need_conv  What is evclock?
     *           NO         NO  perfclock_t
     *           NO        YES  evclock_t
     *          YES         NO  perfclock_t
     *          YES        YES  evclock_t   # heapclock_t is evclock_t, which has been converted.
     */
    if (likely(!main_dev->convert.need_conv) ||
        unlikely(main_dev->order.nr_streams))
        return time;
    else
        return perfclock_to_evclock(main_dev, time).clock;
}

u64 heapclock_to_perfclock(struct prof_dev *dev, heapclock_t time)
{
    struct prof_dev *main_dev = order_main_dev(dev);

    if (unlikely(main_dev->order.nr_streams && main_dev->convert.need_conv))
        return evclock_to_perfclock(main_dev, (evclock_t)time);
    else
        return time;
}

static union perf_event *
perf_mmap_fix_out_of_order(struct prof_dev *main_dev, struct prof_dev *dev,
                           struct heap_event *heap_event, heapclock_t popped_time, int popped_ins)
{
    union perf_event *event = heap_event->event;

    // Currently, All 2 fixes are explainable.
    if (main_dev->env->verbose)
        printf("%s: fix out-of-order event %lu(%d) < %lu(%d)\n", dev->prof->name,
                    heap_event->time, heap_event->ins, popped_time, popped_ins);

    if (!heap_event->writable) {
        struct perf_mmap *map = ((struct perf_mmap_event *)heap_event)->map;
        if (!map->event_copy || map->event_copy_sz < event->header.size) {
            void *new_copy = malloc(event->header.size);
            if (!new_copy) {
                fprintf(stderr, "%s: failed to allocate event_copy\n", __func__);
                return NULL;
            }
            if (map->event_copy)
                free(map->event_copy);
            map->event_copy = new_copy;
            map->event_copy_sz = event->header.size;
        }
        memcpy(map->event_copy, event, event->header.size);
        event = (union perf_event *)map->event_copy;
    }

    *(u64 *)((void *)event->sample.array + dev->pos.time_pos) =
                heapclock_to_perfclock(main_dev, popped_time);

    heap_event->event = event;
    heap_event->time = popped_time;
    heap_event->writable = 1;
    dev->order.nr_fixed_events++;

    return event;
}

static __always_inline bool
perf_mmap_has_space(struct perf_mmap *map, unsigned long size)
{
    return CIRC_SPACE(map->end, map->start, map->mask+1) >= size;
}

static int perf_mmap_event_init(struct heap_event *heap_event, struct prof_dev *main_dev)
{
    struct perf_mmap_event *mmap_event = (struct perf_mmap_event *)heap_event;
    struct prof_dev *dev = heap_event->dev;
    struct perf_mmap *map = mmap_event->map;
    int ins = heap_event->ins;
    union perf_event *event;
    bool writable;
    struct perf_record_lost lost;

    if (perf_mmap__empty(map) ||
        perf_mmap__read_init(map) < 0)
        return -1;

    lost.lost = 0;
retry:
    event = perf_mmap__read_event(map, &writable);
    if (event) {
        /*
         * Within order_process(), the final lost event will be processed.
         * Why do I still read the lost event during init()?
         *
         * After order_process() returns, there may be a lost event, but it
         * is not output until a new event occurs. The kernel will pre-output
         * the lost event and then the new event, see the Linux kernel function
         * __perf_output_begin().
         *
         * However, the lost event will be predicted in order_process(), and
         * generally it will not be read during init(). A BUG will be checked
         * here.
         */
        /* Only the PERF_RECORD_SAMPLE event can sample time. */
        if (unlikely(event->header.type != PERF_RECORD_SAMPLE)) {
            if (event->header.type == PERF_RECORD_LOST)
                dev->order.nr_lost++;
            if (event->header.type == PERF_RECORD_LOST && dev->prof->lost) {
                lost.id = event->lost.id;
                lost.lost = event->lost.lost;
            } else
                perf_event_process_record(dev, event, ins, writable, false);
            perf_mmap__consume(map);
            goto retry;
        }

        heap_event->event = event;
        heap_event->time = heapclock(main_dev, *(u64 *)((void *)event->sample.array + dev->pos.time_pos));
        heap_event->writable = writable;

        /* Keep order at init. Why do this?
         *
         * When the kernel outputs an event, it first obtains the event time and starts
         * outputting. The event cannot be seen in the ringbuffer until the output ends.
         * This takes some time from start to end, nanosecond granularity, very short.
         * However, interrupts will cause output delays. Therefore, between multiple
         * ringbuffers, out of order may occur.
         *
         * order_process() will only process to a known time, and its target_end or
         * target_time is this known time, which is already known at the beginning of
         * order_process(). This is very critical, no matter how fast the heap sort is
         * processed, future events will not be touched.
         *
         * At this known-time moment, all events in the ringbuffer before this time can
         * either be seen or are being output (cannot be seen yet, output delay, which
         * will lead to out-of-order). Events after this known-time will not cause
         * out-of-order. For a perf_mmap, there is and only 1 event being output.
         *
         * In order_process(), perf_mmap becomes empty and pops up. When order_process()
         * returns, heap_popped_time is the known-time, which is the last event timestamp
         * from one of the perf_mmaps. For the popped perf_mmap, it may generate new
         * events before heap_popped_time, but the output is delayed, and we will see
         * out-of-order at the next order_process(), that is in init().
         *
         * For an event being output, its timestamp can be any time from the start to the
         * end of output.
         *
         *             heap_popped_time
         * MAP: A ------|-----
         * MAP: B -|   |~|----
         *         |   | `output end
         *         |   `output start(being output)
         *         | `empty, no events
         *         `be seen
         *
         * In summary, it is okay for us to fix the out-of-order event during init() and
         * adjust the time to heap_popped_time.
         */
        if (unlikely(heap_event->time < main_dev->order.heap_popped_time))
            perf_mmap_fix_out_of_order(main_dev, dev, heap_event, main_dev->order.heap_popped_time,
                                       main_dev->order.heap_popped_ins);

        if (unlikely(lost.lost)) {
            if (mmap_event->event_mono_time/*lost_start*/ < main_dev->order.heap_popped_time)
                fprintf(stderr, "BUG: unsafe lost event %lu < popped %lu\n",
                                 mmap_event->event_mono_time, main_dev->order.heap_popped_time);

            dev->prof->lost(dev, (union perf_event *)&lost, ins,
                            heapclock_to_evclock(main_dev, mmap_event->event_mono_time),
                            heapclock_to_evclock(main_dev, heap_event->time));
        }
        return 0;
    } else
        perf_mmap__read_done(map);

    return -1;
}

static int stream_event_init(struct heap_event *heap_event, bool init)
{
    struct stream_event *stream_event = (struct stream_event *)heap_event;
    struct prof_dev *dev = heap_event->dev;
    union perf_event *event;
    int ins;
    bool writable;
    bool converted;

    if (heap_event->unconsumed) {
        heap_event->unconsumed = 0;
        return 0;
    }

    /*
     * Key points to fix out-of-order.
     *
     * 1) Initialize stream_event first, then perf_mmap_event.
     *    perf_mmap events are real-time, but stream events are cached, so the events
     *    read are earlier than perf_mmap. When I see the latest event of the stream,
     *    I can assume that other perf_mmap events have occurred before this latest
     *    event.
     *    stream_event is all at the head of `dev->order.heap_event_list'.
     *
     * 2) Stream events become empty, breaking heap sort.
     *    Because 1) it is guaranteed that the stream event occurs early in time. The
     *    remaining events of perf_mmap occur later.
     *
     * 3) All stream_events are initialized, start heap sort.
     *    An uninitialized stream_event does not know whether its event is empty or
     *    occurred earlier.
     *
     * 4) Only when `init=true', a batch of stream events can be read.
     *    If after processing this batch of events, new events can be read every time,
     *    it will cause perf_mmap events to be out of order.
     */
retry:
    event = stream_event->read_event(stream_event->stream, init, &ins, &writable, &converted);
    if (event) {
        if (unlikely(event->header.type != PERF_RECORD_SAMPLE &&
                     event->header.type != PERF_RECORD_ORDER_TIME)) {
            perf_event_process_record(dev, event, ins, writable, converted);
            goto retry;
        }
        heap_event->event = event;
        heap_event->time = event->header.type == PERF_RECORD_ORDER_TIME ?
                           ((struct perf_record_order_time *)event)->order_time :
                           *(u64 *)((void *)event->sample.array + dev->pos.time_pos);
        heap_event->ins = ins;
        heap_event->writable = writable;
        heap_event->converted = converted;
        return 0;
    }
    return -1;
}

static int stream_event_process(struct prof_dev *main_dev, struct heap_event *heap_event)
{
    struct stream_event *stream_event = (struct stream_event *)heap_event;
    struct prof_dev *dev = heap_event->dev;
    union perf_event *event = heap_event->event;
    u64 time = heap_event->time;
    int ins = heap_event->ins;
    bool writable = heap_event->writable;
    bool converted = heap_event->converted;

    // out of order
    if (dev != main_dev) dev->order.heap_popped_time = time;
    if (unlikely(time < main_dev->order.heap_popped_time)) {
        dev->order.nr_unordered_events++;
        fprintf(stderr, "%s: out-of-order stream event %lu(%d) < %s %lu(%d)\n", dev->prof->name,
                        time, ins, main_dev->prof->name, main_dev->order.heap_popped_time,
                        main_dev->order.heap_popped_ins);
    } else {
        main_dev->order.heap_popped_time = time;
        main_dev->order.heap_popped_ins = ins;
    }

    perf_event_process_record(dev, event, ins, writable, converted);

    if (stream_event_init(heap_event, false) == 0) {
        if (stream_event->empty_pause) {
            stream_event->empty_pause = 0;
            dev->order.stream_pause_time += get_ktime_ns() - stream_event->pause_start_time;
        }
        return 0;
    } else {
        stream_event->empty_pause = 1;
        stream_event->pause_start_time = get_ktime_ns();
        dev->order.nr_stream_pause++;
        return -1;
    }
}

static int order_heap_init(struct prof_dev *main_dev, struct prof_dev *dev)
{
    DEFINE_MIN_HEAP(struct heap_event *, ) *heap;
    struct heap_event *heap_event;
    struct prof_dev *child, *tmp;

    heap = (void *)&main_dev->order.heapsort;

    list_for_each_entry(heap_event, &dev->order.heap_event_list, link) {
        if (heap_event->type == PERF_MMAP_EVENT) {
            if (perf_mmap_event_init(heap_event, main_dev) < 0)
                continue;
        } else if (heap_event->type == STREAM_EVENT) {
            if (stream_event_init(heap_event, true) < 0) {
                main_dev->order.break_reason = ORDER_BREAK_STREAM_STOP;
                return 1;
            }
        }

        if (heap->nr == heap->size) {
            // expand
            int heap_size = heap->size + dev->order.heap_size;
            void *data = realloc(heap->data, heap_size * sizeof(*heap->data));
            if (!data)
                return -1;
            heap->size = heap_size;
            heap->data = data;
            main_dev->order.heap_size = heap_size;
            main_dev->order.data = data;
        }
        heap->data[heap->nr++] = heap_event;
        prof_dev_get(heap_event->dev);
    }

    /*
     * Make all order-enabled child devices perform heap sort together.
     * Includes: cloned child, forwarding source.
     * Sorting together only adjusts the order of event processing and
     * does not affect the forwarding and close of the device.
     */
    for_each_child_dev_get(child, tmp, dev) {
        int ret = order_heap_init(main_dev, child);
        if (ret)
            return ret;
    }

    return 0;
}

void order_process(struct prof_dev *dev, struct perf_mmap *target_map, perfclock_t target_tm)
{
    /*
     * All ringbuffers of the forwarding source and the forwarding target are
     * heap-sorted together.
     */
    struct prof_dev *main_dev = order_main_dev(dev);
    struct perf_mmap *map;
    union perf_event *event;
    heapclock_t time;
    int ins;
    bool writable;
    u64 wakeup_watermark;
    u64 target_end;
    heapclock_t target_time;
    struct perf_record_lost lost;

    // heap sort
    struct perf_mmap_event *mmap_event;
    struct heap_event *heap_event;
    DEFINE_MIN_HEAP(struct heap_event *, ) *heap;
    struct min_heap_callbacks funcs = {
        .less = less_than,
        .swp = swap_ptr,
    };


    if (main_dev->order.inprocess)
        return;
    if (target_map && perf_mmap__read_init(target_map) < 0)
        return;
    if (target_map && perf_mmap__empty(target_map))
        return;

    prof_dev_get(main_dev);
    main_dev->order.inprocess = 1;
    /*
     * Get the latest event of ringbuffer(perf_mmap). According to the causal
     * relationship, when I see the latest event, events that occurred before
     * the latest on other ringbuffers must have been written.
     *
     * Therefore, if any ringbuffer is empty, there is no need to pay attention
     * to it until the latest event of `target_map' is processed. `target_map->
     * end' points to the end of the latest event.
     */
    target_end = target_map ? target_map->end : 0;
    target_time = target_tm ? heapclock(main_dev, target_tm) : -1UL;

    lost.lost = 0;
    heap = (void *)&main_dev->order.heapsort;
    heap->nr = 0;

    if (order_heap_init(main_dev, main_dev) != 0)
        goto stream_stop;


    // heap sort start
    min_heapify_all(heap, &funcs, NULL);
    while (1) {
        struct heap_event **data = min_heap_peek(heap);
        bool need_break = 0;

        if (!data) {
            main_dev->order.break_reason = ORDER_BREAK_EMPTY;
            break;
        }

        heap_event = data[0];

        if (unlikely(heap_event->type == STREAM_EVENT)) {
            if (stream_event_process(main_dev, heap_event) == 0) {
                min_heap_sift_down(heap, 0, &funcs, NULL);
                continue;
            } else {
                min_heap_pop(heap, &funcs, NULL);
                prof_dev_put(heap_event->dev);
                main_dev->order.break_reason = ORDER_BREAK_STREAM_STOP;
                break;
            }
        }

        mmap_event = (struct perf_mmap_event *)heap_event;

        dev = heap_event->dev;
        map = mmap_event->map;
        event = heap_event->event;
        time = heap_event->time;
        ins = heap_event->ins;
        writable = heap_event->writable;
        wakeup_watermark = dev->order.wakeup_watermark;

        if (time > target_time) {
            main_dev->order.break_reason = ORDER_BREAK_TARGET_TIME;
            break;
        }

        // Don't care about the correctness of lost.
        if (!dev->prof->lost)
            goto skip_lost_detect;

        /*                     lost
         * perf_mmap A: - -A-|=======|-A- -
         * perf_mmap B: - -B-|- -B- -|-B- -
         *                   |   |   `lost_end
         *                   |   ` B is unsafe to perf_mmap A
         *                   `lost_start
         *                   `mmap_event->maybe_lost_end
         *
         * If there is an event loss in A, then B's events are also unsafe.
         * Therefore, events before lost_start can be heap sorted normally, and
         * events after lost_end can also be sorted normally.
         *
         * perf_mmap A: - -A-|=AA====|-A- -
         *                      ` true_lost_end
         *
         * `lost_start' and `lost_end' are the event time before and after
         * the PERF_RECORD_LOST event respectively. This range will be larger,
         * but covers `true_lost_end'.
         *
         * The kernel function __perf_output_begin() will pre-output the lost
         * event and then output the lost_end event. Therefore, the last event of
         * the ringbuffer may be a lost_start event. Only when the lost_end event
         * occurs, we can sort the events between lost_start->lost_end (e.g. the
         * perf_mmap B) and confirm that it is unsafe.
         *
         * Heap sorting needs to continuously predict possible lost events and
         * keep the lost_start event in the ringbuffer.
         *
         * When processing each event in the ringbuffer, it is judged that there
         * is not enough safe space, and `maybe_lost_end' is recorded, which may
         * be a lost_start event. Continue heap sorting and wait for the kernel
         * to write new events.
         *    1) If so, the new event may or may not be a lost_end event, it
         *       doesn't matter. This can enable more heap sorting. And continue
         *       to decide if there is a safe space.
         *    2) If not until the `maybe_lost_end', pause and keep the lost_start
         *       event in the ringbuffer.
         *
         * If there is always enough safe space, keep is not needed.
         */
        if (likely(map->end != mmap_event->maybe_lost_end)) {
             // Have enough safe space, no need to keep event.
            if (perf_mmap_has_space(map, wakeup_watermark+event->header.size))
                mmap_event->maybe_lost_end = map->end - 1;
            else {
                mmap_event->maybe_lost_end = map->end;
                dev->order.nr_maybe_lost++;
            }
        } else if (perf_mmap__empty(map)) {
            // Keep the lost_start event in the ringbuffer.
            if (mmap_event->lost_pause == 1)
                break;
            dev->order.nr_maybe_lost_pause++;
            mmap_event->lost_pause = 1;
            mmap_event->pause_start_time = get_ktime_ns();
            main_dev->order.break_reason = ORDER_BREAK_LOST_WAIT;
            break;
        }
        if (unlikely(mmap_event->lost_pause)) {
            mmap_event->lost_pause = 0;
            dev->order.maybe_lost_pause_time += get_ktime_ns() - mmap_event->pause_start_time;
        }
    skip_lost_detect:


        /* Keep order in perf_mmap. Why do this?
         *
         *       sh  99330 .... [036] 341318.491172: sched:sched_process_free
         *    ffffffff81086437 delayed_put_task_struct+0x87 ([kernel.kallsyms])
         *    ...
         *    ffffffff81089a45 irq_exit+0xd5 ([kernel.kallsyms])
         *    ffffffff81c024f3 smp_apic_timer_interrupt+0x83 ([kernel.kallsyms])
         *    ffffffff81c01a7f apic_timer_interrupt+0xf ([kernel.kallsyms])
         *    ffffffff811cfe6b __perf_event_header__init_id+0x9b ([kernel.kallsyms])
         *    ffffffff811dd5d7 perf_prepare_sample+0x67 ([kernel.kallsyms])
         *    ffffffff811ddaaf perf_event_output_forward+0x2f ([kernel.kallsyms])
         *    ffffffff811d1f77 __perf_event_overflow+0x57 ([kernel.kallsyms])
         *    ffffffff811d2053 perf_swevent_overflow+0x43 ([kernel.kallsyms])
         *    ffffffff811d212d perf_swevent_event+0x5d ([kernel.kallsyms])
         *    ffffffff811d2492 perf_tp_event+0xe2 ([kernel.kallsyms])
         *    ...
         *
         * After the time is obtained in __perf_event_header__init_id(), but before
         * it is output begin, an interrupt occurs, and the events later in time are
         * output first.
         */
        if (unlikely(time < mmap_event->event_mono_time)) {
            event = perf_mmap_fix_out_of_order(main_dev, dev, heap_event, mmap_event->event_mono_time, ins);
            time = mmap_event->event_mono_time;
            writable = 1;
        } else
            mmap_event->event_mono_time = time;

        // out of order
        if (dev != main_dev) dev->order.heap_popped_time = time;
        if (unlikely(time < main_dev->order.heap_popped_time)) {
            dev->order.nr_unordered_events++;
            fprintf(stderr, "%s: out-of-order event %lu(%d) < %s %lu(%d)\n", dev->prof->name,
                            time, ins, main_dev->prof->name, main_dev->order.heap_popped_time,
                            main_dev->order.heap_popped_ins);
        } else {
            main_dev->order.heap_popped_time = time;
            main_dev->order.heap_popped_ins = ins;
        }


    process:
        perf_event_process_record(dev, event, ins, writable, false);
    consume:
        // Not lost. Or lost, fast consuming will result in more losses.
        if (map->end != mmap_event->maybe_lost_end ||
            perf_mmap_has_space(map, wakeup_watermark/2))
            perf_mmap__consume(map);

        if (map == target_map && map->start == target_end) {
            main_dev->order.break_reason = ORDER_BREAK_TARGET_MAP;
            need_break = 1;
        }

        event = perf_mmap__read_event(map, &writable);
        if (event) {
            if (unlikely(event->header.type != PERF_RECORD_SAMPLE)) {
                if (event->header.type == PERF_RECORD_LOST) {
                    dev->order.nr_lost++;
                    if (dev->prof->lost) {
                        lost.id = event->lost.id;
                        lost.lost = event->lost.lost;
                        goto consume;
                    }
                }
                goto process;
            }

            heap_event->event = event;
            heap_event->time = heapclock(main_dev, *(u64 *)((void *)event->sample.array + dev->pos.time_pos));
            heap_event->writable = writable;
            min_heap_sift_down(heap, 0, &funcs, NULL);

            if (unlikely(lost.lost)) {
                if (mmap_event->event_mono_time/*lost_start*/ < main_dev->order.prev_lost_time)
                    fprintf(stderr, "BUG: unorder lost event\n");
                else
                    main_dev->order.prev_lost_time = mmap_event->event_mono_time;

                dev->prof->lost(dev, (union perf_event *)&lost, ins,
                                heapclock_to_evclock(main_dev, mmap_event->event_mono_time),
                                heapclock_to_evclock(main_dev, heap_event->time));
                lost.lost = 0;
            }
        } else {
            perf_mmap__read_done(map);
            min_heap_pop(heap, &funcs, NULL);
            prof_dev_put(dev);
        }

        if (need_break)
            break;
    }


stream_stop:
    while (heap->nr) {
        heap_event = heap->data[--heap->nr];
        if (heap_event->type == PERF_MMAP_EVENT) {
            mmap_event = (struct perf_mmap_event *)heap_event;
            perf_mmap__unread_event(mmap_event->map, heap_event->event);
            perf_mmap__consume(mmap_event->map);
        } else if (heap_event->type == STREAM_EVENT)
            heap_event->unconsumed = 1;

        prof_dev_put(heap_event->dev);
    }
    main_dev->order.inprocess = 0;
    prof_dev_put(main_dev);
}


void reduce_wakeup_times(struct prof_dev *dev, struct perf_event_attr *attr)
{
    struct env *env = dev->env;
    u32 wakeup_watermark = 0;
    u32 wakeup_events = 0;
    int watermark;

    if (!dev->pages)
        return;

    if (attr->sample_period == 0)
        return;

    if (env->watermark_set)
        watermark = env->watermark;
    else {
        if (!attr->watermark) {
            /*
             * Enable watermark to reduce the number of wake-ups.
             * For device wakeup_events = 1, when used as a child-device, adjust to use watermark.
             *
             * In principle, it should be the forwarding source device, which will be flushed by
             * the parent device. However, when reduce_wakeup_times() is called, the device cannot
             * yet be marked as a forwarding source, so prof_dev_has_parent() is used.
             */
            if (prof_dev_has_parent(dev))
                watermark = 50;
            else {
                watermark = 0;
                wakeup_events = attr->wakeup_events;
            }
        } else {
            watermark = 50;
            wakeup_watermark = attr->wakeup_watermark;
        }
    }

    if (watermark == 0) {
        attr->watermark = 0;
        attr->wakeup_events = wakeup_events ? : 1;
    } else {
        if (wakeup_watermark == 0) {
            u32 pages_watermark = (dev->pages << 12);
            wakeup_watermark = pages_watermark * watermark / 100;
        }
        attr->watermark = 1;
        attr->wakeup_watermark = wakeup_watermark;
    }
}

void prof_dev_env2attr(struct prof_dev *dev, struct perf_event_attr *attr)
{
    struct env *env = dev->env;

    // Only for public options within PROFILER_ARGV_OPTION.

    // --watermark
    reduce_wakeup_times(dev, attr);

    // --monotonic
    if (env->monotonic) {
        attr->use_clockid = 1;
        attr->clockid = CLOCK_MONOTONIC;
    }

    // --user-callchain=dwarf
    // Keep kernel FP-based user callchain enabled (exclude_callchain_user = 0),
    // so the kernel collects both FP callchain and DWARF data (regs + stack).
    // build_callchain() merges the two: DWARF unwind first, then appends
    // remaining FP frames for binaries lacking DWARF info (e.g. Go).
    if (env->callchain && env->dwarf_record_size &&
        (attr->sample_type & PERF_SAMPLE_CALLCHAIN)) {
        attr->sample_type |= PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
        attr->sample_regs_user = PERF_REGS_MASK;
        attr->sample_stack_user = env->dwarf_record_size;
        /* Enable kernel FP-based user callchain */
        attr->exclude_callchain_user = 0;
    }
}

