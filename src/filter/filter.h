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

/*
 * Pid ranges for a tracepoint filter that covers a set of pids, and maybe
 * every pid from a tail on, within a length budget: the filter is a superset,
 * the exact match is left to userspace.
 */
struct pid_ranges {
    int nr;
    struct pid_range {
        int lo, hi; // hi == INT_MAX: the open tail, field>=lo.
    } r[];
};

/*
 * Cover the pids of @threads and, when @tail >= 0, the pids allocated next:
 * from @tail up to the next pid of @threads, every pid from @tail on when
 * there is none above it. The gaps are merged, smallest first, until the
 * filter on @field fits in @budget bytes; with no gap left it is taken as it
 * is.
 */
struct pid_ranges *pid_ranges_new(struct perf_thread_map *threads, int tail,
                                  const char *field, size_t budget);
bool pid_ranges_has(const struct pid_ranges *ranges, int pid);
// Return an allocated string, which needs to be freed. No range: field<0.
char *pid_ranges_filter(const struct pid_ranges *ranges, const char *field);

struct tp_filter *tp_filter_new(struct perf_thread_map *threads, const char *pid_field,
                                     const char *filter, const char *comm_field);
void tp_filter_free(struct tp_filter *tp_filter);



#endif
