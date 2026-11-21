#ifndef __FILTER_H
#define __FILTER_H

#include <perf/threadmap.h>

struct bpf_filter {
    void *obj;
    int bpf_fd;
    // args
    bool filter_irqs_disabled;
    bool irqs_disabled;
    bool filter_tif_need_resched;
    bool tif_need_resched;
    bool filter_sched_policy;
    u32 sched_policy;
    char *prio_map;
    bool filter_exclude_pid;
    u32 exclude_pid;
    bool filter_nr_running;
    u32 nr_running_min;
    u32 nr_running_max;
    bool filter_runtime_greater;
    u32 runtime_greater;
};
int bpf_filter_open(struct bpf_filter *filter);
void bpf_filter_close(struct bpf_filter *filter);
int bpf_filter_init(struct bpf_filter *filter, struct env *env);


struct tp_filter {
    char *filter;
    char *comm; // comm ~ "xyz*" || comm ~ "abc?"
    char *pid;  //perf_thread_map, pid==x || pid==y || pid==z
};

// Return an allocated string, which needs to be freed.
char *pid_filter(struct perf_thread_map *threads, const char *field);
// Return an allocated string, which needs to be freed.
char *cpu_filter(struct perf_cpu_map *cpus, const char *field);

struct tp_filter *tp_filter_new(struct perf_thread_map *threads, const char *pid_field,
                                     const char *filter, const char *comm_field);
void tp_filter_free(struct tp_filter *tp_filter);

/*
 * event_filter: the event filter on one field over a set of values,
 * "(and_filter) && (ranges)". @and_filter is an extra condition, the filter
 * lets in what passes both. Inside:
 *  - value_set: the values, 8-byte signed or unsigned, shared by the
 *    filters on the same values.
 *  - value_ranges: the ranges of values the filter lets in, for the length
 *    limit. A tracepoint filter is at most a page (SET_FILTER and ftrace take
 *    no more), "pid==a||pid==b||..." for a few hundred values is over it.
 *    Consecutive values make a range, the smallest gaps between the ranges
 *    are merged until it fits: the merging is done here. The filter is a
 *    superset then, event_filter_exact() is false: the kernel drops most of
 *    the records, the caller the few left by event_filter_has().
 *
 * It only makes the string: no evsel, no prof_dev, no raw data.
 */
struct event_filter;

/* 8-byte values, @is_signed or not; @and_filter may be NULL. */
struct event_filter *event_filter_new(const char *field, const char *and_filter, bool is_signed);
void event_filter_free(struct event_filter *ef);
/* 1, added; 0, there already; <0 on error. */
int  event_filter_add(struct event_filter *ef, s64 value);
/* 1, deleted; 0, not there. */
int  event_filter_del(struct event_filter *ef, s64 value);
bool event_filter_has(struct event_filter *ef, s64 value);
/* The filter over the values now, owned by @ef until the next build; NULL on error. */
const char *event_filter_build(struct event_filter *ef);
/* The filter built lets in the values only: no gap merged, none deleted since. */
bool event_filter_exact(struct event_filter *ef);

/*
 * pid_filter: set the kernel filter late. An event_filter over the pids of
 * the followed threads, for the threads that come and go (--ptrace, a
 * workload); it decides when to sync the filter with the kernel.
 *
 * A fd's filter cannot be changed (SET_FILTER returns EEXIST): each sync
 * replaces the evsel, a fd opened on every cpu. Threads are created and exit
 * fast, a sync each is too many; with many threads the gaps are merged to
 * fit in a page, the filter lets in more anyway. So the kernel filter is
 * wider than the threads: an open tail, pid>=x, lets in the threads to come,
 * they get the next pids; the exited ones stay let in a while. The kernel
 * drops most of the records, pid_filter_match() the few let in for nothing.
 *
 * When to sync: a new thread the kernel filter does not let in, at once; the
 * rest (exited threads, merged gaps, the open tail, too many dropped by
 * pid_filter_match()) is tightened later by its own timer, out of event
 * processing.
 *
 *   pid_filter_new()
 *   pid_filter_add_threads(), pid_filter_add()    the initial threads
 *   pid_filter_start()                            ops->set(initial)
 *   pid_filter_add(), pid_filter_del()            threads come and go
 *   pid_filter_match()                            a record let in
 *   pid_filter_free()
 *
 * The filters of @share share its threads: pid_filter_add() and
 * pid_filter_del() are called on each of them.
 */
struct pid_filter;
struct pid_filter_ops {
    /*
     * Put @filter in the kernel. @initial: by pid_filter_start(), the evsel
     * is not enabled yet; otherwise replace it, prof_dev_replace_filter().
     * 0 on success, the new filter only is in effect from now on: while
     * the old and new evsels both write, prof_dev_replace_dup() of the
     * framework drops the duplicates, the old one's records after the new
     * one's first on each ring buffer. <0, the old filter keeps working,
     * retried later.
     */
    int  (*set)(struct pid_filter *pf, void *opaque, const char *filter, bool initial);
    /*
     * The kernel filter lets in more than the followed threads (@need), or
     * no longer: records need pid_filter_match(). Called on a change only,
     * from false at first.
     */
    void (*ftrace_filter)(struct pid_filter *pf, void *opaque, bool need);
};

struct pid_filter *pid_filter_new(const char *field, const char *and_filter, struct pid_filter *share,
                                  const struct pid_filter_ops *ops, void *opaque);
void pid_filter_free(struct pid_filter *pf);
/* The threads of @threads, cgroups and -1 left out, before pid_filter_start(). */
int  pid_filter_add_threads(struct pid_filter *pf, struct perf_thread_map *threads);
/*
 * Follow @pid: 1, added; 0, followed already; <0 on error. A failed sync is
 * no error, it is retried later.
 */
int  pid_filter_add(struct pid_filter *pf, pid_t pid);
/* Not follow @pid: 1, deleted; 0, not followed. */
int  pid_filter_del(struct pid_filter *pf, pid_t pid);
/* Set the filter over the initial threads; <0 on error, not started. */
int  pid_filter_start(struct pid_filter *pf);
/* @pid of a record let in: 1, followed; 0, not, too many tighten the filter. */
int  pid_filter_match(struct pid_filter *pf, pid_t pid);
/* @pid is followed, not counted. */
bool pid_filter_follows(struct pid_filter *pf, pid_t pid);
/* The filter in the kernel, NULL: not started. */
const char *pid_filter_string(struct pid_filter *pf);


#endif
