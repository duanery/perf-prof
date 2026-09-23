#ifndef __TWO_EVENT_H
#define __TWO_EVENT_H

#include <stack_helpers.h>

struct two_event_class;
struct two_event_impl;

typedef enum {
    REMAINING_CONTINUE,
    REMAINING_BREAK,
} remaining_return;

typedef enum {
    NOT_REMAINING,
    REMAINING_LOST,
    REMAINING_SYSCALLS, // exit, exit_group
    REMAINING_EXIT,
} remaining_reason;

struct two_event_options {
    const char *keyname;
    int keylen;
    bool binding_key;
    bool perins;
    bool comm;
    bool only_print_greater_than;
    unsigned long greater_than;
    unsigned long lower_than;
    unsigned long hide_than; // hide<1ms
    char *heatmap;
    unsigned int first_n;
    bool sort_print;
    struct env *env;
};

struct event_info {
    struct tp *tp1;
    struct tp *tp2;
    u64 key;
    u64 recent_time; // The most recent time for all known events.
    remaining_reason rr;
};

// -|- - - -|- - - -|- - - -
// -1ms     e1      e2     +1ms
//start   event1  event2   `end
struct event_iter {
    void *start;
    void *event1;
    void *event2;
    void *curr;
    union perf_event *event;
    struct tp *tp;
    u64 time;

    int recent_cpu; // samecpu: cpu tracking.
    const char *debug_msg;
    const char *reason;

    // rundelay, sched_switch
    int curr_cpu[3];
    int curr_pid[3];  // samecpu-1, samecpu-2, samecpu-track
    u64 curr_time[3]; // samecpu-1, samecpu-2, samecpu-track
    u64 running_time;
    const char *comm;
};

struct two_event {
    /* object */
    struct two_event_class *class;
    struct rb_node rbnode;
    struct tp *tp1;
    struct tp *tp2;
    struct rb_node rbnode_byid;
    unsigned int id;
    bool deleting;
};

struct two_event_class {
    /* class object */
    struct two_event_impl *impl;
    struct two_event_options opts;
    struct rblist two_events;
    struct rblist two_events_byid;
    unsigned int ids;

    /* object function */
    /*
     * iter, On the timeline, the events between event1 and event2 can be iterated through iter.
     */
    void (*two)(struct two_event *two, union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter);
    remaining_return (*remaining)(struct two_event *two, union perf_event *event1, struct event_info *info, struct event_iter *iter);
    int (*print_header)(struct two_event *two);
    void (*print)(struct two_event *two);
};

struct two_event_impl {
    /* impl object */
    const char *name;
    int class_size;
    struct two_event_class *(*class_new)(struct two_event_impl *impl, struct two_event_options *options);
    void (*class_delete)(struct two_event_class *class);

    /* class function */
    int instance_size;
    struct two_event *(*object_new)(struct two_event_class *class, struct tp *tp1, struct tp *tp2);
    void (*object_delete)(struct two_event_class *class, struct two_event *two);
    struct two_event *(*object_find)(struct two_event_class *class, struct tp *tp1, struct tp *tp2);
};


/* delay analysis:
 * syscall delay
 * kvm_exit to kvm_entry delay
 * hrtimer_start to hrtimer_expire_entry delay
 * and many more
 */
#define TWO_EVENT_DELAY_IMPL "delay"
/*
 * event pair:
 * kmemleak, alloc and free
 * fdleak, open and close
 */
#define TWO_EVENT_PAIR_IMPL "pair"

/*
 * mem profile:
 * mem-profile, alloc and free bytes
 */
#define TWO_EVENT_MEM_PROFILE "kmemprof"


#define TWO_EVENT_SYSCALLS_IMPL "syscalls"
extern const char *syscalls_table[];

/*
 * Analyze function calls.
 */
#define TWO_EVENT_CALL_IMPL "call"

/*
 * Analyze function calls. Also analyze function time.
 * Print function calls and time statistics.
 */
#define TWO_EVENT_CALL_DELAY_IMPL "call-delay"

struct two_event_impl *impl_get(const char *name);
bool impl_based_on_call(const char *name);
void two_event_class_print(struct two_event_class *class, int indent);


// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW
struct multi_trace_type_header {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    __u64   id;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    __u64		period;
};
struct multi_trace_type_callchain {
    struct multi_trace_type_header h;
    struct callchain callchain;
};
struct multi_trace_type_raw {
    struct multi_trace_type_header h;
    struct {
        __u32   size;
        __u8    data[0];
    } raw;
};

void multi_trace_raw_size(union perf_event *event, void **praw, int *psize, struct tp *tp);
void multi_trace_print_title(union perf_event *event, struct tp *tp, const char *title);
static inline void multi_trace_print(union perf_event *event, struct tp *tp)
{
    multi_trace_print_title(event, tp, NULL);
}
bool event_need_to_print(union perf_event *event1, union perf_event *event2, struct event_info *info, struct event_iter *iter);


/*
 * while (event_iter_cmd(iter, CMD_NEXT)) {
 *     multi_trace_print_title(iter->event, iter->tp, title);
 * }
 */
enum event_iter_cmd {
    // locate to start
    CMD_RESET,

    // locate to event1
    CMD_EVENT1,

    // locate to event2
    CMD_EVENT2,

    // prev and next
    CMD_PREV,
    CMD_NEXT,

    CMD_MAX,
};
int event_iter_cmd(struct event_iter *iter, enum event_iter_cmd cmd);


#endif
